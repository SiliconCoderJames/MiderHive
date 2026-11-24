// ---- UI/UX 评测 harness（LLM eval 式）：测试集 → 离屏执行 → 基线对比 → 报告 ----
// 与 gui_selftest 的分工：selftest 管「功能对不对」（行为断言），本 harness 管
// 「长什么样、交互顺不顺」（结构基线 + 不变量规则 + 交互审查）。
//
// 采集四路（2026-11-21 讨论拍板的混合基线）：
//   ① 结构快照：控件树序列化为 JSON（class/objectName/text/visible/几何）+ 该面板的
//      Tab 焦点链，与 tests/baseline/<场景>.json 对比。参与对比的是硬段（文本/结构/
//      可见性/焦点链）；几何（geom）只录进快照供人看，不参与对比（字体与缩放相关）
//   ② 不变量规则（与基线无关，每次都查）：英文场景无 CJK 泄漏、空态有引导、有数据
//      不近乎空白、快捷键到位、标准控件能被 Tab 到达、样式表不残留占位符、容器样式
//      不波及子控件、破坏性确认默认 No、用户状态活过自动刷新
//   ③ 像素金样：旗舰场景 QWidget::grab 的像素哈希，本地目录（不入库）；只报告不计入
//      退出码
//   ④ 每个场景一张截图，进 HTML 报告供人工 review
//
// 场景矩阵（第 2 轮拍板）：核心 27 = 9 面板 × 3 状态（empty/seeded/fault），
// 三态用三遍扫描实现——空态（种子注入前）→ 注确定性种子 → 有数据态 → 故障
// 注入态（Platform::setFaultInjector，harness 独享）；另有主题 5×2 / 语言 8 / 字号 3
// 的正交抽查，和"内置服务未启动"时的总览（健康告警横幅只在那时出现）。
//
// 与真实入口（gui/main.cpp）同一套启动：应用字体、全局样式表、内置 HTTP 服务
// （临时端口）。少任何一步，拍到的就不是用户看到的界面。
//
// 确定性——换台机器、换一天、换个时区，快照必须逐字一致：
//   - 显示用的时钟冻结在"今天第一个合法的整点"（gui_util.h 的 displayNow）；种子时间取
//     "它减固定偏移"，
//     内容表各行再按 rowid 错开几秒（并列时间戳上的排序不稳定）。全部落在"今天/
//     本周"之内，而"最近 N 天"这类相对窗口永远包含它们
//   - 随"现在"变化的文案（8 种语言的相对时间、本地时间戳、日期）序列化时归一化
//   - 工具安装检测指向一棵固定的假目录树（配置根 + PATH），装了/没装不随机器变
//   - 按当天初始化的控件（日志面板的起始日期）钉到固定日期
//   - 每个场景拍之前显式同步刷新面板（主窗口只在导航行变化时刷新）
//   - 滚动区的脚手架（滚动条及其容器）不录进快照：显示与否取决于字体度量，显示时
//     还会被 raise 到子列表末尾——两类都是机器相关的事实
//
// 隔离（强制）：harness 会向数据库灌种子，绝不能碰真实数据目录。没设
// MIDERHIVE_HOME 就用一次性临时目录；设了但目录里已经有库则拒跑。
//
// 字体：Qt 的离屏平台在 Windows 上默认一个字体都不加载，文字全是空方块。harness
// 自己设 QT_QPA_FONTDIR（调用方设过则尊重），并在启动时自检字体与 SVG 插件都已加载。
//
// 不放行：基线缺失、过滤词一个场景都没命中、规则违例，都让退出码非零。
//
// 用法：
//   ui_harness                          # 对比模式（与基线比，报告差异）
//   ui_harness --update-baseline        # 重新生成基线（人工 review diff 后入库）
//   ui_harness --filter dashboard       # 只跑 id 含 dashboard 的子串场景
//
// 运行环境：QT_QPA_PLATFORM=offscreen（CI/包装脚本设置；harness 自身兜底 qputenv）
// + Qt 的 bin 在 PATH 上（DLL 与插件依赖）。像素金样与截图在 build/ui_harness/。
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateEdit>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHash>
#include <QHostAddress>
#include <QImageReader>
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
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QShortcut>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/db/database.h"
#include "core/integrations.hpp"
#include "core/platform.h"
#include "core/util.h"
#include "gui/i18n.h"
#include "gui/mainwindow.h"
#include "gui/panels/graph_panel.h"
#include "gui/panels/panel_base.h"
#include "gui/theme.h"

