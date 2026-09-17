#include "core/http/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "core/platform.h"
#include "core/util.h"

namespace ah {

using nlohmann::json;

namespace {

json ok(const json& data) { return json{{"code", 0}, {"message", "ok"}, {"data", data}}; }
json ok() { return json{{"code", 0}, {"message", "ok"}, {"data", nullptr}}; }
json fail(int code, const std::string& msg) {
    return json{{"code", code}, {"message", msg}, {"data", nullptr}};
}

void send(httplib::Response& res, const json& body) {
    // 统一约定：顶层 code != 0 时同步设置 HTTP 状态码（401/403/404/400/500）
    if (body.contains("code") && body["code"].is_number_integer() && body["code"].get<int>() != 0)
        res.status = body["code"].get<int>();
    else
        res.status = 200;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

// 查询参数 -> 正整数（默认值兜底）；非数字输入返回 400 而非抛异常
bool parseLimit(const httplib::Request& req, httplib::Response& res, int def, int& out) {
    if (!req.has_param("limit")) { out = def; return true; }
    const std::string& v = req.get_param_value("limit");
    try {
        size_t pos = 0;
        int n = std::stoi(v, &pos);
        if (n <= 0 || pos != v.size()) throw std::invalid_argument("range");
        out = n;
        return true;
    } catch (...) {
        send(res, fail(400, "limit must be a positive integer"));
        return false;
    }
}

// 解析库内存储的 JSON 片段（schema/params/audit detail）；损坏时退化为原字符串
json safeStoredJson(const std::string& s) {
    auto j = json::parse(s, nullptr, false);
    return j.is_discarded() ? json(s) : std::move(j);
}

// 从 body 提取 embedding（可选）；维度只做范围校验（64..4096）——
// 按维度分表后，Agent 自带的主流模型向量（1024/1536/3072…）都能进来
bool extractEmbedding(const json& body, bool& hasEmbedding, std::vector<float>& vec,
                      std::string& err) {
    hasEmbedding = false;
    if (!body.contains("embedding")) return true;
    const auto& emb = body["embedding"];
    if (!emb.is_array()) { err = "embedding must be an array of floats"; return false; }
    const int dim = static_cast<int>(emb.size());
    if (!ah::isValidEmbeddingDim(dim)) {
        err = "embedding dimension " + std::to_string(dim) +
              " not supported (expected 64..4096)";
        return false;
    }
    vec.clear();
    vec.reserve(emb.size());
    for (const auto& v : emb) {
        if (!v.is_number()) { err = "embedding must contain numbers"; return false; }
        vec.push_back(v.get<float>());
    }
    hasEmbedding = true;
    return true;
}

json knowledgeToJson(const KnowledgeEntry& e, double score = 0.0) {
    json j = {{"uuid", e.uuid},
              {"title", e.title},
              {"content", e.content},
              {"tags", json(e.tags)},
              {"category", e.category},
              {"author", e.author},
              {"version", e.version},
              {"embedding_provider", e.embedding_provider},
              {"created_at", e.created_at}};
    if (score != 0.0) j["score"] = score;
    return j;
}

// 广播消息的 recipient 序列化为 null（语义：全体可见），点对点为字符串
json recipientJson(const std::string& recipient) {
    return recipient.empty() ? json(nullptr) : json(recipient);
}

}  // namespace

struct HttpServer::Impl {
    httplib::Server srv;
    std::thread thread;
};

HttpServer::HttpServer(Platform& platform) : platform_(platform), impl_(std::make_unique<Impl>()) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(int port, std::string& err) {
    if (running_) return true;
    // ---- 传输层加固：默认构造的 httplib::Server 请求体上限是 SIZE_MAX，
    // 且没有读写超时。此前 body 会先被完整读入内存并 json::parse，之后才轮到
    // Platform 层做长度校验，等于给了本机进程一个内存放大面。 ----
    impl_->srv.set_payload_max_length(1024 * 1024);          // 1 MiB
    impl_->srv.set_read_timeout(15, 0);
    impl_->srv.set_write_timeout(15, 0);
    impl_->srv.set_idle_interval(0, 100000);                 // 100ms，保证 stop() 能及时返回
    // 未捕获异常统一转成平台信封：否则 httplib 会返回裸 500 且响应体不是 JSON，
    // 客户端无法按统一约定解析（JSON 类型不匹配属于请求方问题，按 400 回报）。
    impl_->srv.set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                        std::exception_ptr ep) {
        std::string msg = "internal error";
        int code = 500;
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const nlohmann::json::exception& e) {
            msg = std::string("invalid field type or malformed JSON: ") + e.what();
            code = 400;
        } catch (const std::exception& e) {
            msg = e.what();
        } catch (...) {
        }
        send(res, fail(code, msg));
    });
    setupRoutes();
    if (!impl_->srv.bind_to_port("127.0.0.1", port, 0)) {
        err = "failed to bind 127.0.0.1:" + std::to_string(port);
        return false;
    }
    // bind_to_port 返回 bool 而非端口；port_ 记录请求端口（GUI/CLI 均传显式端口）
    port_ = port;
    running_ = true;
    impl_->thread = std::thread([this] { impl_->srv.listen_after_bind(); });
    // 等待监听就绪，避免客户端在 listen 前连接被拒
    for (int i = 0; i < 100 && !impl_->srv.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
    impl_->srv.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

// 从请求头取出 Agent 身份并校验
static bool checkAgent(const httplib::Request& req, Platform& platform, std::string& actor,
                       httplib::Response& res) {
    if (!req.has_header("X-Agent-Name") || !req.has_header("X-Api-Key")) {
        send(res, fail(401, "missing X-Agent-Name / X-Api-Key headers"));
        return false;
    }
    actor = req.get_header_value("X-Agent-Name");
    std::string key = req.get_header_value("X-Api-Key");
    if (!platform.authenticate(actor, key)) {
        send(res, fail(401, "invalid agent credentials"));
        return false;
    }
    return true;
}

static bool checkMaster(const httplib::Request& req, Platform& platform, httplib::Response& res) {
    if (!req.has_header("X-Master-Key") ||
        !platform.authenticateMaster(req.get_header_value("X-Master-Key"))) {
        send(res, fail(403, "invalid master key"));
        return false;
    }
    return true;
}

void HttpServer::setupRoutes() {
    auto& srv = impl_->srv;
    Platform& p = platform_;

    srv.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        send(res, ok(json{{"service", "miderhive"},
                          {"version", kPlatformVersion},
                          {"db", "sqlite3+sqlite-vec"},
                          {"time", nowIso()}}));
    });

    // ---- Agent 注册（主密钥）----
    srv.Post("/api/agents/register", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string name = body.value("name", "");
        std::string role = body.value("role", kDefaultRole);
        std::string apiKey, err;
        if (!p.registerAgent(req.get_header_value("X-Master-Key"), name, role, apiKey, err)) {
            send(res, fail(400, err));
            return;
        }
        // 密钥明文只返回这一次，Agent 端应妥善保存
        send(res, ok(json{{"name", name}, {"role", role}, {"api_key", apiKey}}));
    });

    // ---- Agent 协作者列表（启动时必查）----
    srv.Get("/api/agents", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::vector<AgentInfo> agents;
        std::string err;
        if (!p.listAgents(agents, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& a : agents)
            arr.push_back({{"name", a.name},
                           {"role", a.role},
                           {"status", a.status},
                           {"current_task", a.current_task},
                           {"last_seen_at", a.last_seen_at},
                           {"created_at", a.created_at}});
        send(res, ok(arr));
    });

    // ---- Agent 移除（主密钥；管理者自身不可删）----
    srv.Post("/api/agents/remove", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("name") ||
            !body["name"].is_string()) {
            send(res, fail(400, "name must be a string"));
            return;
        }
        std::string err;
        const std::string name = body["name"].get<std::string>();
        if (!p.agentRemove(kManagerName, name, err)) { send(res, fail(400, err)); return; }
        send(res, ok(json{{"removed", name}}));
    });

    // ---- API Key 重新生成（主密钥；密钥丢失/泄露时唯一的恢复与轮换途径）----
    srv.Post("/api/agents/rotate", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("name") ||
            !body["name"].is_string()) {
            send(res, fail(400, "name must be a string"));
            return;
        }
        std::string err, apiKey;
        const std::string name = body["name"].get<std::string>();
        if (!p.agentRotateKey(kManagerName, name, apiKey, err)) {
            send(res, fail(400, err));
            return;
        }
        // 新密钥明文只返回这一次
        send(res, ok(json{{"name", name}, {"api_key", apiKey}}));
    });

    // ---- Agent 预配（主密钥）：给指定名字生成/轮换密钥并写入明文缓存——
    // 与界面上的"接入引导"同一套能力，供脚本/自动化测试复用
    srv.Post("/api/agents/provision", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("name") ||
            !body["name"].is_string()) {
            send(res, fail(400, "name must be a string"));
            return;
        }
        std::string err, apiKey;
        const std::string name = body["name"].get<std::string>();
        if (!p.agentProvision(kManagerName, name, apiKey, err)) { send(res, fail(400, err)); return; }
        send(res, ok(json{{"name", name}, {"api_key", apiKey}}));
    });

    // ---- 健康自检（防呆）：总览页健康横幅与自动化测试共用同一份口径 ----
    srv.Get("/api/diagnostics", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        const Diagnostics d = p.diagnostics();
        json missing = json::array();
        for (const auto& n : d.keyfile_missing) missing.push_back(n);
        send(res, ok(json{{"home_dir", d.home_dir},
                          {"home_writable", d.home_writable},
                          {"db_ok", d.db_ok},
                          {"agents_json_readable", d.agents_json_readable},
                          {"http_running", d.http_running},
                          {"port", d.port},
                          {"keyfile_missing", missing}}));
    });

    srv.Post("/api/agents/heartbeat", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        std::string task = body.is_object() ? body.value("current_task", "") : "";
        p.heartbeat(actor, task);
        send(res, ok());
    });

    // ---- 用户记忆（启动时必查）----
    srv.Get("/api/memory", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string section = req.has_param("section") ? req.get_param_value("section") : "";
        std::vector<MemoryEntry> entries;
        std::string err;
        if (!p.memoryList(section, entries, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& m : entries)
            arr.push_back({{"section", m.section},
                           {"key", m.key},
                           {"value", m.value},
                           {"author", m.author},
                           {"version", m.version},
                           {"created_at", m.created_at}});
        send(res, ok(arr));
    });

    srv.Post("/api/memory", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string section = body.value("section", "");
        std::string key = body.value("key", "");
        std::string value = body.value("value", "");
        int baseVersion = body.value("base_version", 0);
        MemoryEntry out;
        std::string err;
        if (!p.memorySet(actor, section, key, value, baseVersion, out, err)) {
            // 乐观并发冲突 → 409，携带服务端最新版本
            if (err.rfind("version conflict", 0) == 0) {
                send(res, fail(409, err));
                return;
            }
            send(res, fail(400, err));
            return;
        }
        send(res, ok(json{{"section", out.section}, {"key", out.key}, {"value", out.value},
                          {"author", out.author}, {"version", out.version},
                          {"created_at", out.created_at}}));
    });

    srv.Get("/api/memory/history", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string section = req.get_param_value("section");
        std::string key = req.get_param_value("key");
        if (section.empty() || key.empty()) { send(res, fail(400, "section and key are required")); return; }
        std::vector<MemoryEntry> entries;
        std::string err;
        if (!p.memoryHistory(section, key, entries, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& m : entries)
            arr.push_back({{"section", m.section},
                           {"key", m.key},
                           {"value", m.value},
                           {"author", m.author},
                           {"version", m.version},
                           {"created_at", m.created_at}});
        send(res, ok(arr));
    });

    // ---- 知识库 ----
    srv.Post("/api/knowledge", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string title = body.value("title", "");
        std::string content = body.value("content", "");
        std::string category = body.value("category", "");
        std::string embedder = body.value("embedder", "");
        std::vector<std::string> tags;
        if (body.contains("tags") && body["tags"].is_array())
            for (const auto& t : body["tags"]) {
                if (!t.is_string()) { send(res, fail(400, "tags must be an array of strings")); return; }
                tags.push_back(t.get<std::string>());
            }
        bool hasEmb = false;
        std::vector<float> emb;
        std::string err;
        if (!extractEmbedding(body, hasEmb, emb, err)) { send(res, fail(400, err)); return; }
        KnowledgeEntry out;
        if (!p.knowledgeCreate(actor, title, content, tags, category, hasEmb ? emb : std::vector<float>{},
                               embedder, out, err)) {
            send(res, fail(400, err));
            return;
        }
        send(res, ok(knowledgeToJson(out)));
    });

    srv.Get("/api/knowledge", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        int limit = 100;
        if (!parseLimit(req, res, 100, limit)) return;
        std::string tag = req.has_param("tag") ? req.get_param_value("tag") : "";
        std::vector<KnowledgeEntry> entries;
        std::string err;
        if (!p.knowledgeList(limit, tag, entries, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& e : entries) arr.push_back(knowledgeToJson(e));
        send(res, ok(arr));
    });

    srv.Post("/api/knowledge/search", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string query = body.value("query", "");
        std::string mode = body.value("mode", "keyword");
        int limit = body.value("limit", 20);
        std::string tag = body.value("tag", "");
        // 词表严格校验：此前 mode 大小写不符或未知时会被静默降级为 keyword 搜索，
        // 而 match_mode 又原样回显调用方的输入——调用方以为做了语义搜索（静默失败）。
        // 现改为：先归一为小写，非法值直接 400 并说明合法取值；match_mode 回显归一后的值。
        std::string norm = mode;
        for (char& c : norm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (norm != "keyword" && norm != "semantic") {
            send(res, fail(400, "invalid mode: '" + mode + "' (expected keyword or semantic)"));
            return;
        }
        SearchMode m = (norm == "semantic") ? SearchMode::Semantic : SearchMode::Keyword;
        std::vector<KnowledgeHit> hits;
        std::string err;
        if (!p.knowledgeSearch(query, m, limit, tag, hits, err)) { send(res, fail(400, err)); return; }
        json arr = json::array();
        for (const auto& h : hits) {
            json j = knowledgeToJson(h.entry, h.score);
            j["match_mode"] = norm;
            arr.push_back(j);
        }
        send(res, ok(arr));
    });

    srv.Get(R"(/api/knowledge/([0-9a-fA-F-]{36}))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        KnowledgeEntry out;
        std::string err;
        if (!p.knowledgeLatest(req.matches[1], out, err)) { send(res, fail(404, err)); return; }
        send(res, ok(knowledgeToJson(out)));
    });

    srv.Get(R"(/api/knowledge/([0-9a-fA-F-]{36})/versions)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::vector<KnowledgeEntry> entries;
        std::string err;
        if (!p.knowledgeVersions(req.matches[1], entries, err)) { send(res, fail(404, err)); return; }
        json arr = json::array();
        for (const auto& e : entries) arr.push_back(knowledgeToJson(e));
        send(res, ok(arr));
    });

    srv.Post(R"(/api/knowledge/([0-9a-fA-F-]{36})/versions)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string content = body.value("content", "");
        std::string title = body.value("title", "");
        std::string embedder = body.value("embedder", "");
        bool hasEmb = false;
        std::vector<float> emb;
        std::string err;
        if (!extractEmbedding(body, hasEmb, emb, err)) { send(res, fail(400, err)); return; }
        KnowledgeEntry out;
        if (!p.knowledgeAddVersion(actor, req.matches[1], title, content,
                                   hasEmb ? emb : std::vector<float>{}, embedder, out, err)) {
            send(res, fail(404, err));
            return;
        }
        send(res, ok(knowledgeToJson(out)));
    });

    // ---- 技能库 ----
    srv.Post("/api/skills", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string name = body.value("name", "");
        std::string display = body.value("display_name", "");
        std::string desc = body.value("description", "");
        std::string category = body.value("category", "");
        std::string schema = body.contains("param_schema") ? body["param_schema"].dump() : "{}";
        SkillInfo out;
        std::string err;
        if (!p.skillRegister(actor, name, display, desc, category, schema, out, err)) {
            send(res, fail(400, err));
            return;
        }
        send(res, ok(json{{"name", out.name},
                          {"display_name", out.display_name},
                          {"description", out.description},
                          {"category", out.category},
                          {"owner_agent", out.owner_agent},
                          {"param_schema", safeStoredJson(out.param_schema)},
                          {"version", out.version},
                          {"status", out.status}}));
    });

    srv.Get("/api/skills", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string category = req.has_param("category") ? req.get_param_value("category") : "";
        std::string owner = req.has_param("owner") ? req.get_param_value("owner") : "";
        std::vector<SkillInfo> skills;
        std::string err;
        if (!p.skillList(category, owner, skills, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& s : skills)
            arr.push_back({{"name", s.name},
                           {"display_name", s.display_name},
                           {"description", s.description},
                           {"category", s.category},
                           {"owner_agent", s.owner_agent},
                           {"param_schema", safeStoredJson(s.param_schema)},
                           {"version", s.version},
                           {"status", s.status},
                           {"updated_at", s.updated_at}});
        send(res, ok(arr));
    });

    srv.Get(R"(/api/skills/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        SkillInfo out;
        std::string err;
        if (!p.skillGet(req.matches[1], out, err)) { send(res, fail(404, err)); return; }
        send(res, ok(json{{"name", out.name},
                          {"display_name", out.display_name},
                          {"description", out.description},
                          {"category", out.category},
                          {"owner_agent", out.owner_agent},
                          {"param_schema", safeStoredJson(out.param_schema)},
                          {"version", out.version},
                          {"status", out.status}}));
    });

    srv.Post(R"(/api/skills/([^/]+)/invoke)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string params = body.contains("params") ? body["params"].dump() : "{}";
        std::string result = body.value("result_summary", "");
        std::string status = body.value("status", "success");
        int64_t duration = body.value("duration_ms", 0);
        int64_t tin = body.value("tokens_in", 0);
        int64_t tout = body.value("tokens_out", 0);
        std::string referenceId = body.value("reference_id", "");
        std::string err;
        if (!p.skillInvoke(actor, req.matches[1], params, result, status, duration, tin, tout,
                           referenceId, err)) {
            send(res, fail(400, err));
            return;
        }
        UsageSummary sum;
        p.usageSummary(sum, err);
        send(res, ok(json{{"skill", req.matches[1]},
                          {"remaining_tokens", sum.budget - sum.total_tokens},
                          {"budget", sum.budget},
                          {"alert_level", sum.alert_level}}));
    });

    srv.Get(R"(/api/skills/([^/]+)/invocations)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        int limit = 100;
        if (!parseLimit(req, res, 100, limit)) return;
        std::vector<SkillInvocation> invs;
        std::string err;
        if (!p.skillInvocations(req.matches[1], limit, invs, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& i : invs)
            arr.push_back({{"skill_name", i.skill_name},
                           {"caller_agent", i.caller_agent},
                           {"params", safeStoredJson(i.params)},
                           {"result_summary", i.result_summary},
                           {"status", i.status},
                           {"duration_ms", i.duration_ms},
                           {"reference_id", i.reference_id.empty() ? json() : json(i.reference_id)},
                           {"created_at", i.created_at}});
        send(res, ok(arr));
    });

    // ---- 消息 ----
    srv.Post("/api/messages", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string kind = body.value("kind", "note");
        std::string recipient = body.value("recipient", "");
        std::string subject = body.value("subject", "");
        std::string text = body.value("body", "");
        Message out;
        std::string err;
        if (!p.messageSend(kind, actor, recipient, subject, text, out, err)) {
            send(res, fail(400, err));
            return;
        }
        send(res, ok(json{{"uuid", out.uuid},
                          {"kind", out.kind},
                          {"sender", out.sender},
                          {"recipient", recipientJson(out.recipient)},
                          {"status", out.status},
                          {"created_at", out.created_at}}));
    });

    srv.Get("/api/messages", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string recipient = req.has_param("recipient") ? req.get_param_value("recipient") : "";
        std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "";
        std::string status = req.has_param("status") ? req.get_param_value("status") : "";
        std::string since = req.has_param("since") ? req.get_param_value("since") : "";
        int limit = 100;
        if (!parseLimit(req, res, 100, limit)) return;
        std::vector<Message> msgs;
        std::string err;
        // 可见性：非管理者只能看到广播 + 发给自己的 + 自己发出的（否则人人可读点对点消息）
        const std::string viewer = p.isManager(actor) ? std::string() : actor;
        if (!p.messageList(recipient, kind, status, since, limit, msgs, err, viewer)) {
            send(res, fail(500, err));
            return;
        }
        json arr = json::array();
        for (const auto& m : msgs)
            arr.push_back({{"uuid", m.uuid},
                           {"kind", m.kind},
                           {"sender", m.sender},
                           {"recipient", recipientJson(m.recipient)},
                           {"subject", m.subject},
                           {"body", m.body},
                           {"status", m.status},
                           {"parent_uuid", m.parent_uuid},
                           {"created_at", m.created_at}});
        send(res, ok(arr));
    });

    srv.Post(R"(/api/messages/([0-9a-fA-F-]{36})/reply)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        std::string text = body.is_object() ? body.value("body", "") : "";
        Message out;
        std::string err;
        if (!p.messageReply(actor, req.matches[1], text, out, err)) { send(res, fail(404, err)); return; }
        send(res, ok(json{{"uuid", out.uuid}, {"recipient", out.recipient}, {"status", out.status}}));
    });

    srv.Post(R"(/api/messages/([0-9a-fA-F-]{36})/status)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        std::string status = body.is_object() ? body.value("status", "") : "";
        Message out;
        std::string err;
        if (!p.messageSetStatus(actor, req.matches[1], status, out, err)) {
            send(res, fail(400, err));
            return;
        }
        send(res, ok(json{{"uuid", out.uuid}, {"status", out.status}}));
    });

    // ---- 错误日志 ----
    srv.Post("/api/errors", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string severity = body.value("severity", "error");
        std::string source = body.value("source", "");
        std::string title = body.value("title", "");
        std::string detail = body.value("detail", "");
        std::string stack = body.value("stack_trace", "");
        ErrorReport out;
        std::string err;
        if (!p.errorReport(actor, severity, source, title, detail, stack, out, err)) {
            send(res, fail(400, err));
            return;
        }
        send(res, ok(json{{"uuid", out.uuid}, {"status", out.status}, {"created_at", out.created_at}}));
    });

    srv.Get("/api/errors", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string status = req.has_param("status") ? req.get_param_value("status") : "";
        std::string severity = req.has_param("severity") ? req.get_param_value("severity") : "";
        int limit = 200;
        if (!parseLimit(req, res, 200, limit)) return;
        std::vector<ErrorReport> errors;
        std::string err;
        if (!p.errorList(status, severity, limit, errors, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& e : errors)
            arr.push_back({{"uuid", e.uuid},
                           {"reporter", e.reporter},
                           {"severity", e.severity},
                           {"source", e.source},
                           {"title", e.title},
                           {"detail", e.detail},
                           {"stack_trace", e.stack_trace},
                           {"status", e.status},
                           {"resolution_notes", e.resolution_notes},
                           {"resolved_by", e.resolved_by},
                           {"created_at", e.created_at},
                           {"resolved_at", e.resolved_at}});
        send(res, ok(arr));
    });

    srv.Post(R"(/api/errors/([0-9a-fA-F-]{36})/resolve)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        std::string notes = body.is_object() ? body.value("notes", "") : "";
        ErrorReport out;
        std::string err;
        if (!p.errorResolve(actor, req.matches[1], notes, out, err)) { send(res, fail(400, err)); return; }
        send(res, ok(json{{"uuid", out.uuid}, {"status", out.status}, {"resolved_by", out.resolved_by}}));
    });

    // ---- Token 用量 ----
    srv.Post("/api/usage/report", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        int64_t tin = body.value("tokens_in", 0);
        int64_t tout = body.value("tokens_out", 0);
        std::string type = body.value("call_type", "");
        std::string ref = body.value("reference_id", "");
        std::string idem = body.value("idempotency_key", "");
        if (body.contains("model") && !body["model"].is_string()) {
            send(res, fail(400, "model must be a string"));
            return;
        }
        std::string model = body.value("model", "");
        bool duplicate = false;
        std::string err;
        if (!p.usageReport(actor, tin, tout, type, model, ref, idem, duplicate, err)) {
            send(res, fail(400, err));
            return;
        }
        UsageSummary sum;
        if (!p.usageSummary(sum, err)) { send(res, fail(500, err)); return; }
        send(res, ok(json{{"week_start", sum.week_start},
                          {"budget", sum.budget},
                          {"used", sum.total_tokens},
                          {"remaining", sum.budget - sum.total_tokens},
                          {"alert_level", sum.alert_level},
                          {"duplicate", duplicate}}));
    });

    srv.Get("/api/usage/summary", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        UsageSummary sum;
        std::string err;
        if (!p.usageSummary(sum, err)) { send(res, fail(500, err)); return; }
        json per = json::array();
        for (const auto& [name, tokens] : sum.per_agent)
            per.push_back({{"agent", name}, {"tokens", tokens}});
        send(res, ok(json{{"week_start", sum.week_start},
                          {"budget", sum.budget},
                          {"total_in", sum.total_in},
                          {"total_out", sum.total_out},
                          {"total_tokens", sum.total_tokens},
                          {"remaining", sum.budget - sum.total_tokens},
                          {"alert_level", sum.alert_level},
                          {"per_agent", per}}));
    });

    srv.Get("/api/usage/daily", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        int days = 14;
        if (req.has_param("days")) {
            const std::string& v = req.get_param_value("days");
            try {
                size_t pos = 0;
                int n = std::stoi(v, &pos);
                if (n <= 0 || pos != v.size()) throw std::invalid_argument("days");
                days = std::min(n, 90);
            } catch (...) {
                send(res, fail(400, "days must be a positive integer"));
                return;
            }
        }
        std::vector<UsageDailyPoint> pts;
        std::string err;
        if (!p.usageDaily(days, pts, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& pt : pts) arr.push_back({{"day", pt.day}, {"tokens", pt.tokens}});
        send(res, ok(json{{"days", arr}}));
    });

    srv.Get("/api/usage/models", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::vector<UsageModelRow> rows;
        std::string err;
        if (!p.usageByModel(rows, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& r : rows) arr.push_back({{"model", r.model}, {"tokens", r.tokens}});
        send(res, ok(json{{"models", arr}}));
    });

    // 多维用量切片：days/agent/model 全部可选；「用量分析」面板与 API 侧筛选共用
    srv.Get("/api/usage/breakdown", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        int days = 14;
        if (req.has_param("days")) {
            const std::string& v = req.get_param_value("days");
            try {
                size_t pos = 0;
                int n = std::stoi(v, &pos);
                if (n <= 0 || pos != v.size()) throw std::invalid_argument("days");
                days = std::min(n, 90);
            } catch (...) {
                send(res, fail(400, "days must be a positive integer"));
                return;
            }
        }
        const std::string agent = req.has_param("agent") ? req.get_param_value("agent") : "";
        const std::string model = req.has_param("model") ? req.get_param_value("model") : "";
        UsageBreakdown bd;
        std::string err;
        if (!p.usageBreakdown(days, agent, model, bd, err)) { send(res, fail(500, err)); return; }
        json agents = json::array();
        for (const auto& r : bd.per_agent)
            agents.push_back({{"agent", r.agent}, {"tokens_in", r.in}, {"tokens_out", r.out},
                              {"tokens", r.tokens}, {"calls", r.calls}});
        json models = json::array();
        for (const auto& r : bd.per_model)
            models.push_back({{"model", r.model}, {"tokens", r.tokens}, {"calls", r.calls}});
        json daysArr = json::array();
        for (const auto& pt : bd.daily) daysArr.push_back({{"day", pt.day}, {"tokens", pt.tokens}});
        send(res, ok(json{{"days", days},
                          {"total_in", bd.total_in},
                          {"total_out", bd.total_out},
                          {"total_tokens", bd.total_tokens},
                          {"calls", bd.calls},
                          {"per_agent", agents},
                          {"per_model", models},
                          {"daily", daysArr}}));
    });

    srv.Get("/api/usage/budget", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string err;
        send(res, ok(json{{"budget", p.usageBudget(err)}}));
    });

    srv.Put("/api/usage/budget", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("budget") ||
            !body["budget"].is_number()) {
            send(res, fail(400, "budget must be a number"));
            return;
        }
        std::string err;
        if (!p.usageSetBudget(kManagerName, body["budget"].get<int64_t>(), err)) {
            send(res, fail(400, err));
            return;
        }
        send(res, ok(json{{"budget", body["budget"].get<int64_t>()}}));
    });

    // ---- 操作日志 ----
    srv.Get("/api/audit", [&](const httplib::Request& req, httplib::Response& res) {
        std::string actor;
        if (!checkAgent(req, p, actor, res)) return;
        std::string a = req.has_param("actor") ? req.get_param_value("actor") : "";
        std::string act = req.has_param("action") ? req.get_param_value("action") : "";
        std::string since = req.has_param("since") ? req.get_param_value("since") : "";
        int limit = 200;
        if (!parseLimit(req, res, 200, limit)) return;
        std::vector<AuditRecord> records;
        std::string err;
        if (!p.auditList(a, act, since, limit, records, err)) { send(res, fail(500, err)); return; }
        json arr = json::array();
        for (const auto& r : records)
            arr.push_back({{"id", r.id},
                           {"actor", r.actor},
                           {"action", r.action},
                           {"target", r.target},
                           {"detail", safeStoredJson(r.detail)},
                           {"created_at", r.created_at}});
        send(res, ok(arr));
    });

    // ---- 运维（主密钥）：维护 / 备份恢复 / 管理性删除 ----
    srv.Post("/api/maintenance", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        std::string stats, err;
        if (!p.maintenanceRun(kManagerName, stats, err)) { send(res, fail(500, err)); return; }
        send(res, ok(safeStoredJson(stats)));
    });

    srv.Post("/api/system/backup", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        std::string path, err;
        if (!p.backupCreate(path, err)) { send(res, fail(500, err)); return; }
        send(res, ok(json{{"path", path}}));
    });

    srv.Get("/api/system/backups", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        std::vector<std::string> files;
        std::string err;
        if (!p.backupList(files, err)) { send(res, fail(500, err)); return; }
        send(res, ok(files));
    });

    srv.Post("/api/system/restore", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) { send(res, fail(400, "invalid JSON body")); return; }
        std::string err;
        if (!p.backupRestore(body.value("file", ""), err)) { send(res, fail(400, err)); return; }
        send(res, ok(json{{"restored", body.value("file", "")}}));
    });

    srv.Delete(R"(/api/knowledge/([0-9a-fA-F-]{36}))", [&](const httplib::Request& req, httplib::Response& res) {
        if (!checkMaster(req, p, res)) return;
        std::string err;
        if (!p.knowledgeRemove(kManagerName, req.matches[1], err)) { send(res, fail(404, err)); return; }
        send(res, ok(json{{"removed", req.matches[1]}}));
    });

    srv.Delete("/api/memory", [&](const httplib::Request& req, httplib::Response& res) {
        // 双通道鉴权：管理者 Agent（MCP miderhive-mcp 以 agent 身份自动化运维）
        // 或主密钥（GUI 的管理操作走主密钥）。无论哪条通道,Platform::memoryRemove
        // 内部仍强制 isManager,越权身份到不了写路径。
        std::string actor = kManagerName;
        const bool hasAgent = req.has_header("X-Agent-Name") && req.has_header("X-Api-Key");
        if (hasAgent) {
            if (!checkAgent(req, p, actor, res)) return;
        } else if (!checkMaster(req, p, res)) {
            return;
        }
        std::string section = req.get_param_value("section");
        std::string key = req.get_param_value("key");
        std::string err;
        if (!p.memoryRemove(actor, section, key, err)) { send(res, fail(404, err)); return; }
        send(res, ok(json{{"removed", section + "/" + key}}));
    });
}

}  // namespace ah
