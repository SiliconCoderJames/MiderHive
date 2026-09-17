// agent-cli：Agent 侧命令行客户端，演示/调试 HTTP API 的全部能力。
// 用法见 README.md 与 docs/api.md。
#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "core/integrations.hpp"  // 接入配置的生成/写入（与 GUI 共用同一实现）
#include "core/platform.h"        // defaultHomeDir
#include "core/util.h"            // envOr

using nlohmann::json;

namespace {

// 查询参数百分号编码：中文/空格/& 等字符安全进入 URL
std::string urlEncode(const std::string& v) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

// 数字选项容错解析：非法输入回落默认值，不崩进程
long long toInt(const std::string& s, long long def) {
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return def;
        return v;
    } catch (...) {
        return def;
    }
}

// 宽松 JSON 解析：失败返回 discarded，由调用方给出友好错误
json parseJsonLenient(const std::string& s) {
    return json::parse(s, nullptr, false);
}

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opts;   // 子命令选项（第一个位置参数之后）
    std::map<std::string, std::string> gopts;  // 全局选项（命令之前），如 --name/--key
};

Args parseArgs(int argc, char** argv) {
    Args a;
    bool afterCmd = false;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            auto& target = afterCmd ? a.opts : a.gopts;
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                target[key] = argv[++i];
            } else {
                target[key] = "true";
            }
        } else {
            a.pos.push_back(s);
            afterCmd = true;
        }
    }
    return a;
}


std::string readMasterKeyFromDisk() {
    std::ifstream in(ah::defaultHomeDir() + "/config/master.key");
    std::string k;
    if (in) std::getline(in, k);
    return k;
}

struct Client {
    httplib::Client http;
    std::string name;
    std::string key;
    std::string masterKey;

    explicit Client(const Args& a)
        : http("http://127.0.0.1:" +
               ah::envOr({"MIDERHIVE_PORT", "AGENTHIVE_PORT", "ZCODE_PLATFORM_PORT"}, "8787")),
          name(a.gopts.count("name")
                   ? a.gopts.at("name")
                   : ah::envOr({"MIDERHIVE_AGENT_NAME", "AGENTHIVE_AGENT_NAME",
                                "ZCODE_AGENT_NAME"})),
          key(a.gopts.count("key")
                  ? a.gopts.at("key")
                  : ah::envOr({"MIDERHIVE_AGENT_KEY", "AGENTHIVE_AGENT_KEY", "ZCODE_AGENT_KEY"})),
          masterKey(a.gopts.count("master-key")
                        ? a.gopts.at("master-key")
                        : ah::envOr({"MIDERHIVE_MASTER_KEY", "AGENTHIVE_MASTER_KEY",
                                     "ZCODE_PLATFORM_MASTER_KEY"},
                                    readMasterKeyFromDisk())) {}

    json call(const std::string& method, const std::string& path, const json* body,
              bool needsAgent) {
        httplib::Headers headers;
        if (needsAgent) {
            if (name.empty() || key.empty()) {
                json err{{"code", -1},
                         {"message",
                          "missing --name/--key (or MIDERHIVE_AGENT_NAME/MIDERHIVE_AGENT_KEY)"},
                         {"data", nullptr}};
                return err;
            }
            headers.emplace("X-Agent-Name", name);
            headers.emplace("X-Api-Key", key);
        }
        if (!masterKey.empty()) headers.emplace("X-Master-Key", masterKey);

        std::string payload = body ? body->dump() : "";
        httplib::Result res;
        if (method == "GET") res = http.Get(path, headers);
        else if (method == "POST") res = http.Post(path, headers, payload, "application/json");
        else if (method == "PUT") res = http.Put(path, headers, payload, "application/json");
        else { return json{{"code", -1}, {"message", "bad method"}, {"data", nullptr}}; }

        if (!res) {
            return json{{"code", -1},
                        {"message", "cannot reach platform: " +
                                        httplib::to_string(res.error())},
                        {"data", nullptr}};
        }
        auto parsed = json::parse(res->body, nullptr, false);
        if (parsed.is_discarded())
            return json{{"code", res->status}, {"message", res->body}, {"data", nullptr}};
        return parsed;
    }
};

