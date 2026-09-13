// ---- GUI 全链路自测（离屏）：把"需要真人点击"的验证自动化 ----
// 拉起真实 MainWindow（发布代码本身，经由 miderhive_gui 静态库），用定时器扮演用户：
//   ① 首启（ui/welcomeSeen=false）→ 400ms 后自动弹出接入引导 → 填身份名 → 选 Claude Code
//      → 校验：本地检测/预配身份/生成 MCP 配置(JSON)/登记待观察/明文密钥落盘
//   ② 模拟 Agent 心跳上线 → 「接入成功」弹窗 → 欢迎记忆写入「用户记忆→项目档案」
//      → 待观察登记清除（同一身份不会二次弹窗）
//   ③ 制造"明文密钥丢失"（改写 agents.json）→ 总览页黄色健康横幅 → 点「轮换密钥修复」
//      → 确认(Yes) → 新钥写回缓存 → 旧密钥立即 401、新密钥 200 → 横幅告警消失
//   ④ 设置 → Agent 管理 → 「重新打开接入引导」→ 引导再次弹出并正常关闭
// 另含纯逻辑断言（A 组）：MCP 配置生成（JSON/TOML）、报错翻译 humanError、
// 启动自检 preflight 的端口探测。
// 运行环境由 scripts/verify_gui_selftest.py 提供：隔离的 MIDERHIVE_HOME/PORT、
// QT_QPA_PLATFORM=offscreen、QSettings 重定向到临时目录（不碰真实注册表）。
// 任何断言失败或步骤超时：打印每步结果与"自动处理过的弹窗日志"后以 1 退出。
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageLogContext>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTimer>

#include <cstdio>
#include <string>
#include <vector>

#include "core/platform.h"
#include "core/util.h"
#include "gui_util.h"
#include "i18n.h"
#include "integrations.h"
#include "mainwindow.h"
#include "settings_dialog.h"
#include "startup_check.h"
#include "theme.h"
#include "welcome_dialog.h"

