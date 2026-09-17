#pragma once
// 接入引导的唯一事实来源（GUI 侧）：每个 AI 工具"装在哪、配置写哪个文件、文案怎么说"。
//
// 行为（配置格式、片段生成、文件合并/写入）已经下沉到 core/integrations.hpp——
// agent-cli、脚本、无 Qt 单元测试共用同一份实现；本头文件只负责：
//   1) 展示元数据（中英文名、安装指引、注意事项）；
//   2) 本机安装检测与真实路径解析（需要 QDir/QStandardPaths）。
// 工作台首次接入引导（WelcomeDialog）、接入向导（ConnectDialog）与自动化测试共用这一套，
// 保证"界面生成什么、文档就写什么、脚本断言什么"，三处永不漂移。
//
// 检测策略（误报可接受、漏报不可接受——宁多给一次配置指引）：
//   1) PATH 上能找到可执行名；
//   2) 或命中常见安装路径（npm 全局目录、%LOCALAPPDATA%、用户目录下的配置目录）。
//
// 配置落点按各工具官方约定（2026-09 核对）：
//   Claude Desktop  %APPDATA%\Claude\claude_desktop_config.json   mcpServers (JSON)
//   Claude Code     项目根 .mcp.json（或 claude mcp add）
//   Codex / ChatGPT %USERPROFILE%\.codex\config.toml              [mcp_servers.*] (TOML)
//   Factory Droid   %USERPROFILE%\.factory\mcp.json               mcpServers (JSON)
//   DSH             %USERPROFILE%\.dsh\profiles\<profile>\cordis.patch.yml
//                                                                 - insert: (YAML)
//   Hermes          %LOCALAPPDATA%\hermes\config.yaml             mcp_servers: (YAML)
//   Cursor          %USERPROFILE%\.cursor\mcp.json                mcpServers (JSON)
//   ZCode           环境变量（agent-cli / platformd 内置支持）
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/integrations.hpp"

#include "i18n.h"

