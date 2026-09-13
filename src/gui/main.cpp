#include <QApplication>
#include <QFile>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <cstdlib>

#include "core/platform.h"
#include "core/util.h"
#include "gui_util.h"
#include "i18n.h"
#include "mainwindow.h"
#include "startup_check.h"
#include "theme.h"

namespace {

// ---- 单实例：安装版有了开始菜单快捷方式后，用户很容易双击出第二个实例，
// 而第二个实例会因 8787 端口被占用弹出"HTTP 服务启动失败"的警告框。
// 这里用本地 socket 做互斥：已有实例就把它唤到前台，然后本进程直接退出。 ----
QString singleInstanceKey() {
    std::string port = ah::envOr({"MIDERHIVE_PORT", "AGENTHIVE_PORT", "ZCODE_PLATFORM_PORT"});
    if (port.empty()) port = "8787";
    // 以数据目录 + 端口为键：同一份数据同时只允许一个工作台
    const std::string home = ah::defaultHomeDir();
    return QString("MiderHive-%1-%2")
        .arg(QString::fromStdString(ah::sha256Hex(home + ":" + port)).left(16),
             QString::fromStdString(port));
}

// 返回 true 表示已有实例在运行（调用方应直接退出）
bool notifyExistingInstance(const QString& key) {
    QLocalSocket sock;
    sock.connectToServer(key);
    if (!sock.waitForConnected(300)) return false;
    sock.write("show");
    sock.flush();
    sock.waitForBytesWritten(300);
    sock.disconnectFromServer();
    return true;
}

}  // namespace

// QSettings 位置迁移：品牌更名后组织/应用名变化，把旧位置（agenthive / AgentHive 多
// Agent 协作工作台）的主题、字号、几何、更新检查等设置一次性搬到新位置。
// 仅当新位置还没有任何键时执行；旧位置的键保留不删，旧版本回退启动仍能读回。
void migrateLegacySettings() {
    QSettings fresh;
    if (!fresh.allKeys().isEmpty()) return;
    QSettings legacyApp("agenthive", "AgentHive 多 Agent 协作工作台");
    const QStringList keys = legacyApp.allKeys();
    for (const QString& k : keys) fresh.setValue(k, legacyApp.value(k));
    // i18n 的 ui/lang 曾单独存在 ("agenthive","agenthive") 位置
    QSettings legacyI18n("agenthive", "agenthive");
    const QVariant lang = legacyI18n.value("ui/lang");
    if (lang.isValid() && !fresh.contains("ui/lang")) fresh.setValue("ui/lang", lang);
    fresh.sync();
}

