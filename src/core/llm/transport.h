#pragma once
// LLM 外呼的底层 HTTP 传输抽象。协议无关：上层（llm_client）只描述
// 「方法 + URL + 头 + 体」，流式与否由分块回调表达。
// Windows 走 WinHTTP：TLS 用系统证书栈（免维护）、代理走系统设置
// （含 PAC——访问境外 provider 在国内环境常需系统代理，这是选 WinHTTP
// 而非 vendor OpenSSL 的主要原因，见 2026-11-18 方案讨论）。
// 非 Windows 构建：本平台暂不支持 LLM 外呼（与"Windows 已验证"的项目现状
// 一致），返回的桩会把 err 置为可读的说明，绝不静默成功。
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ah::llm {

struct HttpRequest {
    std::string url;  // 完整 URL（含 scheme；http 仅用于本机 mock/本地端点）
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // UTF-8（JSON）
    // 超时（秒）。LLM 生成慢，接收给足（与本机嵌入端点 120s 同一口径）
    int connectTimeoutSec = 10;
    int sendTimeoutSec = 30;
    int receiveTimeoutSec = 120;
};

struct HttpResponse {
    long status = 0;    // HTTP 状态码；传输层失败时为 0
    std::string body;   // 响应体（流式调用时已缓冲全量，供错误解析与非流回退）
    std::string err;    // 非空 = 没拿到 HTTP 应答（连接/DNS/TLS/超时/取消），人话
};

// 流式分块回调：每收到一段网络数据触发一次；返回 false 中止读取（调用方
// 取消，如用户关掉问答对话框）。data 不保证以 \0 结尾，长度以 len 为准。
using ChunkSink = std::function<bool(const char* data, std::size_t len)>;

class ITransport {
public:
    virtual ~ITransport() = default;
    // onChunk 可传 nullptr（整体缓冲，不流式）。语义约定：
    //   - err 非空 → 其余字段无意义（未拿到 HTTP 应答）；
    //   - 否则 status 为 HTTP 状态码，body 为全量响应体。
    // 注意：状态码在流式读取前即可判——>=400 的响应绝不回调 onChunk，
    // 错误体只进 body，由上层（协议适配）翻译成人话。
    virtual HttpResponse send(const HttpRequest& req, const ChunkSink& onChunk) = 0;
};

// Windows：WinHTTP 实现；其他平台：恒失败的桩（err 说明平台限制）。
std::unique_ptr<ITransport> makeDefaultTransport();

}  // namespace ah::llm