namespace ui::integrations {

// 行为枚举与 core 同源（welcome_dialog 用它判断"有没有可写的配置文件"）
using Format = ah::integrations::Format;

struct Tool {
    QString id;               // claude-code | codex | droid | dsh | hermes | zcode | cursor | copilot
    QString nameZh, nameEn;   // 展示名
    QString exeName;          // PATH 探测名（可空：纯配置文件型工具）
    QString configPathZh;     // 默认配置文件路径（含环境变量，展示用）
    QString configPathEn;
    QString installZh, installEn;  // 未检测到时的安装指引
    Format format = Format::InstructionsMd;
    QString defaultAgentName;  // 一键接入时预填的身份名（保留名不可用）
    QString noteZh, noteEn;    // 该工具特有的坑（空串 = 不走 MCP）
};

// 八工具注册表。顺序 = 界面展示顺序（把用户最常用的放前面）。
// format / defaultAgentName 以 core 注册表为准（行为元数据单一来源），此处不重复写。
inline QVector<Tool> tools() {
    QVector<Tool> out = {
        {"claude-code", "Claude", "Claude", "claude",
         "项目根 .mcp.json（推荐）\n%APPDATA%\\Claude\\claude_desktop_config.json（Claude Desktop）",
         "`.mcp.json` in your project root (recommended)\n"
         "%APPDATA%\\Claude\\claude_desktop_config.json (Claude Desktop)",
         "未检测到 Claude。Claude Desktop 可从 https://claude.ai/download 安装；"
         "Claude Code 用 npm install -g @anthropic-ai/claude-code，装好后点「重新检测」。",
         "Claude not detected. Install Claude Desktop from https://claude.ai/download, or "
         "Claude Code via `npm install -g @anthropic-ai/claude-code`, then click Re-check.",
         Format::JsonMcpServers, "claude",
         "Claude Code 推荐：让向导把 JSON 写进项目根的 .mcp.json（选一次项目文件夹），"
         "再运行一次 claude 批准即可。命令行方式请用 cmd.exe——PowerShell 调 npm 的 "
         "claude.ps1 时会吞掉 -- 之后的命令。Claude Desktop 则写它自己的 JSON 配置。"
         "改完都要完全退出并重启。",
         "Claude Code (recommended): let the wizard write `.mcp.json` into your project root "
         "(pick the folder once), then run `claude` once to approve it. For the CLI form use "
         "cmd.exe — in PowerShell the npm `claude.ps1` shim swallows everything after `--`. "
         "Claude Desktop uses its own JSON config. Fully restart afterwards."},

        {"codex", "ChatGPT / Codex", "ChatGPT / Codex", "codex",
         "%USERPROFILE%\\.codex\\config.toml",
         "%USERPROFILE%\\.codex\\config.toml",
         "未检测到 Codex CLI。可用 npm install -g @openai/codex 安装后点「重新检测」。"
         "ChatGPT 桌面端请在 设置 → 连接器 里添加自定义 MCP 服务器。",
         "Codex CLI not detected. Install with `npm install -g @openai/codex`, then click "
         "Re-check. In the ChatGPT desktop app add a custom MCP server under Settings → Connectors.",
         Format::TomlCodex, "codex",
         "写入的是 [mcp_servers.miderhive] 表；Codex 完全重启后生效。",
         "Writes the [mcp_servers.miderhive] table; takes effect after Codex restarts."},

        {"droid", "Factory Droid", "Factory Droid", "droid",
         "%USERPROFILE%\\.factory\\mcp.json",
         "%USERPROFILE%\\.factory\\mcp.json",
         "未检测到 Factory Droid。装好后点「重新检测」；也可用 droid mcp add 手工接入。",
         "Factory Droid not detected. Install it and click Re-check; you can also wire it up "
         "manually with `droid mcp add`.",
         Format::JsonMcpServers, "droid",
         "Droid 会自动重载 mcp.json；也可在 Droid 里输入 /mcp 查看连接状态。",
         "Droid reloads mcp.json automatically; type /mcp inside Droid to see connection status."},

        {"dsh", "DSH", "DSH", "dsh",
         "%USERPROFILE%\\.dsh\\profiles\\<profile>\\cordis.patch.yml",
         "%USERPROFILE%\\.dsh\\profiles\\<profile>\\cordis.patch.yml",
         "未检测到 DSH（DeepSeek Harness）。装好后点「重新检测」。",
         "DSH (DeepSeek Harness) not detected. Install it and click Re-check.",
         Format::DshPatchYaml, "dsh",
         "写入的是 profile 的 cordis.patch.yml 补丁层（插入一条 dsh-mcp-client 记录），"
         "不修改 bundles 自带的 cordis.yml。工具会以 mcp__miderhive__* 命名出现。"
         "注意 DSH 会清洗子进程环境，密钥必须写在本配置的 env 里才有效。",
         "Writes an insert row into the profile's cordis.patch.yml (a dsh-mcp-client entry); the "
         "bundle-provided cordis.yml is left untouched. Tools appear as mcp__miderhive__*. DSH "
         "scrubs the child environment, so the key must live in this config's env block."},

        {"hermes", "Hermes", "Hermes", "hermes",
         "%LOCALAPPDATA%\\hermes\\config.yaml",
         "%LOCALAPPDATA%\\hermes\\config.yaml",
         "未检测到 Hermes。装好后点「重新检测」。",
         "Hermes not detected. Install it and click Re-check.",
         Format::HermesYaml, "hermes",
         "写入 mcp_servers: 段下的 miderhive 条目（已有该段时合并进去，保留其他条目）；"
         "Hermes 重启后自动发现工具。",
         "Writes the miderhive entry under the mcp_servers: key (merged into an existing section "
         "without touching other entries); Hermes discovers the tools after a restart."},

        {"zcode", "ZCode", "ZCode", "zcode",
         "环境变量：MIDERHIVE_AGENT_NAME / MIDERHIVE_AGENT_KEY / MIDERHIVE_PORT",
         "Environment: MIDERHIVE_AGENT_NAME / MIDERHIVE_AGENT_KEY / MIDERHIVE_PORT",
         "ZCode 通过环境变量接入（agent-cli / platformd 已内置支持），无需配置文件。",
         "ZCode connects through environment variables (built into agent-cli / platformd); "
         "no config file needed.",
         Format::EnvVars, "zcode-agent",
         "zcode 是平台管理者保留身份，工具侧请用 zcode-agent 之类的名字，避免注册被拒。",
         "zcode is the reserved manager identity — register the tool as zcode-agent (or similar) "
         "to avoid a rejected registration."},

        {"cursor", "Cursor", "Cursor", "cursor",
         "%USERPROFILE%\\.cursor\\mcp.json",
         "%USERPROFILE%\\.cursor\\mcp.json",
         "未检测到 Cursor。可从 https://cursor.com/download 安装后点「重新检测」。",
         "Cursor not detected. Install from https://cursor.com/download, then click Re-check.",
         Format::JsonMcpServers, "cursor",
         "Cursor 采用 mcpServers 结构（含 args 数组）；改动后需重启 Cursor。",
         "Cursor uses the mcpServers shape (with an args array); restart Cursor after editing."},

        {"copilot", "GitHub Copilot", "GitHub Copilot", "code",
         ".github/copilot-instructions.md（仓库内）",
         ".github/copilot-instructions.md (inside your repo)",
         "Copilot 不支持 MCP：把向导生成的指令块粘进仓库的 "
         ".github/copilot-instructions.md 即可。",
         "Copilot has no MCP support: paste the generated instruction block into your repo's "
         ".github/copilot-instructions.md.",
         Format::InstructionsMd, "copilot",
         "指令块让 Copilot 通过 HTTP API 调用本工作台。",
         "The instruction block tells Copilot to call this workbench over its HTTP API."},
    };
    // 行为元数据以 core 注册表为准：GUI 这里只保留展示信息
    for (Tool& t : out) {
        if (const auto* m = ah::integrations::toolById(t.id.toStdString())) {
            t.format = m->format;
            t.defaultAgentName = QString::fromUtf8(m->defaultAgentName);
        }
    }
    return out;
}

inline const Tool* toolById(const QString& id) {
    static const QVector<Tool> kTools = tools();
    for (const auto& t : kTools)
        if (t.id == id) return &t;
    return nullptr;
}

// 常见安装路径（含 npm 全局与用户目录配置目录）。存在目录本身也算已安装的信号。
inline QStringList candidatePaths(const QString& id) {
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    const QString appdata = qEnvironmentVariable("APPDATA");
    const QString home = QDir::homePath();
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
    return {};
}

inline bool installed(const QString& id) {
    if (const Tool* t = toolById(id)) {
        if (!t->exeName.isEmpty() && !QStandardPaths::findExecutable(t->exeName).isEmpty())
            return true;
    }
    for (const QString& p : candidatePaths(id))
        if (QFileInfo::exists(p)) return true;
    return false;
}

// ---------------- 配置落点：解析成这台机器上的真实绝对路径 ----------------

// DSH 的 profile 名不固定（dsh 会按前端建 profile）。取第一个真实存在的
// <profile>/cordis.patch.yml，优先 web（桌面/Web 前端的默认 profile）。
inline QString dshPatchPath() {
    const QString base = QDir::homePath() + "/.dsh/profiles";
    const QString preferred = base + "/web/cordis.patch.yml";
    if (QFileInfo::exists(preferred)) return preferred;
    QDir dir(base);
    if (dir.exists()) {
        const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& s : subs) {
            const QString p = base + "/" + s + "/cordis.patch.yml";
            if (QFileInfo::exists(p)) return p;
        }
    }
    return preferred;  // 尚不存在：返回规范位置，写入时再创建
}

