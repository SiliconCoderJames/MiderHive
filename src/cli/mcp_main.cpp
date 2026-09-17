// miderhive-mcp：把平台协作面以 MCP（Model Context Protocol）工具暴露给 AI agent。
//
// 形态与理由：stdio 子进程。MCP 客户端（Claude Code / Claude Desktop、Cursor 等）
// 以子进程方式拉起本程序，在 stdin/stdout 上按行交换 JSON-RPC 2.0 消息（MCP stdio
// 传输约定：每行一条 UTF-8 JSON，不含内嵌换行）。本程序只做协议适配，内部把工具
// 调用转发到 127.0.0.1 的平台 HTTP API（与 agent-cli / 工作台同一条通道），
// 自身不直接触碰数据库——单写入方原则不被破坏。
//
// 身份与鉴权（与 agent-cli/GUI 语义一致，按序解析）：
//   1) MIDERHIVE_AGENT_NAME + MIDERHIVE_AGENT_KEY 直接使用；
//   2) 否则读 MIDERHIVE_HOME（默认 %USERPROFILE%\.miderhive）/config/agents.json
//      里该名字的密钥（platformd bootstrap 与一键接入都写这里）；
//   3) 否则若提供 MIDERHIVE_MASTER_KEY 则注册一次并写回 agents.json（重启可复用）。
//
// 日志一律走 stderr（stdout 是协议通道，混入任何非 JSON 输出都会杀死客户端会话）。
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "core/types.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// ---------------- 环境与身份 ----------------

std::string envOr(const char* k, const std::string& dflt = "") {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : dflt;
}

struct Identity {
    std::string name;
    std::string key;
};

// agents.json 是平台约定的 name -> api_key 平面映射（bootstrap 预置 zcode，
// 一键接入追加）。读失败/不存在都按空对象处理，不阻塞主流程；
// 损坏（解析失败）返回 nullopt：调用方必须放弃写入，防止清空其他 agent 的明文密钥。
std::optional<json> readAgentsJson(const fs::path& home) {
    std::ifstream in(home / "config" / "agents.json");
    if (!in) return json::object();
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j.is_object() ? j : json::object();
}

void writeAgentsJson(const fs::path& home, const json& j) {
    std::error_code ec;
    const fs::path dir = home / "config";
    fs::create_directories(dir, ec);
    const fs::path target = dir / "agents.json";
    const fs::path tmp = dir / "agents.json.tmp";
    {
        // 先写临时文件再原子替换：中途失败不会留下半截文件（那等于丢失全部明文密钥，
        // 与 platformd persistAgentKey 同一策略、同一原因）
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            std::cerr << "[miderhive-mcp] cannot write " << tmp.string() << std::endl;
            return;
        }
        out << j.dump(2) << "\n";
        out.flush();
        if (!out.good()) {
            out.close();
            std::cerr << "[miderhive-mcp] write failed: " << tmp.string() << std::endl;
            fs::remove(tmp, ec);
            return;
        }
        out.close();
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // Windows 的 rename 不覆盖既有文件：退化为先删再改名（同 persistAgentKey）
        std::error_code ec2;
        fs::remove(target, ec2);
        fs::rename(tmp, target, ec);
        if (ec) std::cerr << "[miderhive-mcp] cannot replace " << target.string()
                          << ": " << ec.message() << std::endl;
    }
}