namespace {

void printNow(const QString& s) {
    const QByteArray u = s.toUtf8();
    fwrite(u.constData(), 1, u.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// ---- 确定性种子：时间取"现在减固定偏移" ----
// 经 Platform API 注入（走正规校验/审计路径）后，用第二个数据库连接把时间列
// 统一改写。WAL 下双连接可写；短连接用完即关。
//
// 曾经把时间列钉成绝对常量（2026-11-01 / 2026-10-25）。但界面的时间窗口是相对
// "现在"的（最近 7/14/30 天、本周、今天）：绝对常量过几周就掉出窗口——用量面板的
// "有数据"场景从 2026-11-08 起拍到的其实是一张空图，而基线对比照样 PASS。
// 取"现在减固定偏移"则永远落在所有窗口里；显示上的差异由 normText 归一化吸收
// （<rel> / <date> / <time>）。
// 偏移的取舍：要小（离周界、日界远），又要避开在线判定窗口（last_seen 在 120 秒
// 内算在线）——5 分钟稳妥地落在"离线"一侧，不会因为 harness 跑得快慢而翻转。
constexpr int kCreatedMinutesAgo = 30;  // created_at：30 分钟前
constexpr int kSeenMinutesAgo = 5;      // last_seen_at、token_usage：5 分钟前

// ---- 冻结的时钟 ----
// 界面显示用的"现在"固定在一个整点（gui_util.h 的 displayNow 读 MIDERHIVE_UI_NOW），上面的
// 偏移相对它取。于是相对时间文案（"30 分钟前"）与绝对时间戳在多次运行之间完全相同：
// 像素金样反映的是"同一份程序、同一份数据"，而不是"什么时候跑的"。
// 取"今天第一个合法的整点"，而不是"当前小时"：同一段时间里每轮的冻结时刻都一样，像素
// 金样才真的可比（取当前小时的话，每过一小时，带绝对时间戳的那张就报一次差异——一个
// 每小时都响的信号很快就被当噪声忽略掉）。"合法"指"整点减 30 分钟"仍然算今天：核心的
// "今日"统计按 UTC 日期、界面的"今日新增"按本地日期，用的都是真实时钟。所以从本地零点
// 起逐小时往后找，直到同时不早于本地午夜与 UTC 午夜（两个日期不在同一零点切换的时区
// 里，一天内会有两个取值）。种子时间戳因此可能比真实时间超前几个小时，只影响 harness
// 里的显示。
QDateTime g_frozenNow;

QDateTime freezeClock() {
    QDateTime frozen(QDateTime::currentDateTime().date().startOfDay());  // 本地零点起逐小时找
    QDateTime mustBeAfter = frozen;                                      // 本地午夜
    {
        const QDateTime utcMidnight(QDateTime::currentDateTimeUtc().date(), QTime(0, 0),
                                    QTimeZone::UTC);
        if (utcMidnight.toLocalTime() > mustBeAfter) mustBeAfter = utcMidnight.toLocalTime();
    }
    while (frozen.addSecs(-kCreatedMinutesAgo * 60) < mustBeAfter) frozen = frozen.addSecs(3600);
    qputenv("MIDERHIVE_UI_NOW", frozen.toUTC().toString(Qt::ISODate).toUtf8());
    return frozen;
}

void seedFixTimestamps(const std::string& dbPath) {
    ah::Database raw;
    std::string err;
    if (!raw.open(dbPath, err)) {
        printNow(QString("FATAL  种子时间戳修正失败（开库）: %1").arg(QString::fromStdString(err)));
        std::exit(1);
    }
    const std::time_t now = g_frozenNow.isValid() ? g_frozenNow.toSecsSinceEpoch()
                                                  : std::time(nullptr);
    const auto isoMinutesAgo = [now](int minutes) {
        const std::time_t t = now - static_cast<std::time_t>(minutes) * 60;
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32] = "";
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return std::string(buf);
    };
    const std::string t0 = isoMinutesAgo(kCreatedMinutesAgo);
    const std::string seen = isoMinutesAgo(kSeenMinutesAgo);
    // 内容表的行按 rowid 再错开若干秒。全都用同一个时间戳的话，"按时间倒序"在这些
    // 并列值上不保证稳定：知识库的行序、图谱节点的初始摆放顺序都可能随运行而变
    // （两次运行同一场景像素差 1%——布局形状相同，整体偏了几个像素）。
    // 错开之后顺序既确定，又都还在"30 分钟前后"这一档里
    const std::string stagger =
        "SET created_at = strftime('%Y-%m-%dT%H:%M:%SZ', created_at, "
        "'+' || (rowid % 600) || ' seconds');";
    const std::string stmts[] = {
        "UPDATE agents SET created_at='" + t0 + "', updated_at='" + t0 + "', last_seen_at='" +
            seen + "';",
        "UPDATE knowledge_entries SET created_at='" + t0 + "';",
        "UPDATE messages SET created_at='" + t0 + "';",
        "UPDATE errors SET created_at='" + t0 + "';",
        "UPDATE skills SET created_at='" + t0 + "';",
        "UPDATE skill_invocations SET created_at='" + t0 + "';",
        "UPDATE memory_entries SET created_at='" + t0 + "';",
        "UPDATE audit_log SET created_at='" + t0 + "';",
        "UPDATE token_usage SET created_at='" + seen + "';",
        "UPDATE knowledge_entries " + stagger,
        "UPDATE messages " + stagger,
        "UPDATE errors " + stagger,
        "UPDATE skills " + stagger,
        "UPDATE memory_entries " + stagger,
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
// 硬段：class/objectName/text/visible/enabled + 树形结构（参与基线对比）
// 软段：geom（字体与缩放相关；只录进快照供人看，不参与对比）
//
// 文案归一化：把随"现在"、时区、日历变化的文本换成固定记号，否则基线隔天、
// 跨时区就会漂移。
//   - 相对时间："12 分钟前" / "3 h ago" / 其余 6 种语言 → <rel>
//   - 本地时间戳："11-01 08:00"（relTime 对超过一周的时间回落到这个格式）、
//     "2026-11-01 08:00:00"（localStamp）→ <stamp>
//   - 时刻："08:00:00"（日志面板按天分组后的行）→ <time>
//   - 日期："2026-11-01" → <date>（同一条 UTC 记录在 UTC 以西的时区会落到前一天）
// 先长后短：完整时间戳先于裸时刻、裸日期被匹配。
//
// 相对时间的写法有 8 种语言，模板直接取自 i18n 表（和界面同源，不另抄一份；
// 漏一种语言就是那种语言的场景隔天必红）。
const QList<QRegularExpression>& relPatterns() {
    static const QList<QRegularExpression> pats = [] {
        struct Tpl {
            const char* zh;
            const char* en;
        };
        static const Tpl tpls[] = {{"刚刚", "just now"},
                                   {"%1 分钟前", "%1 min ago"},
                                   {"%1 小时前", "%1 h ago"},
                                   {"%1 天前", "%1 d ago"}};
        QList<QRegularExpression> out;
        QStringList seen;
        const i18n::Lang saved = i18n::g_lang;
        for (const auto& li : i18n::languages()) {
            i18n::g_lang = li.lang;
            for (const auto& t : tpls) {
                const QString text = QString::fromUtf8(i18n::tr(t.zh, t.en));
                if (seen.contains(text)) continue;
                seen << text;
                QStringList re;
                for (const QString& part : text.split("%1"))
                    re << QRegularExpression::escape(part);
                out << QRegularExpression(re.join(QStringLiteral("\\d+")));
            }
        }
        i18n::g_lang = saved;
        return out;
    }();
    return pats;
}

QString normText(QString s) {
    for (const auto& re : relPatterns()) s.replace(re, QStringLiteral("<rel>"));
    // 前后不能挨着数字或小数点：否则 "127.0.0.1:8787" 里的 "1:87" 会被当成时刻
    static const QRegularExpression stamp(R"((?<![\d:.])\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}(?::\d{2})?(?![\d:.]))");
    static const QRegularExpression monthDay(R"((?<![\d.-])\d{1,2}-\d{1,2} \d{1,2}:\d{2}(?![\d.]))");
    static const QRegularExpression clock(R"((?<![\d:.])\d{1,2}:\d{2}(?::\d{2})?(?![\d:.]))");
    static const QRegularExpression date(R"((?<![\d-])\d{4}-\d{2}-\d{2}(?![\d-]))");
    s.replace(stamp, QStringLiteral("<stamp>"));
    s.replace(monthDay, QStringLiteral("<stamp>"));
    s.replace(clock, QStringLiteral("<time>"));
    s.replace(date, QStringLiteral("<date>"));
    return s;
}

QJsonObject dumpWidget(QWidget* w);

// 某控件的子节点。滚动区自带的脚手架——滚动条及其容器（qt_scrollarea_*container）——
// 不录：它是 Qt 的实现细节，而且随机器变。滚动条显不显示取决于字体度量（行高一两个
// 像素，内容就溢出/不溢出），滚动条一显示 Qt 还会把它的容器 raise 到子列表末尾，
// 顺序跟着变。本机与 CI runner 的日志面板就同时差在这两点上。视口只是个包装，把它的
// 子节点直接接到这一层——那才是界面内容。
void appendKidsOf(QWidget* w, QJsonArray& kids) {
    for (QObject* c : w->children()) {
        auto* child = qobject_cast<QWidget*>(c);
        if (!child) continue;
        const QString name = child->objectName();
        if (qobject_cast<QScrollBar*>(child)) continue;
        if (name.startsWith("qt_scrollarea_") && name != "qt_scrollarea_viewport") continue;
        if (name == "qt_scrollarea_viewport") {
            appendKidsOf(child, kids);
            continue;
        }
        kids.append(dumpWidget(child));
    }
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
    // 从没打开过的弹窗容器（下拉列表、菜单）是顶层窗口：它的内部几何取决于 Qt 何时
    // 处理完布局事件，同一段代码两次运行可以不同；它弹出后长什么样也不属于这个
    // 面板的场景。只记它存在，不往下钻
    if (w->isWindow() && !w->isVisible()) return o;
    QJsonArray kids;
    appendKidsOf(w, kids);  // 滚动区脚手架的取舍见 appendKidsOf
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

// 两份快照的差异：JSON 路径 + 两边的值，最多收集 limit 条。DIFF 只报场景名的话，看
// 日志的人得自己去下载产物逐字段比；CI 上就直接看不到原因了。
QString brief(const QJsonValue& v) {
    QString s = QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
    return s.size() > 70 ? s.left(70) + "…" : s;
}

void collectDiffs(const QJsonValue& a, const QJsonValue& b, const QString& path, QStringList& out,
                  int limit) {
    if (out.size() >= limit) return;
    if (a.type() != b.type()) {
        out << QString("%1: 类型不同（基线 %2 / 本次 %3）").arg(path, brief(a), brief(b));
        return;
    }
    if (a.isObject()) {
        const QJsonObject oa = a.toObject(), ob = b.toObject();
        for (const QString& k : oa.keys()) {
            if (!ob.contains(k))
                out << QString("%1.%2: 基线有、本次没有 = %3").arg(path, k, brief(oa[k]));
            else
                collectDiffs(oa[k], ob[k], path + "." + k, out, limit);
        }
        for (const QString& k : ob.keys())
            if (!oa.contains(k))
                out << QString("%1.%2: 本次有、基线没有 = %3").arg(path, k, brief(ob[k]));
        return;
    }
    if (a.isArray()) {
        const QJsonArray aa = a.toArray(), ab = b.toArray();
        if (aa.size() != ab.size())
            out << QString("%1: 元素个数 基线 %2 / 本次 %3").arg(path).arg(aa.size()).arg(ab.size());
        const int common = std::min(aa.size(), ab.size());
        for (int i = 0; i < common; ++i)
            collectDiffs(aa[i], ab[i], QString("%1[%2]").arg(path).arg(i), out, limit);
        return;
    }
    if (!(a == b)) out << QString("%1: 基线=%2 本次=%3").arg(path, brief(a), brief(b));
}

// ---- Tab 焦点链（交互五项之一）----
// 按 Qt 自己的焦点链顺序列出面板里能用 Tab 到达的控件，录进快照参与基线对比：顺序或
// 成员变了就是键盘可达性回归——控件被改成 NoFocus、建控件的顺序换了、某个入口被挤出
// Tab 环。看的是链的静态顺序，不需要真的移动焦点（离屏下也不可靠）。
QString focusKey(QWidget* w) {
    QString text;
    if (const auto* b = qobject_cast<QAbstractButton*>(w)) text = b->text();
    else if (const auto* e = qobject_cast<QLineEdit*>(w)) text = e->placeholderText();
    else if (const auto* c = qobject_cast<QComboBox*>(w)) text = c->currentText();
    QString key = QString::fromLatin1(w->metaObject()->className());
    if (!w->objectName().isEmpty()) key += "#" + w->objectName();
    if (!text.isEmpty()) key += "「" + normText(text) + "」";
    return key;
}

QList<QWidget*> focusChainWidgets(QWidget* page) {
    QList<QWidget*> out;
    int guard = 0;
    for (QWidget* w = page->nextInFocusChain(); w && w != page && guard < 5000;
         w = w->nextInFocusChain(), ++guard) {
        // 焦点链是整窗环形的：只收本面板自己的子孙
        if (!page->isAncestorOf(w) || w->isWindow()) continue;
        if (!w->isVisible() || !w->isEnabled() || w->focusPolicy() == Qt::NoFocus) continue;
        out.append(w);
    }
    return out;
}

QJsonArray focusChainOf(QWidget* page) {
    QJsonArray out;
    for (QWidget* w : focusChainWidgets(page)) out.append(focusKey(w));
    return out;
}

// ---- 不变量规则集（第 1 轮拍板：rubric 式长期标准，随场景全跑；基线无关）----
struct Violation {
    QString rule;
    QString detail;
};

// 全部规则违例（场景 id + 规则 + 明细）。一条通道：进退出码，也进 HTML 报告。
// 此前规则违例只在 console 里打印、报告里只列弹窗一类、快捷键结果只打印不计数——
// 报告"通过"而没人看得到违例。
std::vector<std::pair<QString, Violation>> g_rules;

void recordRule(const QString& scene, const QString& rule, const QString& detail) {
    g_rules.emplace_back(scene, Violation{rule, detail});
    printNow(QString("RULE      %1 | %2 | %3").arg(scene, rule, detail.left(80)));
}

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
    // R8 可见、可用的标准控件必须能被 Tab 到达（在本面板的焦点链里）。键盘用户只能靠
    // Tab；控件被设成 NoFocus / ClickFocus 之后鼠标照常、键盘到不了。只查标准控件
    // （按钮、复选、单选、输入框、下拉、微调）——自绘卡片的可达性另有约定。
    {
        static const char* const kTabbable[] = {"QPushButton", "QCheckBox", "QRadioButton",
                                                "QLineEdit", "QComboBox", "QSpinBox"};
        QSet<QWidget*> inChain;
        for (QWidget* w : focusChainWidgets(panel)) inChain.insert(w);
        std::function<void(QWidget*)> reach = [&](QWidget* w) {
            // 微调框/日期框内部的输入框不算独立入口
            const bool innerEdit = qobject_cast<QAbstractSpinBox*>(w->parentWidget()) != nullptr;
            if (w->isVisible() && w->isEnabled() && !w->isWindow() && !innerEdit) {
                for (const char* cls : kTabbable) {
                    if (!w->inherits(cls)) continue;
                    const auto* edit = qobject_cast<QLineEdit*>(w);
                    if (edit && edit->isReadOnly()) break;  // 只读输入框不是入口
                    if (!inChain.contains(w))
                        out.push_back({"focus-unreachable-control", focusKey(w)});
                    break;
                }
            }
            for (QObject* c : w->children())
                if (auto* cw = qobject_cast<QWidget*>(c)) reach(cw);
        };
        reach(panel);
    }
    // R6 样式表里不得残留未替换的占位符（@token@）。ui::th() 不认识的标记会原样留在
    // 样式表里，Qt 随即整条解析失败——控件失去样式（底色、边框、按钮文字全没了），只在
    // stderr 留一句 "Could not parse stylesheet"，没人会去翻。总览页接入提示条的
    // @warnbg@ 就是这样溜出去的。
    {
        static bool appSheetChecked = false;
        static const QRegularExpression placeholder("@[a-z]+@");
        // 先剥掉 /* … */（跨行）：dark.qss 的文件头注释里就写着"@token@ 由 ui::th() 运行时替换"
        static const QRegularExpression comment(R"((?s)/\*.*?\*/)");
        const auto leftover = [](const QString& styleSheet) {
            QString s = styleSheet;
            s.remove(comment);
            return placeholder.match(s);
        };
        std::function<void(QWidget*)> qss = [&](QWidget* w) {
            const auto m = leftover(w->styleSheet());
            if (m.hasMatch())
                out.push_back({"qss-unsubstituted-token",
                               QString("%1(%2): %3")
                                   .arg(w->metaObject()->className(), w->objectName(), m.captured())});
            for (QObject* c : w->children())
                if (auto* cw = qobject_cast<QWidget*>(c)) qss(cw);
        };
        qss(panel);
        if (!appSheetChecked) {
            appSheetChecked = true;
            const auto m = leftover(qApp->styleSheet());
            if (m.hasMatch())
                out.push_back({"qss-unsubstituted-token",
                               "application stylesheet: " + m.captured()});
        }
    }
    // R7 容器的样式表规则会沿树波及到交互控件。
    // Qt 的层叠规则是祖先样式表压过全局样式表，而 (a) 不带选择器的声明（如
    // "background:transparent;"）作用于本控件**及其全部子孙**，(b) 纯类型选择器（如
    // "QWidget { … }"、"QFrame { … }"）命中所有该类型的子孙。两者都会让容器里的按钮、
    // 输入框丢掉全局样式：空状态的主按钮只剩一行文字，告警横幅里的按钮丢了底色，
    // 提示条里的主按钮丢了渐变。只想影响自身的写法是 ui::setSelfStyleSheet
    // （#objectName { … }）。叶子控件（标签、自绘控件）上的声明没有子孙可波及，不报；
    // 带颜色/字号等非盒模型属性的规则在别处判定，这里只看波及范围。
    {
        static const char* const kInteractive[] = {"QAbstractButton", "QComboBox", "QLineEdit",
                                                   "QAbstractSpinBox", "QTextEdit",
                                                   "QPlainTextEdit"};
        static const QRegularExpression comment(R"(/\*.*?\*/)");
        static const QRegularExpression rule(R"(([^{}]*)\{([^{}]*)\})");
        static const QRegularExpression bareType(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        std::function<void(QWidget*)> walk = [&](QWidget* w) {
            QString ss = w->styleSheet();
            ss.remove(comment);
            QStringList selectors;  // 本控件样式表里会波及子孙的写法（原样，供报告）
            QStringList types;       // …及其命中的类型
            if (ss.contains('{')) {
                auto it = rule.globalMatch(ss);
                while (it.hasNext()) {
                    const auto m = it.next();
                    for (const QString& sel : m.captured(1).split(','))
                        if (bareType.match(sel.trimmed()).hasMatch()) {
                            selectors << sel.trimmed() + " { … }";
                            types << sel.trimmed();
                        }
                }
            } else if (!ss.trimmed().isEmpty()) {
                selectors << ss.simplified().left(40);
                types << QStringLiteral("QWidget");  // 无选择器 = 全部子孙
            }
            for (int i = 0; i < types.size(); ++i) {
                // 只看泛型容器类型。写 "QComboBox { padding… }""QPushButton { … }" 的意图
                // 就是给里面的那类控件定样式，是合理用法
                if (types[i] != QStringLiteral("QWidget") && types[i] != QStringLiteral("QFrame"))
                    continue;
                for (QWidget* d : w->findChildren<QWidget*>()) {
                    bool interactive = false;
                    for (const char* cls : kInteractive)
                        if (d->inherits(cls)) {
                            interactive = true;
                            break;
                        }
                    if (!interactive || !d->inherits(types[i].toLatin1().constData())) continue;
                    const auto* btn = qobject_cast<QAbstractButton*>(d);
                    out.push_back({"qss-rule-leaks-to-controls",
                                   QString("%1[%2] → %3 %4")
                                       .arg(QString::fromLatin1(w->metaObject()->className()),
                                            selectors[i],
                                            QString::fromLatin1(d->metaObject()->className()),
                                            btn ? "「" + btn->text() + "」" : QString())});
                    break;
                }
            }
            for (QObject* c : w->children())
                if (auto* cw = qobject_cast<QWidget*>(c)) walk(cw);
        };
        walk(panel);
    }
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

// ---- 确定的"本机环境"：工具安装检测不随机器变 ----
// 检测看两处：PATH 上的可执行文件，和 LOCALAPPDATA / APPDATA / USERPROFILE 下的配置
// 目录（core/integrations.hpp）。那是真实环境——本机装了什么，基线里就录什么，换台
// 机器（CI runner 上什么都没装）全变。harness 在自己的进程环境里把这些变量指向
// 数据目录下的一棵空目录树，再按固定的名单放假标记：装了/没装/有配置文件三种
// 界面状态在每台机器上都一样。
// 用户目录类变量必须在 QApplication 之前设（Qt 会缓存 home 路径）；PATH 必须在
// 之后设（Qt 的 DLL 与平台插件要靠它加载），见 limitPath().
struct FakeWorld {
    QString home;
    QString bin;
};

FakeWorld makeFakeWorld(const QString& dataHome) {
    FakeWorld w;
    const QString root = dataHome + "/deterministic-world";
    w.home = root + "/home";
    w.bin = root + "/bin";
    const QString local = root + "/local", appdata = root + "/appdata";
    for (const QString& d : {w.home, w.home + "/.claude", w.home + "/.cursor", local, appdata,
                             appdata + "/npm", w.bin})
        QDir().mkpath(d);
    // 假标记。名单是固定的：claude（配置目录存在）、codex（PATH 上的可执行文件）、
    // cursor（配置文件存在但没有 miderhive 条目）、gemini（npm 全局目录）算已安装；
    // 其余工具在假环境里不存在 → 未安装。
    // 可执行标记刻意不带 .exe/.cmd 后缀（exeOnPath 对裸名也匹配）：新建的可执行文件
    // 会被杀软实时扫描短暂锁住，存在性检查在这几秒里瞬时失败——曾让一次运行的检测
    // 结果从 4 个工具变成 1 个（空目录从不触发扫描，所以只有它从没失手）。
    const auto touch = [](const QString& p) {
        QFile f(p);
        f.open(QIODevice::WriteOnly);
    };
    touch(w.bin + "/codex");
    touch(w.home + "/.cursor/mcp.json");
    touch(appdata + "/npm/gemini");
    // 「是否已接入」的读取也必须落在假环境：GUI 层的 configPath() 用 QDir::homePath()
    // 解析真实主目录（不跟随上面的 qputenv——Qt 有缓存），真机上已接入的工具会被判成
    // Connected，接入提示条就随真机状态漂移（曾让一次运行从"4 个未接入"变成"1 个"）。
    // MIDERHIVE_CONNECT_ROOT 是项目自带的换根开关（gui_selftest 同款用法）：指向这个
    // 根目录之后，所有工具的配置读取都落在这里——目录里什么都不放 → 永远"未接入"。
    qputenv("MIDERHIVE_CONNECT_ROOT", root.toUtf8());
    qputenv("USERPROFILE", w.home.toUtf8());
    qputenv("HOME", w.home.toUtf8());
    qputenv("LOCALAPPDATA", local.toUtf8());
    qputenv("APPDATA", appdata.toUtf8());
    return w;
}

// 假世界的标记必须全部可检出才开始拍：上面提到的杀软扫描窗口里，存在性检查会瞬时
// 失败。这里轮询到期望的四个工具都检出为止（至多 10 秒），超时则中止——宁可这条
// CI 步骤红掉让人来看，也不能带着不确定的检测结果拍快照/基线。返回期望检出的个数。
int waitUntilFakeWorldDetected() {
    const QStringList expected = {"claude-code", "codex", "cursor", "gemini"};
    for (int i = 0; i < 100; ++i) {
        bool all = true;
        for (const QString& id : expected)
            if (!ah::integrations::installedById(id.toStdString())) {
                all = false;
                break;
            }
        if (all) break;
        QThread::msleep(100);
    }
    int detected = 0;
    for (const auto& t : ah::integrations::toolRegistry())
        if (ah::integrations::installedById(t.id)) ++detected;
    return detected;
}

// PATH 只留两段：假环境的 bin，和 Qt 的 bin（原 PATH 上第一个含 Qt6Core.dll 的目录）
// ——惰性加载的 Qt 插件（SVG 图标等）要靠它找依赖 DLL。Qt 的 bin 里不会有
// claude / codex 这类可执行文件，所以检测不会被它影响。
void limitPath(const FakeWorld& w) {
    QString path = w.bin;
    const QChar sep =
#ifdef Q_OS_WIN
        ';';
#else
        ':';
#endif
    for (const QString& entry : qEnvironmentVariable("PATH").split(sep, Qt::SkipEmptyParts))
        if (QFileInfo::exists(entry + "/Qt6Core.dll")) {
            path += sep + entry;
            break;
        }
    qputenv("PATH", path.toUtf8());
}

// ---- 字体：离屏平台默认一个字体都不加载 ----
// Qt 的 offscreen 平台在 Windows 上只从 QT_QPA_FONTDIR（或 Qt 的 lib/fonts——Qt 6 起不再
// 自带）加载字体，不读系统字体。不处理的话所有文字都渲染成空方块：此前的"像素金样"
// 拍的就是一个没有文字的界面，字号 12/13/14 三个场景也拍不出任何差别。
// 调用方已设 QT_QPA_FONTDIR 则尊重；否则用系统字体目录（像素金样因此只在同一台机器
// 上可比——结构快照不依赖字体，跨机对比的是它）。返回 false 表示找不到任何字体。
bool ensureFonts(QString* why) {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) return true;
    const QDir sysFonts(qEnvironmentVariable("WINDIR") + "/Fonts");
    if (sysFonts.entryList({"*.ttf", "*.ttc", "*.otf"}, QDir::Files).isEmpty()) {
        *why = "系统字体目录里没有字体文件: " + sysFonts.absolutePath();
        return false;
    }
    qputenv("QT_QPA_FONTDIR", QDir::toNativeSeparators(sysFonts.absolutePath()).toUtf8());
    return true;
}

// ---- 拍摄前的落定：让面板内容一定是最新的 ----
// 主窗口只在"导航行变化"时刷新对应面板，平时靠 1 秒轮询。靠"跑够了时间"来等轮询，
// 拍到的内容就取决于之前跑了多久：--filter dashboard 时行号从头到尾不变，种子刚注入
// 轮询还没到，拍到的"有数据"场景其实是注入前的旧画面。所以拍之前显式同步刷新。
void settlePage(QWidget* page) {
    // 按当天初始化的控件每次重建都会回到"今天"（主题/语言/字号切换会重建面板）
    for (QDateEdit* d : page->findChildren<QDateEdit*>()) d->setDate(QDate(2026, 11, 1));
    // PanelBase 没有 Q_OBJECT，findChild<PanelBase*>() 会退化成按 QObject 匹配到任意
    // 子对象；用 RTTI 找。页面自己可能就是面板（仪表盘），也可能外面还包着一层
    // 滚动区（日志面板）——两处都要看
    const auto eachWidget = [page](auto&& fn) {
        fn(page);
        for (QWidget* c : page->findChildren<QWidget*>()) fn(c);
    };
    eachWidget([&](QWidget* w) {
        if (auto* pb = dynamic_cast<PanelBase*>(w)) pb->refresh();
    });
    // 图谱：力导向布局同步跑到它自己的停止条件。否则拍到的是"定时器在拍摄前恰好
    // 跑了多少帧"的位置——两次运行的像素因此差 1%
    eachWidget([&](QWidget* w) {
        if (auto* gp = dynamic_cast<GraphPanel*>(w)) gp->settleLayout();
    });
    for (int i = 0; i < 4; ++i) QApplication::processEvents(QEventLoop::AllEvents, 50);
}

// ---- 状态保持（交互五项之一）----
// 面板每 3 秒自动刷新一次。用户刚做的事——选中的行、滚动到的位置、下拉框的当前项、
// 输入到一半的筛选文字——必须活过刷新。v1.5.0 修过"筛选下拉每次刷新被重建、选到一半
// 就塌掉"，这一类问题一直没有回归测试。
// 做法：在面板上留下状态 → 触发一次刷新 → 逐项核对。会改变面板状态，所以必须在所有
// 拍摄结束之后跑。返回 (丢失项说明, 保住的项)。
std::pair<QStringList, QStringList> probeStateRetention(QWidget* page) {
    QStringList lost, kept;
    const auto refresh = [page] { settlePage(page); };
    const auto label = [](QWidget* w) {
        QString s = QString::fromLatin1(w->metaObject()->className());
        if (!w->objectName().isEmpty()) s += "#" + w->objectName();
        return s;
    };
    // 选区：第一张有行的表格 / 列表
    for (QTableWidget* t : page->findChildren<QTableWidget*>()) {
        if (!t->isVisible() || t->rowCount() == 0) continue;
        t->setCurrentCell(0, 0);
        refresh();
        if (t->currentRow() != 0 || t->selectedItems().isEmpty())
            lost << QString("%1：选中的行在刷新后丢失（currentRow=%2）").arg(label(t)).arg(t->currentRow());
        else
            kept << "表格选区";
        break;
    }
    for (QListWidget* l : page->findChildren<QListWidget*>()) {
        if (!l->isVisible() || l->count() == 0) continue;
        l->setCurrentRow(0);
        refresh();
        if (l->currentRow() != 0 || l->selectedItems().isEmpty())
            lost << QString("%1：选中的项在刷新后丢失（currentRow=%2）").arg(label(l)).arg(l->currentRow());
        else
            kept << "列表选区";
        break;
    }
    // 下拉框当前项（候选项被重建时最容易丢）
    for (QComboBox* c : page->findChildren<QComboBox*>()) {
        if (!c->isVisible() || c->count() < 2) continue;
        const QString before = c->currentText();
        refresh();
        if (c->currentText() != before)
            lost << QString("%1：当前项在刷新后从「%2」变成「%3」")
                        .arg(label(c), normText(before), normText(c->currentText()));
        else
            kept << "下拉当前项";
        break;
    }
    // 滚动位置：第一个能滚动的滚动区（页面自身也算）
    {
        QList<QAbstractScrollArea*> areas;
        if (auto* self = qobject_cast<QAbstractScrollArea*>(page)) areas << self;
        for (QAbstractScrollArea* a : page->findChildren<QAbstractScrollArea*>()) areas << a;
        for (QAbstractScrollArea* a : areas) {
            if (!a->isVisible()) continue;
            QScrollBar* bar = a->verticalScrollBar();
            if (!bar || bar->maximum() <= bar->minimum()) continue;
            const int target = bar->minimum() + (bar->maximum() - bar->minimum()) / 2;
            bar->setValue(target);
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            refresh();
            if (bar->value() != target)
                lost << QString("%1：滚动位置在刷新后从 %2 变成 %3")
                            .arg(label(a))
                            .arg(target)
                            .arg(bar->value());
            else
                kept << "滚动位置";
            break;
        }
    }
    // 筛选文字：第一个可编辑的输入框
    for (QLineEdit* e : page->findChildren<QLineEdit*>()) {
        if (!e->isVisible() || !e->isEnabled() || e->isReadOnly()) continue;
        if (qobject_cast<QAbstractSpinBox*>(e->parentWidget())) continue;
        const QString original = e->text();
        e->setText("zz-state-probe");
        refresh();
        if (e->text() != "zz-state-probe")
            lost << QString("%1：输入到一半的筛选文字在刷新后丢失（变成「%2」）")
                        .arg(label(e), normText(e->text()));
        else
            kept << "筛选文字";
        e->setText(original);
        refresh();
        break;
    }
    return {lost, kept};
}

}  // namespace

int main(int argc, char** argv) {
    // 离屏兜底：外部（包装脚本/CI）已设 QT_QPA_PLATFORM 时尊重外部值
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

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

    // ---- 隔离（强制）：harness 会向数据库灌种子，绝不能碰真实数据目录 ----
    // defaultHomeDir() 在没设 MIDERHIVE_HOME 时落到 %USERPROFILE%\.miderhive，而且那个
    // 目录不存在时会把旧品牌目录改名迁移过来——直接敲 ui_harness.exe（或双击）就会往
    // 真实数据库里写。所以：没设就用一次性临时目录；设了但目录里已经有库则拒跑
    // （在旧库上跑出来的基线没有意义，见 seedData）。
    QTemporaryDir tempHome;  // 仅在调用方没给 MIDERHIVE_HOME 时启用；析构时自动清掉
    if (qEnvironmentVariableIsEmpty("MIDERHIVE_HOME")) {
        if (!tempHome.isValid()) {
            printNow("FATAL  无法创建临时数据目录");
            return 1;
        }
        qputenv("MIDERHIVE_HOME", tempHome.path().toUtf8());
        printNow(QString("NOTE  MIDERHIVE_HOME 未设置，使用一次性临时目录: %1")
                     .arg(tempHome.path()));
    }
    {
        const QString given = QString::fromUtf8(qgetenv("MIDERHIVE_HOME"));
        if (QFileInfo::exists(given + "/platform.db")) {
            printNow(QString("FATAL  MIDERHIVE_HOME 里已经有 platform.db（%1）。harness 会向库里"
                             "灌种子，必须在全新的空目录上运行——请换一个空目录。")
                         .arg(given));
            return 3;
        }
        if (!QDir().mkpath(given)) {
            printNow(QString("FATAL  无法创建数据目录: %1").arg(given));
            return 1;
        }
    }
    // 假环境单独放一个临时目录：数据目录是调用方给的（CI / ci_local.ps1），不往里留东西
    QTemporaryDir tempWorld;
    if (!tempWorld.isValid()) {
        printNow("FATAL  无法创建临时目录（工具检测的假环境）");
        return 1;
    }
    const FakeWorld world = makeFakeWorld(tempWorld.path());
    QString why;
    if (!ensureFonts(&why)) {
        printNow("FATAL  " + why + "。离屏平台没有字体时所有文字都是空方块，快照与金样没有"
                 "意义。请设置 QT_QPA_FONTDIR 指向一个含字体文件的目录。");
        return 1;
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName("miderhive");
    QApplication::setApplicationName("MiderHive 多 Agent 协作工作台");
    limitPath(world);
    // 假世界的四个标记全部可检出（轮询等待，杀软扫描窗口见 waitUntilFakeWorldDetected）
    const int detectedTools = waitUntilFakeWorldDetected();
    if (detectedTools != 4) {
        printNow(QString("FATAL  假环境的工具检测不确定：期望 4 个、实际 %1 个。"
                         "带着不确定的检测结果拍快照没有意义，已中止。")
                     .arg(detectedTools));
        return 1;
    }
    if (QFontDatabase::families().isEmpty()) {
        printNow("FATAL  一个字体都没有加载（QT_QPA_FONTDIR=" +
                 qEnvironmentVariable("QT_QPA_FONTDIR") + "）。所有文字会渲染成空方块。");
        return 1;
    }
    // SVG 图标靠惰性加载的图片格式插件。插件缺失时图标全空，像素金样和"图标是否
    // 渲染"的判断都没有意义——在拍任何场景之前先确认它可用
    if (!QImageReader::supportedImageFormats().contains("svg")) {
        printNow("FATAL  Qt 的 SVG 图片格式插件没有加载（Qt bin 不在 PATH 上？）。图标将无法"
                 "渲染，快照与金样没有意义。");
        return 1;
    }
    // 应用字体：与真实入口（gui/main.cpp）同一步骤——等宽数字让用量/时间戳刷新时不
    // 左右抖动，字体族决定回退顺序与度量
    {
        QFont f = app.font();
        f.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                       QStringLiteral("Microsoft YaHei UI")});
        f.setFeature(QFont::Tag("tnum"), 1);
        app.setFont(f);
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
        // 主题与字号显式钉在默认值：基准场景全部在这两个值下拍，后面的正交循环再切换
        s.setValue("ui/theme", "hive");
        s.setValue("ui/fontBase", 12);
        s.sync();
    }
    i18n::load();
    // 全局样式表：与真实入口（gui/main.cpp）同一步骤，而且必须在 QSettings 重定向之后
    // ——主题索引自 QSettings 懒加载。harness 此前漏了这一步：拍出来的是 Qt 默认调色板
    // 下的浅色界面，而不是用户看到的深色主题。主题循环又会触发主窗口重新应用全局
    // 样式表，于是同一次运行里前半的场景没有全局样式、后半的有，基线自身都不自洽
    app.setStyleSheet(ui::themeQss());

    // 冻结显示用的时钟：必须在构造主窗口之前（第一次显示相对时间之前）
    g_frozenNow = freezeClock();

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
        settlePage(panel);
        QJsonObject tree = dumpWidget(panel);
        // 每个场景存一张图：JSON 回答"结构对不对"，图回答"看起来对不对"。报告里按场景
        // 列出，供人工 review；不参与对比（像素随字体与日期变，不能拿别的机器的来比）
        panel->grab().save(outDir + "/" + id + ".png");
        // Tab 焦点链录进快照（参与基线对比）
        tree["focus"] = focusChainOf(panel);
        // 可见控件数（规则 R5 用）
        int visibleCount = 0;
        std::function<void(const QJsonObject&)> countVis = [&](const QJsonObject& o) {
            if (o["vis"].toBool()) ++visibleCount;
            for (const auto& k : o["kids"].toArray()) countVis(k.toObject());
        };
        countVis(tree);
        // 不变量规则（rubric）：违例即记录（不影响快照对比本身）
        for (const auto& v : runRules(id, panel, visibleCount, tree.contains("text")))
            recordRule(id, v.rule, v.detail);
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
            // 基线缺失 = 这个场景从来没有被 review 过，必须计入失败。此前 NEW 只打印
            // 不计数：新场景在基线入库之前一直"什么都不比"地绿着
            recordRule(id, "baseline-missing", "基线不存在——review 后 --update-baseline 并入库");
            ++created;
            results.push_back({id, "NEW"});
            return;
        }
        b.open(QIODevice::ReadOnly);
        const QJsonObject base = QJsonDocument::fromJson(b.readAll()).object();
        const QJsonObject a = stripSoft(base), c = stripSoft(tree);
        if (a != c) {
            ++diffs;
            results.push_back({id, "DIFF"});
            printNow(QString("DIFF      %1（结构/文案/焦点链与基线不一致）").arg(id));
            // 直接指出不同在哪：看日志的人不用去下载产物逐字段比
            QStringList why;
            collectDiffs(a, c, "$", why, 5);
            for (const QString& d : why) printNow("            " + d);
        } else {
            ++passed;
            results.push_back({id, "PASS"});
            printNow(QString("PASS      %1").arg(id));
        }
    };

    // ---- 服务未启动时的总览 ----
    // 健康告警横幅（「本机服务未运行」+「查看详情」）只在服务没起来时出现。常规场景在
    // 服务启动后拍——那才是用户平时看到的状态；这一张专门留给告警态
    runScene("dashboard.noserver", 0);

    // ---- 内置 HTTP 服务：与真实入口同一步骤（仅 127.0.0.1，临时端口）----
    // 临时端口不占固定端口，也不会和正在运行的工作台/测试实例撞车。起不来就中止：
    // 缺了服务，总览页会一直挂着"服务未运行"告警，所有总览场景拍的都是故障态
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) {
            printNow("FATAL  找不到可用的临时端口（内置 HTTP 服务）");
            return 1;
        }
        const quint16 port = probe.serverPort();
        probe.close();
        std::string serr;
        if (!platform.startHttpServer(port, serr)) {
            printNow(QString("FATAL  内置 HTTP 服务启动失败（127.0.0.1:%1）: %2")
                         .arg(port)
                         .arg(QString::fromStdString(serr)));
            return 1;
        }
        for (int i = 0; i < 4; ++i) QApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    // pass 1：空态（bootstrap 后、种子前；zcode 管理者已存在——dashboard 记录它）
    for (const auto& ps : panels) runScene(QString("%1.empty").arg(ps.panel), ps.row);

