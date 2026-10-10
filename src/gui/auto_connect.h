#pragma once
// 批量自动接入：对所有"已安装、有可写配置、还没接"的注册表工具一次完成
// 检测 → 签发 → 写入。两个入口共用（接入对话框的「接入全部已检测」按钮与
// 总览页的启动提示条）。
//
// 三条刻意的保守规则（都在结果里如实说明，不静默）：
//   * 已在线的身份跳过——agentProvision 对已存在身份是轮换语义，重签发会
//     立即作废该工具正在使用的密钥；
//   * 配置里已有 miderhive 条目的跳过（哪怕离线）——同理，动它只会坏事；
//   * 无 MCP 配置文件的工具（zcode 保留身份 / copilot 指令块）与需要用户
//     挑项目目录的 Claude Code（未装 Desktop 且无 lastProjectDir 记忆时）
//     不硬来，跳过并给出手工路径。
#include <QSettings>
#include <QString>
#include <QVector>

#include "core/platform.h"
#include "gui_util.h"
#include "i18n.h"
#include "integrations.h"

namespace ui::autoconnect {

struct Outcome {
    QString toolId;
    QString toolName;
    QString agentName;
    QString state;   // "ok" / "skipped" / "failed"
    QString detail;  // 已按当前语言格式化的说明（含下一步）
};

// 配置文件里是否已有 miderhive 条目（文本近似判定：JSON/TOML/YAML 的条目
// 名都含 "miderhive" 子串；误判方向是"多跳过一次"，安全）
inline bool configHasMiderhive(const QString& id) {
    const QString path = ui::integrations::configPath(id);
    if (path.isEmpty()) return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return f.readAll().contains("miderhive");
}

inline QVector<Outcome> connectAllDetected(ah::Platform& platform) {
    QVector<Outcome> out;
    QSet<QString> online;
    {
        std::vector<ah::AgentInfo> agents;
        std::string err;
        if (platform.listAgents(agents, err))
            for (const auto& a : agents)
                if (a.status == "online") online.insert(QString::fromStdString(a.name));
    }
    QSettings settings;
    for (const auto& t : ui::integrations::tools()) {
        Outcome o;
        o.toolId = t.id;
        o.toolName = i18n::trs(t.nameZh, t.nameEn);
        o.agentName = t.defaultAgentName;

        // 无 MCP 配置文件的工具：批量写不了，指向单独接入
        if (!ah::integrations::hasWritableConfig(t.id.toStdString())) {
            o.state = "skipped";
            o.detail = i18n::trs("不走 MCP 配置文件，请在接入向导里单独生成片段/指令块",
                                 "No MCP config file — generate the snippet/instructions "
                                 "individually in the wizard");
            out.push_back(o);
            continue;
        }
        // 未安装：不进报告（列表会很长且没有动作可给）
        if (!ui::integrations::installed(t.id)) continue;

        // 已在线 / 已接入：跳过，防轮换作废在用密钥
        if (online.contains(o.agentName)) {
            o.state = "skipped";
            o.detail = i18n::trs("已在线——不动它", "already online — left untouched");
            out.push_back(o);
            continue;
        }
        if (configHasMiderhive(t.id)) {
            o.state = "skipped";
            o.detail = i18n::trs("配置里已有 miderhive 条目（当前离线）——不动它，"
                                 "避免作废你已粘贴的密钥",
                                 "config already has a miderhive entry (offline) — left "
                                 "untouched to keep your pasted key valid");
            out.push_back(o);
            continue;
        }

        const QString cfgPath = ui::integrations::configPath(t.id);
        std::string key, err;
        if (!cfgPath.isEmpty()) {
            if (!platform.agentProvision(ah::kManagerName, o.agentName.toStdString(), key, err)) {
                o.state = "failed";
                o.detail = ui::humanError(QString::fromStdString(err));
                out.push_back(o);
                continue;
            }
            const auto r = ui::integrations::applyConfig(
                t.id, ui::integrations::mcpExePath(), o.agentName,
                QString::fromStdString(key));
            o.state = r.ok ? "ok" : "failed";
            o.detail = i18n::trs(r.detailZh, r.detailEn);
        } else if (t.id == QLatin1String("claude-code")) {
            // Claude Code 的项目级落点需要用户挑目录：有上次记忆就直接用
            const QString lastDir = settings.value("ui/lastProjectDir").toString();
            if (lastDir.isEmpty()) {
                o.state = "skipped";
                o.detail = i18n::trs("Claude Code 的 MCP 配置在项目根的 .mcp.json——"
                                     "请在接入向导里挑一次项目文件夹",
                                     "Claude Code's MCP config lives in the project root's "
                                     ".mcp.json — pick the project folder in the wizard once");
                out.push_back(o);
                continue;
            }
            if (!platform.agentProvision(ah::kManagerName, o.agentName.toStdString(), key, err)) {
                o.state = "failed";
                o.detail = ui::humanError(QString::fromStdString(err));
                out.push_back(o);
                continue;
            }
            const auto r = ui::integrations::applyProjectConfig(
                lastDir, ui::integrations::mcpExePath(), o.agentName,
                QString::fromStdString(key));
            o.state = r.ok ? "ok" : "failed";
            o.detail = i18n::trs(r.detailZh, r.detailEn);
        } else {
            o.state = "skipped";
            o.detail = i18n::trs("没有固定配置文件，请在接入向导里单独接入",
                                 "no fixed config file — connect individually in the wizard");
        }
        if (o.state == "ok")
            settings.setValue("ui/onboardingPending/" + o.agentName, t.id);
        out.push_back(o);
    }
    return out;
}

}  // namespace ui::autoconnect
