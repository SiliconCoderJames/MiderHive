#pragma once
// 通用工具：SHA-256、随机标识、UTC 时间、UTF-8 处理、环境变量读取。
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // 本仓库大量使用 std::min/std::max，不许 windows.h 的宏污染
#endif
#include <windows.h>
#endif

namespace ah {

// 读取环境变量：多个名字按顺序取第一个非空者（品牌更名后依次为
// MIDERHIVE_* → AGENTHIVE_* → ZCODE_*，旧名永久兼容识别）。
// Windows 下不能用 std::getenv：CRT 初始化时会把环境块按 ANSI 代码页转成
// 窄字符串（中文系统是 GBK），而平台全程以 UTF-8 存取身份名/数据目录——
// 中文名经 getenv 拿到的 GBK 字节与库中 UTF-8 字节永远匹配不上（401 或
// 重复身份）。必须从 UTF-16 环境块直接转 UTF-8。
inline std::string envOr(std::initializer_list<const char*> names,
                         const std::string& fallback = {}) {
    for (const char* name : names) {
#ifdef _WIN32
        const wchar_t* wname = nullptr;
        wchar_t wbuf[256];
        {
            // 名字是编译期 ASCII 字面量，逐字符加宽即可
            size_t i = 0;
            for (; name[i] != '\0' && i < sizeof(wbuf) / sizeof(wbuf[0]) - 1; ++i)
                wbuf[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
            wbuf[i] = L'\0';
            wname = wbuf;
        }
        const DWORD n = GetEnvironmentVariableW(wname, nullptr, 0);
        if (n > 0 && n < 32768) {
            std::wstring w(static_cast<size_t>(n), L'\0');
            GetEnvironmentVariableW(wname, w.data(), n);
            while (!w.empty() && w.back() == L'\0') w.pop_back();
            if (!w.empty()) {
                if (const int u =
                        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
                    u > 0) {
                    std::string out(static_cast<size_t>(u), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        out.data(), u, nullptr, nullptr);
                    while (!out.empty() && out.back() == '\0') out.pop_back();
                    if (!out.empty()) return out;
                }
            }
        }
#else
        if (const char* env = std::getenv(name); env && *env) return env;
#endif
    }
    return fallback;
}

std::string sha256Hex(const std::string& data);
// 常量时间字符串比较：用于校验密钥。std::string::operator== 逐字节短路返回，
// 比较耗时随"前多少字节相同"变化，理论上可被计时侧信道利用。
bool constantTimeEquals(const std::string& a, const std::string& b);
std::string randomHex(int bytes);
std::string uuid4();                      // 8-4-4-4-12 形式
// RSA-2048 PKCS1v15/SHA256 验签（更新清单签名校验的底层原语）。modulus /
// exponent 为大端 hex（openssl rsa -modulus 输出口径，指数 65537 即 "010001"），
// signature 为 base64。任何输入非法（坏 hex / 坏 base64 / 长度不符）或验签不
// 通过都返回 false；成功与否的处置（拒绝/放行）是调用方的策略。
// 非 Windows 构建恒返回 false（更新器仅存在于 Windows GUI）。
bool rsaVerifySha256Pkcs1(const std::string& modulusHex, const std::string& exponentHex,
                          const std::string& message, const std::string& signatureBase64);
std::string nowIso();                     // UTC ISO8601，如 2026-09-07T05:00:00Z
std::string weekStartIso();               // 本周一（UTC 00:00）
bool parseIso(const std::string& iso, std::time_t& out);  // 解析本平台生成的 ISO 时间
std::string isoDaysAgo(int days);         // days 天前的 UTC ISO8601（维护轮转用）
std::vector<uint32_t> utf8Codepoints(const std::string& s);
std::string toLower(const std::string& s);
std::string join(const std::vector<std::string>& v, const std::string& sep);

}  // namespace ah
