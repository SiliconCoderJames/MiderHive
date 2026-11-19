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
    if (j.is_object() && j.contains(key) && j[key].is_number()) {
        // 个别 provider 把 token 数发成浮点（12.0）：放宽为数值并截断
        return static_cast<int64_t>(j[key].get<double>());
    }
    return -1;
}

// 从 error 对象提取人话消息；message 缺失时兜底整个对象的 dump（replace 模式，
// 防服务端发非法 UTF-8 时 dump 抛异常）
std::string errorMessage(const json& e) {
    if (e.is_object() || e.is_string()) {
        const std::string m = jsonStr(e, "message");
        if (!m.empty()) return m;
        if (e.is_string()) return e.get<std::string>();
    }
    return e.dump(2, ' ', false, nlohmann::detail::error_handler_t::replace);
}

std::string truncateUtf8(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    s.resize(maxBytes);
    // 回退不完整的多字节序列：先弹续字节，再处理"声明的序列长度超过剩余字节"
    // 的孤立首字节（此前只弹续字节，会留下残缺首字节 → 非法 UTF-8，
    // 经 json::dump(strict) 会抛异常，经 QString::fromUtf8 出 U+FFFD）
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        if ((c & 0xC0) == 0x80) { s.pop_back(); continue; }  // 续字节
        size_t need = 1;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        if (need > s.size()) s.pop_back();  // 序列被切断，弹掉孤立首字节
        break;
    }
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

    void cancel() override { transport_->cancel(); }

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
        if (cfg_.maxTokens > 0) body["max_tokens"] = cfg_.maxTokens;
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
        std::string streamErr;  // 流内 in-band error（200 状态下 data:{"error":…}）
        SseFeeder feeder;
        auto onData = [&](const std::string& data) {
            if (cancelled) return;  // 取消后同块剩余事件不再派发（契约：false 即中止）
            if (data == "[DONE]") return;
            json j = parseBody(data);
            if (j.is_discarded() || !j.is_object()) return;
            // 流内 error：OpenRouter/Groq/xAI/部分 vLLM 在 200 状态的 SSE 里直接
            // 发 data:{"error":…}——不认它就会把错误吞成"成功 + 半截回答"
            if (j.contains("error")) {
                streamErr = errorMessage(j["error"]);
                if (streamErr.empty()) streamErr = "provider reported a stream error";
                cancelled = true;
                return;
            }
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
                // finish_reason=length：回复被 max_tokens 截断（如实标记，不掩蔽）
                if (jsonStr(c0, "finish_reason") == "length") acc.truncated = true;
            }
        };
        auto sink = [&](const char* d, std::size_t n) -> bool {
            feeder.feed(d, n, onData);
            return !cancelled;
        };

        HttpResponse res = transport_->send(req, stream ? sink : ChunkSink{});
        if (!res.err.empty()) {
            err = (res.err == "cancelled" && !streamErr.empty()) ? streamErr : res.err;
            return out;
        }
        if (res.status >= 400) { err = describeHttpError(res); return out; }
        if (stream) {
            // 分块已由 sink 实时喂过解析器，这里只补喂末尾无空行结尾的残帧
            feeder.flush(onData);
            // 残帧里的 error 事件在 flush 时才派发——必须在这里再查一次
            if (!streamErr.empty()) { err = streamErr; return out; }
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

    void cancel() override { transport_->cancel(); }

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
                  // Anthropic 必填；调用方可用 cfg.maxTokens 覆盖（如摘要限成本）
                  {"max_tokens", cfg_.maxTokens > 0 ? cfg_.maxTokens : 4096},
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
            if (cancelled) return;  // 取消后同块剩余事件不再派发
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
                if (pout >= 0) {
                    acc.tokensOut = pout;
                    acc.usageKnown = true;  // 与 message_start 对称：已知就置位
                }
                // stop_reason=max_tokens：回复被长度上限截断（Anthropic 正常结束
                // 流、无 error 事件——不读它就静默腰斩）
                if (jsonStr(j.value("delta", json{}), "stop_reason") == "max_tokens")
                    acc.truncated = true;
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
            // 残帧里的 error 事件在 flush 时才派发——这里必须再查一次
            if (!streamErr.empty()) { err = streamErr; return out; }
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
