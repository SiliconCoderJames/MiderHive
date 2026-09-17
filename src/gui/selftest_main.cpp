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
// 可选抓帧（默认关闭）：--shots <dir> [--shots-size 1440x920] 把关键步骤的窗口与面板
// 画面用 QWidget::grab()（应用自身渲染，离屏可用）落成 PNG。README 的演示动图与面板
// 巡览拼图由 scripts/make_demo_gif.py 从这些帧组装。不传参时行为与本开关无关。
// 任何断言失败或步骤超时：打印每步结果与"自动处理过的弹窗日志"后以 1 退出。
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGroupBox>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageLogContext>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStringList>
#include <QTabBar>
#include <QTableWidget>
#include <QTcpServer>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <string>
#include <vector>

#include "core/platform.h"
#include "core/util.h"
#include "connect_dialog.h"
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

// ---- 抓帧（--shots <dir>，默认关闭）----
// 只出图，不参与断言：不传 --shots 时一次 grab() 都不会发生，流程与断言与从前完全一致。
// 用 QWidget::grab()（应用自身渲染）而不是屏幕截图 API——offscreen 平台下同样能出图，
// 因此 CI/无桌面环境也能生成 README 素材。面板另存一份"整页"帧（QScrollArea 页，
// 即用户所见的一屏），供 2×2 巡览拼图直接使用，避免按坐标裁切窗口带来的偏移。
struct ShotWriter {
    QString dir;
    int seq = 0;
    bool on() const { return !dir.isEmpty(); }
    void save(QWidget* w, const QString& name, QWidget* page = nullptr) {
        if (!on() || !w) return;
        QDir().mkpath(dir);
        const QString base =
            QString("%1/%2-%3").arg(dir).arg(seq++, 2, 10, QChar('0')).arg(name);
        QApplication::processEvents();  // 先让布局/排队刷新落地，避免抓到半渲染帧
        const QPixmap pm = w->grab();
        pm.save(base + ".png");
        if (page) page->grab().save(base + "-page.png");
        printNow(QString("SHOT  %1.png  %2x%3").arg(base).arg(pm.width()).arg(pm.height()));
    }
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("miderhive");
    QApplication::setApplicationName("MiderHive 多 Agent 协作工作台");

    const QString home = QString::fromStdString(ah::defaultHomeDir());
    const QString port = envPort();

