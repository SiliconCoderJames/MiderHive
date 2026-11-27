// ui_shots —— 界面截图工具（开发 / 文档用；不属于 CI 门禁）。
//
//   ui_shots pages   --out <dir> --demo [--size 1280x720] [--names a,b,…] [--settle 1200]
//   ui_shots dialogs --out <dir> --demo
//
// 两种模式都要求调用方给 MIDERHIVE_HOME（隔离数据目录）与 MIDERHIVE_PORT。--demo 时工具自己在
// 进程内灌一套演示数据、随即开工；不带 --demo 则等触发文件 <dir>/GO 出现再开工（由调用方
// 通过内置 HTTP 服务的接口造数据），结束时写 <dir>/DONE。
//
// pages：在真实桌面平台上按 gui/main.cpp 的同一步骤起一个工作台窗口，依次切到每个面板，
//        把**整窗**（含系统标题栏）存成 <dir>/<名字>.png。
//        - 窗口不激活（WA_ShowWithoutActivating）并压到 Z 序最底层：不抢焦点、不挡住用户；
//          PrintWindow 直接取窗口自身的像素，被遮住也拍得到；
//        - 界面设置全部落到数据目录下的 INI（主题 / 字号 / 语言 / 几何），真实注册表一个字节
//          都不动；
//        - 显示时钟冻结在启动时刻（MIDERHIVE_UI_NOW，与 ui_harness 同一个接缝）：同一批图里
//          的相对时间文案一致。
//
// dialogs：离屏平台。逐个打开程序里的对话框与菜单，各存一张 <dir>/<名字>.png，并做一轮版式
//        自动检查（控件被裁出对话框、标签文字放不下、破坏性按钮是默认按钮、没有焦点落点），
//        结果写 <dir>/dialogs.log。对话框只截客户区（离屏平台没有系统标题栏）。
//
// 与 ui_harness 的分工：harness 是回归门禁（结构基线 + 不变量规则，数据是它自己的确定性
// 种子）；本工具只出图，数据由调用方决定。

#include <QApplication>
#include <QAbstractAnimation>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include <windows.h>
#include <dwmapi.h>

#include "core/db/database.h"
#include "core/platform.h"
#include "core/util.h"
#include "gui/connect_dialog.h"
#include "gui/i18n.h"
#include "gui/mainwindow.h"
#include "gui/panels/panel_base.h"
#include "gui/settings_dialog.h"
#include "gui/theme.h"
#include "gui/welcome_dialog.h"
#include "gui/widgets.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

QString g_outDir;
QStringList g_log;
QTextStream& err() {
    static QTextStream s(stderr);
    return s;
}
void note(const QString& s) {
    err() << s << '\n';
    err().flush();
    g_log << s;
}

int usage() {
    note("usage: ui_shots pages   --out <dir> [--demo] [--size WxH] [--names a,b,...] [--settle ms]");
    note("       ui_shots dialogs --out <dir> [--demo]");
    note("       --demo  the tool seeds a demo data set itself; without it both modes wait for");
    note("               <out>/GO and the caller seeds through the HTTP API");
    note("env:   MIDERHIVE_HOME (isolated data dir, required), MIDERHIVE_PORT (required),");
    note("       MIDERHIVE_SHOTS_LANG (zh by default)");
    return 2;
}

struct Options {
    QString mode;
    QString outDir;
    QSize size{1280, 720};
    QStringList names;
    int settleMs = 1200;
    bool demo = false;  // --demo：工具自己灌一套演示数据，不等触发文件
};

Options parse(int argc, char** argv) {
    Options o;
    if (argc < 2) return o;
    o.mode = QString::fromLocal8Bit(argv[1]);
    for (int i = 2; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        // 只有带值的选项才去取下一个参数：提前取的话，--demo 这种开关会把后面的
        // --size / --names 一并吞掉
        const auto next = [&]() -> QString {
            return i + 1 < argc ? QString::fromLocal8Bit(argv[++i]) : QString();
        };
        if (a == "--out") o.outDir = next();
        else if (a == "--demo") o.demo = true;
        else if (a == "--size") {
            const QStringList p = next().split('x');
            if (p.size() == 2) o.size = QSize(p[0].toInt(), p[1].toInt());
        } else if (a == "--names") o.names = next().split(',', Qt::SkipEmptyParts);
        else if (a == "--settle") o.settleMs = next().toInt();
    }
    return o;
}

// 把悬停状态清掉：窗口如果开在用户的鼠标下面，Qt 会把鼠标下面的控件标成"悬停"，悬停高亮
// 就被拍进图里（总览的「未解决错误」卡冒出一圈强调色边框），画面取决于鼠标停在哪。
// 做法与鼠标真的离开窗口时一样：给顶层窗口发一个 Leave 事件
void clearHover(QWidget* window) {
    if (QWindow* handle = window->windowHandle()) {
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(handle, &leave);
    }
}

void waitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 子树里运行中的有限时长动画跳到终态（条形扫掠、环弧、淡入）：拍的是用户 450ms 之后看到的
// 画面，而不是"拍之前跑了几个节拍"
void finishAnimations(QWidget* root) {
    for (QAbstractAnimation* a : root->findChildren<QAbstractAnimation*>()) {
        if (a->state() != QAbstractAnimation::Running) continue;
        const int total = a->totalDuration();
        if (total > 0) a->setCurrentTime(total);
    }
}

void writeMarker(const QString& name, const QByteArray& body) {
    QFile f(g_outDir + "/" + name);
    f.open(QIODevice::WriteOnly);
    f.write(body);
    f.close();
}