// 工具的"可安全写入的配置文件"绝对路径；不存在这样的文件时返回空串。
// 只返回我们确实能正确合并的位置，绝不返回一个"写了也不会被读取"的路径。
//   * Claude：只有装了 Claude Desktop 才返回它的 JSON；Claude Code 的 MCP 配置
//     走项目根 .mcp.json（applyProjectConfig）或 `claude mcp add`——顶层 mcpServers
//     写进 ~/.claude.json 不会被读取，因此未装桌面端时返回空串。
//   * ZCode / Copilot：没有配置文件。
inline QString configPath(const QString& id) {
    // 测试/便携重定向：换根不换相对路径（gui_selftest 由此在临时目录里验证写入器）
    const QString overrideRoot = qEnvironmentVariable("MIDERHIVE_CONNECT_ROOT");
    const std::string ids = id.toStdString();
    if (!overrideRoot.isEmpty()) {
        const std::string rel = ah::integrations::relativeConfigPath(ids);
        return rel.empty() ? QString() : overrideRoot + "/" + QString::fromStdString(rel);
    }
    if (id == "claude-code") {
        const QString appdata = qEnvironmentVariable("APPDATA");
        if (QFileInfo::exists(appdata + "/Claude"))
            return appdata + "/Claude/claude_desktop_config.json";
        return QString();
    }
    if (id == "dsh") return dshPatchPath();
    const std::string rel = ah::integrations::relativeConfigPath(ids);
    if (rel.empty()) return QString();
    if (id == "hermes")
        return qEnvironmentVariable("LOCALAPPDATA") + "/" + QString::fromStdString(rel);
    return QDir::homePath() + "/" + QString::fromStdString(rel);  // codex / droid / cursor
}