int usage() {
    std::cout <<
        "agent-cli: MiderHive 多 Agent 协作平台命令行客户端（适用于任意 AI Agent）\n"
        "全局选项需放在命令之前: agent-cli --name X --key K [--master-key M] [--port N] <命令> ...\n"
        "环境: MIDERHIVE_AGENT_NAME MIDERHIVE_AGENT_KEY MIDERHIVE_MASTER_KEY MIDERHIVE_PORT\n"
        "      （旧 AGENTHIVE_* / ZCODE_* 环境变量名仍兼容识别）\n"
        "\n"
        "命令:\n"
        "  connect-snippet --tool T [--command C] [--name N] [--key K]\n"
        "                                 生成该工具的接入配置片段（stdout；不连平台）\n"
        "  apply-config  --tool T (--path P | --dir D) [--command C] [--name N] [--key K]\n"
        "                                 把配置写进指定文件 / 指定根目录下的约定位置\n"
        "                                 （--dir 时 claude-code 写 <dir>/.mcp.json；不连平台）\n"
        "  register --name X [--role member]              注册新 Agent（需 --master-key 或环境变量）\n"
        "  heartbeat                    [--task \"...\"]                 心跳 + 当前任务\n"
        "  agents [remove --name N]                     协作者列表 / 移除（移除需主密钥）\n"
        "  memory get                   [--section S]                 读取用户记忆（启动时必查）\n"
        "  memory set    --section S --key K --value V               写入用户记忆（生成新版本）\n"
        "  memory history --section S --key K                        查看某条记忆的历史版本\n"
        "  knowledge add   --title T --content C [--tag A]... [--category C] [--embedding-file J]\n"
        "  knowledge search --q Q [--mode keyword|semantic] [--tag A]\n"
        "  knowledge get   --uuid U                    knowledge versions --uuid U\n"
        "  knowledge version --uuid U --content C [--title T]\n"
        "  skills register --name N --description D [--category C] [--schema-file F]\n"
        "  skills list     [--category C] [--owner O]    skills get --name N\n"
        "  skills invoke   --name N [--params-json J] [--status success|failed]\n"
        "                  [--result R] [--duration-ms M] [--tokens-in I] [--tokens-out O]\n"
        "  message send   --kind note|question|task [--to X] [--subject S] --body B\n"
        "  message inbox  [--kind K] [--status S]     message reply --uuid U --body B\n"
        "  message status --uuid U --status read|accepted|done|declined\n"
        "  error report   --title T --detail D [--severity S] [--source S] [--stack S]\n"
        "  usage report   --tokens-in I --tokens-out O [--type T] [--model M] [--ref R]\n"
        "  usage daily    [--days N]                usage models（按模型累计）\n"
        "  usage summary                budget get / budget set --value N(主密钥)\n"
        "  audit          [--actor A] [--action A] [--limit N]\n";
    return 0;
}

}  // namespace

#ifdef _WIN32
// argv 的正确形态是 UTF-16（wmain）；转成 UTF-8 供 JSON 使用，任何字符都不丢。
// （若用 ANSI 版 main，CRT 会先把参数转成本机代码页，GBK 外字符在入口即丢失）
static std::string utf16ToUtf8(const wchar_t* w, int wlen) {
    if (wlen <= 0) return {};
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, u.data(), ulen, nullptr, nullptr);
    while (!u.empty() && u.back() == '\0') u.pop_back();
    return u;
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> argStore;
    std::vector<const char*> argPtrs;
    argStore.reserve(static_cast<size_t>(argc));
    argPtrs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        argStore.push_back(utf16ToUtf8(argv[i], static_cast<int>(wcslen(argv[i]))));
        argPtrs.push_back(argStore.back().c_str());
    }
    char** utf8Argv = const_cast<char**>(argPtrs.data());
    const std::string argv0 = argc > 0 ? (utf8Argv[0] ? utf8Argv[0] : "") : "";
    Args a = parseArgs(argc, utf8Argv);