// ---- 演示数据（--demo）----------------------------------------------------------------------
// 一套贴近真实研发场景的数据：三个 Agent、六条知识、五个区块各一条记忆、四个技能（有调用
// 热度）、五条消息（含一条超过预览长度的）、两条错误（一条已解决）、铺满 14 天的用量；
// 周预算按本周实际用量反推到 48% 左右，预算环才有信息量。九个面板的数字因此互相连贯。
// 时间戳整体往前铺开：看起来像已经用了几天，而不是所有条目都"刚刚"。只能用在全新的数据
// 目录上（重复注册同名 Agent 会失败并中止）。
bool seedDemo(ah::Platform& p, QString* why) {
    std::string err;
    const auto fail = [&](const char* what) {
        *why = QString::fromLatin1(what) + ": " + QString::fromStdString(err);
        return false;
    };
    std::string mk;
    {
        std::ifstream f(ah::fsPath(std::string(ah::defaultHomeDir()) + "/config/master.key"));
        std::getline(f, mk);
    }
    // 每个 Agent 每天一笔用量：工作日 / 周末的输入 Token 区间（用量面板"各 Agent 本周用量"
    // 在任何一天出图都有三条柱子）
    struct Agent {
        const char* name;
        const char* task;
        const char* model;
        int64_t wdLo, wdHi, weLo, weHi;
    };
    const Agent agents[] = {
        {"claude", "重构登录模块：抽取验证码服务", "deepseek-chat", 300000, 800000, 80000, 260000},
        {"codex", "为技能市场补充调用示例", "gpt-5.3-codex", 250000, 850000, 70000, 240000},
        {"cursor", "修复 /api/knowledge 分页参数", "composer-1", 90000, 260000, 30000, 90000},
    };
    for (const auto& a : agents) {
        std::string key;
        if (!p.registerAgent(mk, a.name, "member", key, err)) return fail("registerAgent");
    }
    for (const auto& a : agents) p.heartbeat(a.name, a.task);

    ah::KnowledgeEntry k;
    struct Kn {
        const char* author;
        const char* title;
        const char* content;
        std::vector<std::string> tags;
        const char* category;
    };
    const Kn kn[] = {
        {"claude", "MSVC /utf-8 编译选项必须显式开启",
         "中文注释在 MSVC 默认代码页下会被拆坏，导致莫名其妙的编译错误；在 CMake 里"
         "add_compile_options(/utf-8) 一次解决。",
         {"msvc", "cmake"}, "踩坑"},
        {"claude", "SQLite WAL 模式的并发写要点",
         "两个进程同时写同一个库时，busy_timeout 必须显式设置；写事务尽量短，长事务会放大"
         "SQLITE_BUSY 的概率。",
         {"sqlite"}, "经验"},
        {"cursor", "PowerShell 5.1 传参防截断",
         "给子进程传 -DKEY=1.0.0 这类带点号的参数必须整体加引号，否则会被截断成 KEY=1 且不报错。",
         {"powershell"}, "踩坑"},
        {"codex", "Qt6 高 DPI 下截图要用物理像素",
         "PrintWindow + GetDIBits 拿到的就是物理像素；混用逻辑坐标会让裁剪框整体偏移一个缩放比例。",
         {"qt6", "dpi"}, "经验"},
        {"claude", "cpp-httplib 请求体上限与超时",
         "默认没有请求体上限，对外暴露前必须 set_payload_max_length；本机服务也要设，防止误传"
         "大文件占内存。",
         {"httplib", "安全"}, "经验"},
        {"cursor", "图谱视图按语义相似度连线",
         "图谱的边来自知识的向量 kNN：条目写入时自动做内置嵌入，不需要外部模型；标签相同的"
         "条目通常落在同一簇。",
         {"graph", "embedding"}, "经验"},
    };
    for (const auto& e : kn)
        if (!p.knowledgeCreate(e.author, e.title, e.content, e.tags, e.category, {}, "", k, err))
            return fail("knowledgeCreate");

    ah::MemoryEntry me;
    struct Mem { const char* section; const char* key; const char* value; };
    const Mem mem[] = {
        {"project", "当前迭代", "界面审查与截图交付已完成；下一轮评估本地嵌入模型（ONNX bge-small）。"},
        {"decision", "为什么只监听 127.0.0.1",
         "协作数据包含明文密钥与私人笔记，默认不开外网监听；要跨机访问就自己架反向代理。"},
        {"preference", "回复风格", "中文优先、先给结论再给依据；代码改动要说明影响面，不要静默改行为。"},
        {"environment", "开发机",
         "Windows 11 + VS2026 + Qt 6.8.3；构建用 cmake --preset full，出包走 scripts/package.ps1。"},
        {"work_style", "提交前自查",
         "推送前跑 scripts/ci_local.ps1，全绿再推；改动界面后逐张看 harness 截图。"},
    };
    for (const auto& m : mem)
        if (!p.memorySet("claude", m.section, m.key, m.value, 0, me, err)) return fail("memorySet");

    ah::SkillInfo sk;
    struct Sk { const char* name; const char* display; const char* desc; const char* category; const char* owner; };
    const Sk sks[] = {
        {"release-package", "出包流水线", "一条命令出 MSI、ZIP 与 latest.json，并做可运行性自检", "构建发布", "codex"},
        {"db-backup", "数据库快照", "用 VACUUM INTO 生成一致性快照到备份目录，恢复需要主密钥", "运维", "claude"},
        {"api-smoke", "接口冒烟", "起一个隔离数据目录，把注册、心跳、知识、用量主链路跑一遍", "测试", "codex"},
        {"log-digest", "日志摘要", "把 platformd 的输出按错误级别归类，只把 warning 以上推给值班 Agent", "运维", "cursor"},
    };
    for (const auto& s : sks)
        if (!p.skillRegister(s.owner, s.name, s.display, s.desc, s.category,
                             R"({"type":"object","properties":{}})", sk, err))
            return fail("skillRegister");
    // 调用记录（技能库的"使用热度"）。status 是后端契约枚举：success | failed
    struct Inv {
        const char* caller;
        const char* skill;
        const char* summary;
        const char* status;
        int64_t ms, tin, tout;
    };
    const Inv invs[] = {
        {"claude", "api-smoke", "23 项全部通过，用时 41 秒", "success", 41000, 1800, 420},
        {"claude", "db-backup", "快照已写入 backups/platform.db", "success", 950, 640, 180},
        {"codex", "log-digest", "12 条 warning、2 条 error，已归并成 3 组", "success", 2200, 1300, 610},
        {"cursor", "release-package", "MSI 与 ZIP 已生成，ICE 校验通过", "success", 64000, 2400, 900},
        {"codex", "api-smoke", "23 项全部通过，用时 38 秒", "success", 38000, 1750, 400},
        {"claude", "db-backup", "快照已写入 backups/platform.db", "success", 900, 610, 170},
        {"codex", "log-digest", "9 条 warning，无 error", "success", 1900, 1150, 540},
        {"cursor", "release-package", "MSI 与 ZIP 已生成，ICE 校验通过", "success", 61000, 2350, 880},
        {"claude", "api-smoke", "23 项全部通过，用时 44 秒", "success", 44000, 1900, 460},
        {"codex", "api-smoke", "端口 8790 被占用，隔离实例没能启动", "failed", 2100, 310, 90},
        {"cursor", "log-digest", "无 warning 以上级别的日志", "success", 800, 420, 120},
    };
    for (const auto& v : invs)
        if (!p.skillInvoke(v.caller, v.skill, "{}", v.summary, v.status, v.ms, v.tin, v.tout, "", err))
            return fail("skillInvoke");

    ah::Message m;
    // 超过预览长度（500 字）的一条：「查看全文」才有东西可看。最先发出：列表按时间倒序，
    // 它排在最下面，不会一上来就占掉半屏
    if (!p.messageSend("note", "claude", "", "本周界面审查结论与后续约定",
                       "本周把九个面板逐张截图审查了一遍，结论和处理方式同步给所有 Agent：\n"
                       "1. 总览、用量分析、知识库、技能库、用户记忆、Agent 交流、错误报告、操作日志、"
                       "图谱，每个面板的有数据、空、读取失败三种状态都看过了；发现的问题已全部"
                       "修复，并进入回归门禁。\n"
                       "2. 以后凡是改动界面的提交，推送前必须跑 scripts/ci_local.ps1。UI harness 是"
                       "必过项：结构快照与基线不一致，或者触发了版式规则，CI 都会变红。\n"
                       "3. 新增的界面文案必须同时给出中文和英文，并在六个语言包里补齐译文。切换"
                       "语言后要核对一遍布局——德语的词更长，按钮和标签更容易被挤出边界。\n"
                       "4. 需要下断点调试时，用 scripts/deploy_dev.ps1 部署到 dist。那里是带符号的"
                       "RelWithDebInfo 构建，可以直接在 Visual Studio 里附加到进程，变量可能显示为"
                       "已优化掉。\n"
                       "5. 给下载页做网页重建用的参考截图放在 design-refs 目录下，按日期分文件夹；"
                       "界面有较大改动后要重新拍一组，九张图必须在同一窗口尺寸、同一份数据下一次"
                       "拍齐。\n"
                       "6. 自动摘要默认按设定间隔运行；连续三轮失败会暂停，重新保存 LLM 设置或"
                       "重启工作台后恢复。暂停期间手动触发也会被拒绝，并在用量面板说明原因。\n"
                       "7. 图谱的边来自知识的向量 kNN，条目越多越能看出聚类；标签相同的条目通常"
                       "落在同一簇，可以先用标签下拉筛选再逐个查看节点详情。\n"
                       "8. 提交代码前自己先看一遍 harness 的截图：它查的是结构，重叠、裁切、对比度"
                       "这类版式问题它不知道，只有人眼能看出来。\n"
                       "有不同意见直接回复这条消息。",
                       m, err))
        return fail("messageSend");
    if (!p.messageSend("note", "claude", "", "构建提示",
                       "发行包请走 scripts/package.ps1，不要手动 windeployqt——自检会拦住缺插件、"
                       "缺运行库的包。",
                       m, err))
        return fail("messageSend");
    if (!p.messageSend("question", "cursor", "claude", "知识库分页参数用哪种？",
                       "limit/offset 还是 cursor？想跟用量面板的查询风格保持一致。", m, err))
        return fail("messageSend");
    const std::string question = m.uuid;
    if (!p.messageSend("task", "claude", "codex", "补齐用量面板空状态文案",
                       "预算未设置时给一句说明，避免新用户以为用量没在统计。", m, err))
        return fail("messageSend");
    // 回复只能由原消息的发件人或收件人发出（与真实规则一致：提问的收件人来答）
    if (!p.messageReply("claude", question,
                        "建议 limit/offset：知识条目是追加式的，很少中途删除，游标带来的好处不大；"
                        "而且用量面板已经用了同一套参数。",
                        m, err))
        return fail("messageReply");

    ah::ErrorReport er;
    if (!p.errorReport("codex", "warning", "build", "技能调用超时：编译缓存重建",
                       "ccache 冷缓存重建超过 120 秒，期间技能调用排队等待；已改为后台任务。",
                       "", er, err))
        return fail("errorReport");
    ah::ErrorReport er2;
    if (!p.errorReport("cursor", "critical", "mcp", "agents.json 写到一半导致注册失败",
                       "临时文件替换前进程崩溃，留下半截文件；已改为写临时文件后原子替换，并显式"
                       "拒绝解析损坏的文件。",
                       "", er2, err))
        return fail("errorReport");
    ah::ErrorReport resolved;
    if (!p.errorResolve("cursor", er2.uuid,
                        "persistAgentKey 改为临时文件 + 原子替换，附带损坏文件拒绝解析的显式报错。",
                        resolved, err))
        return fail("errorResolve");

    // 用量铺满最近 14 天（确定性伪随机，出图稳定）
    std::mt19937 rng(20261125u);
    std::uniform_real_distribution<double> frac(0.26, 0.36);
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    const QDate today = nowUtc.date();
    for (int back = 0; back < 14; ++back) {
        const bool weekend = today.addDays(-back).dayOfWeek() >= 6;
        for (const auto& a : agents) {
            const int64_t lo = weekend ? a.weLo : a.wdLo;
            const int64_t hi = weekend ? a.weHi : a.wdHi;
            const int64_t tin =
                lo + int64_t(rng() % static_cast<unsigned>((hi - lo) / 1000 + 1)) * 1000;
            const std::string key = "demo-" + std::to_string(back) + "-" + a.name;
            bool dup = false;
            if (!p.usageReport(a.name, tin, int64_t(tin * frac(rng)), "chat", a.model, "", key, dup,
                               err))
                return fail("usageReport");
        }
    }

    // 时间戳往前铺开。上报接口只认"现在"，所以先上报再直接改库：created_at 与 week_start
    // 必须一起改，否则周聚合会串周
    std::vector<std::string> sql;
    for (int back = 0; back < 14; ++back) {
        int idx = 0;
        for (const auto& a : agents) {
            const std::string key = std::string("demo-") + std::to_string(back) + "-" + a.name;
            if (back == 0) {
                // 今天：十几到几十分钟前（用相对偏移，跨过零点也不会落到将来）
                sql.push_back("UPDATE token_usage SET created_at = strftime('%Y-%m-%dT%H:%M:%SZ',"
                              " created_at, '-" + std::to_string(15 + idx * 7) + " minutes'),"
                              " week_start = date(created_at, 'weekday 1', '-7 days')"
                              " WHERE idempotency_key = '" + key + "';");
            } else {
                // 上午 9 点到下午 3 点（本地）：一天里三笔落在同一个本地日期上。此前用 9 点
                // 起的 UTC 时刻，在 UTC+8 的下午会跨过本地零点、串到第二天
                char at[16];
                std::snprintf(at, sizeof at, "%02d:%02d:00", 1 + (back * 3 + idx) % 7,
                              (back * 17 + idx * 23) % 60);
                sql.push_back("UPDATE token_usage SET created_at = strftime('%Y-%m-%dT%H:%M:%SZ',"
                              " date(created_at, '-" + std::to_string(back) + " days'), '" + at + "'),"
                              " week_start = date(date(created_at, '-" + std::to_string(back) +
                              " days'), 'weekday 1', '-7 days')"
                              " WHERE idempotency_key = '" + key + "';");
            }
            ++idx;
        }
    }
    const auto stagger = [](const char* table, int minutes) {
        // 最新的一行留在几分钟前，每早一行再往前推 minutes 分钟
        return std::string("UPDATE ") + table +
               " SET created_at = strftime('%Y-%m-%dT%H:%M:%SZ', created_at, '-' || (4 + ((SELECT MAX(id) FROM " +
               table + ") - id) * " + std::to_string(minutes) + ") || ' minutes')";
    };
    sql.push_back(stagger("audit_log", 11));
    sql.push_back(stagger("knowledge_entries", 9 * 60));
    sql.push_back(stagger("memory_entries", 31 * 60));
    sql.push_back(stagger("skills", 23 * 60));
    sql.push_back(stagger("skill_invocations", 5 * 60));
    sql.push_back(stagger("messages", 26));
    sql.push_back(stagger("errors", 9 * 60));
    sql.push_back(stagger("agents", 6 * 24 * 60));
    // 用量上报是日志里条数最多、也最单调的一种：把它们整体往前推，操作日志的第一屏才会
    // 是注册、知识、消息、错误这些各有看头的事件
    sql.push_back("UPDATE audit_log SET created_at = strftime('%Y-%m-%dT%H:%M:%SZ',"
                  " created_at, '-20 hours') WHERE action = 'usage.report';");
    {
        ah::Database raw;
        if (!raw.open(std::string(ah::defaultHomeDir()) + "/platform.db", err)) return fail("open db");
        for (const auto& s : sql) {
            if (!raw.execScript(s, err)) {
                raw.close();
                return fail("execScript");
            }
        }
        raw.close();
    }

    // 预算按本周实际用量反推：占比落在 48% 左右
    ah::UsageSummary sum;
    if (!p.usageSummary(sum, err)) return fail("usageSummary");
    const double want = double(sum.total_tokens) / 0.48;
    double mag = 1.0;
    while (want > mag * 10.0) mag *= 10.0;
    int64_t budget = int64_t(10 * mag);
    for (double m : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0})
        if (m * mag >= want) {
            budget = int64_t(m * mag);
            break;
        }
    if (!p.usageSetBudget(ah::kManagerName, budget, err)) return fail("usageSetBudget");

    // 心跳放在最后：在线状态按最近一次心跳算（120 秒内），拍摄要在那之前做完
    for (const auto& a : agents) p.heartbeat(a.name, a.task);
    return true;
}

