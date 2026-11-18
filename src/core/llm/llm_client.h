#pragma once
// 双协议 LLM 客户端：OpenAI 兼容（/chat/completions，DeepSeek/OpenAI 及一切
// 兼容端点通用）与 Anthropic（/v1/messages）。只做「请求构造 + 响应/SSE 解析 +
// 错误人话化」，网络走 transport.h 的 ITransport（自带 WinHTTP 实现）。
// 设计约束：绝不抛异常；密钥绝不进 err/任何日志；usage 缺失时 usageKnown=false
// （记账如实记 0，不编造估算）。
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/llm/transport.h"

namespace ah::llm {

enum class Protocol { OpenAiCompat, Anthropic };

struct LlmConfig {
    std::string providerId;  // deepseek | openai | anthropic | custom
    Protocol protocol = Protocol::OpenAiCompat;
    std::string baseUrl;  // 形如 https://api.deepseek.com，不含方法路径（尾斜杠自动剥）
    std::string model;    // 如 deepseek-chat
    std::string apiKey;
};

struct LlmMessage {
    std::string role;     // user | assistant | system
    std::string content;  // UTF-8
};

struct LlmReply {
    std::string text;
    int64_t tokensIn = 0;
    int64_t tokensOut = 0;
    bool usageKnown = false;  // provider 未回 usage 时 false——绝不编造估算
};

class LlmClient {
public:
    virtual ~LlmClient() = default;
    virtual std::string name() const = 0;
    // onDelta 可空：空 = 非流式（等完整回复）；非空 = 流式，逐段回调增量文本，
    // 返回 false 表示调用方取消。失败时 err 为人话描述且返回 {}。
    virtual LlmReply complete(const std::vector<LlmMessage>& history, const ChunkSink& onDelta,
                              std::string& err) = 0;
};

// 预设：id 供配置存储与 GUI 引用；baseUrl/model 是可编辑的默认值
struct ProviderPreset {
    const char* id;
    Protocol protocol;
    const char* baseUrl;
    const char* model;
};
const std::vector<ProviderPreset>& providerPresets();
const ProviderPreset* findPreset(const std::string& id);

// 按配置构造客户端（自带 WinHTTP 传输；每调用一个会话，开销毫秒级可接受）。
// 协议按 cfg.protocol 分派；custom 走 OpenAI 兼容形状（Anthropic 形状如需
// 自建反代，配 custom 时改 protocol 即可）。
std::unique_ptr<LlmClient> makeClient(const LlmConfig& cfg);

}  // namespace ah::llm