namespace {

// ---- 报告：每条断言一行，脚本按 FAIL 计数与退出码判定 ----
struct Report {
    QStringList lines;
    QStringList dialogLog;
    QStringList fails;
    int passed = 0;
};
Report g_rep;

void printNow(const QString& s) {
    const QByteArray u = s.toUtf8();
    fwrite(u.constData(), 1, u.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void chk(bool ok, const QString& what) {
    if (ok) {
        ++g_rep.passed;
        g_rep.lines << "PASS  " + what;
        printNow(g_rep.lines.last());
    } else {
        g_rep.fails << what;
        g_rep.lines << "FAIL  " + what;
        printNow(g_rep.lines.last());
    }
}

// ---- 小工具 ----
QString envPort() {
    std::string p = ah::envOr({"MIDERHIVE_PORT", "AGENTHIVE_PORT", "ZCODE_PLATFORM_PORT"});
    return p.empty() ? QStringLiteral("8787") : QString::fromStdString(p);
}

QJsonObject readAgentsJson(const QString& home) {
    QFile f(home + "/config/agents.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeAgentsJson(const QString& home, const QJsonObject& o) {
    QDir().mkpath(home + "/config");
    QFile f(home + "/config/agents.json");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) > 0;
}

// 同步 HTTP POST（仅本机服务；返回 HTTP 状态码，0 = 超时/连接失败）
int httpPost(const QString& port, const QString& path, const QByteArray& agentName,
             const QByteArray& agentKey) {
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl("http://127.0.0.1:" + port + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!agentName.isEmpty()) {
        req.setRawHeader("X-Agent-Name", agentName);
        req.setRawHeader("X-Api-Key", agentKey);
    }
    QNetworkReply* reply = nam.post(req, "{}");
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("miderhive");
    QApplication::setApplicationName("MiderHive 多 Agent 协作工作台");

    const QString home = QString::fromStdString(ah::defaultHomeDir());
    const QString port = envPort();

    // ---- 测试隔离：QSettings（含具名构造的 i18n 读取）全部落到临时 ini，不碰真实注册表 ----
    const QString settingsDir = home + "/ui-settings";
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);

    // 界面偏好（与生产同库）：关自动更新检查（测试期间不许弹更新框）、加快刷新轮询、
    // 置为首启状态（引导要弹）
    {
        QSettings s;
        s.setValue("ui/autoCheck", false);
        s.setValue("ui/refreshMs", 1000);
        s.setValue("ui/welcomeSeen", false);
        s.sync();
    }

    i18n::load();
    app.setStyleSheet(ui::themeQss());

    printNow("==== gui_selftest 开始 ====");
    printNow(QString("home=%1 port=%2 lang=%3").arg(home, port).arg(int(i18n::g_lang)));

    // ================= A 组：纯逻辑断言（不起界面） =================

    // A1 MCP 配置生成：三工具各按其格式（claude/cursor→JSON，codex→TOML）
    {
        const QString exe = "C:/x/miderhive-mcp.exe";
        const QString j = ui::integrations::generateConfig("claude-code", exe, "a1", "k1");
        const QJsonObject srv = QJsonDocument::fromJson(j.toUtf8())
                                    .object()
                                    .value("mcpServers")
                                    .toObject()
                                    .value("miderhive")
                                    .toObject();
        chk(srv.value("command").toString() == exe, "integrations: claude-code 配置 command");
        chk(srv.value("env").toObject().value("MIDERHIVE_AGENT_NAME").toString() == "a1" &&
                srv.value("env").toObject().value("MIDERHIVE_AGENT_KEY").toString() == "k1",
            "integrations: claude-code 配置 env（身份名+密钥）");
        chk(!srv.contains("args"), "integrations: claude-code 无 args 字段（cursor 才有）");
        const QString jc = ui::integrations::generateConfig("cursor", exe, "a1", "k1");
        const QJsonObject srvc = QJsonDocument::fromJson(jc.toUtf8())
                                     .object()
                                     .value("mcpServers")
                                     .toObject()
                                     .value("miderhive")
                                     .toObject();
        chk(srvc.value("args").isArray(), "integrations: cursor 配置含 args 数组");
        const QString toml = ui::integrations::generateConfig("codex", exe, "a1", "k1");
        chk(toml.contains("[mcp_servers.miderhive]") && toml.contains("MIDERHIVE_AGENT_NAME"),
            "integrations: codex 生成 TOML 配置");
        chk(ui::integrations::tools().size() == 3 &&
                ui::integrations::toolById("claude-code") && ui::integrations::toolById("cursor") &&
                ui::integrations::toolById("codex"),
            "integrations: 三工具注册表完整");
        (void)ui::integrations::installed("claude-code");  // 冒烟：不崩溃即可（结果随环境）
    }

    // A2 报错翻译：技术错误 → 人话；未知错误保留原文
    {
        const QString locked = ui::humanError("SQLite database is locked");
        chk(locked.contains("占用") || locked.contains("lock"),
            "humanError: database is locked → 人话");
        const QString bind = ui::humanError("Address already in use: bind");
        chk(bind.contains("端口") || bind.contains("port"), "humanError: 端口占用 → 人话");
        const QString unknown = ui::humanError("zzz-unknown-err");
        chk(unknown.contains("zzz-unknown-err"), "humanError: 未知错误保留原文兜底");
    }

    // A3 启动自检：全新目录无问题；端口被占 → port_occupied
    {
        const QString probeHome = home + "/preflight-probe";
        chk(ui::startup::preflight(probeHome, port.toInt(), probeHome + "/platform.db",
                                   probeHome + "/config/agents.json")
                .isEmpty(),
            "startup: 全新数据目录自检无问题");
        QTcpServer occupy;
        const quint16 p = static_cast<quint16>(port.toInt());
        if (occupy.listen(QHostAddress::LocalHost, p)) {
            const auto issues = ui::startup::preflight(probeHome, port.toInt(), QString(), QString());
            bool hit = false;
            for (const auto& i : issues) hit |= (i.code == "port_occupied");
            chk(hit, "startup: 端口被占用时报告 port_occupied");
            occupy.close();
        } else {
            chk(false, "startup: 测试自身无法占住临时端口（环境异常）");
        }
    }

    // ================= B 组：GUI 端到端（真实 MainWindow + 生产路径） =================
    ah::Platform platform(ah::defaultHomeDir());
    std::string err;
    if (!platform.bootstrap(err)) {
        printNow(QString("FATAL  平台初始化失败: %1").arg(QString::fromStdString(err)));
        return 1;
    }
    std::string serr;
    if (!platform.startHttpServer(port.toInt(), serr)) {
        printNow(QString("FATAL  HTTP 服务启动失败: %1").arg(QString::fromStdString(serr)));
        return 1;
    }

    MainWindow w(platform);
    w.setMinimumSize(1080, 680);
    w.show();  // 首启引导由 MainWindow 自己的 400ms 单发定时器弹出（真实生产路径）

    // ---- 弹窗自动处理器：扮演用户的手，把流程中的模态框逐个"点掉"并留痕 ----
    struct Flow {
        int step = 0;                    // 0=等引导弹出 1=等引导关闭+心跳 2=接入成功断言
                                         // 3=密钥丢失+横幅修复 4=设置重入 5=收尾
        int ticks = 0;
        const QString agent = "claude-selftest";
        QString oldKey, newKey;
        int popupSeen = 0;               // 「接入成功」弹窗出现次数
        bool allowWelcomeClose = false;  // 允许 closer 关闭引导对话框（点「开始使用」）
        int autoClicks = 0;
    } flow;

    auto finish = [&flow] {
        printNow("==== gui_selftest 报告 ====");
        printNow(QString("passed=%1 failed=%2").arg(g_rep.passed).arg(g_rep.fails.size()));
        for (const auto& l : g_rep.dialogLog) printNow("  dialog: " + l);
        if (!g_rep.fails.isEmpty()) {
            printNow("---- 失败明细 ----");
            for (const auto& f : g_rep.fails) printNow("  FAIL: " + f);
        }
        if (flow.step < 5 && g_rep.fails.isEmpty())
            printNow(QString("  中止于步骤 %1").arg(flow.step));
        QApplication::exit(g_rep.fails.isEmpty() ? 0 : 1);
    };

    QTimer closer;
    QObject::connect(&closer, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) return;
        if (auto* mb = qobject_cast<QMessageBox*>(modal)) {
            const QString title = mb->windowTitle();
            if (title.contains("接入成功") || title.contains("Connected")) {
                ++flow.popupSeen;
                g_rep.dialogLog << "「接入成功」弹窗 #" + QString::number(flow.popupSeen);
            } else {
                g_rep.dialogLog << title + " :: " + mb->text().left(140);
            }
            if (++flow.autoClicks > 60) {
                g_rep.fails << "弹窗自动处理超过上限（60）——流程疑似循环";
                finish();
                return;
            }
            if (auto* b = mb->button(QMessageBox::Yes)) { b->click(); return; }
            if (auto* b = mb->button(QMessageBox::Close)) { b->click(); return; }
            if (auto* b = mb->button(QMessageBox::Ok)) { b->click(); return; }
        } else if (qobject_cast<WelcomeDialog*>(modal) && flow.allowWelcomeClose) {
            for (auto* b : modal->findChildren<QPushButton*>())
                if (b->text().contains("开始使用") || b->text().contains("Get started")) {
                    if (++flow.autoClicks > 60) {
                        g_rep.fails << "弹窗自动处理超过上限（60）";
                        finish();
                        return;
                    }
                    b->click();
                    return;
                }
        }
    });
    closer.start(80);