// 等调用方放触发文件（它造完演示数据再放），期间事件循环照常跑
void waitForGo(QObject* host, std::function<void()> onGo) {
    auto* ticker = new QTimer(host);
    QObject::connect(ticker, &QTimer::timeout, host, [ticker, onGo] {
        if (!QFile::exists(g_outDir + "/GO")) return;
        ticker->stop();
        ticker->deleteLater();
        onGo();
    });
    ticker->start(250);
}

// ---- 整窗截取（含系统标题栏）------------------------------------------------------------------
// PrintWindow(PW_RENDERFULLCONTENT) 取的是 DWM 给这个窗口留的画面：不依赖遮挡、不需要窗口在
// 前台。窗口边框用 DWM 的"扩展边界"（不含 Win11 的阴影），物理像素。
bool captureWholeWindow(WId winId, const QString& path, QString* why) {
    const HWND hwnd = reinterpret_cast<HWND>(winId);
    RECT rc{};
    if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)) != S_OK)
        GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        *why = "window rectangle is empty";
        return false;
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    GdiFlush();
    bool saved = false;
    if (ok && bits) {
        QImage img(static_cast<const uchar*>(bits), w, h, w * 4, QImage::Format_RGB32);
        saved = img.save(path, "PNG");
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!saved) {
        *why = QString("PrintWindow failed (ok=%1, %2x%3)").arg(int(ok)).arg(w).arg(h);
        return false;
    }
    note(QString("saved %1  %2x%3 px").arg(QDir::toNativeSeparators(path)).arg(w).arg(h));
    return true;
}

