// ---- UI/UX 评测 harness（LLM eval 式）：测试集 → 离屏执行 → 基线对比 → 报告 ----
// 与 gui_selftest 的分工：selftest 管「功能对不对」（行为断言），本 harness 管
// 「长什么样、交互顺不顺」（结构基线 + 不变量规则 + 交互审查）。
//
// 三种采集（2026-11-21 讨论拍板的混合基线）：
//   ① 结构快照：控件树序列化为 JSON（class/objectName/text/visible/几何），与
//      tests/baseline/<场景>.json 对比——硬段（文本/结构/可见性）跨机稳定，
//      软段（几何，字体相关）仅本机对比
//   ② 不变量规则：英文无 CJK 泄漏、控件越界、破坏性确认默认 No 等（提交 2）
//   ③ 像素金样：旗舰场景 QWidget::grab 哈希，本地目录（不入库）（提交 3）
//
// 场景矩阵（第 2 轮拍板）：核心 27 = 9 面板 × 3 状态（empty/seeded/fault），
// 三态用三遍扫描实现——空态（种子注入前）→ 注确定性种子 → 有数据态 → 故障
// 注入态（Platform::setFaultInjector，harness 独享）。
//
// 确定性（第 5 轮拍板）：种子数据经 Platform API 注入后，用第二个数据库连接
// 把全部时间列 UPDATE 为固定常量（固定时间戳），且快照序列化时把相对时间
// 文案归一化为 <rel>——基线不随真实时间漂移。
//
// 用法：
//   ui_harness                          # 对比模式（与基线比，报告差异）
//   ui_harness --update-baseline        # 重新生成基线（人工 review diff 后入库）
//   ui_harness --filter dashboard       # 只跑 id 含 dashboard 的场景
//
// 运行环境：QT_QPA_PLATFORM=offscreen + MIDERHIVE_HOME 指向隔离目录
// （CI/包装脚本设置；harness 自身兜底 qputenv offscreen）。
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/platform.h"
#include "core/util.h"
#include "gui/i18n.h"
#include "gui/mainwindow.h"
#include "gui/theme.h"