    // ---- 步骤驱动器：等待步轮询推进；动作步单次执行（先改步号再动作，防嵌套重入） ----
    QTimer driver;
    bool inDriver = false;
    constexpr int kTickCap = 75;  // 75 × 200ms = 15s / 步
    QObject::connect(&driver, &QTimer::timeout, [&] {
        if (inDriver || flow.step >= 5) return;
        inDriver = true;
        flow.ticks++;
        if (flow.ticks > kTickCap) {
            QWidget* m = QApplication::activeModalWidget();
            g_rep.fails << QString("步骤 %1 超时（15s 无进展）").arg(flow.step);
            g_rep.dialogLog << QString("超时时模态框: %1")
                                   .arg(m ? m->metaObject()->className() : "null");
            finish();
            inDriver = false;
            return;
        }
        switch (flow.step) {
            // ---- ① 等待首启引导自动弹出，然后驱动它 ----
            case 0: {
                auto* dlg = qobject_cast<WelcomeDialog*>(QApplication::activeModalWidget());
                if (!dlg) break;
                chk(true, "首启自动弹出接入引导（未写 ui/welcomeSeen 时）");
                const QList<QLineEdit*> edits = dlg->findChildren<QLineEdit*>();
                chk(edits.size() == 1, "引导含唯一「接入身份名」输入框");
                if (!edits.isEmpty()) edits.first()->setText(flow.agent);
                QPushButton* toolBtn = nullptr;
                for (auto* b : dlg->findChildren<QPushButton*>())
                    if (b->property("toolId").toString() == "claude-code") toolBtn = b;
                chk(toolBtn != nullptr, "引导含 Claude Code 工具按钮");
                if (toolBtn) toolBtn->click();  // 检测 → 预配 → 生成配置 → 登记（同步完成）
                QPlainTextEdit* view = dlg->findChild<QPlainTextEdit*>();
                const QString cfg = view ? view->toPlainText() : QString();
                const QJsonObject srv = QJsonDocument::fromJson(cfg.toUtf8())
                                            .object()
                                            .value("mcpServers")
                                            .toObject()
                                            .value("miderhive")
                                            .toObject();
                chk(!srv.isEmpty(), "生成 Claude Code MCP 配置（JSON 含 mcpServers.miderhive）");
                chk(srv.value("command").toString().endsWith("miderhive-mcp.exe"),
                    "配置 command 指向 miderhive-mcp 可执行");
                chk(srv.value("env").toObject().value("MIDERHIVE_AGENT_NAME").toString() ==
                        flow.agent,
                    "配置 env 写入接入身份名");
                chk(!srv.value("env").toObject().value("MIDERHIVE_AGENT_KEY").toString().isEmpty(),
                    "配置 env 写入接入密钥");
                const QSettings s;
                chk(s.value("ui/onboardingPending/" + flow.agent).toString() == "claude-code",
                    "预配身份登记进待观察注册表（ui/onboardingPending）");
                flow.oldKey = readAgentsJson(home).value(flow.agent).toString();
                chk(!flow.oldKey.isEmpty(), "预配密钥已写入本机明文缓存 agents.json");
                flow.allowWelcomeClose = true;  // closer 负责点「开始使用 →」关闭
                flow.step = 1;
                flow.ticks = 0;
            } break;

            // ---- ② 等引导关闭，模拟 Agent 心跳上线 ----
            case 1: {
                if (QApplication::activeModalWidget() != nullptr) break;
                const QSettings s;
                chk(s.value("ui/welcomeSeen", false).toBool(), "引导完成后写入 ui/welcomeSeen（首启只弹一次）");
                const int code = httpPost(port, "/api/agents/heartbeat", flow.agent.toUtf8(),
                                          flow.oldKey.toUtf8());
                chk(code == 200, "预配身份心跳上线（HTTP 200）");
                bool online = false;
                std::vector<ah::AgentInfo> agents;
                std::string lerr;
                if (platform.listAgents(agents, lerr))
                    for (const auto& a : agents)
                        if (a.name == flow.agent.toStdString()) online = (a.status == "online");
                chk(online, "Agent 列表中该身份状态为 online");
                flow.step = 2;
                flow.ticks = 0;
            } break;

            // ---- ③ 刷新一次：应弹「接入成功」+ 写欢迎记忆 + 清登记 ----
            case 2: {
                flow.step = 3;
                flow.ticks = 0;
                QMetaObject::invokeMethod(&w, "onRefresh");  // 私有 slot，程序化触发
                QSettings s;
                s.beginGroup("ui/onboardingPending");
                const bool cleared = s.childKeys().isEmpty();
                s.endGroup();
                chk(cleared, "「接入成功」后待观察登记清除（不会二次弹窗）");
                chk(flow.popupSeen >= 1, "Agent 上线后弹出「接入成功」提示");
                bool welcomeMem = false;
                std::vector<ah::MemoryEntry> mem;
                std::string merr;
                if (platform.memoryList("project", mem, merr))
                    for (const auto& m : mem) welcomeMem |= (m.key == "welcome/claude-code");
                chk(welcomeMem, "欢迎记忆写入 用户记忆→项目档案（welcome/claude-code）");
            } break;

            // ---- ④ 制造密钥丢失 → 横幅出现 → 轮换修复 → 新旧密钥验证 ----
            case 3: {
                flow.step = 4;
                flow.ticks = 0;
                QJsonObject cache = readAgentsJson(home);
                cache.remove(flow.agent);
                chk(writeAgentsJson(home, cache), "制造密钥缓存丢失现场（改写 agents.json）");
                QMetaObject::invokeMethod(&w, "onRefresh");  // 总览页刷新 → 横幅重建
                QPushButton* fixBtn = nullptr;
                for (auto* b : w.findChildren<QPushButton*>())
                    if (b->isVisible() &&
                        (b->text().contains("轮换密钥修复") || b->text().contains("Fix: rotate key")))
                        fixBtn = b;
                chk(fixBtn != nullptr, "密钥丢失后总览页出现健康横幅与「轮换密钥修复」按钮");
                if (fixBtn) {
                    fixBtn->click();  // 确认(Yes)→轮换→新钥弹窗(Close) 由 closer 依次处理
                    flow.newKey = readAgentsJson(home).value(flow.agent).toString();
                    chk(!flow.newKey.isEmpty() && flow.newKey != flow.oldKey,
                        "轮换后新密钥写回缓存且与旧密钥不同");
                    chk(httpPost(port, "/api/agents/heartbeat", flow.agent.toUtf8(),
                                 flow.oldKey.toUtf8()) == 401,
                        "旧密钥轮换后立即失效（HTTP 401）");
                    chk(httpPost(port, "/api/agents/heartbeat", flow.agent.toUtf8(),
                                 flow.newKey.toUtf8()) == 200,
                        "新密钥可正常心跳（HTTP 200）");
                    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    QMetaObject::invokeMethod(&w, "onRefresh");
                    QPushButton* stale = nullptr;
                    for (auto* b : w.findChildren<QPushButton*>())
                        if (b->isVisible() &&
                            (b->text().contains("轮换密钥修复") ||
                             b->text().contains("Fix: rotate key")))
                            stale = b;
                    chk(stale == nullptr, "修复完成后健康横幅的密钥告警消失");
                }
            } break;

            // ---- ⑤ 设置 → 重新打开接入引导（真实点击按钮） ----
            case 4: {
                flow.step = 5;
                auto* dlg = new SettingsDialog(platform, &w);
                dlg->show();
                QApplication::processEvents();
                QPushButton* onboardBtn = nullptr;
                for (auto* b : dlg->findChildren<QPushButton*>())
                    if (b->text().contains("重新打开接入引导") ||
                        b->text().contains("Reopen onboarding"))
                        onboardBtn = b;
                chk(onboardBtn != nullptr, "设置 → Agent 管理 提供「重新打开接入引导」入口");
                if (onboardBtn) {
                    flow.allowWelcomeClose = true;
                    onboardBtn->click();  // 引导 exec 由 closer 点「开始使用」结束，随后刷新列表
                    chk(true, "再次进入接入引导并正常关闭（重入路径可用）");
                }
                const QSettings s;
                chk(s.value("ui/welcomeSeen", false).toBool(), "重入引导后 ui/welcomeSeen 仍为真");
                dlg->close();
                dlg->deleteLater();
                finish();
            } break;
        }
        inDriver = false;
    });
    driver.start(200);

    // 兜底看门狗：整程超过 150s 强制收卷（避免 CI 挂死）
    QTimer::singleShot(150000, &app, [&] {
        g_rep.fails << "整体看门狗超时（150s）";
        finish();
    });

    return app.exec();
}
