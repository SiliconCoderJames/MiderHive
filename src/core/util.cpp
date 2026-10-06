#include "core/util.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace ah {

namespace {
// ---- SHA-256 (FIPS 180-4) ----
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    // 恰好容纳 len + 0x80 + 8 字节长度，按 64 对齐；多出的零块会改变摘要
    std::vector<uint8_t> buf(((len + 9 + 63) / 64) * 64, 0);
    std::memcpy(buf.data(), data, len);
    buf[len] = 0x80;
    uint64_t bitlen = static_cast<uint64_t>(len) * 8;
    // 64 位大端长度
    for (int i = 0; i < 8; ++i)
        buf[buf.size() - 8 + i] = static_cast<uint8_t>(bitlen >> (56 - 8 * i));
    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(buf[off + 4 * i]) << 24) | (uint32_t(buf[off + 4 * i + 1]) << 16) |
                   (uint32_t(buf[off + 4 * i + 2]) << 8) | uint32_t(buf[off + 4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = uint8_t(h[i] >> 24); out[4 * i + 1] = uint8_t(h[i] >> 16);
        out[4 * i + 2] = uint8_t(h[i] >> 8);  out[4 * i + 3] = uint8_t(h[i]);
    }
}

const char* kHex = "0123456789abcdef";
std::string hexEncode(const uint8_t* data, size_t n) {
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = kHex[data[i] >> 4];
        s[2 * i + 1] = kHex[data[i] & 0x0f];
    }
    return s;
}

// 密码学安全的随机字节：master.key / Agent 密钥 / 盐 / uuid 全部出自这里。
// 此前是仅 64 位种子的 mt19937_64——在声明的威胁模型（本机互信）内不构成可利用
// 缺口，但凭据生成不该依赖可预测源，纵深防御几乎零成本。
void secureRandomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    // CNG 的系统首选 RNG；NTSTATUS 为 0 即成功。失败意味着熵源不可用——
    // 与其返回可预测的"密钥"不如显式中止（"绝不静默失败"的极限情形）
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        std::abort();
    }
#else
    static std::random_device rd;
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(rd());
#endif
}

std::tm utcTm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

}  // namespace

std::string sha256Hex(const std::string& data) {
    uint8_t out[32];
    sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
    return hexEncode(out, 32);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    // 长度差异先折进结果，再对 max(len) 逐字节异或累加：
    // 不提前 return，比较步数不随内容变化（std::string::operator== 会在首个不同字节短路）
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned>(ca ^ cb);
    }
    return diff == 0;
}

std::string randomHex(int bytes) {
    std::string s(bytes * 2, '0');
    std::vector<uint8_t> buf(static_cast<size_t>(bytes));
    secureRandomBytes(buf.data(), buf.size());
    for (int i = 0; i < bytes; ++i) {
        s[2 * i] = kHex[buf[i] >> 4];
        s[2 * i + 1] = kHex[buf[i] & 0x0f];
    }
    return s;
}

namespace {
// 标准 base64 解码（忽略空白）；非法输入返回空串。仅服务 rsaVerifySha256Pkcs1。
std::string base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3);
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int v = val(c);
        if (v < 0) return {};
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xff);
        }
    }
    return out;
}
}  // namespace