inline QString configDir(const QString& id) {
    const QString p = configPath(id);
    return p.isEmpty() ? QString() : QFileInfo(p).absolutePath();
}

// ---------------- 片段生成与写入（全部委托 core）----------------

inline QString generateConfig(const QString& id, const QString& command, const QString& agentName,
                              const QString& agentKey) {
    return QString::fromStdString(ah::integrations::generateConfig(
        id.toStdString(), command.toStdString(), agentName.toStdString(), agentKey.toStdString()));
}

inline QString cliCommand(const QString& id, const QString& command, const QString& agentName,
                          const QString& agentKey) {
    return QString::fromStdString(ah::integrations::cliCommand(
        id.toStdString(), command.toStdString(), agentName.toStdString(), agentKey.toStdString()));
}

struct ApplyResult {
    bool ok = false;
    QString detailZh;
    QString detailEn;
};

inline ApplyResult toResult(const ah::integrations::WriteResult& r) {
    ApplyResult out;
    out.ok = r.ok;
    out.detailZh = QString::fromStdString(r.detailZh);
    out.detailEn = QString::fromStdString(r.detailEn);
    return out;
}

// 把配置写进该工具在这台机器上的真实配置文件。失败时给出可执行的下一步，绝不静默。
inline ApplyResult applyConfig(const QString& id, const QString& command, const QString& agentName,
                               const QString& agentKey) {
    return toResult(ah::integrations::writeConfigFile(
        id.toStdString(), configPath(id).toStdString(), command.toStdString(),
        agentName.toStdString(), agentKey.toStdString()));
}

// Claude Code 的项目级写入：<项目根>/.mcp.json（向导让用户挑一次项目文件夹）。
inline ApplyResult applyProjectConfig(const QString& projectDir, const QString& command,
                                      const QString& agentName, const QString& agentKey) {
    return toResult(ah::integrations::writeProjectConfig(
        projectDir.toStdString(), command.toStdString(), agentName.toStdString(),
        agentKey.toStdString()));
}

}  // namespace ui::integrations