    // 注确定性种子（时间取"现在减固定偏移"，见 seedFixTimestamps）
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
        settlePage(panel);
        // 图谱画布的力导向模拟由定时器驱动，收敛后自动停帧；没跑完就拍，像素随拍摄
        // 时刻变。多泵几秒让它收敛（收敛后是定点，多出来的时间不改变画面）
        for (int i = 0; i < 30; ++i) QApplication::processEvents(QEventLoop::AllEvents, 100);
        const QPixmap pm = panel->grab();
        QBuffer buf;
        buf.open(QIODevice::WriteOnly);
        pm.save(&buf, "PNG");
        const QByteArray png = buf.data();
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        // 先比后写。此前是先写后比：查到的永远是刚写进去的新值，GOLDEN-DIFF 一次都
        // 不可能触发——当时报的"金样差异 0"是一条空检查
        const auto it = goldenHashes.constFind(id);
        if (it == goldenHashes.constEnd()) {
            results.push_back({id, "GOLDEN-NEW"});
            printNow(QString("GOLDEN-NEW  %1（第一次采集，之后与本次比较）").arg(id));
        } else if (it.value() != hash) {
            ++goldenDiff;
            results.push_back({id, "GOLDEN-DIFF"});
            // 把上一次的图留成 .prev.png，方便并排看差在哪。QFile::copy 不覆盖已存在的
            // 目标文件且静默失败——先删再拷，否则 .prev 会一直是某次更早的旧图
            QFile::remove(goldenDir + "/" + id + ".prev.png");
            QFile::copy(goldenDir + "/" + id + ".png", goldenDir + "/" + id + ".prev.png");
            printNow(QString("GOLDEN-DIFF  %1（像素与上一次运行不同——本机信号，不计入退出码）")
                         .arg(id));
        }
        goldenHashes[id] = hash;
        QFile pf(goldenDir + "/" + id + ".png");
        pf.open(QIODevice::WriteOnly);
        pf.write(png);
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
    // 结果计入规则违例。此前只打印不计数：缺一个快捷键或触发后没切过去，报告照样通过
    int shortcutOk = 0, shortcutTotal = 0, shortcutMissing = 0;
    for (int i = 0; i < nav->count() && i < 9; ++i) {
        const QString key = QString("Ctrl+%1").arg(i + 1);
        QShortcut* found = nullptr;
        for (QShortcut* sc : w.findChildren<QShortcut*>())
            if (sc->key() == QKeySequence(key)) {
                found = sc;
                break;
            }
        if (!found) {
            ++shortcutMissing;
            recordRule("shortcuts", "shortcut-missing", key + " 未找到对应快捷键");
            continue;
        }
        ++shortcutTotal;
        emit found->activated();
        QApplication::processEvents(QEventLoop::AllEvents, 80);
        if (stack->currentIndex() == i && nav->currentRow() == i) ++shortcutOk;
        else recordRule("shortcuts", "shortcut-no-switch", key + " 触发后没有切到对应面板");
    }