// ---- 启动（与 gui/main.cpp 同一步骤；界面设置改落到数据目录下的 INI）-----------------------
// 把工具检测与"已接入"判定和这台机器隔离开：与 ui_harness 同一套换根办法。不这样做的话，
// 总览页会出现"检测到 N 个未接入的 Agent 工具"的接入横幅——它取决于这台机器装了什么，
// 而且和演示数据里"三个 Agent 已在线"自相矛盾；参考截图应当在任何机器上都一样。
// 必须在 QApplication 创建之前调用（Qt 会缓存主目录）。
void isolateWorld(const QString& home) {
    const QString root = home + "/empty-world";
    for (const char* d : {"/home", "/local", "/appdata", "/bin"})
        QDir().mkpath(root + QLatin1String(d));
    qputenv("MIDERHIVE_CONNECT_ROOT", QDir::toNativeSeparators(root).toUtf8());
    qputenv("USERPROFILE", QDir::toNativeSeparators(root + "/home").toUtf8());
    qputenv("HOME", QDir::toNativeSeparators(root + "/home").toUtf8());
    qputenv("LOCALAPPDATA", QDir::toNativeSeparators(root + "/local").toUtf8());
    qputenv("APPDATA", QDir::toNativeSeparators(root + "/appdata").toUtf8());
    // PATH 只留 Qt 的 bin（Qt6Core.dll 所在目录）与系统目录：工具按可执行文件名检测
    QString qtBin;
    const QStringList entries =
        qEnvironmentVariable("PATH").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& e : entries)
        if (QFile::exists(e + "/Qt6Core.dll")) qtBin = e;
    qputenv("PATH", (qtBin + ";C:\\Windows\\System32").toUtf8());
}

struct Boot {
    std::unique_ptr<ah::Platform> platform;
    int port = 0;
};

bool boot(Boot* b) {
    if (qEnvironmentVariableIsEmpty("MIDERHIVE_HOME") || qEnvironmentVariableIsEmpty("MIDERHIVE_PORT")) {
        note("FATAL  MIDERHIVE_HOME 与 MIDERHIVE_PORT 必须由调用方指定"
             "（工具不碰默认数据目录与默认端口）");
        return false;
    }
    const QString home = QString::fromStdString(ah::defaultHomeDir());
    if (!QDir().mkpath(home)) {
        note("FATAL  无法创建数据目录: " + home);
        return false;
    }
    const QString settingsDir = home + "/ui-shots-settings";
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    {
        QSettings s;
        s.setValue("ui/autoCheck", false);  // 截图期间不做更新检查
        s.setValue("ui/welcomeSeen", true); // 首启引导在 dialogs 模式里单独拍
        // 默认主题、默认字号：参考图不该取决于这台机器上一次用过什么外观
        s.setValue("ui/theme", "hive");
        s.setValue("ui/fontBase", 12);
        s.remove("ui/geometry");
        s.sync();
    }
    {
        QSettings lang("miderhive", "miderhive");
        lang.setValue("ui/lang", qEnvironmentVariable("MIDERHIVE_SHOTS_LANG", "zh"));
        lang.sync();
    }
    i18n::load();
    {
        QFont f = qApp->font();
        f.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                       QStringLiteral("Microsoft YaHei UI")});
        f.setFeature(QFont::Tag("tnum"), 1);
        qApp->setFont(f);
    }
    qApp->setStyleSheet(ui::themeQss());

    // 显示用的"现在"冻结在启动时刻：同一批截图里的相对时间不随拍摄先后变化
    qputenv("MIDERHIVE_UI_NOW",
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());

    b->platform = std::make_unique<ah::Platform>(ah::defaultHomeDir());
    std::string e;
    if (!b->platform->bootstrap(e)) {
        note("FATAL  平台初始化失败: " + QString::fromStdString(e));
        return false;
    }
    b->port = qEnvironmentVariableIntValue("MIDERHIVE_PORT");
    if (!b->platform->startHttpServer(b->port, e)) {
        note(QString("FATAL  内置 HTTP 服务启动失败（127.0.0.1:%1）: %2")
                 .arg(b->port)
                 .arg(QString::fromStdString(e)));
        return false;
    }
    return true;
}

