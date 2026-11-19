// WinHTTP 传输实现。要点：
//   - 会话级 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY：系统代理/PAC 免费生效
//     （访问 api.openai.com / api.anthropic.com 常需系统代理，这是选型主因）；
//   - URL 用 WinHttpCrackUrl 拆解，UTF-8 一律先转 UTF-16 再进 WinHTTP
//     （与平台 fsPath/envOr 同一防错位纪律——绝不按 ANSI 代码页窄转换）；
//   - 状态码先判：>=400 不流式回调，整读进 body 供上层翻译错误；
//   - 流式：QueryDataAvailable/ReadData 循环逐块喂 onChunk，回调返回 false
//     即中止（用户取消场景，err="cancelled"）；
//   - 错误码翻译成人话并附原始码，绝不静默失败（项目防呆纪律）。
// 平台守卫：非 Windows 编译为恒败桩（与 rsaVerifySha256Pkcs1 同策略）。
#include "core/llm/transport.h"

#include <atomic>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace ah::llm {

namespace {

#ifndef _WIN32
// 非 Windows：本平台暂不支持 LLM 外呼（诚实报错，绝不静默成功）
class StubTransport final : public ITransport {
public:
    HttpResponse send(const HttpRequest&, const ChunkSink&) override {
        HttpResponse out;
        out.err = "LLM calls are not supported on this platform yet "
                  "(the transport currently ships on Windows only)";
        return out;
    }
    void cancel() override {}
};
#else

// ---- UTF-8 <-> UTF-16（显式声明编码，绝不走 ANSI 代码页窄转换）----
std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

// 常见 WinHTTP 错误码 → 人话。查表未中时给通用文案 + 原始码（便于排障）。
std::string winhttpError(DWORD code) {
    switch (code) {
        case 12002:  // ERROR_WINHTTP_TIMEOUT
            return "request timed out (12002)";
        case 12007:  // ERROR_WINHTTP_NAME_NOT_RESOLVED
            return "cannot resolve the host name (12007) - check the URL and your network";
        case 12029:  // ERROR_WINHTTP_CANNOT_CONNECT
            return "connection refused (12029) - is the endpoint reachable?";
        case 12030:  // ERROR_WINHTTP_CONNECTION_ERROR
            return "connection dropped (12030)";
        case 12005:  // ERROR_WINHTTP_SECURE_FAILURE
            return "secure channel error (12005)";
        case 12157:  // ERROR_WINHTTP_SECURE_CHANNEL_ERROR（最常见 TLS 错误：
                     // 老系统缺根证书/代理 MITM/协议不匹配）
            return "TLS handshake error (12157)";
        case 12175:  // ERROR_WINHTTP_SECURE_FAILURE (证书链/算法)
            return "TLS certificate error (12175)";
        case 12008:  // ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR
            return "system proxy auto-detection failed (12008)";
        default: {
            return "network error (WinHTTP " + std::to_string(code) + ")";
        }
    }
}

class WinHttpTransport final : public ITransport {
public:
    // 硬中止：从另一线程关闭请求句柄，正在阻塞的 send 立即以错误返回
    // （MSDN：跨线程 WinHttpCloseHandle 是取消同步调用的正规手段）。
    void cancel() override {
        cancelled_ = true;
        std::lock_guard lock(hm_);
        if (hreq_) {
            WinHttpCloseHandle(hreq_);
            hreq_ = nullptr;  // 所有权移交给 cancel；send 线程不再重复关它
        }
    }