Identity resolveIdentity() {
    Identity id{envOr("MIDERHIVE_AGENT_NAME"), envOr("MIDERHIVE_AGENT_KEY")};
    fs::path home = envOr("MIDERHIVE_HOME").empty()
                        ? fs::path(envOr("USERPROFILE")) / ".miderhive"
                        : fs::path(envOr("MIDERHIVE_HOME"));
    // 2) 已有密钥文件：直接复用（同名重复注册会被平台拒绝）
    if (!id.name.empty() && id.key.empty()) {
        std::optional<json> agents = readAgentsJson(home);
        if (agents && agents->contains(id.name) && (*agents)[id.name].is_string())
            id.key = (*agents)[id.name].get<std::string>();
    }
    // 3) 有主密钥就自注册一次并落盘；此后重启走第 2 步复用同一身份
    std::string master = envOr("MIDERHIVE_MASTER_KEY");
    if (!id.name.empty() && id.key.empty() && !master.empty()) {
        // 密钥文件损坏时绝不自注册：合并写入会把其他 agent 的明文密钥清掉
        std::optional<json> agents = readAgentsJson(home);
        if (!agents) {
            std::cerr << "[miderhive-mcp] config/agents.json is corrupt; refusing to "
                         "auto-register (fix or remove the file first)" << std::endl;
        } else {
            httplib::Client cli("http://127.0.0.1:" + envOr("MIDERHIVE_PORT", "8787"));
            cli.set_connection_timeout(5, 0);
            cli.set_read_timeout(15, 0);
            httplib::Headers headers{{"X-Master-Key", master}};
            json body = {{"name", id.name}, {"role", "member"}};
            if (auto res = cli.Post("/api/agents/register", headers, body.dump(), "application/json")) {
                json r = json::parse(res->body, nullptr, false);
                if (res->status == 200 && r.is_object() && r.value("code", -1) == 0 &&
                    r.contains("data") && r["data"].contains("api_key")) {
                    id.key = r["data"]["api_key"].get<std::string>();
                    json merged = *agents;
                    merged[id.name] = id.key;
                    writeAgentsJson(home, merged);
                    std::cerr << "[miderhive-mcp] registered agent '" << id.name
                              << "', key saved to config/agents.json" << std::endl;
                } else {
                    std::cerr << "[miderhive-mcp] auto-register failed: HTTP " << res->status
                              << " " << r.value("message", "") << std::endl;
                }
            } else {
                // 平台没起来/端口不对：说清原因，别让用户以为是环境变量配错
                std::cerr << "[miderhive-mcp] cannot reach platform at 127.0.0.1:"
                          << envOr("MIDERHIVE_PORT", "8787")
                          << " for auto-register (is platformd running?)" << std::endl;
            }
        }
    }
    return id;
}

// ---------------- 平台 HTTP 客户端 ----------------

// 查询参数值做百分号编码（unreserved 之外全部编码，UTF-8 字节安全）；
// 不手工拼 SQL 的同样理由：不把外部输入裸拼进 URL。
std::string percentEncode(const std::string& v) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

struct ApiResult {
    bool ok = false;      // HTTP 2xx 且平台信封 code==0
    int status = 0;
    std::string message;  // 失败原因（信封 message 或连接错误）
    json data;            // 成功时的 data 字段
};

// 进程级会话：本程序一次服务一个 MCP 客户端、一个 agent 身份,全局持有最直接
Identity g_id;
int g_port = 8787;

ApiResult apiCall(const std::string& methodAndPath, const json* body) {
    ApiResult r;
    httplib::Client cli("http://127.0.0.1:" + std::to_string(g_port));
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    httplib::Headers headers = {{"X-Agent-Name", g_id.name}, {"X-Api-Key", g_id.key}};
    httplib::Result res;
    if (methodAndPath.rfind("GET ", 0) == 0) {
        res = cli.Get(methodAndPath.substr(4), headers);
    } else if (methodAndPath.rfind("DELETE ", 0) == 0) {
        res = cli.Delete(methodAndPath.substr(7), headers);
    } else if (methodAndPath.rfind("PUT ", 0) == 0) {
        res = cli.Put(methodAndPath.substr(4), headers, body ? body->dump() : std::string("{}"),
                      "application/json");
    } else {  // POST <path>
        res = cli.Post(methodAndPath.substr(5), headers, body ? body->dump() : std::string("{}"),
                       "application/json");
    }
    if (!res) {
        r.message = "cannot reach platform on 127.0.0.1:" + std::to_string(g_port) +
                    " (is the workbench or platformd running?)";
        return r;
    }
    r.status = res->status;
    json env = json::parse(res->body, nullptr, false);
    if (env.is_object() && env.contains("code")) {
        r.message = env.value("message", std::string());
        if (env.contains("data")) r.data = env["data"];
        r.ok = res->status >= 200 && res->status < 300 && env.value("code", -1) == 0;
    } else {
        r.message = "malformed platform response (HTTP " + std::to_string(res->status) + ")";
    }
    return r;
}

// ---------------- 参数取值助手 ----------------
// 类型不对抛异常，由 tools/call 统一转成 isError 工具结果（协议错误只留给协议层）。

const json& arg(const json& args, const char* key) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null())
        throw std::runtime_error(std::string("missing argument: ") + key);
    return *it;
}
std::string sarg(const json& args, const char* key) {
    const json& v = arg(args, key);
    if (!v.is_string()) throw std::runtime_error(std::string("argument must be a string: ") + key);
    return v.get<std::string>();
}
std::string sopt(const json& args, const char* key, const std::string& dflt = "") {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return dflt;
    if (!it->is_string()) throw std::runtime_error(std::string("argument must be a string: ") + key);
    return it->get<std::string>();
}
int64_t iopt(const json& args, const char* key, int64_t dflt) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return dflt;
    if (!it->is_number_integer())
        throw std::runtime_error(std::string("argument must be an integer: ") + key);
    return it->get<int64_t>();
}