// ---- pages -----------------------------------------------------------------------------------
int runPages(const Options& o) {
    qputenv("QT_QPA_PLATFORM", "windows");
    Boot b;
    if (!boot(&b)) return 1;
    if (o.demo) {
        QString why;
        if (!seedDemo(*b.platform, &why)) {
            note("FATAL  演示数据注入失败: " + why);
            return 3;
        }
    }

    MainWindow w(*b.platform);
    w.setAttribute(Qt::WA_ShowWithoutActivating);
    // 托盘图标会出现在用户的通知区：截图工具不需要它
    for (QSystemTrayIcon* t : w.findChildren<QSystemTrayIcon*>()) t->hide();
    w.setMinimumSize(o.size);
    w.resize(o.size);
    // 窗口尽量别开在用户的鼠标下面（悬停高亮会被拍进图里）：鼠标在上半屏就放下面，反之放上面
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        const int y = QCursor::pos().y() < avail.center().y()
                          ? qMax(avail.top(), avail.bottom() - o.size.height() + 1 - 48)
                          : avail.top();
        w.move(avail.left() + qMax(0, (avail.width() - o.size.width()) / 2), y);
    }
    w.show();
    clearHover(&w);
    // 压到 Z 序最底层：不挡用户手头的窗口（PrintWindow 不在乎遮挡）
    SetWindowPos(reinterpret_cast<HWND>(w.winId()), HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    auto* stack = w.findChild<QStackedWidget*>();
    auto* nav = w.findChild<QListWidget*>();
    if (!stack || !nav) {
        note("FATAL  找不到主窗口的导航 / 堆叠（结构变化？）");
        return 1;
    }
    if (qEnvironmentVariableIntValue("MIDERHIVE_SHOTS_DEBUG")) {
        // 版式排查用：把状态栏每个格子的几何与样式打出来（曾靠它查出"每格右缘一道白线"）
        if (auto* sb = w.findChild<QStatusBar*>()) {
            note("status bar children:");
            for (QWidget* c : sb->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (!c->isVisible()) continue;
                note(QString("  status | %1 '%2' x=%3 y=%4 w=%5 h=%6 hint=%7x%8 | %9")
                         .arg(QString::fromLatin1(c->metaObject()->className()), c->objectName())
                         .arg(c->x())
                         .arg(c->y())
                         .arg(c->width())
                         .arg(c->height())
                         .arg(c->sizeHint().width())
                         .arg(c->sizeHint().height())
                         .arg(c->styleSheet().left(60)));
            }
        }
    }
    const int rows = qMin(nav->count(), stack->count());
    note(QString("window up: %1x%2 logical, %3 panels, port %4")
             .arg(o.size.width())
             .arg(o.size.height())
             .arg(rows)
             .arg(b.port));

    const std::function<void()> captureAll = [&] {
        int failed = 0;
        for (int row = 0; row < rows; ++row) {
            nav->setCurrentRow(row);
            if (auto* pb = dynamic_cast<PanelBase*>(stack->currentWidget()))
                pb->refresh();  // 同步刷新：不等 3 秒轮询
            finishAnimations(&w);
            waitMs(o.settleMs);
            finishAnimations(&w);
            clearHover(&w);  // 等待期间鼠标若动过，悬停会回来
            waitMs(80);
            const QString name = row < o.names.size() ? o.names[row]
                                                      : QString("panel-%1").arg(row + 1);
            QString why;
            if (!captureWholeWindow(w.winId(), g_outDir + "/" + name + ".png", &why)) {
                note("FAIL  " + name + ": " + why);
                ++failed;
            }
        }
        writeMarker("DONE", QString("failed=%1\n").arg(failed).toUtf8());
        note(QString("pages done, %1 failed").arg(failed));
        QCoreApplication::exit(failed ? 1 : 0);
    };
    if (o.demo) {
        // 首轮刷新与入场动画落定之后再拍
        QTimer::singleShot(1500, &w, captureAll);
    } else {
        note("waiting for " + QDir::toNativeSeparators(g_outDir + "/GO"));
        waitForGo(&w, captureAll);
    }
    return QApplication::exec();
}

// ---- dialogs ---------------------------------------------------------------------------------
QWidget* g_mainWindow = nullptr;

// 当前该截的窗口：模态对话框 → 弹出菜单 → 对话框 → 其它可见的顶层（Toast 之类的气泡排在最后）
QWidget* topDialog() {
    if (QWidget* m = QApplication::activeModalWidget()) return m;
    QWidget* dialog = nullptr;
    QWidget* plain = nullptr;
    for (QWidget* t : QApplication::topLevelWidgets()) {
        if (t == g_mainWindow || !t->isVisible()) continue;
        if (t->windowFlags() & Qt::Popup) return t;
        if (qobject_cast<QDialog*>(t)) return t;
        if (!plain) plain = t;
    }
    return dialog ? dialog : plain;
}

// 主窗口之外的顶层全部关掉（模态的关掉 = 让 exec() 返回）
void closeAllTopLevels() {
    for (QWidget* t : QApplication::topLevelWidgets()) {
        if (t == g_mainWindow || !t->isVisible()) continue;
        if (t->windowFlags() & Qt::Popup) t->hide();
        else t->close();
    }
    waitMs(150);
}

bool looksDestructive(const QString& text) {
    static const char16_t* zh[] = {u"删除", u"清空", u"重置", u"断开", u"拒绝", u"退出", u"覆盖",
                                   u"撤销", u"注销", u"吊销", u"卸载", u"清除"};
    for (const auto* w : zh)
        if (text.contains(QString::fromUtf16(w))) return true;
    static const char* en[] = {"Delete", "Clear",    "Reset",       "Disconnect", "Decline",
                               "Quit",  "Overwrite", "Revoke",      "Remove",     "Discard"};
    for (const char* e : en)
        if (text.contains(QLatin1String(e), Qt::CaseInsensitive)) return true;
    return false;
}