#else
int main(int argc, char** argv) {
    const std::string argv0 = argc > 0 ? (argv[0] ? argv[0] : "") : "";
    Args a = parseArgs(argc, argv);
#endif
    if (a.pos.empty()) return usage();

    const std::string cmd = a.pos[0];

    // ---- 离线命令：接入配置的生成与写入（不连平台）----
    // 与 GUI 共用 core/integrations.hpp 的同一份实现：向导生成什么，命令行就给什么，
    // 脚本（scripts/verify_clients.py）断言的也是它。
    if (cmd == "connect-snippet" || cmd == "apply-config") {
        std::string tool =
            a.opts.count("tool") ? a.opts.at("tool") : (a.pos.size() > 1 ? a.pos[1] : "");
        if (!ah::integrations::toolById(tool)) {
            std::cerr << "unknown tool: " << tool << "\navailable:";
            for (const auto& t : ah::integrations::toolRegistry()) std::cerr << " " << t.id;
            std::cerr << "\n";
            return 2;
        }
        std::string command = a.opts.count("command") ? a.opts.at("command") : "";
        if (command.empty()) {
            // 默认指向与 agent-cli 同目录的 miderhive-mcp（安装后四个可执行文件总在同一目录）
            namespace fs = std::filesystem;
#ifdef _WIN32
            const char* mcpName = "miderhive-mcp.exe";
#else
            const char* mcpName = "miderhive-mcp";
#endif
            command = (fs::path(argv0).parent_path() / mcpName).string();
        }
        const std::string name = a.opts.count("name") ? a.opts.at("name") : tool;
        const std::string key = a.opts.count("key") ? a.opts.at("key") : "";

        if (cmd == "connect-snippet") {
            std::cout << ah::integrations::generateConfig(tool, command, name, key);
            return 0;
        }

        const bool hasPath = a.opts.count("path") != 0;
        const bool hasDir = a.opts.count("dir") != 0;
        if (hasPath == hasDir) {
            std::cerr << "apply-config 需要 --path <文件> 或 --dir <根目录> 二选一\n";
            return 2;
        }
        // --dir 的语义按工具分两种：claude-code 的落点是**项目根**的 .mcp.json
        // （Claude Code 的项目作用域约定）；其余工具是配置根下的约定相对路径。
        ah::integrations::WriteResult r =
            hasPath ? ah::integrations::writeConfigFile(tool, a.opts.at("path"), command, name, key)
                    : (tool == "claude-code"
                           ? ah::integrations::writeProjectConfig(a.opts.at("dir"), command, name,
                                                                  key)
                           : ah::integrations::writeConfigUnderRoot(a.opts.at("dir"), tool, command,
                                                                    name, key));
        std::cout << (r.ok ? "OK: " : "FAIL: ") << r.detailZh << "\n";
        return r.ok ? 0 : 1;
    }

    Client c(a);
    const std::string& sub = a.pos.size() > 1 ? a.pos[1] : "";
    // --master-key 也允许写在命令后（register / budget set 场景）
    if (a.opts.count("master-key")) c.masterKey = a.opts.at("master-key");
    json body;
    std::string path;
    std::string method = "GET";
    bool needsAgent = true;

    if (cmd == "register") {
        method = "POST"; path = "/api/agents/register"; needsAgent = false;
        body = {{"name", a.opts.count("name") ? a.opts["name"] : ""},
                {"role", a.opts.count("role") ? a.opts["role"] : "member"}};
    } else if (cmd == "heartbeat") {
        method = "POST"; path = "/api/agents/heartbeat";
        body = {{"current_task", a.opts.count("task") ? a.opts["task"] : ""}};
    } else if (cmd == "agents") {
        if (sub == "remove") {
            method = "POST"; path = "/api/agents/remove"; needsAgent = false;
            body = {{"name", a.opts.count("name") ? a.opts["name"] : ""}};
        } else {
            path = "/api/agents";
        }
    } else if (cmd == "memory") {
        if (sub == "get") {
            path = "/api/memory";
            if (a.opts.count("section")) path += "?section=" + urlEncode(a.opts["section"]);
        } else if (sub == "set") {
            method = "POST"; path = "/api/memory";
            body = {{"section", a.opts["section"]}, {"key", a.opts["key"]}, {"value", a.opts["value"]}};
        } else if (sub == "history") {
            if (!a.opts.count("section") || !a.opts.count("key")) return usage();
            path = "/api/memory/history?section=" + urlEncode(a.opts["section"]) +
                   "&key=" + urlEncode(a.opts["key"]);
        } else return usage();
    } else if (cmd == "knowledge") {
        if (sub == "add") {
            method = "POST"; path = "/api/knowledge";
            json tags = json::array();
            auto range = a.opts.equal_range("tag");
            for (auto it = range.first; it != range.second; ++it) tags.push_back(it->second);
            body = {{"title", a.opts["title"]}, {"content", a.opts["content"]}, {"tags", tags},
                    {"category", a.opts.count("category") ? a.opts["category"] : ""}};
            if (a.opts.count("embedding-file")) {
                std::ifstream in(a.opts["embedding-file"]);
                json emb;
                if (in) {
                    std::istreambuf_iterator<char> it(in), end;
                    emb = parseJsonLenient(std::string(it, end));
                }
                if (emb.is_discarded() || !emb.is_array()) {
                    std::cerr << "embedding-file 必须是 JSON 数组\n";
                    return 2;
                }
                body["embedding"] = emb;
                body["embedder"] = "agent";
            }
        } else if (sub == "search") {
            method = "POST"; path = "/api/knowledge/search";
            body = {{"query", a.opts["q"]},
                    {"mode", a.opts.count("mode") ? a.opts["mode"] : "keyword"},
                    {"tag", a.opts.count("tag") ? a.opts["tag"] : ""},
                    {"limit", 20}};
        } else if (sub == "get") {
            path = "/api/knowledge/" + a.opts["uuid"];
        } else if (sub == "versions") {
            path = "/api/knowledge/" + a.opts["uuid"] + "/versions";
        } else if (sub == "version") {
            method = "POST"; path = "/api/knowledge/" + a.opts["uuid"] + "/versions";
            body = {{"content", a.opts["content"]},
                    {"title", a.opts.count("title") ? a.opts["title"] : ""}};
        } else return usage();
    } else if (cmd == "skills") {
        if (sub == "register") {
            method = "POST"; path = "/api/skills";
            json schema = json::object();
            if (a.opts.count("schema-file")) {
                std::ifstream in(a.opts["schema-file"]);
                if (in) {
                    std::istreambuf_iterator<char> it(in), end;
                    schema = parseJsonLenient(std::string(it, end));
                }
                if (schema.is_discarded() || !schema.is_object()) {
                    std::cerr << "schema-file 必须是 JSON 对象\n";
                    return 2;
                }
            }
            body = {{"name", a.opts["name"]},
                    {"display_name", a.opts.count("display-name") ? a.opts["display-name"] : ""},
                    {"description", a.opts["description"]},
                    {"category", a.opts.count("category") ? a.opts["category"] : ""},
                    {"param_schema", schema}};
        } else if (sub == "list") {
            path = "/api/skills";
            std::string q;
            if (a.opts.count("category")) q += "?category=" + urlEncode(a.opts["category"]);
            if (a.opts.count("owner"))
                q += (q.empty() ? "?" : "&") + std::string("owner=") + urlEncode(a.opts["owner"]);
            path += q;
        } else if (sub == "get") {
            path = "/api/skills/" + urlEncode(a.opts["name"]);
        } else if (sub == "invoke") {
            method = "POST"; path = "/api/skills/" + urlEncode(a.opts["name"]) + "/invoke";
            json params = json::object();
            if (a.opts.count("params-json")) {
                params = parseJsonLenient(a.opts["params-json"]);
                if (params.is_discarded() || !params.is_object()) {
                    std::cerr << "--params-json 必须是 JSON 对象\n";
                    return 2;
                }
            }
            body = {{"params", params},
                    {"status", a.opts.count("status") ? a.opts["status"] : "success"},
                    {"result_summary", a.opts.count("result") ? a.opts["result"] : ""},
                    {"duration_ms", a.opts.count("duration-ms") ? toInt(a.opts["duration-ms"], 0) : 0},
                    {"tokens_in", a.opts.count("tokens-in") ? toInt(a.opts["tokens-in"], 0) : 0},
                    {"tokens_out", a.opts.count("tokens-out") ? toInt(a.opts["tokens-out"], 0) : 0}};
        } else return usage();
    } else if (cmd == "message") {
        if (sub == "send") {
            method = "POST"; path = "/api/messages";
            body = {{"kind", a.opts["kind"]},
                    {"recipient", a.opts.count("to") ? a.opts["to"] : ""},
                    {"subject", a.opts.count("subject") ? a.opts["subject"] : ""},
                    {"body", a.opts["body"]}};
        } else if (sub == "inbox") {
            path = "/api/messages";
            std::string q;
            if (a.opts.count("kind")) q += "?kind=" + urlEncode(a.opts["kind"]);
            if (a.opts.count("status"))
                q += (q.empty() ? "?" : "&") + std::string("status=") + urlEncode(a.opts["status"]);
            path += q;
        } else if (sub == "reply") {
            method = "POST"; path = "/api/messages/" + a.opts["uuid"] + "/reply";
            body = {{"body", a.opts["body"]}};
        } else if (sub == "status") {
            method = "POST"; path = "/api/messages/" + a.opts["uuid"] + "/status";
            body = {{"status", a.opts["status"]}};
        } else return usage();
    } else if (cmd == "error") {
        if (sub != "report") return usage();
        method = "POST"; path = "/api/errors";
        body = {{"severity", a.opts.count("severity") ? a.opts["severity"] : "error"},
                {"source", a.opts.count("source") ? a.opts["source"] : ""},
                {"title", a.opts["title"]},
                {"detail", a.opts["detail"]},
                {"stack_trace", a.opts.count("stack") ? a.opts["stack"] : ""}};
    } else if (cmd == "usage") {
        if (sub == "report") {
            method = "POST"; path = "/api/usage/report";
            body = {{"tokens_in", toInt(a.opts["tokens-in"], 0)},
                    {"tokens_out", toInt(a.opts["tokens-out"], 0)},
                    {"call_type", a.opts.count("type") ? a.opts["type"] : ""},
                    {"model", a.opts.count("model") ? a.opts["model"] : ""},
                    {"reference_id", a.opts.count("ref") ? a.opts["ref"] : ""},
                    {"idempotency_key", a.opts.count("idem") ? a.opts["idem"] : ""}};
        } else if (sub == "summary") {
            path = "/api/usage/summary";
        } else if (sub == "daily") {
            path = "/api/usage/daily";
            if (a.opts.count("days")) path += "?days=" + a.opts["days"];
        } else if (sub == "models") {
            path = "/api/usage/models";
        } else return usage();
    } else if (cmd == "budget") {
        if (sub == "get") {
            path = "/api/usage/budget";
        } else if (sub == "set") {
            method = "PUT"; path = "/api/usage/budget"; needsAgent = false;
            body = {{"budget", toInt(a.opts["value"], 0)}};
        } else return usage();
    } else if (cmd == "audit") {
        path = "/api/audit";
        std::string q;
        if (a.opts.count("actor")) q += "?actor=" + urlEncode(a.opts["actor"]);
        if (a.opts.count("action"))
            q += (q.empty() ? "?" : "&") + std::string("action=") + urlEncode(a.opts["action"]);
        if (a.opts.count("limit"))
            q += (q.empty() ? "?" : "&") + std::string("limit=") + urlEncode(a.opts["limit"]);
        path += q;
    } else {
        return usage();
    }

    json result = c.call(method, path, &body, needsAgent);
    std::cout << result.dump(2) << "\n";
    return result.value("code", -1) == 0 ? 0 : 1;
}