// 程序化绘制蜂巢图标：深色圆角底 + 琥珀色六边形蜂巢 + 入口点
static QPixmap hiveIcon(int side) {
    QPixmap pm(side, side);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    qreal m = side * 0.08, w = side - 2 * m;
    QRectF box(m, m, w, w);
    p.setBrush(QColor("#10141c"));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(box, side * 0.2, side * 0.2);
    auto hex = [](QPainter& pp, const QPointF& c, qreal r) {
        QPolygonF h;
        for (int i = 0; i < 6; ++i) {
            qreal a = M_PI / 180.0 * (60 * i - 30);
            h << c + QPointF(r * std::cos(a), r * std::sin(a));
        }
        pp.drawPolygon(h);
    };
    QPen pen(QColor("#f59e0b"), side * 0.055, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    qreal r = w * 0.21;
    hex(p, QPointF(side / 2.0, side / 2.0 - r * 1.02), r);
    hex(p, QPointF(side / 2.0 - r * 0.9, side / 2.0 + r * 0.55), r);
    hex(p, QPointF(side / 2.0 + r * 0.9, side / 2.0 + r * 0.55), r);
    p.setBrush(QColor("#0ea5e9"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(side / 2.0, side / 2.0 - r * 1.02), r * 0.32, r * 0.32);
    return pm;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("MiderHive 多 Agent 协作工作台");
    app.setOrganizationName("miderhive");
    migrateLegacySettings();
    i18n::load();
    {
        // 优先使用仓库品牌图标（与 README 一致），缺失时回退到程序化绘制的蜂巢
        QIcon brandIcon(":/brand/logo.png");
        if (!brandIcon.isNull())
            app.setWindowIcon(brandIcon);
        else
            app.setWindowIcon(QIcon(hiveIcon(64)));
    }

    // 应用字体：优先 Segoe UI Variable（Win11 可变字体，低版本自动回落），并开启
    // 等宽数字(tnum)——用量/时间戳每 3 秒刷新一次，等宽数字让它们不会左右抖动。
    {
        QFont f = app.font();
        f.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                       QStringLiteral("Microsoft YaHei UI")});
        f.setFeature(QFont::Tag("tnum"), 1);
        app.setFont(f);
    }

    // 强制深色主题：QSS 模板经 ui::th() 主题化（当前主题与字号持久化于 QSettings）
    // 注意 qt_add_resources(PREFIX "/theme") 会把子目录 qss/ 拼进资源路径
    app.setStyleSheet(ui::themeQss());

    // 单实例：已有工作台在跑就唤醒它并退出，避免第二个实例因端口占用报错
    const QString instanceKey = singleInstanceKey();
    if (notifyExistingInstance(instanceKey)) return 0;
    QLocalServer::removeServer(instanceKey);  // 清理上次异常退出残留的 socket 文件
    QLocalServer instanceGuard;
    instanceGuard.listen(instanceKey);

    // 启动自检（防呆）：初始化前把本机环境问题用"人话"摆出来（可关闭、双语、
    // 附下一步操作），不静默失败也不崩溃；之后仍按原流程尝试初始化。
    int port = 8787;
    {
        std::string portEnv =
            ah::envOr({"MIDERHIVE_PORT", "AGENTHIVE_PORT", "ZCODE_PLATFORM_PORT"});
        if (!portEnv.empty()) port = std::atoi(portEnv.c_str());
    }
    const QString homeDir = QString::fromStdString(ah::defaultHomeDir());
    const QVector<ui::startup::Issue> issues = ui::startup::preflight(
        homeDir, port, homeDir + "/platform.db", homeDir + "/config/agents.json");
    if (!issues.isEmpty()) {
        QMessageBox warnBox(QMessageBox::Warning, i18n::trs("MiderHive 启动自检", "MiderHive "
                                                                          "startup check"),
                            ui::startup::joinIssues(issues), QMessageBox::Close);
        warnBox.exec();  // 可关闭：用户确认后继续启动，问题同时保留在总览页健康横幅
    }

    ah::Platform platform(ah::defaultHomeDir());
    std::string err;
    if (!platform.bootstrap(err)) {
        QMessageBox::critical(nullptr, "MiderHive 工作台",
                              i18n::trs("平台初始化失败，工作台无法继续运行。",
                                        "Platform initialization failed; the workbench cannot "
                                        "continue.") +
                                  "\n" + ui::humanError(QString::fromStdString(err)));
        return 1;
    }

    // 内置 HTTP 服务供 Agent 接入（仅绑定 127.0.0.1）
    std::string serr;
    if (!platform.startHttpServer(port, serr)) {
        // 端口被占用已在自检里说明过，这里不再重复弹窗（横幅会持续可见）
        const bool alreadyTold = std::any_of(issues.begin(), issues.end(),
                                             [](const ui::startup::Issue& i) {
                                                 return i.code == "port_occupied";
                                             });
        if (!alreadyTold)
            QMessageBox::warning(nullptr, "MiderHive 工作台",
                                 i18n::trs("HTTP 服务启动失败，Agent 将无法接入。",
                                           "HTTP service failed to start; agents cannot connect.") +
                                     "\n" + ui::humanError(QString::fromStdString(serr)));
    }

    MainWindow w(platform);
    w.setMinimumSize(1080, 680);
    // 无历史几何时首次运行最大化：总览页四行卡片在 1080x680 下放不下，会需要滚动。
    // 注意不能用 showMaximized() 后再 show()——后者会把窗口状态重置回普通尺寸，
    // 应先把最大化写进 windowState 再统一 show()。用户调整过窗口后，
    // 几何写入 ui/geometry，后续启动按上次的尺寸恢复。
    if (!QSettings().contains("ui/geometry")) w.setWindowState(Qt::WindowMaximized);
    // 第二个实例来敲门时：把现有窗口显示/置前（而不是新开一个）
    QObject::connect(&instanceGuard, &QLocalServer::newConnection, &w, [&instanceGuard, &w] {
        while (QLocalSocket* c = instanceGuard.nextPendingConnection()) {
            c->deleteLater();
            w.setVisible(true);
            w.raise();
            w.activateWindow();
        }
    });
    w.show();
    return app.exec();
}