namespace {

void printNow(const QString& s) {
    const QByteArray u = s.toUtf8();
    fwrite(u.constData(), 1, u.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// ---- 确定性种子（第 5 轮拍板：固定时间戳）----
// 经 Platform API 注入（走正规校验/审计路径）后，用第二个数据库连接把时间列
// 统一 UPDATE 成常量。WAL 下双连接可写；短连接用完即关。
constexpr const char* kT0 = "2026-11-01T00:00:00Z";   // 基准时刻
constexpr const char* kSeen = "2026-10-25T00:00:00Z"; // last_seen/逐日用量时刻

void seedFixTimestamps(const std::string& dbPath) {
    ah::Database raw;
    std::string err;
    if (!raw.open(dbPath, err)) {
        printNow(QString("FATAL  种子时间戳修正失败（开库）: %1").arg(QString::fromStdString(err)));
        std::exit(1);
    }
    const std::string stmts[] = {
        std::string("UPDATE agents SET created_at='") + kT0 + "', updated_at='" + kT0 +
            "', last_seen_at='" + kSeen + "';",
        std::string("UPDATE knowledge_entries SET created_at='") + kT0 + "';",
        std::string("UPDATE messages SET created_at='") + kT0 + "';",
        std::string("UPDATE errors SET created_at='") + kT0 + "';",
        std::string("UPDATE skills SET created_at='") + kT0 + "';",
        std::string("UPDATE skill_invocations SET created_at='") + kT0 + "';",
        std::string("UPDATE memory_entries SET created_at='") + kT0 + "';",
        std::string("UPDATE audit_log SET created_at='") + kT0 + "';",
        std::string("UPDATE token_usage SET created_at='") + kSeen + "';",
    };
    for (const auto& sql : stmts)
        if (!raw.execScript(sql, err)) {
            printNow(QString("FATAL  时间戳修正失败: %1 (%2)")
                         .arg(QString::fromStdString(err), QString::fromStdString(sql)));
            std::exit(1);
        }
    raw.close();
}

void seedData(ah::Platform& p, const std::string& dbPath) {
    std::string err, mk;
    {
        std::ifstream f(ah::fsPath(std::string(ah::defaultHomeDir()) + "/config/master.key"));
        std::getline(f, mk);
    }
    for (const char* n : {"claude", "cursor", "codex"}) {
        std::string k;
        if (!p.registerAgent(mk, n, "member", k, err))
            printNow(QString("FATAL  种子注册失败 %1: %2").arg(n, QString::fromStdString(err)));
    }
    ah::KnowledgeEntry e;
    std::string kerr;
    p.knowledgeCreate("claude", "sqlite-vec 向量检索用法", "用 sqlite-vec 做向量检索需要先注册扩展",
                      {"sqlite"}, "踩坑", {}, "", e, kerr);
    p.knowledgeCreate("claude", "sqlite-vec 向量索引注意事项", "向量索引维度必须与查询向量一致",
                      {"sqlite"}, "踩坑", {}, "", e, kerr);
    p.knowledgeCreate("cursor", "向量检索的过滤技巧", "kNN 查询可以带标签过滤缩小召回",
                      {"sqlite"}, "经验", {}, "", e, kerr);
    p.knowledgeCreate("codex", "CI 流水线踩坑记录", "Windows runner 上 Qt 安装很慢",
                      {"ci"}, "踩坑", {}, "", e, kerr);
    p.knowledgeCreate("cursor", "今天天气不错", "出去散步晒太阳", {}, "生活", {}, "", e, kerr);

    bool dup = false;
    p.usageReport("claude", 1200, 340, "chat", "deepseek-chat", "", "", dup, err);
    p.usageReport("cursor", 800, 160, "chat", "deepseek-chat", "", "", dup, err);
    p.usageReport("codex", 500, 90, "review", "deepseek-chat", "", "", dup, err);

    ah::ErrorReport er;
    p.errorReport("claude", "warning", "build", "MSVC C2062 示例错误", "语法错误示例", "", er, err);
    ah::ErrorReport er2;
    p.errorReport("cursor", "critical", "runtime", "崩溃示例", "空指针解引用示例", "", er2, err);

    ah::Message m;
    p.messageSend("note", "claude", "", "广播：构建脚本已更新", "详见知识库 CI 条目", m, err);
    p.messageSend("question", "cursor", "claude", "检索技巧求助", "kNN 过滤怎么写？", m, err);
    p.messageSend("task", "claude", "cursor", "整理本周踩坑", "沉淀进知识库", m, err);

    ah::SkillInfo sk;
    p.skillRegister("claude", "code-review", "代码审查", "对指定文件做审查", "dev", "{}", sk, err);

    ah::MemoryEntry me;
    p.memorySet("user", "project", "current", "MiderHive 图谱视图开发", 0, me, err);

    p.heartbeat("claude", "整理本周踩坑");
    p.heartbeat("cursor", "kNN 过滤调研");

    seedFixTimestamps(dbPath);
}

// ---- 控件树 JSON 序列化 ----
// 硬段：class/objectName/text/visible/enabled + 树形结构（跨机稳定，进基线）
// 软段：geom（字体相关，仅本机对比）
// 相对时间归一化：relTime 依赖真实 now——固定种子时间戳下仍随真实时间漂移
// （"N 天前"），序列化时归一为 <rel>。
QString normText(QString s) {
    static const QRegularExpression rel(R"(\b\d+\s*(?:天|小时|分钟|秒|d|h|m|min)s?\b)");
    s.replace(rel, QStringLiteral("<rel>"));
    return s;
}

QJsonObject dumpWidget(QWidget* w) {
    QJsonObject o;
    o["class"] = w->metaObject()->className();
    if (!w->objectName().isEmpty()) o["name"] = w->objectName();
    o["vis"] = w->isVisible();
    o["en"] = w->isEnabled();
    if (auto* x = qobject_cast<QLabel*>(w)) o["text"] = normText(x->text());
    else if (auto* x = qobject_cast<QPushButton*>(w)) o["text"] = normText(x->text());
    else if (auto* x = qobject_cast<QCheckBox*>(w)) o["text"] = normText(x->text());
    else if (auto* x = qobject_cast<QLineEdit*>(w)) o["text"] = normText(x->text());
    else if (auto* x = qobject_cast<QPlainTextEdit*>(w)) o["text"] = normText(x->toPlainText());
    else if (auto* x = qobject_cast<QComboBox*>(w)) o["text"] = normText(x->currentText());
    else if (auto* x = qobject_cast<QGroupBox*>(w)) o["text"] = normText(x->title());
    else if (auto* x = qobject_cast<QListWidget*>(w)) o["items"] = static_cast<int>(x->count());
    else if (auto* x = qobject_cast<QTableWidget*>(w)) {
        o["rows"] = static_cast<int>(x->rowCount());
        o["cols"] = static_cast<int>(x->columnCount());
    }
    const QRect g = w->geometry();
    o["geom"] = QJsonArray{g.x(), g.y(), g.width(), g.height()};
    QJsonArray kids;
    for (QObject* c : w->children())
        if (auto* cw = qobject_cast<QWidget*>(c)) kids.append(dumpWidget(cw));
    if (!kids.isEmpty()) o["kids"] = kids;
    return o;
}

QJsonObject stripSoft(const QJsonObject& o) {
    QJsonObject c = o;
    c.remove("geom");
    if (c.contains("kids")) {
        QJsonArray kids;
        for (const auto& k : c["kids"].toArray()) kids.append(stripSoft(k.toObject()));
        c["kids"] = kids;
    }
    return c;
}

// ---- 场景矩阵（第 2 轮拍板：核心 27）----
struct PanelSpec {
    const char* panel;
    int row;                          // 导航行（与 panelFactories_ 一致）
    ah::Platform::Fault fault;        // 读失败态注入点（None = 只跑 empty/seeded）
};

}  // namespace

int main(int argc, char** argv) {
    // 离屏兜底：外部（包装脚本/CI）已设 QT_QPA_PLATFORM 时尊重外部值
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("miderhive");
    QApplication::setApplicationName("MiderHive 多 Agent 协作工作台");

    // ---- 参数 ----
    bool update = false;
    QString filter, baselineDir = QStringLiteral("tests/baseline"),
                   outDir = QStringLiteral("build/ui_harness");
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--update-baseline") update = true;
        else if (a == "--filter" && i + 1 < argc) filter = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--baseline-dir" && i + 1 < argc)
            baselineDir = QString::fromLocal8Bit(argv[++i]);
    }

    // ---- 隔离（照 gui_selftest 惯例）：QSettings 落临时 ini；关自动更新与
    // 引导（harness 无弹窗约束）；加快刷新轮询 ----
    const QString home = QString::fromStdString(ah::defaultHomeDir());
    const QString settingsDir = home + "/ui-settings";
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    {
        QSettings s;
        s.setValue("ui/autoCheck", false);
        s.setValue("ui/refreshMs", 1000);
        s.setValue("ui/welcomeSeen", true);
        s.sync();
    }
    i18n::load();

    ah::Platform platform(ah::defaultHomeDir());
    std::string err;
    if (!platform.bootstrap(err)) {
        printNow(QString("FATAL  平台初始化失败: %1").arg(QString::fromStdString(err)));
        return 1;
    }

    MainWindow w(platform);
    w.setMinimumSize(1080, 680);
    w.resize(1440, 920);  // 固定画幅：几何基线稳定
    w.show();
    // 稳定一拍：构造期的 DeferredDelete 与首次 refresh 落地
    for (int i = 0; i < 6; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 120);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    auto* stack = w.findChild<QStackedWidget*>();
    auto* nav = w.findChild<QListWidget*>();
    if (!stack || !nav) {
        printNow("FATAL  找不到主窗口导航/堆叠（结构变化？）");
        return 1;
    }

    const std::vector<PanelSpec> panels = {
        {"dashboard", 0, ah::Platform::Fault::AgentList},
        {"usage", 1, ah::Platform::Fault::UsageSummary},
        {"knowledge", 2, ah::Platform::Fault::KnowledgeList},
        {"skills", 3, ah::Platform::Fault::SkillList},
        {"memory", 4, ah::Platform::Fault::None},   // memoryList 无注入点：只跑 empty/seeded
        {"messages", 5, ah::Platform::Fault::MessageList},
        {"errors", 6, ah::Platform::Fault::ErrorList},
        {"logs", 7, ah::Platform::Fault::AuditList},
        {"graph", 8, ah::Platform::Fault::None},    // snapshot 无注入点：只跑 empty/seeded
    };

    QDir().mkpath(outDir);
    QDir().mkpath(baselineDir);
    int compared = 0, diffs = 0, created = 0, passed = 0;

    auto runScene = [&](const QString& id, int navRow) {
        if (!filter.isEmpty() && !id.contains(filter)) return;
        nav->setCurrentRow(navRow);
        for (int i = 0; i < 5; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QWidget* panel = stack->currentWidget();
        if (!panel) return;
        const QJsonObject tree = dumpWidget(panel);
        QFile f(outDir + "/" + id + ".json");
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(tree).toJson(QJsonDocument::Indented));
        f.close();
        ++compared;
        if (update) {
            QFile b(baselineDir + "/" + id + ".json");
            b.open(QIODevice::WriteOnly);
            b.write(QJsonDocument(tree).toJson(QJsonDocument::Indented));
            ++created;
            printNow(QString("BASELINE  %1").arg(id));
            return;
        }
        QFile b(baselineDir + "/" + id + ".json");
        if (!b.exists()) {
            printNow(QString("NEW       %1（基线不存在——review 后入库）").arg(id));
            ++created;
            return;
        }
        b.open(QIODevice::ReadOnly);
        const QJsonObject base = QJsonDocument::fromJson(b.readAll()).object();
        if (stripSoft(base) != stripSoft(tree)) {
            ++diffs;
            printNow(QString("DIFF      %1（结构/文案与基线不一致——review 后 --update-baseline）")
                         .arg(id));
        } else {
            ++passed;
            printNow(QString("PASS      %1").arg(id));
        }
    };

    // pass 1：空态（bootstrap 后、种子前；zcode 管理者已存在——dashboard 记录它）
    for (const auto& ps : panels) runScene(QString("%1.empty").arg(ps.panel), ps.row);

    // 注确定性种子 + 固定时间戳
    const std::string dbPath = std::string(ah::defaultHomeDir()) + "/platform.db";
    seedData(platform, dbPath);
    for (int i = 0; i < 6; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 120);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // pass 2：有数据态
    for (const auto& ps : panels) runScene(QString("%1.seeded").arg(ps.panel), ps.row);

    // pass 3：读失败态（逐面板注入对应 fault；无注入点的面板跳过）
    for (const auto& ps : panels) {
        if (ps.fault == ah::Platform::Fault::None) continue;
        platform.setFaultInjector(ps.fault);
        runScene(QString("%1.fault").arg(ps.panel), ps.row);
        platform.setFaultInjector(ah::Platform::Fault::None);
    }

    // ---- 汇总 ----
    printNow(QString("==== ui_harness 完成：场景 %1，PASS %2，新建基线 %3，结构差异 %4 ====")
                 .arg(compared)
                 .arg(passed)
                 .arg(created)
                 .arg(diffs));
    if (update) {
        printNow("基线已更新——请 git diff tests/baseline/ 人工 review 后入库。");
        return 0;
    }
    return diffs == 0 ? 0 : 2;
}