    // ---- 状态保持（交互五项之一）：在所有拍摄之后做，它会改变面板状态 ----
    for (const auto& ps : panels) {
        if (!filter.isEmpty() && !QString(ps.panel).contains(filter)) continue;
        nav->setCurrentRow(ps.row);
        for (int i = 0; i < 5; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QWidget* panel = stack->currentWidget();
        if (!panel) continue;
        settlePage(panel);
        const auto [lost, kept] = probeStateRetention(panel);
        for (const QString& l : lost) recordRule(ps.panel, "state-lost-on-refresh", l);
        printNow(QString("STATE     %1: %2 项状态经刷新保持%3")
                     .arg(ps.panel)
                     .arg(kept.size())
                     .arg(lost.isEmpty() ? QString() : QString("，%1 项丢失").arg(lost.size())));
    }

    // ---- 汇总 ----
    const int ruleViolations = static_cast<int>(g_rules.size() + g_dialogs.violations.size());
    const QString envLine = QString("Qt %1 · 时区 %2")
                                .arg(QString::fromLatin1(qVersion()),
                                     QString::fromLatin1(QTimeZone::systemTimeZoneId()));
    printNow(QString("==== ui_harness 完成：场景 %1，PASS %2，新建基线 %3，结构差异 %4，规则违例 %5，弹窗 %6，金样差异 %7 ====")
                 .arg(compared)
                 .arg(passed)
                 .arg(created)
                 .arg(diffs)
                 .arg(ruleViolations)
                 .arg(g_dialogs.total)
                 .arg(goldenDiff));
    printNow(QString("ENV   %1 · 数据目录 %2 · 显示时钟冻结在 %3 · 假环境检出工具 %4")
                 .arg(envLine, home, g_frozenNow.toUTC().toString(Qt::ISODate))
                 .arg(detectedTools));

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
            "pre-wrap}h2{border-bottom:1px solid #333}.shot{margin:18px 0}.shot img{max-width:100%;"
            "border:1px solid #333;border-radius:6px}h3{margin:6px 0}</style></head><body>");
        html += QStringLiteral("<h1>MiderHive UI Harness Report</h1><p>%1 · 场景 %2 · PASS %3 · "
                               "DIFF %4 · NEW %5 · 规则违例 %6 · 弹窗 %7 · 金样差异 %8</p><p>%9</p>")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                    .arg(compared)
                    .arg(passed)
                    .arg(diffs)
                    .arg(created)
                    .arg(ruleViolations)
                    .arg(g_dialogs.total)
                    .arg(goldenDiff)
                    .arg(envLine.toHtmlEscaped());
        html += "<h2>场景结果</h2><table><tr><th>场景</th><th>状态</th></tr>";
        for (const auto& [id, st] : results)
            html += QStringLiteral("<tr><td>%1</td><td class='%2'>%3</td></tr>")
                        .arg(id, st.toLower(), st);
        html += "</table>";
        // 全部规则违例（含弹窗与快捷键），一张表。此前报告只列弹窗一类，其余写了一句
        // "明细见 console"——而 CI 上没人去翻 console
        html += "<h2>规则违例</h2>";
        if (ruleViolations == 0) {
            html += "<p>无</p>";
        } else {
            html += "<table><tr><th>场景</th><th>规则</th><th>明细</th></tr>";
            for (const auto& [scene, v] : g_rules)
                html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>")
                            .arg(scene, v.rule, v.detail.toHtmlEscaped());
            for (const auto& v : g_dialogs.violations)
                html += QStringLiteral("<tr><td>弹窗</td><td>%1</td><td>%2</td></tr>")
                            .arg(v.rule, v.detail.toHtmlEscaped());
            html += "</table>";
        }
        html += QStringLiteral("<h2>快捷键</h2><p>Ctrl+1..9：%1 / %2 触发后切换到位，%3 个缺失</p>")
                    .arg(shortcutOk)
                    .arg(shortcutTotal)
                    .arg(shortcutMissing);
        if (!g_dialogs.entries.isEmpty()) {
            html += "<h2>弹窗记录（自动应答）</h2><pre>" +
                    g_dialogs.entries.join('\n').toHtmlEscaped() + "</pre>";
        }
        html += QStringLiteral(
                    "<h2>金样哈希（本机信号，不计入退出码）</h2><p>显示时钟冻结在今天第一个合法的整点，"
                    "所以同一天里重复运行的像素应当一致；跨天（以及本地日期与 UTC 日期交替的那个小时）"
                    "绝对时间戳会变，金样随之不同。换机器不可比（字体不同）。</p><pre>%1</pre>")
                    .arg(QString::fromUtf8(QJsonDocument(goldenHashes)
                                               .toJson(QJsonDocument::Indented)
                                               .constData())
                             .toHtmlEscaped());
        // 场景截图：与 ui_report.html 同目录，直接相对引用
        html += "<h2>场景截图</h2><p>每个场景一张（有数据的场景时间文案是相对时间，与基线里的归一化"
                "记号不同属正常）。</p>";
        for (const auto& [id, st] : results) {
            if (!QFile::exists(outDir + "/" + id + ".png")) continue;
            html += QStringLiteral("<div class='shot'><h3>%1 <span class='%2'>%3</span></h3>"
                                   "<img src='%4.png' loading='lazy'></div>")
                        .arg(id, st.toLower(), st, id);
        }
        html += "</body></html>";
        hf.write(html.toUtf8());
    }
    printNow(QString("报告：build/ui_harness/ui_report.html（金样 %1 张在 build/ui_harness/golden/）")
                 .arg(goldenHashes.size()));
    // 弹窗违例已计入 ruleViolations；这里只为 console 输出（不再登记，避免重复计数）
    for (const auto& v : g_dialogs.violations)
        printNow(QString("RULE      dialogs | %1 | %2").arg(v.rule, v.detail.left(80)));
    if (!g_dialogs.entries.isEmpty()) {
        QFile dl(outDir + "/dialogs.log");
        dl.open(QIODevice::WriteOnly);
        dl.write(g_dialogs.entries.join('\n').toUtf8());
    }
    if (update) {
        printNow("基线已更新——请 git diff tests/baseline/ 人工 review 后入库。");
        return 0;
    }
    // 一个场景都没跑（--filter 写错、面板矩阵改了名）绝不能算通过：那是"什么都没检查"
    // 的绿灯
    if (compared == 0) {
        printNow(QString("FATAL  没有任何场景匹配 --filter \"%1\"（子串匹配）——什么都没有检查，"
                         "不能算通过。")
                     .arg(filter));
        return 1;
    }
    // 金样差异不计入（见上）。NEW 必须计入：没有基线的场景等于没有检查
    return (diffs == 0 && created == 0 && ruleViolations == 0) ? 0 : 2;
}
