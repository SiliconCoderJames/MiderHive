// 双协议实现。SSE 事件按空行分帧（\n\n 与 \r\n\r\n 都认），逐条取 data: 行；
// 增量文本透传 onDelta，取消沿传输层回传链向上中止。
#include "core/llm/llm_client.h"

#include <nlohmann/json.hpp>

namespace ah::llm {

namespace {

using nlohmann::json;

// ---- SSE 分帧：网络块进 buffer，按空行切事件，抽 data: 行回调 ----
class SseFeeder {
public:
    void feed(const char* d, std::size_t n, const std::function<void(const std::string&)>& onData) {
        buf_.append(d, n);
        drain(onData);
    }
    // 流结束时补一次：最后的事件可能没有空行结尾
    void flush(const std::function<void(const std::string&)>& onData) {
        const std::string rest = buf_;
        buf_.clear();
        if (rest.find_first_not_of(" \r\n") != std::string::npos) emitEvent(rest, onData);
    }

private:
    void drain(const std::function<void(const std::string&)>& onData) {
        for (;;) {
            const size_t a = buf_.find("\n\n");
            const size_t b = buf_.find("\r\n\r\n");
            size_t pos = std::string::npos, sep = 0;
            if (b != std::string::npos && (a == std::string::npos || b < a)) {
                pos = b;
                sep = 4;
            } else if (a != std::string::npos) {
                pos = a;
                sep = 2;
            }
            if (pos == std::string::npos) break;
            const std::string event = buf_.substr(0, pos);
            buf_.erase(0, pos + sep);
            emitEvent(event, onData);
        }
    }
    static void emitEvent(const std::string& event,
                          const std::function<void(const std::string&)>& onData) {
        std::string data;
        size_t start = 0;
        for (;;) {
            const size_t eol = event.find('\n', start);
            std::string line =
                event.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                if (!data.empty()) data += '\n';
                data += payload;
            }
            if (eol == std::string::npos) break;
            start = eol + 1;
        }
        if (!data.empty()) onData(data);
    }
    std::string buf_;
};

// 非 JSON 或缺字段都安全返回 ""
std::string jsonStr(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return {};
}
int64_t jsonInt(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number_integer())
        return j[key].get<int64_t>();
    return -1;
}

std::string truncateUtf8(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    s.resize(maxBytes);
    // 别把多字节字符截一半：回退到码点边界
    while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (s.back() & 0x80) == 0x80) s.pop_back();
    return s + "...";
}

// HTTP >= 400 → 人话。两协议错误体都优先取 error.message；取不到就带上截断的原文。
std::string describeHttpError(const HttpResponse& res) {
    std::string serverMsg;
    json j = json::parse(res.body, nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded() && j.is_object() && j.contains("error")) {
        const json& e = j["error"];
        if (e.is_object()) serverMsg = jsonStr(e, "message");
    }
    std::string head;
    switch (res.status) {
        case 400: head = "the provider rejected the request (400)"; break;
        case 401: head = "the API key was rejected (401) - check it in Settings > LLM"; break;
        case 402: head = "insufficient balance at the provider (402)"; break;
        case 403: head = "access denied (403)"; break;
        case 404: head = "model or endpoint not found (404) - check base URL and model name"; break;
        case 429: head = "rate limited (429) - retry later"; break;
        default:
            head = res.status >= 500 ? "provider server error (" + std::to_string(res.status) + ")"
                                     : "provider returned HTTP " + std::to_string(res.status);
    }
    if (serverMsg.empty() && !res.body.empty())
        serverMsg = truncateUtf8(res.body, 200);  // 非 JSON 错误体也如实展示
    return serverMsg.empty() ? head : head + ": " + truncateUtf8(serverMsg, 300);
}

std::string stripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

json parseBody(const std::string& body) {
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    return j.is_discarded() ? json{} : j;
}

// ---- OpenAI 兼容协议（含 SSE）----
class OpenAiClient final : public LlmClient {
public:
    explicit OpenAiClient(LlmConfig cfg) : cfg_(std::move(cfg)) {}

    std::string name() const override { return "openai-compat:" + cfg_.providerId; }

