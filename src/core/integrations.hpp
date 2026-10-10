#pragma once
// MCP 客户端配置的生成与写入（core 层，Qt-free，只用 std::string / nlohmann / std::filesystem）。
//
// 为什么放在 core 而不是 GUI：配置格式是**产品行为**而不是界面细节。agent-cli、
// 自动化脚本、无 Qt 的单元测试都要用同一份实现——困在 GUI 里的话，"界面生成的"、
// "文档写的"、"脚本断言的" 三处必然漂移。
//
// 写入原则（与 persistAgentKey 同一价值观）：
//   * 能合并才合并；形状没把握就拒绝并给出可执行的下一步，绝不部分写入；
//   * 覆盖前必留 .miderhive.bak；先写 .tmp 再改名，中途失败不留半截文件；
//   * 重复写入是**替换**而不是追加（不产生第二份 [mcp_servers.x] / 第二个 insert 块）。
#include <nlohmann/json.hpp>

#include "core/util.h"  // envOr：环境变量走宽字符读取（getenv 的 GBK 错位见 util.h 注释）

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ah::integrations {

// 配置落点形态：决定 generateConfig 生成什么、能不能安全合并。
enum class Format {
    JsonMcpServers,  // {"mcpServers": {...}}       —— Claude Desktop / Droid / Cursor
    TomlCodex,       // [mcp_servers.<name>]        —— Codex CLI
    DshPatchYaml,    // - insert: [...]             —— DSH 的 cordis.patch.yml
    HermesYaml,      // mcp_servers:                —— Hermes config.yaml
    InstructionsMd,  // 指令块粘进 AGENTS.md 一类    —— 无 MCP 的工具兜底
    EnvVars,         // 只需环境变量                —— ZCode
};

struct ToolMeta {
    const char* id;
    Format format;
    const char* defaultAgentName;  // 一键接入预填的身份名（zcode 是保留名，故 ZCode 用 zcode-agent）
    const char* exeName;           // PATH 探测名（空串 = 无 CLI，只靠候选路径检测）
};

// 客户端注册表（GUI 的展示名/安装指引在 gui/integrations.h，这里只放行为元数据）
inline const std::vector<ToolMeta>& toolRegistry() {
    static const std::vector<ToolMeta> kTools = {
        {"claude-code", Format::JsonMcpServers, "claude",      "claude"},
        {"codex",       Format::TomlCodex,      "codex",       "codex"},
        {"droid",       Format::JsonMcpServers, "droid",       "droid"},
        {"dsh",         Format::DshPatchYaml,   "dsh",         "dsh"},
        {"hermes",      Format::HermesYaml,     "hermes",      "hermes"},
        {"zcode",       Format::EnvVars,        "zcode-agent", "zcode"},
        {"cursor",      Format::JsonMcpServers, "cursor",      "cursor"},
        {"gemini",      Format::JsonMcpServers, "gemini",      "gemini"},
        {"qwen",        Format::JsonMcpServers, "qwen",        "qwen"},
        {"iflow",       Format::JsonMcpServers, "iflow",       "iflow"},
        {"windsurf",    Format::JsonMcpServers, "windsurf",    ""},
        {"kiro",        Format::JsonMcpServers, "kiro",        ""},
        {"copilot",     Format::InstructionsMd, "copilot",     "code"},
    };
    return kTools;
}

inline const ToolMeta* toolById(const std::string& id) {
    for (const auto& t : toolRegistry())
        if (id == t.id) return &t;
    return nullptr;
}

inline Format formatOf(const std::string& id) {
    const ToolMeta* t = toolById(id);
    return t ? t->format : Format::JsonMcpServers;
}

// 是否存在"可以向导自动写入"的配置文件（指令块 / 环境变量没有）
inline bool hasWritableConfig(const std::string& id) {
    const Format f = formatOf(id);
    return f != Format::InstructionsMd && f != Format::EnvVars;
}

// 各工具配置文件相对"配置根"的位置。真实根（%APPDATA% / %USERPROFILE% / %LOCALAPPDATA%）
// 由调用方决定；MIDERHIVE_CONNECT_ROOT 这类重定向也是"换根不换相对路径"。
// claude-code 返回 Claude Desktop 的位置：Claude Code 走项目根 .mcp.json（writeProjectConfig）。
inline std::string relativeConfigPath(const std::string& id) {
    if (id == "claude-code") return "Claude/claude_desktop_config.json";
    if (id == "codex") return ".codex/config.toml";
    if (id == "droid") return ".factory/mcp.json";
    if (id == "dsh") return ".dsh/profiles/web/cordis.patch.yml";
    if (id == "hermes") return "hermes/config.yaml";
    if (id == "cursor") return ".cursor/mcp.json";
    if (id == "gemini") return ".gemini/settings.json";
    if (id == "qwen") return ".qwen/settings.json";
    if (id == "iflow") return ".iflow/settings.json";
    if (id == "windsurf") return ".codeium/windsurf/mcp_config.json";
    if (id == "kiro") return ".kiro/settings/mcp.json";
    return "";  // zcode / copilot：无固定配置文件
}

// ---------------- 本机安装检测（Qt-free；GUI 与 CLI 共用）----------------

// UTF-8 路径 → std::filesystem。窄字符串构造在 Windows 上按 ANSI 代码页解释：
// 中文用户的 %USERPROFILE%（UTF-8，经 ah::envOr 宽字符读取）直接传给窄构造会
// exists()/读写全部错位。char8_t 构造才是真正的 UTF-8 → 宽字符转换。
// 本头文件的所有文件系统操作一律经此函数。
inline std::filesystem::path fsPath(const std::string& p) {
#ifdef _WIN32
    return std::filesystem::path(reinterpret_cast<const char8_t*>(p.data()),
                                 reinterpret_cast<const char8_t*>(p.data() + p.size()));
#else
    return std::filesystem::path(p);
#endif
}

// 在 PATH 上查找可执行文件（Windows 依次补 .exe/.cmd/.bat 后缀；POSIX 原样）。
// 只回答"在不在"——路径值不出这个函数，因此不涉及宽窄转换的返回问题。
inline bool exeOnPath(const std::string& exeName) {
    if (exeName.empty()) return false;
    const std::string pathEnv = ah::envOr({"PATH"});
    if (pathEnv.empty()) return false;
#ifdef _WIN32
    const char sep = ';';
    const char* suffixes[] = {"", ".exe", ".cmd", ".bat"};
#else
    const char sep = ':';
    const char* suffixes[] = {""};
#endif
    size_t start = 0;
    while (start <= pathEnv.size()) {
        const size_t end = pathEnv.find(sep, start);
        std::string dir =
            pathEnv.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? pathEnv.size() + 1 : end + 1;
        // 去引号与空白（PATH 项偶尔带引号）
        if (!dir.empty() && dir.front() == '"') dir = dir.substr(1);
        while (!dir.empty() && (dir.back() == '"' || dir.back() == ' ')) dir.pop_back();
        if (dir.empty()) continue;
        for (const char* suf : suffixes) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(fsPath(dir) / fsPath(exeName + suf), ec))
                return true;
        }
    }
    return false;
}