    // ---- 抓帧开关（默认关闭）：--shots <dir> [--shots-size 1440x920] ----
    ShotWriter shots;
    int shotW = 1440, shotH = 920;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        QString val;
        auto takeNext = [&] {
            if (i + 1 < argc) val = QString::fromLocal8Bit(argv[++i]);
        };
        auto parseSize = [&](const QString& s) {
            const QStringList xy = s.split('x', Qt::SkipEmptyParts);
            if (xy.size() == 2 && xy[0].toInt() > 0 && xy[1].toInt() > 0) {
                shotW = xy[0].toInt();
                shotH = xy[1].toInt();
            }
        };
        if (a == "--shots") {
            takeNext();
            shots.dir = val;
        } else if (a.startsWith("--shots=")) {
            shots.dir = a.mid(8);
        } else if (a == "--shots-size") {
            takeNext();
            parseSize(val);
        } else if (a.startsWith("--shots-size=")) {
            parseSize(a.mid(13));
        }
    }
    if (shots.on())
        printNow(QString("抓帧开启：%1  画幅 %2x%3").arg(shots.dir).arg(shotW).arg(shotH));

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

    // A1 MCP 配置生成：各工具按各自格式（claude/droid/cursor→JSON，codex→TOML，
    //    dsh→patch YAML，hermes→YAML，zcode→环境变量，copilot→指令块）
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

        // Codex TOML 必须用字面量字符串（单引号）：Windows 路径里的 '\Q' 在 TOML
        // 基本字符串里是非法转义，会让整份 config.toml 解析失败。
        const QString winExe = "C:\\Qt\\6.8.3\\msvc2022_64\\bin\\miderhive-mcp.exe";
        const QString toml = ui::integrations::generateConfig("codex", winExe, "a1", "k1");
        chk(toml.contains("[mcp_servers.miderhive]") && toml.contains("MIDERHIVE_AGENT_NAME"),
            "integrations: codex 生成 TOML 配置");
        chk(toml.contains("command = '" + winExe + "'"),
            "integrations: codex 路径用 TOML 字面量字符串（反斜杠不需转义）");
        chk(!toml.contains("command = \""), "integrations: codex 不使用双引号基本字符串");

        // DSH：插入式补丁 + 密钥必须落在 env 里（DSH 会清洗子进程环境中的 *KEY*/*TOKEN*）
        const QString dsh = ui::integrations::generateConfig("dsh", winExe, "a1", "k1");
        chk(dsh.contains("- insert:") && dsh.contains("dsh-mcp-client") &&
                dsh.contains("serverName: miderhive"),
            "integrations: dsh 生成 cordis 补丁插入行");
        chk(dsh.contains("MIDERHIVE_AGENT_KEY"), "integrations: dsh 密钥写入 env 块");

        // Hermes：mcp_servers 映射
        const QString hermes = ui::integrations::generateConfig("hermes", winExe, "a1", "k1");
        chk(hermes.contains("mcp_servers:") && hermes.contains("miderhive:") &&
                hermes.contains("MIDERHIVE_AGENT_NAME"),
            "integrations: hermes 生成 mcp_servers 条目");

        // ZCode 走环境变量；Copilot 走指令块
        const QString zcode = ui::integrations::generateConfig("zcode", winExe, "a1", "k1");
        chk(zcode.contains("MIDERHIVE_AGENT_NAME=a1") && zcode.contains("MIDERHIVE_PORT"),
            "integrations: zcode 生成环境变量块");
        const QString copilot = ui::integrations::generateConfig("copilot", winExe, "a1", "k1");
        chk(copilot.contains("X-Agent-Name: a1") && copilot.contains("/api/knowledge"),
            "integrations: copilot 生成 HTTP 指令块");

        // 八工具注册表：六个用户点名接入的 Agent 都必须在册
        const QStringList ids = {"claude-code", "codex",     "droid", "dsh",
                                 "hermes",      "zcode",     "cursor", "copilot"};
        bool allPresent = ui::integrations::tools().size() == ids.size();
        for (const QString& id : ids)
            if (!ui::integrations::toolById(id)) allPresent = false;
        chk(allPresent, "integrations: 八工具注册表完整（含 Claude/ChatGPT/Droid/DSH/Hermes/ZCode）");

        // 落点解析：无配置文件的工具必须返回空串，其余必须给绝对路径
        chk(ui::integrations::configPath("zcode").isEmpty() &&
                ui::integrations::configPath("copilot").isEmpty(),
            "integrations: zcode/copilot 无配置文件（返回空）");
        chk(ui::integrations::configPath("hermes").contains("hermes") &&
                ui::integrations::configPath("codex").endsWith("config.toml"),
            "integrations: hermes/codex 落点解析为真实路径");
        (void)ui::integrations::installed("claude-code");  // 冒烟：不崩溃即可（结果随环境）
    }

    // A1b 配置写入（auto-write）：在重定向的配置根里验证新建/合并/替换/拒绝四条路径。
    // 通过 MIDERHIVE_CONNECT_ROOT 把落点指到临时目录，绝不碰真实用户配置。
    {
        const QString root = home + "/connect-root";
        qputenv("MIDERHIVE_CONNECT_ROOT", root.toUtf8());
        const QString exe = "C:/x/miderhive-mcp.exe";

        // ---- JSON（Cursor 同构）：新建 ----
        auto ar = ui::integrations::applyConfig("cursor", exe, "c1", "k1");
        chk(ar.ok, "applyConfig cursor 新建 JSON 成功");
        QString jtxt;
        {
            QFile f(ui::integrations::configPath("cursor"));
            if (f.open(QIODevice::ReadOnly)) jtxt = QString::fromUtf8(f.readAll());
        }
        const QJsonObject cursorSrv = QJsonDocument::fromJson(jtxt.toUtf8())
                                         .object()
                                         .value("mcpServers")
                                         .toObject()
                                         .value("miderhive")
                                         .toObject();
        chk(cursorSrv.value("command").toString() == exe && cursorSrv.value("args").isArray(),
            "applyConfig cursor 写入的 JSON 内容正确");

        // ---- JSON：合并进已有文件时必须保留他人的 server 条目 ----
        {
            QJsonObject other{{"mcpServers", QJsonObject{{"other", QJsonObject{{"command", "x"}}}}}};
            QFile f(ui::integrations::configPath("cursor"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QJsonDocument(other).toJson());
        }
        ar = ui::integrations::applyConfig("cursor", exe, "c2", "k2");
        {
            QFile f(ui::integrations::configPath("cursor"));
            QString merged;
            if (f.open(QIODevice::ReadOnly)) merged = QString::fromUtf8(f.readAll());
            const QJsonObject o = QJsonDocument::fromJson(merged.toUtf8()).object();
            chk(ar.ok && o.value("mcpServers").toObject().contains("other"),
                "applyConfig 合并保留了他人的 server 条目");
            chk(o.value("mcpServers").toObject().value("miderhive").toObject().value("env")
                    .toObject().value("MIDERHIVE_AGENT_NAME").toString() == "c2",
                "applyConfig 合并后 miderhive 为新值");
            chk(QFile::exists(ui::integrations::configPath("cursor") + ".miderhive.bak"),
                "applyConfig 覆盖前留下 .miderhive.bak 备份");
        }

        // ---- JSON：坏文件必须拒绝，且一字不改（绝不弄坏用户配置）----
        {
            const QString p = ui::integrations::configPath("cursor");
            const QString garbage = "{ this is not json";
            {
                QFile f(p);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(garbage.toUtf8());
            }
            const auto bad = ui::integrations::applyConfig("cursor", exe, "c3", "k3");
            QString after;
            {
                QFile f(p);
                if (f.open(QIODevice::ReadOnly)) after = QString::fromUtf8(f.readAll());
            }
            chk(!bad.ok, "applyConfig 遇到非法 JSON 时拒绝写入");
            chk(after == garbage, "applyConfig 拒绝时原文件一字未改");
        }

        // ---- TOML（Codex）：写入字面量路径，且重复写入替换而非追加 ----
        ar = ui::integrations::applyConfig("codex", "C:\\Qt\\mcp.exe", "x1", "k1");
        {
            QFile f(ui::integrations::configPath("codex"));
            QString t;
            if (f.open(QIODevice::ReadOnly)) t = QString::fromUtf8(f.readAll());
            chk(ar.ok && t.contains("[mcp_servers.miderhive]") &&
                    t.contains("command = 'C:\\Qt\\mcp.exe'"),
                "applyConfig codex 写入 TOML 字面量路径");
        }
        ar = ui::integrations::applyConfig("codex", "C:\\Qt\\mcp2.exe", "x2", "k2");
        {
            QFile f(ui::integrations::configPath("codex"));
            QString t;
            if (f.open(QIODevice::ReadOnly)) t = QString::fromUtf8(f.readAll());
            chk(ar.ok && t.count("[mcp_servers.miderhive]") == 1,
                "applyConfig codex 重复写入只留一张表（不追加第二份）");
            chk(t.contains("mcp2.exe"), "applyConfig codex 已更新为新值");
        }

        // ---- YAML（DSH 补丁）：已有 "# 注释 + []" 的文件要摘掉 [] 再追加 ----
        {
            const QString p = ui::integrations::configPath("dsh");
            QDir().mkpath(QFileInfo(p).absolutePath());
            QFile f(p);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write("# patch layer\n[]\n");
        }
        ar = ui::integrations::applyConfig("dsh", exe, "dsh1", "k1");
        {
            QFile f(ui::integrations::configPath("dsh"));
            QString y;
            if (f.open(QIODevice::ReadOnly)) y = QString::fromUtf8(f.readAll());
            // 顶层裸 "[]" 必须被摘掉；但条目里的 "args: []" 是合法内容，不能一并否掉
            bool bareEmptyArray = false;
            for (const QString& line : y.split('\n'))
                if (line.trimmed() == "[]") bareEmptyArray = true;
            chk(ar.ok && y.contains("mcp-miderhive") && y.contains("dsh-mcp-client") &&
                    y.contains("- insert:") && !bareEmptyArray,
                "applyConfig dsh 生成合法补丁（摘掉顶层空数组 []）");
        }
        ar = ui::integrations::applyConfig("dsh", exe, "dsh2", "k2");
        {
            QFile f(ui::integrations::configPath("dsh"));
            QString y;
            if (f.open(QIODevice::ReadOnly)) y = QString::fromUtf8(f.readAll());
            chk(ar.ok && y.count("mcp-miderhive") == 1 && y.contains("dsh2"),
                "applyConfig dsh 重复写入替换原条目（不追加第二份 insert）");
        }

        // ---- YAML（Hermes）：新建 mcp_servers 段 ----
        ar = ui::integrations::applyConfig("hermes", exe, "h1", "k1");
        {
            QFile f(ui::integrations::configPath("hermes"));
            QString y;
            if (f.open(QIODevice::ReadOnly)) y = QString::fromUtf8(f.readAll());
            chk(ar.ok && y.contains("mcp_servers:") && y.contains("miderhive:"),
                "applyConfig hermes 写入 mcp_servers 条目");
        }

        // ---- 没有可写配置文件的工具必须明确失败，而不是假装成功 ----
        chk(!ui::integrations::applyConfig("zcode", exe, "z", "k").ok,
            "applyConfig zcode 明确报告没有可写配置文件");
        qunsetenv("MIDERHIVE_CONNECT_ROOT");
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
    if (shots.on()) w.resize(shotW, shotH);  // 抓帧模式固定画幅：出图尺寸与构图稳定
    w.show();  // 首启引导由 MainWindow 自己的 400ms 单发定时器弹出（真实生产路径）

    // ---- 接入向导（ConnectDialog）：真实构造 + 一键接入走通 ----
    // 覆盖"选工具→签发身份→按该工具格式生成片段"这条主路径；不点"写入配置文件"，
    // 因为那会碰真实落点（写入逻辑本身已由 A1b 在重定向配置根里验证）。
    {
        ConnectDialog dlg(platform);
        chk(true, "接入向导可无头构造");
        auto* view = dlg.findChild<QPlainTextEdit*>();
        auto* list = dlg.findChild<QListWidget*>();
        auto* edit = dlg.findChild<QLineEdit*>();
        chk(view != nullptr && list != nullptr && edit != nullptr,
            "接入向导含工具列表 / 身份名输入 / 片段视图");
        chk(list && list->count() == ui::integrations::tools().size(),
            "接入向导列出注册表里的全部工具");
        // 第 1 行是 Codex（工具表顺序：claude-code, codex, droid, dsh, hermes, zcode, cursor, copilot）
        if (list) list->setCurrentRow(1);
        if (edit) edit->setText("codex-wizard-test");
        QPushButton* btn = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>())
            if (b->text().contains("一键接入") || b->text().contains("Connect")) {
                btn = b;
                break;
            }
        chk(btn != nullptr, "接入向导含「一键接入」按钮");
        if (btn) btn->click();
        const QString snippet = view ? view->toPlainText() : QString();
        chk(snippet.contains("[mcp_servers.miderhive]"),
            "接入向导为 Codex 生成 TOML（按工具格式，而非一律 JSON）");
        chk(snippet.contains("codex-wizard-test"), "接入向导片段写入接入身份名");
        chk(!snippet.contains("\"mcpServers\""), "接入向导没有退回 CLI 缺省 JSON 格式");
    }

    // ---- 英文模式全量扫描：切到英文后，用户可见文案里不应再出现中文 ----
    // 这是"语言问题是否真的解决"的权威判据：静态 grep 分不清注释 / 枚举值 / 数据 /
    // zh-en 配对表（都会误报），而这里是真把界面切到英文、遍历所有控件的文案找 CJK。
    // 必须在写任何中文数据之前做（欢迎记忆、错误报告都会把中文当数据渲染出来）。
    {
        // 全程同步：绝不在扫描里跑事件循环——MainWindow 构造时挂了 400ms 的单发定时器，
        // 一旦它在这里触发就会 exec() 出模态引导对话框，而此刻 closer 还没装上 → 死等。
        i18n::apply(i18n::Lang::En);
        // rebuildPanels() 用 deleteLater() 拆旧面板：不显式冲掉 DeferredDelete，
        // 重建前的旧中文面板仍挂在窗口下，扫描会把它们一并算进来（假阳性）。
        // sendPostedEvents 是同步的，不会推进定时器。
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        const auto hasCjk = [](const QString& s) {
            for (const QChar c : s)
                if (c.unicode() >= 0x4E00 && c.unicode() <= 0x9FFF) return true;
            return false;
        };
        QStringList offenders;
        const auto note = [&](const char* what, const QString& text) {
            if (hasCjk(text)) offenders << QString("%1=%2").arg(what, text.left(48));
        };

        // 语言切换按钮本身写的就是"中文"（它显示"切过去"的目标语言），按设计放行。
        // 它是 QToolButton（不是 QPushButton），所以必须按 QAbstractButton 查。
        QAbstractButton* langBtn = nullptr;
        for (auto* b : w.findChildren<QAbstractButton*>())
            if (b->text() == "中文" || b->text() == "EN") langBtn = b;

        for (auto* l : w.findChildren<QLabel*>()) note("QLabel", l->text());
        for (auto* b : w.findChildren<QAbstractButton*>()) {
            if (b == langBtn) continue;
            note("Button", b->text());
        }
        for (auto* c : w.findChildren<QComboBox*>())
            for (int i = 0; i < c->count(); ++i) note("Combo", c->itemText(i));
        for (auto* e : w.findChildren<QLineEdit*>()) note("LineEditHint", e->placeholderText());
        for (auto* e : w.findChildren<QPlainTextEdit*>())
            note("TextHint", e->placeholderText());
        for (auto* t : w.findChildren<QTableWidget*>())
            for (int c = 0; c < t->columnCount(); ++c)
                if (auto* h = t->horizontalHeaderItem(c)) note("TableHeader", h->text());
        for (auto* t : w.findChildren<QTreeWidget*>())
            for (int c = 0; c < t->columnCount(); ++c) note("TreeHeader", t->headerItem()->text(c));
        for (auto* l : w.findChildren<QListWidget*>())
            for (int i = 0; i < l->count(); ++i) note("ListItem", l->item(i)->text());
        for (auto* g : w.findChildren<QGroupBox*>()) note("GroupBox", g->title());
        for (auto* tb : w.findChildren<QTabBar*>())
            for (int i = 0; i < tb->count(); ++i) note("Tab", tb->tabText(i));
        for (auto* wd : w.findChildren<QWidget*>()) note("tooltip", wd->toolTip());
        note("windowTitle", w.windowTitle());

        if (offenders.isEmpty()) {
            chk(true, "英文模式全控件扫描：界面文案无中文残留");
        } else {
            chk(false, QString("英文模式仍有 %1 处中文文案：%2")
                           .arg(offenders.size())
                           .arg(offenders.mid(0, 6).join(" | ")));
            for (const auto& o : offenders) printNow("   [en-cjk] " + o);
        }

        i18n::apply(i18n::Lang::Zh);  // 还原：后续流程仍按中文跑
    }

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
    bool confirmShot = false;  // 抓帧：轮换确认框只留一帧
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
            // 抓帧：把「轮换密钥」确认框本身留一帧（演示"交互"这一步）。
            // 标题精确匹配，绝不含糊——紧随其后的「密钥已轮换」弹窗里带新密钥原文，
            // 那种画面绝不能进公开素材。
            if (shots.on() && !confirmShot &&
                (title.contains("轮换密钥") || title.contains("Rotate key"))) {
                confirmShot = true;
                shots.save(modal, "rotate-confirm");
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
                shots.save(dlg, "onboarding");
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
                shots.save(dlg, "onboarding-config");
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
                shots.save(&w, "overview");
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
                shots.save(&w, "overview-health");
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
                    shots.save(&w, "overview-fixed");
                }
            } break;

            // ---- ⑤ 设置 → 重新打开接入引导（真实点击按钮） ----
            case 4: {
                flow.step = 5;

                // 抓帧模式：面板巡览。走真实导航行切换（左侧高亮随之移动——这正是静态
                // 截图给不了的信息），每页另存"整页"帧供 2×2 拼图使用。
                if (shots.on()) {
                    QListWidget* nav = nullptr;
                    auto hasId = [](QListWidget* lw, const char* id) {
                        for (int i = 0; i < lw->count(); ++i)
                            if (lw->item(i)->data(Qt::UserRole).toString() == QLatin1String(id))
                                return true;
                        return false;
                    };
                    // 按内容认导航（面板内部也有 QListWidget，如事件流/列表），不靠下标
                    for (auto* lw : w.findChildren<QListWidget*>())
                        if (hasId(lw, "overview") && hasId(lw, "errors")) {
                            nav = lw;
                            break;
                        }
                    auto* stack = w.findChild<QStackedWidget*>();
                    if (!nav || !stack) {
                        printNow("WARN  抓帧巡览：未定位到导航或页面栈，跳过");
                    } else {
                        const QStringList tour{"usage",  "knowledge", "skills",
                                               "memory", "messages",  "errors"};
                        for (const QString& id : tour) {
                            int row = -1;
                            for (int i = 0; i < nav->count(); ++i)
                                if (nav->item(i)->data(Qt::UserRole).toString() == id) {
                                    row = i;
                                    break;
                                }
                            if (row < 0 || row >= stack->count()) {
                                printNow("WARN  抓帧巡览：导航缺面板 " + id);
                                continue;
                            }
                            nav->setCurrentRow(row);  // 真实切换：高亮移动 + 面板刷新
                            QMetaObject::invokeMethod(&w, "onRefresh");
                            shots.save(&w, id, stack->widget(row));
                        }
                    }
                }

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
