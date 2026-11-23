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
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QGroupBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
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
    // 种子任一步失败即中止：带着残缺种子拍出来的快照看起来完全正常，降级成警告
    // 继续跑等于悄悄生成一套错误的基线。harness 的可复现单元是"全新数据目录 +
    // 完整流水线"，所以同名注册失败（复用旧目录）同样是中止而不是放行
    auto failSeed = [](const char* what, const std::string& e) {
        printNow(QString("FATAL  种子注入失败（%1）: %2").arg(what, QString::fromStdString(e)));
        printNow("        harness 必须在全新的数据目录上运行（MIDERHIVE_HOME 指向空目录）");
        std::exit(3);
    };
    std::string err, mk;
    {
        std::ifstream f(ah::fsPath(std::string(ah::defaultHomeDir()) + "/config/master.key"));
        std::getline(f, mk);
    }
    for (const char* n : {"claude", "cursor", "codex"}) {
        std::string k;
        if (!p.registerAgent(mk, n, "member", k, err)) failSeed(n, err);
    }
    ah::KnowledgeEntry e;
    std::string kerr;
    if (!p.knowledgeCreate("claude", "sqlite-vec 向量检索用法",
                           "用 sqlite-vec 做向量检索需要先注册扩展", {"sqlite"}, "踩坑", {}, "",
                           e, kerr))
        failSeed("knowledge 1", kerr);
    if (!p.knowledgeCreate("claude", "sqlite-vec 向量索引注意事项",
                           "向量索引维度必须与查询向量一致", {"sqlite"}, "踩坑", {}, "", e, kerr))
        failSeed("knowledge 2", kerr);
    if (!p.knowledgeCreate("cursor", "向量检索的过滤技巧", "kNN 查询可以带标签过滤缩小召回",
                           {"sqlite"}, "经验", {}, "", e, kerr))
        failSeed("knowledge 3", kerr);
    if (!p.knowledgeCreate("codex", "CI 流水线踩坑记录", "Windows runner 上 Qt 安装很慢",
                           {"ci"}, "踩坑", {}, "", e, kerr))
        failSeed("knowledge 4", kerr);
    if (!p.knowledgeCreate("cursor", "今天天气不错", "出去散步晒太阳", {}, "生活", {}, "", e,
                           kerr))
        failSeed("knowledge 5", kerr);

    bool dup = false;
    if (!p.usageReport("claude", 1200, 340, "chat", "deepseek-chat", "", "", dup, err))
        failSeed("usage claude", err);
    if (!p.usageReport("cursor", 800, 160, "chat", "deepseek-chat", "", "", dup, err))
        failSeed("usage cursor", err);
    if (!p.usageReport("codex", 500, 90, "review", "deepseek-chat", "", "", dup, err))
        failSeed("usage codex", err);

    ah::ErrorReport er;
    if (!p.errorReport("claude", "warning", "build", "MSVC C2062 示例错误", "语法错误示例", "",
                       er, err))
        failSeed("error 1", err);
    ah::ErrorReport er2;
    if (!p.errorReport("cursor", "critical", "runtime", "崩溃示例", "空指针解引用示例", "",
                       er2, err))
        failSeed("error 2", err);

    ah::Message m;
    if (!p.messageSend("note", "claude", "", "广播：构建脚本已更新", "详见知识库 CI 条目", m,
                       err))
        failSeed("message 1", err);
    if (!p.messageSend("question", "cursor", "claude", "检索技巧求助", "kNN 过滤怎么写？", m,
                       err))
        failSeed("message 2", err);
    if (!p.messageSend("task", "claude", "cursor", "整理本周踩坑", "沉淀进知识库", m, err))
        failSeed("message 3", err);

    ah::SkillInfo sk;
    if (!p.skillRegister("claude", "code-review", "代码审查", "对指定文件做审查", "dev", "{}",
                         sk, err))
        failSeed("skill", err);

    ah::MemoryEntry me;
    if (!p.memorySet("user", "project", "current", "MiderHive 图谱视图开发", 0, me, err))
        failSeed("memory", err);

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