// 常见安装/配置路径候选（含 npm 全局与用户目录配置目录）。目录本身存在也算
// 已安装的信号——语义是"误报可接受、漏报不可接受"（检测不到只影响提示文案，
// 不拦任何操作）。
inline std::vector<std::string> candidatePaths(const std::string& id) {
    const std::string local = ah::envOr({"LOCALAPPDATA"});
    const std::string appdata = ah::envOr({"APPDATA"});
    const std::string home = ah::envOr({"USERPROFILE"});
    if (id == "claude-code")
        return {local + "/Programs/claude/claude.exe", home + "/.claude/local/claude.exe",
                appdata + "/npm/claude.cmd", appdata + "/npm/claude", appdata + "/Claude",
                home + "/.claude"};
    if (id == "codex")
        return {appdata + "/npm/codex.cmd", appdata + "/npm/codex", appdata + "/npm/codex.ps1",
                home + "/.codex", local + "/Programs/ChatGPT", local + "/ChatGPT"};
    if (id == "droid")
        return {home + "/.factory", local + "/Programs/droid", appdata + "/npm/droid.cmd"};
    if (id == "dsh")
        return {home + "/.dsh", appdata + "/npm/dsh.cmd", appdata + "/npm/dsh"};
    if (id == "hermes")
        return {local + "/hermes", local + "/Programs/hermes", appdata + "/npm/hermes.cmd"};
    if (id == "zcode")
        return {appdata + "/npm/zcode.cmd", local + "/Programs/zcode", home + "/.zcode"};
    if (id == "cursor")
        return {local + "/Programs/cursor/Cursor.exe", local + "/Programs/cursor/cursor.exe",
                home + "/.cursor"};
    if (id == "copilot")
        return {home + "/.vscode/extensions", local + "/Programs/Microsoft VS Code"};
    if (id == "gemini")
        return {home + "/.gemini", appdata + "/npm/gemini.cmd", appdata + "/npm/gemini"};
    if (id == "qwen")
        return {home + "/.qwen", appdata + "/npm/qwen.cmd", appdata + "/npm/qwen"};
    if (id == "iflow")
        return {home + "/.iflow", appdata + "/npm/iflow.cmd", appdata + "/npm/iflow"};
    if (id == "windsurf")
        return {local + "/Programs/Windsurf", home + "/.codeium/windsurf"};
    if (id == "kiro")
        return {local + "/Programs/Kiro", home + "/.kiro"};
    return {};
}

