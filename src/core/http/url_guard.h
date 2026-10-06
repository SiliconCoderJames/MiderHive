#pragma once
// 出站 URL 安全校验（防 SSRF）：
//   仅允许 http/https；host 先做百分号解码与尾点归一（编码/全限定名形态不能
//   骗过字面量检查），再拒绝 localhost、环回、私有、保留地址。
// 平台本体只监听 127.0.0.1、不发外部请求；未来任何「代为抓取 URL」
// 类的能力必须先经过本校验。纯头文件，可独立单测。
#include <cstdint>
#include <string>

namespace ah::net {

inline bool ipv4InRange(uint32_t ip, uint8_t a, uint8_t b, uint8_t c, uint8_t d, int prefix) {
    uint32_t net = (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
    uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
    return (ip & mask) == (net & mask);
}

inline bool parseIpv4(const std::string& s, uint32_t& out) {
    unsigned parts[4];
    int n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i != s.size() && s[i] != '.') continue;
        const size_t len = i - start;
        if (len == 0 || len > 3) return false;          // 空段 / 过长的段
        if (len > 1 && s[start] == '0') return false;   // 前导零：0177 会被系统按八进制解析
        unsigned v = 0;
        for (size_t k = start; k < i; ++k) {
            if (s[k] < '0' || s[k] > '9') return false;
            v = v * 10 + static_cast<unsigned>(s[k] - '0');
        }
        if (v > 255) return false;
        if (n >= 4) return false;                       // 多于四段
        parts[n++] = v;
        start = i + 1;
    }
    if (n != 4) return false;
    out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

inline bool isPrivateIpv4(uint32_t ip) {
    return ipv4InRange(ip, 0, 0, 0, 0, 8) ||        // 0.0.0.0/8 本网
           ipv4InRange(ip, 10, 0, 0, 0, 8) ||       // 私有
           ipv4InRange(ip, 100, 64, 0, 0, 10) ||    // CGNAT
           ipv4InRange(ip, 127, 0, 0, 0, 8) ||      // 环回
           ipv4InRange(ip, 169, 254, 0, 0, 16) ||   // 链路本地
           ipv4InRange(ip, 172, 16, 0, 0, 12) ||    // 私有
           ipv4InRange(ip, 192, 0, 0, 0, 24) ||     // IETF 保留
           ipv4InRange(ip, 192, 168, 0, 0, 16) ||   // 私有
           ipv4InRange(ip, 198, 18, 0, 0, 15) ||    // 基准测试
           ipv4InRange(ip, 224, 0, 0, 0, 4) ||      // 组播
           ipv4InRange(ip, 240, 0, 0, 0, 4);        // 保留
}

inline bool isPrivateIpv6(const std::string& host) {
    std::string h;
    for (char ch : host) h += static_cast<char>((ch >= 'A' && ch <= 'F') ? ch - 'A' + 'a' : ch);
    if (h == "::" || h == "::1" || h == "0:0:0:0:0:0:0:1") return true;            // 环回
    if (h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) return true;               // ULA fc00::/7
    if (h.rfind("fe8", 0) == 0 || h.rfind("fe9", 0) == 0 || h.rfind("fea", 0) == 0 ||
        h.rfind("feb", 0) == 0) return true;                                       // 链路本地 fe80::/10
    if (h.rfind("::ffff:", 0) == 0) {                                              // IPv4 映射
        uint32_t v4 = 0;
        return !parseIpv4(h.substr(7), v4) || isPrivateIpv4(v4);
    }
    return false;
}

// host 规范化：百分号解码（%31%32%37.0.0.1 会被解码方还原成 127.0.0.1，不能
// 让编码形态骗过字面量检查）+ 去一个尾点（"localhost." 是指向同一处
// 的全限定名写法，Windows 解析器实测解析到环回）。失败（非法 % 序列）返回 false。
inline bool normalizeHost(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char ch = in[i];
        if (ch == '%') {
            if (i + 2 >= in.size()) return false;
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            ch = static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        out += ch;
    }
    if (out.empty()) return false;
    if (out.back() == '.') out.pop_back();  // 只去一个尾点；"a..b" 属怪异形态走后面默认拒绝
    if (out.empty()) return false;
    return true;
}

// 返回 true 表示允许访问；否则拒绝。
inline bool isSafeOutboundUrl(const std::string& url) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    std::string scheme;
    for (size_t i = 0; i < schemeEnd; ++i)
        scheme += static_cast<char>((url[i] >= 'A' && url[i] <= 'Z') ? url[i] - 'A' + 'a' : url[i]);
    if (scheme != "http" && scheme != "https") return false;

    std::string rest = url.substr(schemeEnd + 3);
    size_t slash = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, slash);
    std::string host = authority;
    if (host.empty()) return false;
    // 去端口（v6 用方括号）
    if (host.front() == '[') {
        size_t close = host.find(']');
        if (close == std::string::npos) return false;
        host = host.substr(1, close - 1);
    } else {
        size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
    }
    if (host.empty()) return false;
    // 用户名信息 user@host —— 拒绝含 @ 的歧义形式
    if (host.find('@') != std::string::npos) return false;
    // 百分号解码 + 去尾点：先还原成解析方真正会看到的形态再做字面量检查
    std::string normalized;
    if (!normalizeHost(host, normalized)) return false;
    host = normalized;

    uint32_t v4 = 0;
    if (parseIpv4(host, v4)) return !isPrivateIpv4(v4);
    // 非规范的 IPv4 写法会被系统解析器（inet_addr / getaddrinfo）当成真实地址，
    // 但上面的"四段十进制"解析认不出来：127.1、0177.0.0.1、0x7f.1、2130706433
    // 都指向 127.0.0.1。凡是"含数字且只由数字/点/十六进制字符组成"的 host 一律拒绝。
    // 注意：本校验只处理字面地址、不做 DNS 解析——解析到内网的域名仍会放行，
    // 因此这层过滤不能替代真正的出站网络策略。
    bool digit = false, numeric = true;
    for (char ch : host) {
        const bool isDigit = ch >= '0' && ch <= '9';
        digit |= isDigit;
        const bool isHex = isDigit || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!isHex && ch != '.' && ch != 'x' && ch != 'X') { numeric = false; break; }
    }
    if (digit && numeric) return false;
    if (host.find(':') != std::string::npos) return !isPrivateIpv6(host);

    std::string h;
    for (char ch : host) h += static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    if (h == "localhost") return false;
    if (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0) return false;
    return true;
}

}  // namespace ah::net