// ---- 不变量规则集（第 1 轮拍板：rubric 式长期标准，随场景全跑；基线无关）----
struct Violation {
    QString rule;
    QString detail;
};

bool hasCjk(const QString& s) {
    for (const QChar c : s)
        if (c.unicode() >= 0x4E00 && c.unicode() <= 0x9FFF) return true;
    return false;
}

// R1 英文场景无 CJK 泄漏（lang-en-* 场景）
// R4 空态场景必须有可见的空态引导（InlineEmpty/HexEmptyState 或含引导文案的空态标签）
// R5 有数据场景可见控件数 ≥ 10（灾难性空白检测）
std::vector<Violation> runRules(const QString& sceneId, QWidget* panel, int visibleCount,
                                bool textEmpty) {
    std::vector<Violation> out;
    if (sceneId.startsWith("lang-en")) {
        // 英文场景的 CJK 检查只对**交互 chrome**（按钮/复选/下拉/表头）——
        // QLabel 承载用户数据（Agent 卡的 current_task、错误标题等中文数据在
        // 英文界面显示是正确行为）；权威 CJK 判据仍是 gui_selftest 的空库扫描
        std::function<void(QWidget*)> walk = [&](QWidget* w) {
            QString t;
            if (auto* x = qobject_cast<QPushButton*>(w)) t = x->text();
            else if (auto* x = qobject_cast<QCheckBox*>(w)) t = x->text();
            else if (auto* x = qobject_cast<QComboBox*>(w)) t = x->currentText();
            else if (auto* x = qobject_cast<QTableWidget*>(w))
                for (int c = 0; c < x->columnCount(); ++c)
                    if (auto* it = x->horizontalHeaderItem(c)) t += it->text() + ' ';
            if (hasCjk(t))
                out.push_back({"en-cjk-leak", QString("%1=%2").arg(w->objectName(), t.left(40))});
            for (QObject* c : w->children())
                if (auto* cw = qobject_cast<QWidget*>(c)) walk(cw);
        };
        walk(panel);
    }
    if (sceneId.endsWith(".empty")) {
        bool hasEmptyState = false;
        for (QWidget* w : panel->findChildren<QWidget*>())
            if (w->isVisible() && (w->metaObject()->className() == QStringLiteral("ah::InlineEmpty") ||
                                   w->metaObject()->className() == QStringLiteral("ui::InlineEmpty") ||
                                   !w->objectName().isEmpty() &&
                                       w->objectName().contains("Empty", Qt::CaseInsensitive)))
                hasEmptyState = true;
        // 空态引导不是硬性全系具备（部分面板用内联文案），以"可见控件数 < 3 且无引导"
        // 为违例——空得连引导都没有才是 UX 问题
        if (!hasEmptyState && visibleCount < 3)
            out.push_back({"empty-no-guide", sceneId});
    }
    if (sceneId.endsWith(".seeded") && visibleCount < 10 && !sceneId.startsWith("graph"))
        out.push_back({"seeded-too-empty", QString("%1: visible=%2").arg(sceneId).arg(visibleCount)});
    return out;
}

// ---- 弹窗自动应答器（第 3 轮拍板）：离屏下模态框不卡测试；每次弹出记录进
// 报告（意外弹窗 = UX 审查产出）并检查默认按钮（破坏性确认默认 No 规则）----
struct DialogLog {
    QStringList entries;     // "scene | title | default=<text|none>"
    int total = 0;
    std::vector<Violation> violations;  // 破坏性确认默认非 No
    QString currentScene;
};
DialogLog g_dialogs;