// 版式检查：控件被裁出对话框 / 标签文字放不下 / 破坏性按钮是默认按钮 / 没有焦点落点
QStringList lint(QWidget* dlg) {
    QStringList out;
    if (qobject_cast<QMenu*>(dlg)) return out;
    std::function<void(QWidget*)> walk = [&](QWidget* parent) {
        // 滚动区、下拉框、微调框的内部构件自己管裁切，不在这里判
        if (qobject_cast<QAbstractScrollArea*>(parent) || qobject_cast<QComboBox*>(parent) ||
            qobject_cast<QAbstractSpinBox*>(parent))
            return;
        for (QWidget* c : parent->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (c->isVisible() && !c->isWindow() &&
                !c->testAttribute(Qt::WA_TransparentForMouseEvents)) {
                const QRect r(c->mapTo(dlg, QPoint(0, 0)), c->size());
                if (!dlg->rect().contains(r)) {
                    const QString id = c->objectName().isEmpty() ? c->accessibleName() : c->objectName();
                    out << QString("CLIPPED   %1 '%2' at (%3,%4) %5x%6, outside %7x%8")
                               .arg(QString::fromLatin1(c->metaObject()->className()), id)
                               .arg(r.x())
                               .arg(r.y())
                               .arg(r.width())
                               .arg(r.height())
                               .arg(dlg->width())
                               .arg(dlg->height());
                }
                if (auto* lb = qobject_cast<QLabel*>(c);
                    lb && !lb->wordWrap() && !lb->text().isEmpty() &&
                    lb->sizeHint().width() > lb->width() + 1) {
                    // 不换行的标签放不下就是被裁；富文本的 sizeHint 也是整段宽度
                    out << QString("LABEL-CUT %1px missing: \"%2\"")
                               .arg(lb->sizeHint().width() - lb->width())
                               .arg(lb->text().left(40));
                }
            }
            walk(c);
        }
    };
    walk(dlg);
    for (QPushButton* b : dlg->findChildren<QPushButton*>()) {
        if (b->isVisible() && b->isEnabled() && b->isDefault() && looksDestructive(b->text()))
            out << QString("DEFAULT-DESTRUCTIVE  \"%1\" is the default button").arg(b->text());
    }
    // 要写多行正文的对话框太窄：一行只有十来个字的宽度，说明写不开
    // （新建知识 310 像素、登记解决 278 像素的时候就是这样）
    if (dlg->width() < 480 && !dlg->findChildren<QPlainTextEdit*>().isEmpty())
        out << QString("CRAMPED-EDITOR  a dialog with a multi-line editor is only %1 px wide")
                   .arg(dlg->width());
    if (!QApplication::focusWidget())
        out << "NO-FOCUS  no widget has keyboard focus while the dialog is open";
    return out;
}

QString describe(QWidget* dlg) {
    QStringList parts;
    parts << QString("%1x%2 (minimum hint %3x%4)")
                 .arg(dlg->width())
                 .arg(dlg->height())
                 .arg(dlg->minimumSizeHint().width())
                 .arg(dlg->minimumSizeHint().height());
    if (QWidget* f = QApplication::focusWidget())
        parts << "focus=" + QString::fromLatin1(f->metaObject()->className());
    QStringList buttons;
    for (QPushButton* b : dlg->findChildren<QPushButton*>())
        if (b->isVisible()) buttons << (b->isDefault() ? "*" : "") + b->text();
    if (!buttons.isEmpty()) parts << "buttons: " + buttons.join(" | ");
    return parts.join(";  ");
}

// 截指定窗口并检查
bool captureDialog(QWidget* dlg, const QString& name) {
    finishAnimations(dlg);
    dlg->grab().save(g_outDir + "/" + name + ".png", "PNG");
    const QString title = qobject_cast<QMenu*>(dlg) ? QStringLiteral("(menu)") : dlg->windowTitle();
    note(QString("DIALOG    %1 | %2 | \"%3\" | %4")
             .arg(name, QString::fromLatin1(dlg->metaObject()->className()), title, describe(dlg)));
    for (const QString& l : lint(dlg)) note("LINT      " + name + " | " + l);
    return true;
}

// 截当前该截的窗口并检查；没有对话框就返回 false（调用方决定算不算问题）
bool grabTop(const QString& name) {
    QWidget* dlg = topDialog();
    return dlg && captureDialog(dlg, name);
}

void dumpTopLevels(const char* why) {
    for (QWidget* t : QApplication::topLevelWidgets()) {
        if (!t->isVisible()) continue;
        note(QString("          %1: visible top-level %2 '%3' %4x%5 modal=%6")
                 .arg(QString::fromLatin1(why), QString::fromLatin1(t->metaObject()->className()),
                      t->objectName())
                 .arg(t->width())
                 .arg(t->height())
                 .arg(t->isModal() ? 1 : 0));
    }
}

// 打开一个对话框 → 等它落定 → 截图 + 检查 → 关掉。trigger 可能阻塞在 exec() 里（模态），也可能
// 立刻返回（非模态 / 弹出菜单），两种都成立：定时器在两种事件循环里都会触发。
void shot(const QString& name, const std::function<void()>& trigger) {
    bool grabbed = false;
    QTimer::singleShot(500, QCoreApplication::instance(), [&] {
        grabbed = grabTop(name);
        // 模态对话框必须在这里关：此刻 trigger 还卡在 exec() 里，等它返回再关就永远关不掉
        if (grabbed) closeAllTopLevels();
    });
    trigger();
    if (!grabbed) {
        waitMs(700);  // 没阻塞也没有弹窗：非模态的 show() 之后定时器才会触发
        if (!grabbed) {
            note("MISSING   " + name + ": no dialog or menu appeared");
            dumpTopLevels("after");
        }
        closeAllTopLevels();
    }
}

QPushButton* buttonByText(QWidget* root, const QStringList& needles) {
    for (QPushButton* b : root->findChildren<QPushButton*>())
        for (const QString& n : needles)
            if (b->text().contains(n)) return b;
    return nullptr;
}