// 两级安装检测：PATH 可执行 + 候选路径存在其一
inline bool installedById(const std::string& id) {
    if (const ToolMeta* t = toolById(id))
        if (t->exeName && *t->exeName && exeOnPath(t->exeName)) return true;
    for (const auto& p : candidatePaths(id)) {
        std::error_code ec;
        if (std::filesystem::exists(fsPath(p), ec)) return true;
    }
    return false;
}

// ---------------- 片段生成 ----------------

// YAML 单引号标量：内部单引号写成 ''，反斜杠字面保留——Windows 路径因此无需任何转义，
// 比 TOML/JSON 的双引号安全得多。
inline std::string yamlQuote(const std::string& v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

// TOML 基本字符串（双引号）：反斜杠与双引号必须转义，控制字符不允许裸出现。
// 此前 TOML 分支复用 yamlQuote——TOML 的**字面量字符串**（单引号）不支持 ''
// 转义，名字/路径含单引号（如 O'Brien）会生成整份解析失败的 config.toml。
inline std::string tomlQuote(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

// 生成该工具的接入配置片段。command 为 miderhive-mcp 可执行文件完整路径。
inline std::string generateConfig(const std::string& id, const std::string& command,
                                  const std::string& agentName, const std::string& agentKey) {
    std::ostringstream o;
    switch (formatOf(id)) {
        case Format::TomlCodex:
            // TOML 基本字符串（双引号）：反斜杠必须双写（\\ 在 TOML 里是字面反斜杠，
            // 单个 \Q 才是非法转义），引号与含单引号的值也都能安全表达。
            o << "[mcp_servers.miderhive]\n"
              << "command = " << tomlQuote(command) << "\n"
              << "args = []\n"
              << "env = { MIDERHIVE_AGENT_NAME = " << tomlQuote(agentName)
              << ", MIDERHIVE_AGENT_KEY = " << tomlQuote(agentKey) << " }\n";
            return o.str();
        case Format::DshPatchYaml:
            // DSH 会清洗子进程环境（名字含 KEY/TOKEN/SECRET 的一律删除），
            // 所以密钥必须显式写进 env，不能指望继承父进程环境。
            o << "- insert:\n"
              << "    - id: mcp-miderhive\n"
              << "      name: '@deepseek-ai/dsh-mcp-client'\n"
              << "      config:\n"
              << "        serverName: miderhive\n"
              << "        transport: stdio\n"
              << "        command: " << yamlQuote(command) << "\n"
              << "        args: []\n"
              << "        env:\n"
              << "          MIDERHIVE_AGENT_NAME: " << yamlQuote(agentName) << "\n"
              << "          MIDERHIVE_AGENT_KEY: " << yamlQuote(agentKey) << "\n";
            return o.str();
        case Format::HermesYaml:
            o << "mcp_servers:\n"
              << "  miderhive:\n"
              << "    command: " << yamlQuote(command) << "\n"
              << "    args: []\n"
              << "    env:\n"
              << "      MIDERHIVE_AGENT_NAME: " << yamlQuote(agentName) << "\n"
              << "      MIDERHIVE_AGENT_KEY: " << yamlQuote(agentKey) << "\n";
            return o.str();
        case Format::EnvVars:
            o << "MIDERHIVE_AGENT_NAME=" << agentName << "\n"
              << "MIDERHIVE_AGENT_KEY=" << agentKey << "\n"
              << "MIDERHIVE_PORT=8787\n";
            return o.str();
        case Format::InstructionsMd:
            o << "## MiderHive (local multi-agent hive)\n\n"
              << "This machine runs a local-first agent hive at http://127.0.0.1:8787.\n"
              << "Before starting work, read shared context; when you learn something reusable,\n"
              << "write it back so other agents do not rediscover it.\n\n"
              << "Identity headers (send on every request):\n"
              << "  X-Agent-Name: " << agentName << "\n"
              << "  X-Api-Key: " << agentKey << "\n\n"
              << "Startup: GET /api/agents -> GET /api/memory?section=project -> "
              << "POST /api/agents/heartbeat (then every 30-60s).\n"
              << "While working: POST /api/knowledge to distil know-how, "
              << "POST /api/knowledge/search to look things up, "
              << "POST /api/messages to ask or delegate, "
              << "POST /api/errors to report failures, "
              << "POST /api/usage/report after model calls.\n";
            return o.str();
        case Format::JsonMcpServers:
        default: {
            // ordered_json：输出键序 = 插入序（command → args → env），与官方示例一致
            nlohmann::ordered_json env{{"MIDERHIVE_AGENT_NAME", agentName},
                                       {"MIDERHIVE_AGENT_KEY", agentKey}};
            nlohmann::ordered_json server{{"command", command}};
            if (id == "cursor") server["args"] = nlohmann::ordered_json::array();
            server["env"] = env;
            nlohmann::ordered_json doc{{"mcpServers", nlohmann::ordered_json{{"miderhive", server}}}};
            return doc.dump(2) + "\n";
        }
    }
}

// 有可替代配置文件写法的手工命令（复制给用户；claude-code 另有 .mcp.json 文件路线）。
// 名字与密钥一律加引号：值里出现 & | ^ " 等 cmd.exe 元字符时，不加引号的拼装会被
// 当成命令分隔符——名字含 & 时，提示用户粘贴的命令会在 cmd.exe 里执行名字中携带
// 的后续内容。
inline std::string cmdQuote(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

inline std::string cliCommand(const std::string& id, const std::string& command,
                              const std::string& agentName, const std::string& agentKey) {
    if (id == "claude-code")
        return "claude mcp add miderhive \\\n"
               "  -e MIDERHIVE_AGENT_NAME=" + cmdQuote(agentName) +
               " \\\n  -e MIDERHIVE_AGENT_KEY=" + cmdQuote(agentKey) +
               " \\\n  -- \"" + command + "\"";
    if (id == "droid")
        return "droid mcp add miderhive \"" + command +
               "\" \\\n  --env MIDERHIVE_AGENT_NAME=" + cmdQuote(agentName) +
               " --env MIDERHIVE_AGENT_KEY=" + cmdQuote(agentKey);
    return "";
}

// ---------------- 文本工具 ----------------

inline std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);  // 最后一行（可能为空）
    return lines;
}

inline std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += lines[i];
    }
    return out;
}