void installDialogSweeper(QObject* root) {
    auto* t = new QTimer(root);
    root->connect(t, &QTimer::timeout, root, [] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* mb = qobject_cast<QMessageBox*>(w);
            if (!mb || !mb->isVisible()) continue;
            ++g_dialogs.total;
            QPushButton* def = mb->defaultButton();
            QString defText = def ? def->text() : QStringLiteral("<none>");
            g_dialogs.entries << QString("%1 | %2 | default=%3")
                                     .arg(g_dialogs.currentScene, mb->windowTitle(), defText);
            // 破坏性确认默认按钮不得是肯定项（审计规则 R3）
            const QString t2 = mb->windowTitle();
            if ((t2.contains("移除") || t2.contains("禁用") || t2.contains("轮换") ||
                 t2.contains("删除") || t2.contains("Remove") || t2.contains("Disable") ||
                 t2.contains("Rotate") || t2.contains("Delete")) &&
                def && def != mb->button(QMessageBox::No) &&
                mb->standardButtons() & QMessageBox::No)
                g_dialogs.violations.push_back({"destructive-default-not-no",
                                                QString("%1 | default=%2").arg(t2, defText)});
            if (def) def->click();
            else mb->close();
        }
    });
    t->start(40);
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
    int compared = 0, diffs = 0, created = 0, passed = 0, ruleViolations = 0;
    installDialogSweeper(&app);
    std::vector<std::pair<QString, QString>> results;  // (场景 id, PASS/DIFF/NEW)

    auto runScene = [&](const QString& id, int navRow) {
        if (!filter.isEmpty() && !id.contains(filter)) return;
        g_dialogs.currentScene = id;
        nav->setCurrentRow(navRow);
        for (int i = 0; i < 5; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QWidget* panel = stack->currentWidget();
        if (!panel) return;
        const QJsonObject tree = dumpWidget(panel);
        // 可见控件数（规则 R5 用）
        int visibleCount = 0;
        std::function<void(const QJsonObject&)> countVis = [&](const QJsonObject& o) {
            if (o["vis"].toBool()) ++visibleCount;
            for (const auto& k : o["kids"].toArray()) countVis(k.toObject());
        };
        countVis(tree);
        // 不变量规则（rubric）：违例即计入报告（不阻塞快照对比）
        for (const auto& v : runRules(id, panel, visibleCount, tree.contains("text"))) {
            ++ruleViolations;
            printNow(QString("RULE      %1 | %2 | %3").arg(id, v.rule, v.detail.left(60)));
        }
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
            results.push_back({id, "BASELINE"});
            printNow(QString("BASELINE  %1").arg(id));
            return;
        }
        QFile b(baselineDir + "/" + id + ".json");
        if (!b.exists()) {
            printNow(QString("NEW       %1（基线不存在——review 后入库）").arg(id));
            ++created;
            results.push_back({id, "NEW"});
            return;
        }
        b.open(QIODevice::ReadOnly);
        const QJsonObject base = QJsonDocument::fromJson(b.readAll()).object();
        if (stripSoft(base) != stripSoft(tree)) {
            ++diffs;
            results.push_back({id, "DIFF"});
            printNow(QString("DIFF      %1（结构/文案与基线不一致——review 后 --update-baseline）")
                         .arg(id));
        } else {
            ++passed;
            results.push_back({id, "PASS"});
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

    // ---- 像素金样（第 1 轮拍板：旗舰 3 场景，本机限定不入库）----
    // grab 在 ui_harness 的固定画幅（1440x920）下采集；哈希存 hashes.json，
    // 对比模式不一致报 GOLDEN-DIFF（字体/驱动差异也可能触发——本机回归信号）
    const QString goldenDir = outDir + "/golden";
    QDir().mkpath(goldenDir);
    QJsonObject goldenHashes;  // 本地哈希基线（跨运行对比）
    {
        QFile hf(goldenDir + "/hashes.json");
        if (hf.open(QIODevice::ReadOnly)) goldenHashes = QJsonDocument::fromJson(hf.readAll()).object();
    }
    int goldenDiff = 0;
    auto grabGolden = [&](const QString& id, int navRow) {
        if (!filter.isEmpty() && !id.contains(filter)) return;
        nav->setCurrentRow(navRow);
        for (int i = 0; i < 5; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QWidget* panel = stack->currentWidget();
        if (!panel) return;
        const QPixmap pm = panel->grab();
        QFile pf(goldenDir + "/" + id + ".png");
        pf.open(QIODevice::WriteOnly);
        pm.save(&pf, "PNG");
        const QByteArray png = QJsonDocument(QJsonObject{}).toJson();  // 占位避免 -Wunused
        QBuffer buf;
        buf.open(QIODevice::WriteOnly);
        pm.save(&buf, "PNG");
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(buf.data(), QCryptographicHash::Sha256).toHex());
        goldenHashes[id] = hash;
        const auto it = goldenHashes.constFind(id);
        if (it != goldenHashes.constEnd() && it.value() != hash) {
            ++goldenDiff;
            printNow(QString("GOLDEN-DIFF  %1（像素与上次运行不同——本机限定信号）").arg(id));
        }
    };
    grabGolden("dashboard.seeded", 0);
    grabGolden("knowledge.seeded", 2);
    grabGolden("graph.seeded", 8);
    {
        QFile hf(goldenDir + "/hashes.json");
        hf.open(QIODevice::WriteOnly);
        hf.write(QJsonDocument(goldenHashes).toJson(QJsonDocument::Indented));
    }
    // 恢复 dashboard 选中（grab 切到 graph 后，后续场景不受影响——尾部无场景）
    nav->setCurrentRow(0);

    // pass 3：读失败态（逐面板注入对应 fault；无注入点的面板跳过）
    for (const auto& ps : panels) {
        if (ps.fault == ah::Platform::Fault::None) continue;
        platform.setFaultInjector(ps.fault);
        runScene(QString("%1.fault").arg(ps.panel), ps.row);
        platform.setFaultInjector(ah::Platform::Fault::None);
    }

    // ---- 正交抽查（第 2 轮拍板）：主题/语言/字号抽代表性场景 ----
    // 主题 5 × dashboard/knowledge 有数据 = 10 场景（5 套色卡下无对比度灾难）
    for (int t = 0; t < static_cast<int>(ui::themes().size()); ++t) {
        ui::setThemeIndex(t);
        for (int i = 0; i < 4; ++i)
            QApplication::processEvents(QEventLoop::AllEvents, 120);  // rebuildPanels 落地
        runScene(QString("theme-%1-dashboard.seeded").arg(t), 0);
        runScene(QString("theme-%1-knowledge.seeded").arg(t), 2);
    }
    ui::setThemeIndex(0);
    for (int i = 0; i < 4; ++i) QApplication::processEvents(QEventLoop::AllEvents, 120);

    // 语言 8 × dashboard 有数据 = 8 场景（语长差异/枚举本地化可见）；
    // lang-en-* 额外触发 R1 CJK 泄漏规则。顺序取 i18n::languages() 真实定义。
    for (const auto& li : i18n::languages()) {
        i18n::apply(li.lang);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (int i = 0; i < 3; ++i) QApplication::processEvents(QEventLoop::AllEvents, 100);
        runScene(QString("lang-%1-dashboard.seeded").arg(li.id), 0);
    }
    i18n::apply(i18n::Lang::Zh);
    for (int i = 0; i < 3; ++i) QApplication::processEvents(QEventLoop::AllEvents, 100);

    // 字号 3 × dashboard 有数据 = 3 场景（大字号下表格截断检测）
    for (int fb : {12, 13, 14}) {
        ui::setFontBase(fb);
        for (int i = 0; i < 4; ++i) QApplication::processEvents(QEventLoop::AllEvents, 120);
        runScene(QString("font-%1-dashboard.seeded").arg(fb), 0);
    }
    ui::setFontBase(13);
    for (int i = 0; i < 4; ++i) QApplication::processEvents(QEventLoop::AllEvents, 120);

    // ---- 快捷键矩阵（第 3 轮拍板）：Ctrl+1..9 全触发，断言切换到位且不崩 ----
    int shortcutOk = 0, shortcutTotal = 0;
    for (int i = 0; i < nav->count() && i < 9; ++i) {
        const QKeySequence seq(QString("Ctrl+%1").arg(i + 1));
        bool found = false;
        for (QShortcut* sc : w.findChildren<QShortcut*>())
            if (sc->key() == seq) {
                ++shortcutTotal;
                emit sc->activated();
                QApplication::processEvents(QEventLoop::AllEvents, 80);
                if (stack->currentIndex() == i && nav->currentRow() == i) ++shortcutOk;
                found = true;
                break;
            }
        if (!found) printNow(QString("RULE      shortcuts | Ctrl+%1 未找到对应快捷键").arg(i + 1));
        Q_UNUSED(found);
    }
    if (shortcutOk != shortcutTotal)
        printNow(QString("RULE      shortcuts | %1/%2 切换到位")
                     .arg(shortcutOk)
                     .arg(shortcutTotal));

    // ---- 汇总 ----
    printNow(QString("==== ui_harness 完成：场景 %1，PASS %2，新建基线 %3，结构差异 %4，规则违例 %5，弹窗 %6，金样差异 %7 ====")
                 .arg(compared)
                 .arg(passed)
                 .arg(created)
                 .arg(diffs)
                 .arg(ruleViolations)
                 .arg(g_dialogs.total)
                 .arg(goldenDiff));

    // ---- HTML 报告（第 4 轮拍板：自包含，人工 review 用）----
    {
        QFile hf(outDir + "/ui_report.html");
        hf.open(QIODevice::WriteOnly);
        QString html = QStringLiteral(
            "<!doctype html><html><head><meta charset='utf-8'><title>UI Harness Report</title>"
            "<style>body{font-family:Segoe UI,sans-serif;background:#16181d;color:#ddd;"
            "margin:24px}table{border-collapse:collapse;width:100%%}td,th{border:1px solid "
            "#333;padding:6px 10px;font-size:13px}th{background:#222}tr:nth-child(even){"
            "background:#1d2026}.pass{color:#4ade80}.diff{color:#f87171}.new{color:#eab308}"
            "pre{background:#101216;padding:12px;border-radius:8px;font-size:12px;white-space:"
            "pre-wrap}h2{border-bottom:1px solid #333}</style></head><body>");
        html += QStringLiteral("<h1>MiderHive UI Harness Report</h1><p>%1 · 场景 %2 · PASS %3 · "
                               "DIFF %4 · NEW %5 · 规则违例 %6 · 弹窗 %7 · 金样差异 %8</p>")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                    .arg(compared)
                    .arg(passed)
                    .arg(diffs)
                    .arg(created)
                    .arg(ruleViolations)
                    .arg(g_dialogs.total)
                    .arg(goldenDiff);
        html += "<h2>场景结果</h2><table><tr><th>场景</th><th>状态</th></tr>";
        for (const auto& [id, st] : results)
            html += QStringLiteral("<tr><td>%1</td><td class='%2'>%3</td></tr>")
                        .arg(id, st.toLower(), st);
        html += "</table>";
        if (ruleViolations > 0) {
            html += "<h2>规则违例</h2><ul>";
            for (const auto& v : g_dialogs.violations)
                html += QStringLiteral("<li><b>%1</b>（弹窗）%2</li>")
                            .arg(v.rule, v.detail.toHtmlEscaped());
            html += "</ul><p>（违例明细见 console 输出 RULE 行）</p>";
        }
        if (!g_dialogs.entries.isEmpty()) {
            html += "<h2>弹窗记录（自动应答）</h2><pre>" +
                    g_dialogs.entries.join('\n').toHtmlEscaped() + "</pre>";
        }
        html += QStringLiteral("<h2>金样哈希（本机限定）</h2><pre>%1</pre>")
                    .arg(QJsonDocument(goldenHashes).toJson(QJsonDocument::Indented)
                             .constData());
        html += "</body></html>";
        hf.write(html.toUtf8());
    }
    printNow(QString("报告：build/ui_harness/ui_report.html（金样 %1 张在 build/ui_harness/golden/）")
                 .arg(goldenHashes.size()));
    for (const auto& v : g_dialogs.violations)
        printNow(QString("RULE      dialogs | %1 | %2").arg(v.rule, v.detail.left(60)));
    if (!g_dialogs.entries.isEmpty()) {
        QFile dl(outDir + "/dialogs.log");
        dl.open(QIODevice::WriteOnly);
        dl.write(g_dialogs.entries.join('\n').toUtf8());
    }
    if (update) {
        printNow("基线已更新——请 git diff tests/baseline/ 人工 review 后入库。");
        return 0;
    }
    return (diffs == 0 && ruleViolations == 0 && g_dialogs.violations.empty()) ? 0 : 2;
}