int runDialogs(const Options& o) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR",
                QDir::toNativeSeparators(qEnvironmentVariable("WINDIR") + "/Fonts").toUtf8());
    Boot b;
    if (!boot(&b)) return 1;
    if (o.demo) {
        QString why;
        if (!seedDemo(*b.platform, &why)) {
            note("FATAL  演示数据注入失败: " + why);
            return 3;
        }
    }

    MainWindow w(*b.platform);
    g_mainWindow = &w;
    w.setMinimumSize(o.size);
    w.resize(o.size);
    w.show();
    for (int i = 0; i < 6; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 120);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    auto* stack = w.findChild<QStackedWidget*>();
    auto* nav = w.findChild<QListWidget*>();
    if (!stack || !nav) {
        note("FATAL  找不到主窗口的导航 / 堆叠（结构变化？）");
        return 1;
    }
    const auto panelAt = [&](int row) {
        nav->setCurrentRow(row);
        if (auto* pb = dynamic_cast<PanelBase*>(stack->currentWidget())) pb->refresh();
        waitMs(250);
        return stack->currentWidget();
    };

    // 看门狗：巡览途中冒出一个没人关的模态框，整个工具就会卡死在 exec() 里。同一个模态框
    // 连续几拍还在就关掉并记下来
    QWidget* stuckSince = nullptr;
    int stuckTicks = 0;
    auto* watchdog = new QTimer(&w);
    QObject::connect(watchdog, &QTimer::timeout, &w, [&] {
        QWidget* m = QApplication::activeModalWidget();
        if (!m) {
            stuckSince = nullptr;
            stuckTicks = 0;
            return;
        }
        if (m != stuckSince) {
            stuckSince = m;
            stuckTicks = 0;
            return;
        }
        if (++stuckTicks >= 6) {  // 3 秒
            note(QString("STUCK     a modal dialog stayed open: \"%1\" (closed by the watchdog)")
                     .arg(m->windowTitle()));
            m->close();
            stuckSince = nullptr;
            stuckTicks = 0;
        }
    });
    watchdog->start(500);

    const std::function<void()> tour = [&] {
        // ---- 首启引导（单页）：首屏 → 填一个演示身份名 → 点 Claude Code → 结果面板出现
        //      （身份名预填、片段生成、面板滚入视野）→「开始使用」。名字由巡览先填好：
        //      用工具的默认名会撞上演示数据里已在线的同名身份，触发"继续重新签发吗？" ----
        {
            auto* welcome = new WelcomeDialog(*b.platform, &w);
            welcome->show();
            waitMs(400);
            captureDialog(welcome, "welcome");
            if (auto* edit = welcome->findChild<QLineEdit*>()) edit->setText("docs-shots");
            QPushButton* tool = nullptr;
            for (QPushButton* b : welcome->findChildren<QPushButton*>())
                if (b->property("toolId").toString() == QLatin1String("claude-code")) tool = b;
            if (tool) {
                // pickTool 里的确认框（身份在线时问"继续重新签发吗？"）是模态的：定时器在
                // 它自己的事件循环里拍下，并选"否"
                QTimer::singleShot(500, QCoreApplication::instance(), [] {
                    QWidget* m = QApplication::activeModalWidget();
                    if (!m) return;
                    captureDialog(m, "welcome-pick-confirm");
                    for (QPushButton* b : m->findChildren<QPushButton*>())
                        if (b->text().contains("No") || b->text().contains("否")) {
                            b->click();
                            return;
                        }
                    m->close();
                });
                tool->click();  // 有确认框时阻塞到它关掉为止
                waitMs(300);
                captureDialog(welcome, "welcome-tool-picked");
            }
            if (QPushButton* go = buttonByText(welcome, {"开始使用", "Get started"})) go->click();
            welcome->close();
        }
        closeAllTopLevels();

        // ---- 接入向导 ----
        {
            auto* connect = new ConnectDialog(*b.platform, &w);
            shot("connect", [&] { connect->show(); });
            delete connect;
        }

        // ---- 设置（每一页）/ 临时问答 / 语言菜单 ----
        {
            // 侧栏脚注里的三个按钮是同一个父控件下的直接子控件。不能在整棵树里按"排除法"
            // 找：面板里的区块折叠按钮、欢迎页里的"复制"也都是 QToolButton，上次就这样
            // 误点了"复制"（顺带覆盖了剪贴板）
            QToolButton* settingsBtn = nullptr;
            QToolButton* askBtn = nullptr;
            QToolButton* langBtn = w.findChild<QToolButton*>("langBtn");
            if (langBtn && langBtn->parentWidget()) {
                for (QToolButton* t : langBtn->parentWidget()->findChildren<QToolButton*>(
                         QString(), Qt::FindDirectChildrenOnly)) {
                    if (t == langBtn) continue;
                    if (t->accessibleName().contains(QStringLiteral("设置")) ||
                        t->accessibleName().contains("Setting"))
                        settingsBtn = t;
                    else askBtn = t;
                }
            }
            if (settingsBtn) {
                settingsBtn->click();
                waitMs(500);
                if (QWidget* dlg = topDialog()) {
                    // 设置页的导航是"列表 + 堆叠"；没有列表就按堆叠的页数切
                    QListWidget* list = dlg->findChild<QListWidget*>();
                    QStackedWidget* pages = dlg->findChild<QStackedWidget*>();
                    const int n = pages ? pages->count() : 0;
                    for (int i = 0; i < n; ++i) {
                        if (list && i < list->count()) list->setCurrentRow(i);
                        else pages->setCurrentIndex(i);
                        waitMs(250);
                        finishAnimations(dlg);
                        const QString name = QString("settings-%1").arg(i + 1, 2, 10, QLatin1Char('0'));
                        dlg->grab().save(g_outDir + "/" + name + ".png", "PNG");
                        const QString page = list && i < list->count() ? list->item(i)->text() : QString();
                        note(QString("DIALOG    %1 | \"%2\" | %3").arg(name, page, describe(dlg)));
                        for (const QString& l : lint(dlg)) note("LINT      " + name + " | " + l);
                    }
                } else {
                    note("MISSING   settings: the settings window did not appear");
                }
                closeAllTopLevels();
            }
            if (askBtn) shot("ask", [&] { askBtn->click(); });
            if (langBtn && langBtn->menu()) {
                QMenu* menu = langBtn->menu();
                shot("menu-language", [&] { menu->popup(langBtn->mapToGlobal(QPoint(0, langBtn->height()))); });
            }
            for (QSystemTrayIcon* tray : w.findChildren<QSystemTrayIcon*>())
                if (QMenu* menu = tray->contextMenu()) shot("menu-tray", [&] { menu->popup(QPoint(80, 80)); });
        }

        // ---- 面板里的对话框（按侧栏顺序）----
        struct PanelShot {
            int row;
            QString name;
            std::function<QWidget*(QWidget* panel)> trigger;  // 返回空 = 没有可点的入口
        };
        const std::vector<PanelShot> shots = {
            {0, "dashboard-budget-edit", [](QWidget* p) -> QWidget* {
                 QPushButton* b = p->findChild<QPushButton*>("editBudget");
                 if (b) b->click();
                 return b;
             }},
            {1, "usage-budget-edit", [](QWidget* p) -> QWidget* {
                 QPushButton* b = p->findChild<QPushButton*>("editBudget");
                 if (b) b->click();
                 return b;
             }},
            {2, "knowledge-new", [](QWidget* p) -> QWidget* {
                 QPushButton* b = buttonByText(p, {"新建条目", "New Entry"});
                 if (b) b->click();
                 return b;
             }},
            {2, "knowledge-append", [](QWidget* p) -> QWidget* {
                 QPushButton* b = buttonByText(p, {"追加新版本", "Append Version"});
                 if (b && b->isEnabled()) b->click();
                 return b;
             }},
            {2, "knowledge-viewer", [](QWidget* p) -> QWidget* {
                 auto* t = p->findChild<QTableWidget*>();
                 // 双击首行 = 大窗阅读全文。从外部触发信号与真实双击走同一条槽
                 if (t && t->rowCount() > 0)
                     QMetaObject::invokeMethod(t, "cellDoubleClicked", Q_ARG(int, 0), Q_ARG(int, 0));
                 return t;
             }},
            {2, "menu-table-context", [](QWidget* p) -> QWidget* {
                 auto* t = p->findChild<QTableWidget*>();
                 // 真实的右键落在视口上，由视口转交给表格；直接发给表格走不到菜单
                 if (t) {
                     const QPoint at(40, qMax(0, t->horizontalHeader()->height()) + t->rowHeight(0) / 2);
                     QContextMenuEvent e(QContextMenuEvent::Mouse, at, t->viewport()->mapToGlobal(at));
                     QApplication::sendEvent(t->viewport(), &e);
                 }
                 return t;
             }},
            {3, "skills-register", [](QWidget* p) -> QWidget* {
                 QPushButton* b = buttonByText(p, {"注册技能", "Register Skill"});
                 if (b) b->click();
                 return b;
             }},
            {4, "memory-edit", [](QWidget* p) -> QWidget* {
                 QPushButton* b = buttonByText(p, {"编辑 / 新增", "Edit / Add"});
                 if (b) b->click();
                 return b;
             }},
            {5, "messages-compose", [](QWidget* p) -> QWidget* {
                 QPushButton* b = buttonByText(p, {"发消息", "Message / Assign"});
                 if (b) b->click();
                 return b;
             }},
            {6, "errors-resolve", [](QWidget* p) -> QWidget* {
                 QPushButton* b = buttonByText(p, {"登记解决", "Mark Resolved"});
                 if (b && b->isEnabled()) b->click();
                 return b;
             }},
            {6, "errors-report", [](QWidget* p) -> QWidget* {
                 // 手动上报的入口在空状态里（有错误时是隐藏的）：按钮仍在，直接点
                 QPushButton* b = buttonByText(p, {"手动上报错误", "Report an issue"});
                 if (b) b->click();
                 return b;
             }},
        };
        for (const auto& s : shots) {
            QWidget* panel = panelAt(s.row);
            shot(s.name, [s, panel] { s.trigger(panel); });
        }

        // ---- 用户记忆：历史版本是两级对话框（先查询、再看表）。查询框里填一条真实存在的
        // 区块 / 键并确定，才看得到表；没有记录时程序弹的是"无记录"提示框 ----
        {
            QWidget* panel = panelAt(4);
            QPushButton* btn = buttonByText(panel, {"查看历史版本", "History"});
            std::vector<ah::MemoryEntry> entries;
            std::string merr;
            b.platform->memoryList("", entries, merr);
            if (btn && !entries.empty()) {
                const QString section = QString::fromStdString(entries.front().section);
                const QString key = QString::fromStdString(entries.front().key);
                QTimer::singleShot(500, QCoreApplication::instance(), [&, section, key] {
                    QWidget* dlg = QApplication::activeModalWidget();
                    if (!dlg) return;
                    grabTop("memory-history-query");
                    QTimer::singleShot(600, QCoreApplication::instance(), [] {
                        QWidget* v = QApplication::activeModalWidget();
                        if (v) {
                            grabTop("memory-history-view");
                            v->close();
                        }
                    });
                    if (auto* combo = dlg->findChild<QComboBox*>()) {
                        const int at = combo->findData(section);
                        if (at >= 0) combo->setCurrentIndex(at);
                    }
                    if (auto* edit = dlg->findChild<QLineEdit*>()) edit->setText(key);
                    QPushButton* ok = nullptr;
                    for (QPushButton* pb : dlg->findChildren<QPushButton*>())
                        if (pb->text().contains("OK") || pb->text().contains("确定")) ok = pb;
                    if (ok) ok->click();
                    else dlg->close();
                });
                btn->click();  // 阻塞在 exec() 里，直到上面的定时器把它关掉
            } else {
                note("SKIP      memory-history: no entry to look up");
            }
            closeAllTopLevels();
        }

        // ---- Agent 交流：先选中一条消息（与点击气泡走同一个信号），回复 / 查看全文才可用。
        //      「查看全文」只在正文超过预览长度时可用，所以选最长的一条 ----
        {
            QWidget* panel = panelAt(5);
            auto* chat = panel->findChild<QTextBrowser*>();
            std::vector<ah::Message> messages;
            std::string merr;
            b.platform->messageList("", "", "", "", 200, messages, merr);
            if (chat && !messages.empty()) {
                const ah::Message* longest = &messages.front();
                for (const auto& m : messages)
                    if (m.body.size() > longest->body.size()) longest = &m;
                const QUrl url(QString::fromStdString(longest->uuid));
                QMetaObject::invokeMethod(chat, "anchorClicked", Q_ARG(QUrl, url));
                waitMs(200);
                QPushButton* reply = buttonByText(panel, {"回复", "Reply"});
                if (reply && reply->isEnabled())
                    shot("messages-reply", [reply] { reply->click(); });
                else
                    note("SKIP      messages-reply: no message selected");
                QMetaObject::invokeMethod(chat, "anchorClicked", Q_ARG(QUrl, url));
                waitMs(200);
                QPushButton* view = buttonByText(panel, {"查看全文", "View full text"});
                if (view && view->isEnabled())
                    shot("messages-view", [view] { view->click(); });
                else
                    note("SKIP      messages-view: needs a message longer than the preview");
            } else {
                note("SKIP      messages-reply / messages-view: no messages");
            }
        }

        // ---- 总览：Agent 卡片点击弹出的操作菜单 ----
        {
            QWidget* panel = panelAt(0);
            ui::AgentCard* card = nullptr;
            for (QWidget* c : panel->findChildren<QWidget*>()) card = dynamic_cast<ui::AgentCard*>(c) ? static_cast<ui::AgentCard*>(c) : card;
            if (card) {
                shot("dashboard-agent-actions", [card] {
                    // 卡片只在松开左键时响应（见 AgentCard::mouseReleaseEvent）
                    QMouseEvent press(QEvent::MouseButtonPress, QPoint(12, 12), Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, QPoint(12, 12), Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(card, &press);
                    QApplication::sendEvent(card, &release);
                });
            } else {
                note("SKIP      dashboard-agent-actions: no agent card");
            }
        }

        writeMarker("DONE", "");
        writeMarker("dialogs.log", g_log.join('\n').toUtf8());
        note("dialogs done");
        QCoreApplication::exit(0);
    };
    if (o.demo) QTimer::singleShot(1200, &w, tour);
    else waitForGo(&w, tour);
    return QApplication::exec();
}

}  // namespace

int main(int argc, char** argv) {
    err().setEncoding(QStringConverter::Utf8);
    const Options o = parse(argc, argv);
    if (o.mode.isEmpty() || o.outDir.isEmpty()) return usage();
    // 换根必须在 QApplication 创建之前（dialogs 模式要看真实的工具检测结果，不换）
    if (o.mode == "pages" && o.demo && !qEnvironmentVariableIsEmpty("MIDERHIVE_HOME"))
        isolateWorld(QString::fromUtf8(qgetenv("MIDERHIVE_HOME")));

    QApplication app(argc, argv);
    app.setApplicationName("MiderHive 多 Agent 协作工作台");
    app.setOrganizationName("miderhive");
    g_outDir = QDir::toNativeSeparators(QDir(o.outDir).absolutePath());
    if (!QDir().mkpath(g_outDir)) {
        note("FATAL  无法创建输出目录: " + g_outDir);
        return 1;
    }
    if (o.mode == "pages") return runPages(o);
    if (o.mode == "dialogs") return runDialogs(o);
    return usage();
}