    HttpResponse send(const HttpRequest& req, const ChunkSink& onChunk) override {
        cancelled_ = false;
        // ---- URL 拆解（UTF-8 → 宽；query 段并回路径，不能丢）----
        std::wstring url = toWide(req.url);
        if (url.empty()) {
            HttpResponse bad;
            bad.err = "invalid request URL (not valid UTF-8)";
            return bad;
        }
        URL_COMPONENTSW uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t schemeBuf[16] = L"";
        wchar_t hostBuf[256] = L"";
        wchar_t pathBuf[2048] = L"";
        wchar_t extraBuf[1024] = L"";
        uc.dwSchemeLength = static_cast<DWORD>(std::size(schemeBuf)) - 1;
        uc.lpszScheme = schemeBuf;
        uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf)) - 1;
        uc.lpszHostName = hostBuf;
        uc.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf)) - 1;
        uc.lpszUrlPath = pathBuf;
        uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extraBuf)) - 1;
        uc.lpszExtraInfo = extraBuf;
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
            HttpResponse bad;
            bad.err = "cannot parse the endpoint URL: " + req.url;
            return bad;
        }
        const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
        if (!secure && uc.nScheme != INTERNET_SCHEME_HTTP) {
            HttpResponse bad;
            bad.err = "endpoint URL must start with http:// or https://";
            return bad;
        }
        const INTERNET_PORT port = uc.nPort ? uc.nPort : (secure ? 443 : 80);

        // ---- 会话（系统代理自动生效；老系统退回默认代理解析）----
        HINTERNET session = WinHttpOpen(L"MiderHive", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"MiderHive", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            HttpResponse net;
            net.err = winhttpError(GetLastError());
            return net;
        }
        // TLS 钉在 1.2+（1.3 的 SDK 常量缺就只钉 1.2）：防系统策略回退旧协议
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET connect = WinHttpConnect(session, hostBuf, port, 0);
        if (!connect) {
            HttpResponse net;
            net.err = winhttpError(GetLastError());
            WinHttpCloseHandle(session);
            return net;
        }

        std::wstring path = pathBuf;
        path += extraBuf;  // query 段（LLM 端点一般为空，但不可静默丢弃）
        HINTERNET request = WinHttpOpenRequest(connect, L"POST", path[0] ? path.c_str() : L"/",
                                               nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            HttpResponse net;
            net.err = winhttpError(GetLastError());
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return net;
        }
        {
            std::lock_guard lock(hm_);
            hreq_ = request;  // cancel() 可见：从此刻起另一线程可硬中止
        }

        WinHttpSetTimeouts(request, req.connectTimeoutSec * 1000, req.connectTimeoutSec * 1000,
                           req.sendTimeoutSec * 1000, req.receiveTimeoutSec * 1000);

        // 头部：宽字符拼接。Authorization/x-api-key 是请求的命根子——任一头
        // 转换失败（非 UTF-8）必须整单拒绝，而不是静默发一个缺头的请求
        std::wstring headers;
        for (const auto& [k, v] : req.headers) {
            const std::wstring wk = toWide(k), wv = toWide(v);
            if (wk.empty() || wv.empty()) {
                std::lock_guard lock(hm_);
                hreq_ = nullptr;
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                HttpResponse bad;
                bad.err = "request header is not valid UTF-8: " + k;
                return bad;
            }
            headers += wk + L": " + wv + L"\r\n";
        }

        HttpResponse res;
        auto bail = [&](const std::string& msg) {
            res.err = msg;
            {
                std::lock_guard lock(hm_);
                if (hreq_) { WinHttpCloseHandle(hreq_); hreq_ = nullptr; }
                // hreq_ 已被 cancel() 关走时不要再碰那个句柄值
            }
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return res;
        };

        const BOOL ok = WinHttpSendRequest(
            request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            static_cast<DWORD>(headers.size()),
            req.body.empty() ? WINHTTP_NO_REQUEST_DATA
                             : reinterpret_cast<LPVOID>(const_cast<char*>(req.body.data())),
            static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0);
        if (!ok) return bail(cancelled_ ? "cancelled" : winhttpError(GetLastError()));
        if (!WinHttpReceiveResponse(request, nullptr))
            return bail(cancelled_ ? "cancelled" : winhttpError(GetLastError()));

        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX))
            return bail(cancelled_ ? "cancelled" : winhttpError(GetLastError()));
        res.status = static_cast<long>(status);

        // ---- 读体：2xx 且调用方要流式 → 逐块回调；错误响应只缓冲（无需流式）----
        constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;  // 32MB：防慢滴端点无限吃内存
        const bool stream = status >= 200 && status < 300 && static_cast<bool>(onChunk);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail))
                return bail(cancelled_ ? "cancelled" : winhttpError(GetLastError()));
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read))
                return bail(cancelled_ ? "cancelled" : winhttpError(GetLastError()));
            chunk.resize(read);
            if (!chunk.empty()) {
                res.body += chunk;
                if (res.body.size() > kMaxBodyBytes)
                    return bail("response body too large (>32MB) - aborting to protect memory");
                if (stream && !onChunk(chunk.data(), chunk.size())) {
                    res.err = "cancelled";  // 调用方主动中止（如关闭对话框）
                    break;
                }
            }
        }
        {
            std::lock_guard lock(hm_);
            if (hreq_) { WinHttpCloseHandle(hreq_); hreq_ = nullptr; }
        }
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return res;
    }

private:
    // cancel() 与 send() 分属两线程：句柄登记在互斥下（被 cancel 关走后
    // send 侧不再重复关闭同一句柄值）
    std::mutex hm_;
    HINTERNET hreq_ = nullptr;
    std::atomic<bool> cancelled_{false};
};
#endif  // _WIN32

}  // namespace

std::unique_ptr<ITransport> makeDefaultTransport() {
#ifdef _WIN32
    return std::make_unique<WinHttpTransport>();
#else
    return std::make_unique<StubTransport>();
#endif
}

}  // namespace ah::llm