inline std::string trimCopy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline int indentOf(const std::string& s) {
    int n = 0;
    while (n < static_cast<int>(s.size()) && s[n] == ' ') ++n;
    return n;
}

inline bool endsWithNewline(const std::string& s) { return !s.empty() && s.back() == '\n'; }

// ---------------- 合并原语 ----------------

// 把 miderhive 条目并进 {"mcpServers": {...}} 文本。existing 为空/纯空白表示新建。
// 返回 false 时 whyZh/whyEn 给出可直接展示的原因（绝不部分写入）。
//
// 用 ordered_json：用户配置里的键顺序必须原样保留——默认 json 是 std::map（按字母序），
// 合并会把整份文件重排，升级 diff 满屏噪声。
inline bool mergeMcpServersJson(const std::string& existing, const std::string& snippet,
                                std::string& out, std::string& whyZh, std::string& whyEn) {
    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    if (!trimCopy(existing).empty()) {
        nlohmann::ordered_json doc;
        try {
            doc = nlohmann::ordered_json::parse(existing);
        } catch (const nlohmann::json::exception& e) {
            whyZh = "现有配置不是合法 JSON（" + std::string(e.what()) +
                    "），为避免弄坏它，我没有写入。请手工把片段粘进去。";
            whyEn = "The existing config is not valid JSON; nothing was written to avoid corrupting "
                    "it. Please paste the snippet in by hand.";
            return false;
        }
        if (!doc.is_object()) {
            whyZh = "现有配置不是 JSON 对象，我没有写入。";
            whyEn = "The existing config is not a JSON object; nothing was written.";
            return false;
        }
        root = std::move(doc);
    }
    if (root.contains("mcpServers") && !root["mcpServers"].is_object()) {
        whyZh = "现有配置的 mcpServers 不是对象，我没有写入。";
        whyEn = "The existing config's mcpServers is not an object; nothing was written.";
        return false;
    }
    nlohmann::ordered_json server = nlohmann::ordered_json::object();
    try {
        const nlohmann::ordered_json parsed = nlohmann::ordered_json::parse(snippet);
        if (parsed.is_object() && parsed.contains("mcpServers") &&
            parsed["mcpServers"].contains("miderhive")) {
            server = parsed["mcpServers"]["miderhive"];
        }
    } catch (const nlohmann::json::exception&) {
        // snippet 由本模块生成，正常不会走到这里；兜底成空对象并如实失败
        whyZh = "生成的片段无法解析，我没有写入。";
        whyEn = "The generated snippet could not be parsed; nothing was written.";
        return false;
    }
    root["mcpServers"]["miderhive"] = server;
    out = root.dump(2) + "\n";
    return true;
}