bool rsaVerifySha256Pkcs1(const std::string& modulusHex, const std::string& exponentHex,
                          const std::string& message, const std::string& signatureBase64) {
#ifdef _WIN32
    auto hexDecode = [](const std::string& hex) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        if (hex.empty() || hex.size() % 2 != 0) return out;
        for (size_t i = 0; i < hex.size(); i += 2) {
            const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return std::string();
            out += static_cast<char>((hi << 4) | lo);
        }
        return out;
    };
    const std::string modulus = hexDecode(modulusHex);
    const std::string exponent = hexDecode(exponentHex);
    const std::string sig = base64Decode(signatureBase64);
    if (modulus.empty() || exponent.empty() || sig.empty()) return false;

    // BCRYPT_RSAKEY_BLOB（公钥）：Magic + BitLength + 三段长度，随后大端指数、大端模数
    std::string blob(sizeof(BCRYPT_RSAKEY_BLOB), '\0');
    auto putU32 = [&blob](size_t off, uint32_t v) {
        blob[off] = static_cast<char>(v & 0xff);
        blob[off + 1] = static_cast<char>((v >> 8) & 0xff);
        blob[off + 2] = static_cast<char>((v >> 16) & 0xff);
        blob[off + 3] = static_cast<char>((v >> 24) & 0xff);
    };
    putU32(0, BCRYPT_RSAPUBLIC_MAGIC);
    putU32(4, static_cast<uint32_t>(modulus.size() * 8));
    putU32(8, static_cast<uint32_t>(exponent.size()));
    putU32(12, static_cast<uint32_t>(modulus.size()));
    putU32(16, 0);  // 公钥无素数段
    putU32(20, 0);
    blob += exponent;
    blob += modulus;

    BCRYPT_ALG_HANDLE keyAlg = nullptr, hashAlg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t sha[32];
    bool ok = false;
    do {
        if (BCryptOpenAlgorithmProvider(&keyAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptImportKeyPair(keyAlg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                                reinterpret_cast<PUCHAR>(blob.data()),
                                static_cast<ULONG>(blob.size()), 0) != 0) break;
        if (BCryptOpenAlgorithmProvider(&hashAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptCreateHash(hashAlg, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
                           static_cast<ULONG>(message.size()), 0) != 0) break;
        if (BCryptFinishHash(hash, sha, sizeof(sha), 0) != 0) break;
        // RSA 的签名验签必须显式声明 PKCS1 填充与哈希算法（dwFlags=0 仅对 ECDSA
        // 合法，RSA 下返回 STATUS_INVALID_PARAMETER）；与 .NET SignData Pkcs1、
        // openssl dgst -sign 产出的 PKCS1 v1.5/SHA256 同口径
        BCRYPT_PKCS1_PADDING_INFO info;
        info.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        const NTSTATUS st = BCryptVerifySignature(
            key, reinterpret_cast<PUCHAR>(&info), sha, sizeof(sha),
            reinterpret_cast<PUCHAR>(const_cast<char*>(sig.data())),
            static_cast<ULONG>(sig.size()), BCRYPT_PAD_PKCS1);
        ok = st == 0;
    } while (false);
    if (hash) BCryptDestroyHash(hash);
    if (hashAlg) BCryptCloseAlgorithmProvider(hashAlg, 0);
    if (key) BCryptDestroyKey(key);
    if (keyAlg) BCryptCloseAlgorithmProvider(keyAlg, 0);
    return ok;
#else
    (void)modulusHex;
    (void)exponentHex;
    (void)message;
    (void)signatureBase64;
    return false;  // 非 Windows 构建无更新器，此原语不应被调用
#endif
}

std::string uuid4() {
    std::string h = randomHex(16);
    h[12] = '4';
    uint8_t variant = 0;
    secureRandomBytes(&variant, 1);
    h[16] = kHex[(variant & 0x3) | 0x8];
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" +
           h.substr(16, 4) + "-" + h.substr(20, 12);
}

std::string nowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm = utcTm(t);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string weekStartIso() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto dayCount = floor<days>(now);
    weekday wd{dayCount};
    // 周一为一周起点（UTC）
    dayCount -= days{(wd - Monday).count()};
    std::time_t t = system_clock::to_time_t(dayCount);
    std::tm tm = utcTm(t);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d");
    return os.str();
}

std::string isoDaysAgo(int days) {
    using namespace std::chrono;
    auto t = system_clock::now() - hours{24} * days;
    std::time_t tt = system_clock::to_time_t(t);
    std::tm tm = utcTm(tt);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

bool parseIso(const std::string& iso, std::time_t& out) {
    std::tm tm{};
    std::istringstream is(iso);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (is.fail()) return false;
#ifdef _WIN32
    out = _mkgmtime(&tm);
#else
    out = timegm(&tm);
#endif
    return out != static_cast<std::time_t>(-1);
}

std::vector<uint32_t> utf8Codepoints(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        int len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& ch : r) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return r;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

}  // namespace ah