// ---------------- 工具表 ----------------
// handler 直接返回 ApiResult；参数解析抛出的异常由调度层兜住。

struct ToolDef {
    const char* name;
    const char* desc;
    json schema;
    std::function<ApiResult(const json& args)> handle;
};

json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }

json objSchema(std::map<std::string, json> props, std::vector<std::string> required) {
    json s = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
    for (auto& [k, v] : props) s["properties"][k] = v;
    if (!required.empty()) s["required"] = required;
    return s;
}

std::vector<ToolDef> buildTools() {
    std::vector<ToolDef> t;

    // ---- 协作面：谁在、记什么、查什么、说什么、报什么、会什么 ----

    // 自检入口：MCP 客户端最常问的一句话是"我到底连上了没有"。把"平台可达 + 我是谁 +
    // 蜂巢里还有谁 + 本周用量"压成一次调用，新手不必先学会拼三个端点再判断。
    t.push_back({"hive_status",
                 "One-call connectivity check: is the platform reachable, which identity am I "
                 "using, who else is in the hive, and how many tokens were used this week. "
                 "Call this first if you are unsure whether MiderHive is up.",
                 objSchema({}, {}),
                 [](const json&) {
                     ApiResult health = apiCall("GET /api/health", nullptr);
                     if (!health.ok) return health;  // 平台不可达：如实回报连接错误
                     ApiResult agents = apiCall("GET /api/agents", nullptr);
                     ApiResult usage = apiCall("GET /api/usage/summary", nullptr);
                     json peers = json::array();
                     int online = 0;
                     if (agents.ok && agents.data.is_array()) {
                         for (const auto& a : agents.data) {
                             if (a.value("status", "") == "online") ++online;
                             peers.push_back(a);
                         }
                     }
                     ApiResult out;
                     out.ok = true;
                     out.data = {{"you", g_id.name},
                                 {"connected", true},
                                 {"platform", health.data},
                                 {"online_count", online},
                                 {"peers", peers},
                                 {"usage_this_week", usage.ok ? usage.data : json::object()}};
                     if (!agents.ok) out.data["peers_error"] = agents.message;
                     if (!usage.ok) out.data["usage_error"] = usage.message;
                     return out;
                 }});

    t.push_back({"agents_list",
                 "List all agents in the hive with role, online status and current task. "
                 "Call first to see who is available.",
                 objSchema({}, {}),
                 [](const json&) { return apiCall("GET /api/agents", nullptr); }});

    t.push_back({"heartbeat",
                 "Announce what you are working on so teammates see your status. "
                 "Call when picking up or finishing work.",
                 objSchema({{"current_task", prop("string", "short free-text status, may be empty")}}, {}),
                 [](const json& a) {
                     json body = {{"current_task", sopt(a, "current_task")}};
                     return apiCall("POST /api/agents/heartbeat", &body);
                 }});

    t.push_back({"memory_list",
                 "List shared memory entries (is_latest only). Optionally filter by section "
                 "(project/preference/work_style/decision/environment).",
                 objSchema({{"section", prop("string", "optional section filter")}}, {}),
                 [](const json& a) {
                     std::string section = sopt(a, "section");
                     std::string path = "/api/memory" +
                                        (section.empty() ? "" : "?section=" + percentEncode(section));
                     return apiCall("GET " + path, nullptr);
                 }});

    t.push_back({"memory_write",
                 "Create or update a shared memory key. Append-only: the old version is kept. "
                 "Pass base_version of the version you read to refuse overwriting others' updates "
                 "(409 on conflict).",
                 objSchema({{"section", prop("string", "project|preference|work_style|decision|environment")},
                            {"key", prop("string", "key within the section")},
                            {"value", prop("string", "value to store (<=20000 chars)")},
                            {"base_version", prop("integer", "version you read; omit to force-write")}},
                           {"section", "key", "value"}),
                 [](const json& a) {
                     json body = {{"section", sarg(a, "section")},
                                  {"key", sarg(a, "key")},
                                  {"value", sarg(a, "value")},
                                  {"base_version", iopt(a, "base_version", 0)}};
                     return apiCall("POST /api/memory", &body);
                 }});

    t.push_back({"memory_history",
                 "List every version of one memory key, newest first.",
                 objSchema({{"section", prop("string", "section")}, {"key", prop("string", "key")}},
                           {"section", "key"}),
                 [](const json& a) {
                     return apiCall("GET /api/memory/history?section=" +
                                        percentEncode(sarg(a, "section")) + "&key=" +
                                        percentEncode(sarg(a, "key")),
                                    nullptr);
                 }});

    t.push_back({"memory_remove",
                 "Delete a memory key with all its versions. Manager (zcode) only.",
                 objSchema({{"section", prop("string", "section")}, {"key", prop("string", "key")}},
                           {"section", "key"}),
                 [](const json& a) {
                     return apiCall("DELETE /api/memory?section=" +
                                        percentEncode(sarg(a, "section")) + "&key=" +
                                        percentEncode(sarg(a, "key")),
                                    nullptr);
                 }});

    t.push_back({"knowledge_add",
                 "Add an entry to the shared knowledge base (auto-embedded for semantic search).",
                 objSchema({{"title", prop("string", "short title (<=200 chars)")},
                            {"content", prop("string", "full content (<=100000 chars)")},
                            {"tags", prop("array", "optional list of tag strings")},
                            {"category", prop("string", "optional category")}},
                           {"title", "content"}),
                 [](const json& a) {
                     json body = {{"title", sarg(a, "title")}, {"content", sarg(a, "content")}};
                     auto tags = a.find("tags");
                    if (tags != a.end() && !tags->is_null()) {
                        if (!tags->is_array())
                            throw std::runtime_error("tags must be an array of strings");
                        body["tags"] = *tags;
                    }
                     body["category"] = sopt(a, "category");
                     return apiCall("POST /api/knowledge", &body);
                 }});

    t.push_back({"knowledge_search",
                 "Search the knowledge base. mode=keyword does substring match; "
                 "mode=semantic does vector similarity (best for natural-language questions). "
                 "Pass the same embedding you wrote the entry with (64..4096 numbers) to search "
                 "entries created with a model-supplied embedding; omit it to search the "
                 "built-in embedding space.",
                 objSchema({{"query", prop("string", "search text")},
                            {"mode", prop("string", "keyword (default) or semantic")},
                            {"limit", prop("integer", "max hits, default 20")},
                            {"tag", prop("string", "optional tag filter")},
                            {"embedding", prop("array", "optional query vector; only used with "
                                                        "mode=semantic")}},
                           {"query"}),
                 [](const json& a) {
                     json body = {{"query", sarg(a, "query")},
                                  {"mode", sopt(a, "mode", "keyword")},
                                  {"limit", iopt(a, "limit", 20)},
                                  {"tag", sopt(a, "tag")}};
                     auto emb = a.find("embedding");
                     if (emb != a.end() && !emb->is_null()) {
                         if (!emb->is_array())
                             throw std::runtime_error("embedding must be an array of numbers");
                         body["embedding"] = *emb;
                     }
                     return apiCall("POST /api/knowledge/search", &body);
                 }});

    t.push_back({"knowledge_list",
                 "Browse latest knowledge entries, newest first.",
                 objSchema({{"limit", prop("integer", "default 100")},
                            {"tag", prop("string", "optional tag filter")}},
                          {}),
                 [](const json& a) {
                     std::string q = "limit=" + std::to_string(iopt(a, "limit", 100));
                     std::string tag = sopt(a, "tag");
                     if (!tag.empty()) q += "&tag=" + percentEncode(tag);
                     return apiCall("GET /api/knowledge?" + q, nullptr);
                 }});

    t.push_back({"knowledge_get",
                 "Read the latest version of one knowledge entry by uuid.",
                 objSchema({{"uuid", prop("string", "knowledge entry uuid")}}, {"uuid"}),
                 [](const json& a) {
                     return apiCall("GET /api/knowledge/" + percentEncode(sarg(a, "uuid")), nullptr);
                 }});

    t.push_back({"knowledge_versions",
                 "List every historical version of one knowledge entry, newest first. "
                 "Versions are append-only: nothing is ever overwritten.",
                 objSchema({{"uuid", prop("string", "knowledge entry uuid")}}, {"uuid"}),
                 [](const json& a) {
                     return apiCall(
                         "GET /api/knowledge/" + percentEncode(sarg(a, "uuid")) + "/versions",
                         nullptr);
                 }});

    t.push_back({"knowledge_add_version",
                 "Append a new version to an existing knowledge entry (knowledge_get or "
                 "knowledge_search to find its uuid). The previous version is kept, so this is "
                 "how you refine shared know-how instead of creating a duplicate entry. "
                 "Omit title to keep the current one.",
                 objSchema({{"uuid", prop("string", "knowledge entry uuid")},
                            {"content", prop("string", "new full content (<=100000 chars)")},
                            {"title", prop("string", "optional new title; omit to keep current")}},
                           {"uuid", "content"}),
                 [](const json& a) {
                     json body = {{"content", sarg(a, "content")}, {"title", sopt(a, "title")}};
                     return apiCall("POST /api/knowledge/" + percentEncode(sarg(a, "uuid")) +
                                        "/versions",
                                    &body);
                 }});

    t.push_back({"message_send",
                 "Send a message. kind=note (default) or task or question. "
                 "Omit recipient to broadcast to everyone; tasks start as pending and can be "
                 "accepted/done/declined by the recipient.",
                 objSchema({{"kind", prop("string", "note | task | question, default note")},
                            {"recipient", prop("string", "target agent name; omit to broadcast")},
                            {"subject", prop("string", "one-line summary")},
                            {"body", prop("string", "full message body")}},
                           {"subject", "body"}),
                 [](const json& a) {
                     json body = {{"kind", sopt(a, "kind", "note")},
                                  {"subject", sarg(a, "subject")},
                                  {"body", sarg(a, "body")}};
                     std::string rcpt = sopt(a, "recipient");
                     if (!rcpt.empty()) body["recipient"] = rcpt;  // 缺省=广播（平台约定）
                     return apiCall("POST /api/messages", &body);
                 }});

    t.push_back({"message_list",
                 "List messages you may see (broadcasts, ones addressed to you, ones you sent). "
                 "Filter by kind/status, e.g. status=question-unread is not valid: use kind=question.",
                 objSchema({{"kind", prop("string", "note|task|question")},
                            {"status", prop("string", "unread|read|pending|accepted|done|declined")},
                            {"limit", prop("integer", "default 100")}},
                           {}),
                 [](const json& a) {
                     std::string q = "limit=" + std::to_string(iopt(a, "limit", 100));
                     std::string kind = sopt(a, "kind");
                     std::string status = sopt(a, "status");
                     if (!kind.empty()) q += "&kind=" + percentEncode(kind);
                     if (!status.empty()) q += "&status=" + percentEncode(status);
                     return apiCall("GET /api/messages?" + q, nullptr);
                 }});

    t.push_back({"message_set_status",
                 "Update a message's status: read for notes/questions; "
                 "pending->accepted->done or declined for tasks.",
                 objSchema({{"uuid", prop("string", "message uuid")},
                            {"status", prop("string", "read|accepted|done|declined")}},
                           {"uuid", "status"}),
                 [](const json& a) {
                     json body = {{"status", sarg(a, "status")}};
                     return apiCall("POST /api/messages/" + percentEncode(sarg(a, "uuid")) + "/status", &body);
                 }});

    t.push_back({"message_reply",
                 "Reply to a message (message_list to find its uuid). Replying also marks the "
                 "parent message as read, so this is the normal way to answer a question or "
                 "report progress back on a task.",
                 objSchema({{"uuid", prop("string", "uuid of the message being replied to")},
                            {"body", prop("string", "reply text (<=50000 chars)")}},
                           {"uuid", "body"}),
                 [](const json& a) {
                     json body = {{"body", sarg(a, "body")}};
                     return apiCall("POST /api/messages/" + percentEncode(sarg(a, "uuid")) + "/reply",
                                    &body);
                 }});

    t.push_back({"error_report",
                 "Report an error or blocker to the hive so it shows up on the dashboard alerts.",
                 objSchema({{"title", prop("string", "one-line summary")},
                            {"detail", prop("string", "what happened, context, reproduction")},
                            {"severity", prop("string", "info|warning|error|critical, default error")},
                            {"source", prop("string", "component/module that hit the error")},
                            {"stack_trace", prop("string", "optional stack trace")}},
                           {"title", "detail"}),
                 [](const json& a) {
                     json body = {{"title", sarg(a, "title")}, {"detail", sarg(a, "detail")},
                                  {"severity", sopt(a, "severity", "error")},
                                  {"source", sopt(a, "source")},
                                  {"stack_trace", sopt(a, "stack_trace")}};
                     // 词表与平台一致(info|warning|error|critical),这里显式拒绝拼错,
                     // 不依赖平台把未知词静默归一成 error 的兜底行为
                     const std::string sev = body["severity"].get<std::string>();
                     if (sev != "info" && sev != "warning" && sev != "error" && sev != "critical")
                         throw std::runtime_error(
                             "severity must be one of: info, warning, error, critical");
                     return apiCall("POST /api/errors", &body);
                 }});

    t.push_back({"error_list",
                 "List error reports. status=open (default) shows unresolved ones.",
                 objSchema({{"status", prop("string", "open|resolved, default open")},
                            {"severity", prop("string", "info|warning|error|critical")},
                            {"limit", prop("integer", "default 200")}},
                           {}),
                 [](const json& a) {
                     std::string q = "limit=" + std::to_string(iopt(a, "limit", 200));
                     std::string status = sopt(a, "status", "open");
                     std::string severity = sopt(a, "severity");
                     if (!status.empty()) q += "&status=" + percentEncode(status);
                     if (!severity.empty()) q += "&severity=" + percentEncode(severity);
                     return apiCall("GET /api/errors?" + q, nullptr);
                 }});

    t.push_back({"error_resolve",
                 "Mark an error report resolved with a note. Manager (zcode) only.",
                 objSchema({{"uuid", prop("string", "error uuid")},
                            {"notes", prop("string", "how it was resolved")}},
                           {"uuid"}),
                 [](const json& a) {
                     json body = {{"notes", sopt(a, "notes")}};
                     return apiCall("POST /api/errors/" + percentEncode(sarg(a, "uuid")) + "/resolve", &body);
                 }});

    t.push_back({"skill_list",
                 "List skills registered by agents, with input schema and owner.",
                 objSchema({{"category", prop("string", "optional category filter")}}, {}),
                 [](const json& a) {
                     std::string cat = sopt(a, "category");
                     std::string path =
                         "/api/skills" + (cat.empty() ? "" : "?category=" + percentEncode(cat));
                     return apiCall("GET " + path, nullptr);
                 }});

    t.push_back({"skill_invoke",
                 "Record a skill invocation (the platform logs params/result/tokens/duration for "
                 "audit and budget accounting; the actual work is performed by the caller). "
                 "The skill must exist (skill_list to discover). Params are validated against the "
                 "registered param_schema (missing required keys or wrong top-level types are rejected).",
                 objSchema({{"name", prop("string", "registered skill name")},
                            {"params", prop("object", "arguments matching the skill's param schema")},
                            {"result_summary", prop("string", "what the invocation produced")},
                            {"status", prop("string", "success|failed, default success")},
                            {"duration_ms", prop("integer", "how long the work took")},
                            {"tokens_in", prop("integer", "tokens consumed, input")},
                            {"tokens_out", prop("integer", "tokens consumed, output")},
                            {"reference_id", prop("string", "optional uuid of the message/task/error that motivated this call, for collaboration traceability")}},
                           {"name"}),
                 [](const json& a) {
                     json body = {{"status", sopt(a, "status", "success")},
                                  {"duration_ms", iopt(a, "duration_ms", 0)},
                                  {"tokens_in", iopt(a, "tokens_in", 0)},
                                  {"tokens_out", iopt(a, "tokens_out", 0)}};
                     auto params = a.find("params");
                     body["params"] =
                         (params != a.end() && params->is_object()) ? *params : json::object();
                     body["result_summary"] = sopt(a, "result_summary");
                     body["reference_id"] = sopt(a, "reference_id");
                     return apiCall("POST /api/skills/" + percentEncode(sarg(a, "name")) + "/invoke",
                                    &body);
                 }});

    t.push_back({"skill_register",
                 "Publish a skill to the hive's skill market so any agent can discover and "
                 "invoke it (skill_list to browse, skill_invoke to log a call). Registering is "
                 "idempotent per name. param_schema is a JSON Schema object describing the "
                 "arguments callers must pass; later invocations are validated against it.",
                 objSchema({{"name", prop("string", "unique skill name (<=64 chars), e.g. code-review")},
                            {"description", prop("string", "what the skill does (<=2000 chars)")},
                            {"display_name", prop("string", "human-friendly title")},
                            {"category", prop("string", "optional grouping label")},
                            {"param_schema", prop("object", "optional JSON Schema for the arguments")}},
                           {"name", "description"}),
                 [](const json& a) {
                     json body = {{"name", sarg(a, "name")},
                                  {"description", sarg(a, "description")},
                                  {"display_name", sopt(a, "display_name")},
                                  {"category", sopt(a, "category")}};
                     auto schema = a.find("param_schema");
                     if (schema != a.end() && !schema->is_null()) {
                         if (!schema->is_object())
                             throw std::runtime_error("param_schema must be an object");
                         body["param_schema"] = *schema;
                     }
                     return apiCall("POST /api/skills", &body);
                 }});

    t.push_back({"usage_summary",
                 "This week's token usage: total, budget, remaining, alert level, per-agent split.",
                 objSchema({}, {}),
                 [](const json&) { return apiCall("GET /api/usage/summary", nullptr); }});

    t.push_back({"usage_report",
                 "Report the tokens a model call consumed so the hive's budget view stays "
                 "accurate. Pass an idempotency_key to make retries safe: a repeated key is "
                 "counted once and the response reports duplicate=true. The response carries the "
                 "live weekly balance and alert level — observation only, it never blocks you.",
                 objSchema({{"tokens_in", prop("integer", "prompt/input tokens (>=0)")},
                            {"tokens_out", prop("integer", "completion/output tokens (>=0)")},
                            {"model", prop("string", "optional model name, e.g. claude-sonnet-4")},
                            {"call_type", prop("string", "optional label, e.g. chat|skill|review")},
                            {"reference_id", prop("string", "optional uuid or label tying this spend to a task")},
                            {"idempotency_key", prop("string", "optional unique key so a retry is not double-counted (<=200 chars)")}},
                           {}),
                 [](const json& a) {
                     json body = {{"tokens_in", iopt(a, "tokens_in", 0)},
                                  {"tokens_out", iopt(a, "tokens_out", 0)},
                                  {"model", sopt(a, "model")},
                                  {"call_type", sopt(a, "call_type")},
                                  {"reference_id", sopt(a, "reference_id")},
                                  {"idempotency_key", sopt(a, "idempotency_key")}};
                     return apiCall("POST /api/usage/report", &body);
                 }});

    t.push_back({"usage_daily",
                 "Day-by-day token usage for the last N days (missing days filled with 0).",
                 objSchema({{"days", prop("integer", "1..90, default 14")}}, {}),
                 [](const json& a) {
                     return apiCall("GET /api/usage/daily?days=" +
                                        std::to_string(iopt(a, "days", 14)),
                                    nullptr);
                 }});

    t.push_back({"usage_breakdown",
                 "Token usage sliced by time range, agent and model in one call — the same data "
                 "the Usage panel shows. Use it to answer 'who spent what on which model'.",
                 objSchema({{"days", prop("integer", "1..90, default 14")},
                            {"agent", prop("string", "optional agent name filter")},
                            {"model", prop("string", "optional model name filter")}},
                           {}),
                 [](const json& a) {
                     std::string q = "days=" + std::to_string(iopt(a, "days", 14));
                     std::string agent = sopt(a, "agent");
                     std::string model = sopt(a, "model");
                     if (!agent.empty()) q += "&agent=" + percentEncode(agent);
                     if (!model.empty()) q += "&model=" + percentEncode(model);
                     return apiCall("GET /api/usage/breakdown?" + q, nullptr);
                 }});

    return t;
}