// 顶层 YAML 列表项（以 "- " 开头的块）替换/追加。needle 用于定位已存在的项（DSH 的
// cordis.patch.yml 就是这种形状：一个顶层补丁数组）。返回 false 表示形状无法安全合并。
inline bool upsertYamlListItem(std::string& text, const std::string& needle,
                               const std::string& block, std::string& whyZh, std::string& whyEn) {
    const std::vector<std::string> blockLines = splitLines(block);
    std::vector<std::string> lines = splitLines(text);

    // 找以 "- " 开头、且（自身或其后若干行内）含 needle 的顶层列表项
    int start = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (lines[i].rfind("- ", 0) != 0) continue;
        for (int j = i; j < static_cast<int>(lines.size()) && j <= i + 8; ++j) {
            if (j > i && lines[j].rfind("- ", 0) == 0) break;  // 已进入下一个列表项
            if (lines[j].find(needle) != std::string::npos) { start = i; break; }
        }
        if (start >= 0) break;
    }

    if (start >= 0) {
        int end = static_cast<int>(lines.size());
        for (int i = start + 1; i < static_cast<int>(lines.size()); ++i) {
            if (lines[i].rfind("- ", 0) == 0) { end = i; break; }
        }
        std::vector<std::string> out(lines.begin(), lines.begin() + start);
        out.insert(out.end(), blockLines.begin(), blockLines.end());
        out.insert(out.end(), lines.begin() + end, lines.end());
        text = joinLines(out);
        if (!endsWithNewline(text)) text += "\n";
        return true;
    }

    // 不存在：追加一条。顶层裸 "[]" 必须摘掉，否则追加后 YAML 非法。
    std::vector<std::string> out;
    for (const auto& l : lines) {
        if (trimCopy(l) == "[]") continue;
        out.push_back(l);
    }
    while (!out.empty() && trimCopy(out.back()).empty()) out.pop_back();
    out.insert(out.end(), blockLines.begin(), blockLines.end());
    text = joinLines(out);
    if (!endsWithNewline(text)) text += "\n";
    (void)whyZh;
    (void)whyEn;
    return true;
}

// 块标量开 opener：形如 "key: |" / "key: >-" —— 其内容行会以更深的缩进伪装成
// 键与节结构，让基于缩进的行级边界计算失真。
inline bool isBlockScalarOpener(const std::string& trimmed) {
    const size_t colon = trimmed.find(':');
    if (colon == std::string::npos) return false;
    const std::string v = trimCopy(trimmed.substr(colon + 1));
    if (v == "|" || v == ">") return true;
    if (v.size() == 2 && (v[0] == '|' || v[0] == '>') && (v[1] == '-' || v[1] == '+')) return true;
    return false;
}