    LlmReply complete(const std::vector<LlmMessage>& history, const ChunkSink& onDelta,
                      std::string& err) override {
        LlmReply out;
        if (cfg_.baseUrl.empty()) { err = "base URL is required"; return out; }
        if (cfg_.model.empty()) { err = "model is required"; return out; }
        if (cfg_.apiKey.empty()) { err = "API key is required"; return out; }

        json arr = json::array();
        for (const auto& m : history) arr.push_back(json{{"role", m.role}, {"content", m.content}});
        const bool stream = static_cast<bool>(onDelta);
        json body{{"model", cfg_.model}, {"messages", arr}};
        if (stream) {
            body["stream"] = true;
            // 主流兼容端点（OpenAI/DeepSeek/vLLM/LM Studio）都认；个别老端点
            // 若因该字段 400，错误体会如实上抛，届时可按 provider 关闭
            body["stream_options"] = json{{"include_usage", true}};
        }

        HttpRequest req;
        req.url = stripTrailingSlash(cfg_.baseUrl) + "/chat/completions";
        req.headers = {{"Content-Type", "application/json"},
                       {"Authorization", "Bearer " + cfg_.apiKey},
                       {"Accept", stream ? "text/event-stream" : "application/json"}};
        req.body = body.dump();

        LlmReply acc;
        bool cancelled = false;
        SseFeeder feeder;
        auto onData = [&](const std::string& data) {
            if (data == "[DONE]") return;
            json j = parseBody(data);
            if (j.is_discarded() || !j.is_object()) return;
            // usage（include_usage 的末块或非流响应）：取最后一次出现的
            const auto pin = jsonInt(j.value("usage", json{}), "prompt_tokens");
            const auto pout = jsonInt(j.value("usage", json{}), "completion_tokens");
            if (pin >= 0 && pout >= 0) {
                acc.tokensIn = pin;
                acc.tokensOut = pout;
                acc.usageKnown = true;
            }
            if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                const json& c0 = j["choices"][0];
                // 非流：message.content；流：delta.content
                const json& part =
                    stream ? c0.value("delta", json{}) : c0.value("message", json{});
                const std::string piece = jsonStr(part, "content");
                if (!piece.empty()) {
                    acc.text += piece;
                    if (onDelta && !onDelta(piece.data(), piece.size())) cancelled = true;
                }
            }
        };
        auto sink = [&](const char* d, std::size_t n) -> bool {
            feeder.feed(d, n, onData);
            return !cancelled;
        };

        HttpResponse res = transport_->send(req, stream ? sink : ChunkSink{});
        if (!res.err.empty()) { err = res.err; return out; }
        if (res.status >= 400) { err = describeHttpError(res); return out; }
        if (stream) {
            // 分块已由 sink 实时喂过解析器，这里只补喂末尾无空行结尾的残帧
            feeder.flush(onData);
        } else {
            const json j = parseBody(res.body);
            if (j.is_discarded()) { err = "provider returned a non-JSON response"; return out; }
            onData(res.body);
        }
        out = acc;
        return out;
    }

private:
    LlmConfig cfg_;
    std::unique_ptr<ITransport> transport_ = makeDefaultTransport();
};

// ---- Anthropic 协议（/v1/messages；system 是顶层字段，max_tokens 必填）----
class AnthropicClient final : public LlmClient {
public:
    explicit AnthropicClient(LlmConfig cfg) : cfg_(std::move(cfg)) {}

    std::string name() const override { return "anthropic:" + cfg_.providerId; }