// ---------------- JSON-RPC / MCP 协议层 ----------------

struct Session {
    Identity id;
    int port = 8787;
    std::vector<ToolDef> tools;
    std::map<std::string, const ToolDef*> byName;

    void init() {
        tools = buildTools();
        for (const auto& d : tools) byName[d.name] = &d;
    }
};

void writeMessage(const json& msg) {
    // stdout 是协议通道：单行 JSON + 换行，立即冲刷
    std::cout << msg.dump() << '\n' << std::flush;
}

json textContent(const std::string& text) {
    return json::array({{{"type", "text"}, {"text", text}}});
}

json toolCallResult(const ApiResult& r) {
    json out = {{"content", textContent(r.ok ? r.data.dump(2) : "error: " + r.message)}};
    if (!r.ok) out["isError"] = true;  // 业务失败在结果里表达,不算协议错误
    return out;
}

// 处理一条已解析的请求/通知。needReply 表示对端在等响应（有 id）。
void dispatch(Session& s, const json& req, bool needReply) {
    const std::string method = req.value("method", "");
    const json id = needReply ? req.value("id", json()) : json();
    json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();

    auto replyResult = [&](const json& result) {
        if (needReply) writeMessage({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    };
    auto replyError = [&](int code, const std::string& message) {
        if (needReply)
            writeMessage({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
    };

    if (method == "initialize") {
        // 协议版本协商：客户端声明版本,服务端回答自己支持的版本；
        // 已知版本原样确认,未知版本回落到基础版（工具面在三个修订里语义一致）
        static const char* kKnown[] = {"2024-11-05", "2025-03-26", "2025-06-18"};
        std::string ver = "2024-11-05";
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
            std::string want = params["protocolVersion"].get<std::string>();
            for (const char* k : kKnown)
                if (want == k) ver = want;
        }
        json result = {{"protocolVersion", ver},
                       {"capabilities", {{"tools", {{"listChanged", false}}}}},
                       {"serverInfo",
                        {{"name", "miderhive"},
                         {"title", "MiderHive"},
                         {"version", ah::kPlatformVersion}}},
                       {"instructions",
                        "Tools for a local-first multi-agent hive (127.0.0.1 only). "
                        "Start with hive_status to confirm you are connected, then agents_list "
                        "to see teammates. Shared user memory: memory_list / memory_write / "
                        "memory_history (read it at startup so you do not re-ask what the user "
                        "already answered). Shared knowledge: knowledge_search (mode=semantic is "
                        "best for natural-language questions), knowledge_add, and "
                        "knowledge_add_version to refine an existing entry without overwriting "
                        "it. Coordination: message_send with kind=note|task|question (omit "
                        "recipient to broadcast), message_list, message_reply, "
                        "message_set_status. Failures: error_report, and error_resolve to close "
                        "the loop. Skills: skill_list, skill_register, skill_invoke. Cost: "
                        "usage_report after model calls, usage_summary / usage_daily / "
                        "usage_breakdown to inspect it. Be a good teammate: heartbeat, write "
                        "memory, distil knowledge, report errors."}};
        replyResult(result);
        return;
    }
    if (method == "notifications/initialized" || method.rfind("notifications/", 0) == 0) {
        return;  // 通知不回包
    }
    if (method == "ping") {
        replyResult(json::object());
        return;
    }
    if (method == "tools/list") {
        json arr = json::array();
        for (const auto& d : s.tools) arr.push_back({{"name", d.name}, {"description", d.desc}, {"inputSchema", d.schema}});
        replyResult({{"tools", arr}});
        return;
    }
    if (method == "tools/call") {
        // 客户端可能发来任意 JSON：name 必须类型安全地取，否则 type_error 会杀死进程
        std::string name = params.contains("name") && params["name"].is_string()
                               ? params["name"].get<std::string>()
                               : "";
        auto it = s.byName.find(name);
        if (it == s.byName.end()) {
            // 未知工具按 JSON-RPC 参数错误回报（MCP 约定）
            replyError(-32602, "unknown tool: " + name);
            return;
        }
        json args = params.contains("arguments") && params["arguments"].is_object()
                        ? params["arguments"]
                        : json::object();
        try {
            replyResult(toolCallResult(it->second->handle(args)));
        } catch (const std::exception& e) {
            ApiResult r;
            r.message = e.what();
            replyResult(toolCallResult(r));  // 参数问题也是工具级失败,客户端可读可改再试
        }
        return;
    }
    replyError(-32601, "method not found: " + method);
}

}  // namespace

int main() {
#ifdef _WIN32
    // stdin/stdout 切二进制：防止 CRLF 翻译破坏“一行一条 JSON”的分帧
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);

    Session s;
    s.id = resolveIdentity();
    {
        std::string p = envOr("MIDERHIVE_PORT", "8787");
        try {
            s.port = std::stoi(p);
        } catch (...) {
            std::cerr << "[miderhive-mcp] invalid MIDERHIVE_PORT '" << p << "', using 8787" << std::endl;
        }
    }
    // 会话全局：HTTP 工具转发与 initialize 的版本号都从这里取
    g_id = s.id;
    g_port = s.port;
    if (s.id.name.empty() || s.id.key.empty()) {
        std::cerr << "[miderhive-mcp] no agent identity: set MIDERHIVE_AGENT_NAME and "
                     "MIDERHIVE_AGENT_KEY (or MIDERHIVE_MASTER_KEY to auto-register)"
                  << std::endl;
        return 2;
    }
    s.init();
    std::cerr << "[miderhive-mcp] agent '" << s.id.name << "' -> 127.0.0.1:" << s.port
              << ", " << s.tools.size() << " tools" << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        json req = json::parse(line, nullptr, false);
        if (req.is_discarded()) {
            // 无法解析:按 JSON-RPC 约定回 parse error（id 未知,置 null）
            writeMessage({{"jsonrpc", "2.0"},
                          {"id", nullptr},
                          {"error", {{"code", -32700}, {"message", "parse error"}}}});
            continue;
        }
        if (!req.is_object() || !req.contains("method") || !req["method"].is_string()) continue;
        dispatch(s, req, req.contains("id"));
    }
    return 0;
}