// 把 "<childKey>:" 条目插入/替换到顶层 "<parentKey>:" 节下（Hermes 的 mcp_servers）。
// block 需包含完整的父键行（即 generateConfig("hermes") 的原样输出）。
//
// 形状安全边界（**全文件**扫描，理由见下）：
//   * 制表符缩进——本模块的边界计算只数空格，tab 行会被误当成顶格键；
//   * 块标量 opener——其内容行会伪装成节结构。
// 这两种形状下"节边界"本身就不可信，所以不做自作聪明的局部扫描，整体拒绝并让
// 用户手工粘贴。（锚点/别名/行内 `key: &a` 不影响按缩进的插入，不拒绝。）
inline bool upsertYamlMapEntry(std::string& text, const std::string& parentKey,
                               const std::string& childKey, const std::string& block,
                               std::string& whyZh, std::string& whyEn) {
    const std::vector<std::string> blockLines = splitLines(block);
    if (blockLines.empty() || trimCopy(blockLines[0]) != parentKey) {
        whyZh = "内部错误：合并块缺少父键行。";
        whyEn = "internal error: merge block is missing the parent-key line";
        return false;
    }

    std::vector<std::string> lines = splitLines(text);

    // 1) 顶层父键（允许行尾注释）。前缀匹配后必须确认剩余部分为空、注释，或**单独的
    //    锚点/标签**（"mcp_servers: &base"——锚点作用于下面的整块映射，块内容照常合并，
    //    fuzz 用例锁定该语义）。其余内联值拒绝：{} / [..] 是 flow 值，下面再挂缩进块是
    //    结构性非法 YAML；普通标量（"mcp_servers: foo"）与缩进块语义冲突；带值的锚点
    //    （&a {} / &a v）同理。命中即按"形状没把握就拒绝"契约给出可执行的下一步。
    int parent = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string& l = lines[i];
        if (indentOf(l) != 0) continue;
        if (l.compare(0, parentKey.size(), parentKey) != 0) continue;
        const std::string tail = trimCopy(l.substr(parentKey.size()));
        if (!tail.empty() && tail[0] != '#') {
            const bool anchorish = tail[0] == '&' || tail[0] == '!';
            const size_t sp = tail.find_first_of(" \t");
            const bool justAnchor = anchorish && sp == std::string::npos;
            if (!justAnchor) {
                whyZh = "该 YAML 的 " + parentKey +
                        " 行带有内联值（如 {}），形状无法安全合并，请手工粘贴。";
                whyEn = "The " + parentKey +
                        " line in this YAML carries an inline value (e.g. {}); I cannot merge it "
                        "safely — please paste manually.";
                return false;
            }
        }
        parent = i;
        break;
    }

    if (parent < 0) {
        // 父键不存在：整段追加为新的顶层小节
        std::string t = text;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        if (!t.empty()) t += "\n\n";
        t += block;
        if (!endsWithNewline(t)) t += "\n";
        text = t;
        return true;
    }

    // 2) 父节结束行：父键之后第一个"顶格且非注释"的行。
    // 顶格注释按 YAML 语义属于本节（用户常把注释写在节尾），不能当边界——
    // 否则插入点会跑到注释之前，注释的视觉归属被改变。
    const auto isBoundary = [](const std::string& l) {
        if (trimCopy(l).empty()) return false;
        if (l[0] == '#') return false;
        return indentOf(l) == 0;
    };
    int end = static_cast<int>(lines.size());
    for (int i = parent + 1; i < static_cast<int>(lines.size()); ++i) {
        if (isBoundary(lines[i])) { end = i; break; }
    }

    // 3) 节内残留检查：制表符/块标量已在 writeConfigFile 里做全文件扫描；
    //    这里只挡"缩进为 1"——这种行在本模块的兄弟/内容判定里两边都不属于。
    for (int i = parent + 1; i < end; ++i) {
        if (trimCopy(lines[i]).empty()) continue;
        if (indentOf(lines[i]) == 1) {
            whyZh = "该 YAML 缩进不是 2 空格的倍数，形状无法安全合并，请手工粘贴。";
            whyEn = "This YAML is not indented in multiples of two spaces; I cannot merge it safely — "
                    "please paste manually.";
            return false;
        }
    }

    // 4) 定位/替换已有子键，或插到父节末尾
    const std::string childMarker = "  " + childKey + ":";
    int cs = -1;
    for (int i = parent + 1; i < end; ++i) {
        if (lines[i].compare(0, childMarker.size(), childMarker) == 0) { cs = i; break; }
    }

    std::vector<std::string> out;
    if (cs >= 0) {
        // 子块结束：下一个缩进 <= 2 的非空行（同级子键或父节结束）
        int ce = end;
        for (int i = cs + 1; i < end; ++i) {
            if (trimCopy(lines[i]).empty()) continue;
            if (indentOf(lines[i]) <= 2) { ce = i; break; }
        }
        out.insert(out.end(), lines.begin(), lines.begin() + cs);
        out.insert(out.end(), blockLines.begin() + 1, blockLines.end());
        out.insert(out.end(), lines.begin() + ce, lines.end());
    } else {
        // 父节内最后一个"非空且非顶格注释"的行之后（让节尾注释留在原处）
        int insertAt = parent + 1;
        for (int i = parent + 1; i < end; ++i) {
            const std::string& l = lines[i];
            if (trimCopy(l).empty()) continue;
            if (l[0] == '#' && indentOf(l) == 0) continue;
            insertAt = i + 1;
        }
        out.insert(out.end(), lines.begin(), lines.begin() + insertAt);
        out.insert(out.end(), blockLines.begin() + 1, blockLines.end());
        out.insert(out.end(), lines.begin() + insertAt, lines.end());
    }
    text = joinLines(out);
    if (!endsWithNewline(text)) text += "\n";
    return true;
}

// ---------------- 文件读写 ----------------

