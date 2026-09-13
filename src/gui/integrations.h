#pragma once
// 接入引导：三大 AI 编码工具（Claude Code / Cursor / Codex CLI）的本地安装检测
// 与 MCP 配置片段生成。纯函数、无对话框依赖——工作台首次接入引导（WelcomeDialog）
// 与 scripts/ 下的自动化测试共用同一套规则，保证"界面生成什么，文档就写什么"。
//
// 检测策略（误报可接受、漏报不可接受——宁多给一次配置指引）：
//   1) PATH 上能找到可执行名（claude / cursor / codex）；
//   2) 或命中常见安装路径（npm 全局目录、%LOCALAPPDATA%、用户目录下的配置目录）。
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ui::integrations {

struct Tool {
    QString id;               // claude-code | cursor | codex
    QString nameZh, nameEn;   // 展示名
    QString exeName;          // PATH 探测名
    QString configPathZh;     // 默认配置文件路径（含环境变量，展示用）
    QString configPathEn;
    QString installZh, installEn;  // 未检测到时的安装指引
};

inline QVector<Tool> tools() {
    return {
        {"claude-code", "Claude Code", "Claude Code", "claude",
         "%APPDATA%\\Claude\\claude_desktop_config.json（Claude Desktop）\n"
         "%USERPROFILE%\\.claude.json 或 claude mcp add（Claude Code）",
         "%APPDATA%\\Claude\\claude_desktop_config.json (Claude Desktop)\n"
         "%USERPROFILE%\\.claude.json or `claude mcp add` (Claude Code)",
         "未检测到 Claude Code。可从 https://claude.ai/download 安装，"
         "或用 npm install -g @anthropic-ai/claude-code；装好后点「重新检测」。",
         "Claude Code not detected. Install from https://claude.ai/download or "
         "`npm install -g @anthropic-ai/claude-code`, then click Re-check."},
        {"cursor", "Cursor", "Cursor", "cursor",
         "%USERPROFILE%\\.cursor\\mcp.json",
         "%USERPROFILE%\\.cursor\\mcp.json",
         "未检测到 Cursor。可从 https://cursor.com/download 安装后点「重新检测」。",
         "Cursor not detected. Install from https://cursor.com/download, then click Re-check."},
        {"codex", "Codex CLI", "Codex CLI", "codex",
         "%USERPROFILE%\\.codex\\config.toml",
         "%USERPROFILE%\\.codex\\config.toml",
         "未检测到 Codex CLI。可用 npm install -g @openai/codex 安装后点「重新检测」。",
         "Codex CLI not detected. Install with `npm install -g @openai/codex`, then "
         "click Re-check."},
    };
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
                appdata + "/npm/claude.cmd",           appdata + "/npm/claude",
                home + "/.claude"};
    if (id == "cursor")
        return {local + "/Programs/cursor/Cursor.exe", local + "/Programs/cursor/cursor.exe",
                home + "/.cursor"};
    if (id == "codex")
        return {appdata + "/npm/codex.cmd", appdata + "/npm/codex", appdata + "/npm/codex.ps1",
                home + "/.codex"};
    return {};
}

inline bool installed(const QString& id) {
    if (const Tool* t = toolById(id)) {
        if (!QStandardPaths::findExecutable(t->exeName).isEmpty()) return true;
    }
    for (const QString& p : candidatePaths(id))
        if (QFileInfo::exists(p)) return true;
    return false;
}

// 生成该工具的 MCP 配置片段。command 为 miderhive-mcp 可执行文件完整路径；
// agentName/agentKey 由引导流程预配的接入身份填入（与 agents.json 中一致）。
// Claude/Cursor 为 JSON（分别对应 claude_desktop_config.json 与 mcp.json 的格式），
// Codex CLI 为 TOML（config.toml 格式）。
inline QString generateConfig(const QString& id, const QString& command, const QString& agentName,
                              const QString& agentKey) {
    if (id == "codex") {
        auto esc = [](QString v) { return QStringLiteral("\"%1\"").arg(v); };
        return QStringLiteral("[mcp_servers.miderhive]\n"
                              "command = %1\n"
                              "env = { MIDERHIVE_AGENT_NAME = %2, MIDERHIVE_AGENT_KEY = %3 }\n")
            .arg(esc(command), esc(agentName), esc(agentKey));
    }
    QJsonObject env{{"MIDERHIVE_AGENT_NAME", agentName}, {"MIDERHIVE_AGENT_KEY", agentKey}};
    QJsonObject server;
    if (id == "cursor") {
        server = {{"command", command}, {"args", QJsonArray{}}, {"env", env}};
    } else {  // claude-code（claude_desktop_config.json / claude mcp add-json 同构）
        server = {{"command", command}, {"env", env}};
    }
    QJsonDocument doc(QJsonObject{{"mcpServers", QJsonObject{{"miderhive", server}}}});
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

}  // namespace ui::integrations