    LlmReply complete(const std::vector<LlmMessage>& history, const ChunkSink& onDelta,
                      std::string& err) override {
        LlmReply out;
        if (cfg_.baseUrl.empty()) { err = "base URL is required"; return out; }
        if (cfg_.model.empty()) { err = "model is required"; return out; }
        if (cfg_.apiKey.empty()) { err = "API key is required"; return out; }

        json arr = json::array();
        std::string system;
        for (const auto& m : history) {
            if (m.role == "system") {
                if (!system.empty()) system += "\n\n";
                system += m.content;
            } else {
                arr.push_back(json{{"role", m.role}, {"content", m.content}});
            }
        }
        const bool stream = static_cast<bool>(onDelta);
        json body{{"model", cfg_.model},
                  {"max_tokens", 4096},  // Anthropic 必填；P1 不暴露配置
                  {"messages", arr}};
        if (!system.empty()) body["system"] = system;
        if (stream) body["stream"] = true;

        HttpRequest req;
        req.url = stripTrailingSlash(cfg_.baseUrl) + "/v1/messages";
        req.headers = {{"Content-Type", "application/json"},
                       {"x-api-key", cfg_.apiKey},
                       {"anthropic-version", "2023-06-01"},
                       {"Accept", stream ? "text/event-stream" : "application/json"}};
        req.body = body.dump();

        LlmReply acc;
        bool cancelled = false;
        std::string streamErr;  // 流中 error 事件的真实原因（transport 只会报 cancelled）
        SseFeeder feeder;
        auto onData = [&](const std::string& data) {
            json j = parseBody(data);
            if (j.is_discarded() || !j.is_object()) return;
            const std::string type = jsonStr(j, "type");
            if (type == "error") {
                const json& e = j.value("error", json{});
                streamErr = jsonStr(e, "message");
                if (streamErr.empty()) streamErr = "provider reported a stream error";
                cancelled = true;
                return;
            }
            if (type == "message_start") {
                const auto pin = jsonInt(j.value("message", json{}).value("usage", json{}),
                                         "input_tokens");
                if (pin >= 0) {
                    acc.tokensIn = pin;
                    acc.usageKnown = true;
                }
                return;
            }
            if (type == "content_block_delta") {
                const std::string piece =
                    jsonStr(j.value("delta", json{}), "text");
                if (!piece.empty()) {
                    acc.text += piece;
                    if (onDelta && !onDelta(piece.data(), piece.size())) cancelled = true;
                }
                return;
            }
            if (type == "message_delta") {
                const auto pout = jsonInt(j.value("usage", json{}), "output_tokens");
                if (pout >= 0) acc.tokensOut = pout;
                return;
            }
            // ping / content_block_start / content_block_stop / message_stop：忽略
        };
        auto sink = [&](const char* d, std::size_t n) -> bool {
            feeder.feed(d, n, onData);
            return !cancelled;
        };

        HttpResponse res = transport_->send(req, stream ? sink : ChunkSink{});
        if (!res.err.empty()) {
            // 流中 error 事件触发的取消不得掩蔽真实原因（transport 只报 cancelled）
            err = (res.err == "cancelled" && !streamErr.empty()) ? streamErr : res.err;
            return out;
        }
        if (res.status >= 400) { err = describeHttpError(res); return out; }
        if (stream) {
            feeder.flush(onData);  // 同 OpenAI：分块已实时喂过，只补残帧
        } else {
            const json j = parseBody(res.body);
            if (j.is_discarded()) { err = "provider returned a non-JSON response"; return out; }
            if (j.contains("content") && j["content"].is_array())
                for (const auto& block : j["content"])
                    if (jsonStr(block, "type") == "text") {
                        const std::string piece = jsonStr(block, "text");
                        if (!piece.empty()) acc.text += piece;
                    }
            const auto pin = jsonInt(j.value("usage", json{}), "input_tokens");
            const auto pout = jsonInt(j.value("usage", json{}), "output_tokens");
            if (pin >= 0 && pout >= 0) {
                acc.tokensIn = pin;
                acc.tokensOut = pout;
                acc.usageKnown = true;
            }
        }
        out = acc;
        return out;
    }

private:
    LlmConfig cfg_;
    std::unique_ptr<ITransport> transport_ = makeDefaultTransport();
};

}  // namespace

const std::vector<ProviderPreset>& providerPresets() {
    static const std::vector<ProviderPreset> kPresets = {
        {"deepseek", Protocol::OpenAiCompat, "https://api.deepseek.com", "deepseek-chat"},
        {"openai", Protocol::OpenAiCompat, "https://api.openai.com/v1", "gpt-4o-mini"},
        {"anthropic", Protocol::Anthropic, "https://api.anthropic.com", "claude-sonnet-4-5"},
        {"custom", Protocol::OpenAiCompat, "", ""},
    };
    return kPresets;
}

const ProviderPreset* findPreset(const std::string& id) {
    for (const auto& p : providerPresets())
        if (id == p.id) return &p;
    return nullptr;
}

std::unique_ptr<LlmClient> makeClient(const LlmConfig& cfg) {
    if (cfg.protocol == Protocol::Anthropic) return std::make_unique<AnthropicClient>(cfg);
    return std::make_unique<OpenAiClient>(cfg);
}

}  // namespace ah::llm