inline bool readFileUtf8(const std::string& path, std::string& out) {
    std::ifstream in(fsPath(path), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 把文本统一成 CRLF（先归一为 LF 再去加，避免出现 \r\r\n）
inline std::string toCrlf(const std::string& text) {
    std::string lf;
    lf.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        lf += text[i];
    }
    std::string out;
    out.reserve(lf.size() + lf.size() / 16);
    for (char c : lf) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// 原子写：先写 .tmp 再改名。rename 在 MSVC 上按 POSIX 语义覆盖目标，
// 因此不需要"先删目标"——那一步会在删与改名之间留出"文件不存在"的窗口
// （进程此刻被杀就只剩 .miderhive.bak）。
inline bool writeFileUtf8(const std::string& path, const std::string& text) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p = fsPath(path);
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
    const std::string tmp = path + ".miderhive.tmp";
    {
        std::ofstream o(fsPath(tmp), std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << text;
        o.flush();
        if (!o.good()) {
            o.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(fsPath(tmp), fsPath(path), ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// 覆盖前留一份 .miderhive.bak（单份、可预测，不堆目录）。
// 返回 false = 备份失败：调用方必须中止写入。"原文件已备份"是用户敢让工具动手
// 改配置的前提——此前 remove/copy_file 的错误码全部被吞、成功文案无条件宣称
// 已备份，备份失败时用户被无备份覆盖却被告知有安全网。bak 为只读/被锁/目录时
// 可真实发生。
inline bool backupFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fsPath(path))) return true;  // 没有原文件就不存在"覆盖丢内容"
    const std::string bak = path + ".miderhive.bak";
    fs::remove(fsPath(bak), ec);
    ec.clear();
    fs::copy_file(fsPath(path), fsPath(bak), fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// ---------------- 写入 ----------------

struct WriteResult {
    bool ok = false;
    std::string detailZh;  // 给用户看的结果说明（中文）
    std::string detailEn;  // 同上（英文）；GUI 按 i18n::trs 二选一
};

inline WriteResult makeResult(bool ok, std::string zh, std::string en) {
    WriteResult r;
    r.ok = ok;
    r.detailZh = std::move(zh);
    r.detailEn = std::move(en);
    return r;
}

// 把文案里第一个 "%1" 换成实际值（与 GUI 的 i18n::trs("%1").arg() 占位约定一致）
inline std::string replaceFirst(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

// 把某工具的配置写到**显式给定**的路径（格式由 id 决定）。GUI / CLI 共用。
inline WriteResult writeConfigFile(const std::string& id, const std::string& path,
                                   const std::string& command, const std::string& agentName,
                                   const std::string& agentKey) {
    const Format fmt = formatOf(id);
    if (path.empty() || !hasWritableConfig(id)) {
        return makeResult(false, "该工具没有可自动写入的配置文件。",
                          "This tool has no config file that can be written automatically.");
    }
    const std::string snippet = generateConfig(id, command, agentName, agentKey);

    std::string existing;
    const bool hasFile = std::filesystem::exists(fsPath(path));
    if (hasFile && !readFileUtf8(path, existing)) {
        return makeResult(false,
                          "配置文件存在但读不出来（权限不足？）：" + path,
                          "The config file exists but cannot be read (permissions?): " + path);
    }
    // 原文件的换行风格必须**在这里**捕获：下面的 YAML 合并是就地修改，
    // splitLines/joinLines 会把 \r 剥掉，到写之前再判断就只剩 LF 了（实测踩过）
    const bool wasCrlf = existing.find("\r\n") != std::string::npos;
    // YAML 两条路径的形状安全（全文件扫描，理由见 upsertYamlMapEntry 注释）
    if (fmt == Format::DshPatchYaml || fmt == Format::HermesYaml) {
        {
            std::vector<std::string> lines = splitLines(existing);
            for (const auto& l : lines) {
                if (!l.empty() && l[0] == '\t') {
                    return makeResult(false,
                                      "该 YAML 用了制表符缩进，形状无法安全合并，请手工粘贴。",
                                      "This YAML is indented with tabs; I cannot merge it safely — "
                                      "please paste manually.");
                }
                if (isBlockScalarOpener(trimCopy(l))) {
                    return makeResult(false,
                                      "该 YAML 含块标量（key: | 或 >），形状无法安全合并，请手工粘贴。",
                                      "This YAML contains a block scalar (key: | or >); I cannot "
                                      "merge it safely — please paste manually.");
                }
            }
        }
    }

    std::string out;
    bool ok = false;

    if (fmt == Format::JsonMcpServers) {
        std::string whyZh, whyEn;
        ok = mergeMcpServersJson(hasFile ? existing : "", snippet, out, whyZh, whyEn);
        if (!ok) return makeResult(false, whyZh, whyEn);
    } else if (fmt == Format::TomlCodex) {
        // 替换整张表：从 "[mcp_servers.miderhive]" 行到下一个行首 '[' 之前
        const std::string marker = "[mcp_servers.miderhive]";
        std::vector<std::string> lines = splitLines(existing);
        int at = -1;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (lines[i].compare(0, marker.size(), marker) == 0) { at = i; break; }
        }
        // 追加前拒绝"本工具无法安全合并"的既有形状：[mcp_servers] 表内的 inline
        // table / 点号键已经占用了 miderhive 这个键，此时追加整张
        // [mcp_servers.miderhive] 表会构成 TOML 重定义冲突——整份 config.toml
        // 拒载（实测 tomllib 报 Cannot declare twice）。形状没把握就不写。
        if (at < 0 && hasFile) {
            for (const auto& l : lines) {
                const std::string t = trimCopy(l);
                if (t == "[mcp_servers]" ||
                    t.find("mcp_servers.miderhive") != std::string::npos) {
                    return makeResult(
                        false,
                        "现有 config.toml 用了本工具无法安全合并的写法（[mcp_servers] 内联表"
                        "或 mcp_servers.miderhive 点号键），请手工粘贴，避免整份文件解析失败。",
                        "The existing config.toml uses a shape I cannot merge safely "
                        "(an inline table under [mcp_servers] or a dotted "
                        "mcp_servers.miderhive key) — please paste manually to avoid "
                        "breaking the whole file.");
                }
            }
        }
        if (at >= 0) {
            int end = static_cast<int>(lines.size());
            for (int i = at + 1; i < static_cast<int>(lines.size()); ++i) {
                if (!trimCopy(lines[i]).empty() && lines[i][0] == '[') { end = i; break; }
            }
            std::vector<std::string> outLines(lines.begin(), lines.begin() + at);
            for (const auto& l : splitLines(snippet)) outLines.push_back(l);
            outLines.insert(outLines.end(), lines.begin() + end, lines.end());
            out = joinLines(outLines);
            if (!endsWithNewline(out)) out += "\n";
        } else {
            out = existing;
            if (!out.empty() && !endsWithNewline(out)) out += "\n";
            if (!trimCopy(out).empty()) out += "\n";
            out += snippet;
        }
        ok = true;
    } else if (fmt == Format::DshPatchYaml) {
        std::string whyZh, whyEn;
        ok = upsertYamlListItem(existing, "mcp-miderhive", snippet, whyZh, whyEn);
        if (!ok) return makeResult(false, whyZh, whyEn);
        out = existing;
    } else if (fmt == Format::HermesYaml) {
        std::string whyZh, whyEn;
        ok = upsertYamlMapEntry(existing, "mcp_servers:", "miderhive", snippet, whyZh, whyEn);
        if (!ok) return makeResult(false, whyZh, whyEn);
        out = existing;
    } else {
        return makeResult(false, "该工具没有可自动写入的配置文件。",
                          "This tool has no config file that can be written automatically.");
    }

    // 备份失败必须中止：不能在安全网缺失时覆盖用户配置（成功文案宣称的
    // ".miderhive.bak" 必须是真的）
    if (hasFile && !backupFile(path)) {
        return makeResult(false,
                          "备份原文件失败（.miderhive.bak 无法写入），为避免丢失原内容没有写入：" +
                              path,
                          "Backing up the original failed (.miderhive.bak is not writable); "
                          "nothing was written, to avoid losing it: " +
                              path);
    }
    // 原文件是 CRLF 就写回 CRLF：整份被改成 LF 会产生满屏 diff（配置常进版本库）
    if (wasCrlf) out = toCrlf(out);
    if (!writeFileUtf8(path, out)) {
        return makeResult(false,
                          "写入失败：" + path + "（可能有权限或文件被占用）。原文件未被改动，副本在 .miderhive.bak。",
                          "Write failed: " + path +
                              " (permissions, or the file is locked). The original is untouched; "
                              "a copy is at .miderhive.bak.");
    }
    const char* zh = hasFile ? "已写入 %1（原文件备份为 .miderhive.bak）。" : "已创建 %1。";
    const char* en = hasFile ? "Written to %1 (original backed up as .miderhive.bak)."
                             : "Created %1.";
    // 路径在返回前就地替换：调用方拿到的就是完整句子，不用再关心占位符
    return makeResult(true, replaceFirst(zh, "%1", path), replaceFirst(en, "%1", path));
}

// 写到 rootDir 下该工具的约定相对路径（CLI / 测试 / MIDERHIVE_CONNECT_ROOT 重定向共用）。
inline WriteResult writeConfigUnderRoot(const std::string& rootDir, const std::string& id,
                                        const std::string& command, const std::string& agentName,
                                        const std::string& agentKey) {
    const std::string rel = relativeConfigPath(id);
    if (rel.empty() || rootDir.empty()) {
        return makeResult(false, "该工具没有可自动写入的配置文件。",
                          "This tool has no config file that can be written automatically.");
    }
    std::string root = rootDir;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return writeConfigFile(id, root + "/" + rel, command, agentName, agentKey);
}

// Claude Code 的项目级落点：<项目根>/.mcp.json（Claude Code 官方约定的项目作用域 MCP 配置，
// 首次需在该目录运行 `claude` 批准一次）。工作台不知道用户的项目在哪，由向导让用户挑一次。
inline std::string projectConfigPath(const std::string& projectDir) {
    if (projectDir.empty()) return "";
    std::string dir = projectDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir + "/.mcp.json";
}

inline WriteResult writeProjectConfig(const std::string& projectDir, const std::string& command,
                                      const std::string& agentName, const std::string& agentKey) {
    return writeConfigFile("claude-code", projectConfigPath(projectDir), command, agentName,
                           agentKey);
}

}  // namespace ah::integrations
