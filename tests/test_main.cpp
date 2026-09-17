// 核心层单元测试（无外部测试框架，断言失败即退出码非 0）。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/embed/embedder.h"
#include "core/http/url_guard.h"
#include "core/integrations.hpp"
#include "core/platform.h"
#include "core/util.h"
#include "core/version_util.h"
#include "sqlite-vec.h"  // 生成头：第二个连接查 knowledge_vec 前需注册 vec0

namespace fs = std::filesystem;
using nlohmann::json;

static int g_checks = 0;
static int g_failures = 0;

static std::string toText(const std::string& s) { return "\"" + s + "\""; }
template <class T>
static std::string toText(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        auto va = (a);                                                       \
        auto vb = (b);                                                       \
        if (!(va == vb)) {                                                   \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__,       \
                        __LINE__, #a, #b, toText(va).c_str(),                \
                        toText(vb).c_str());                                 \
        }                                                                    \
    } while (0)

static void test_sha256() {
    CHECK_EQ(ah::sha256Hex("abc"),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(ah::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_week_start() {
    std::string ws = ah::weekStartIso();  // YYYY-MM-DD
    std::tm tm{};
    std::istringstream is(ws);
    is >> std::get_time(&tm, "%Y-%m-%d");
    CHECK(!is.fail());
    // get_time 不填 tm_wday，需经 time_t 往返换算星期
#ifdef _WIN32
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    std::tm lt{};
#ifdef _WIN32
    gmtime_s(&lt, &t);
#else
    gmtime_r(&t, &lt);
#endif
    CHECK_EQ(lt.tm_wday, 1);  // 周一
}

static void test_embedder() {
    ah::NgramHashEmbedder emb(384);
    auto a = emb.embed("CMake 项目使用 sqlite-vec 做向量检索");
    auto b = emb.embed("CMake 项目使用 sqlite-vec 做向量检索");
    CHECK_EQ(a.size(), 384u);
    CHECK(a == b);  // 确定性
    // L2 归一化
    double norm = 0;
    for (float v : a) norm += static_cast<double>(v) * v;
    CHECK(std::fabs(std::sqrt(norm) - 1.0) < 1e-5);
    // 不同文本向量不同
    auto c = emb.embed("今天晚饭吃什么，去楼下吃面吧");
    CHECK(a != c);
    // 相似文本的余弦相似度高于无关文本
    auto d = emb.embed("sqlite-vec 向量检索在 CMake 项目中的用法");
    auto dot = [](const std::vector<float>& x, const std::vector<float>& y) {
        double s = 0;
        for (size_t i = 0; i < x.size(); ++i) s += static_cast<double>(x[i]) * y[i];
        return s;
    };
    CHECK(dot(a, d) > dot(a, c));
    // 词边界：空白折叠为分隔符，"ab c" 与 "a bc" 的跨词 n-gram 不同
    CHECK(emb.embed("ab c") != emb.embed("a bc"));
    // 大小写折叠 + 连续空白等价于单词
    CHECK(emb.embed("AB  C") == emb.embed("ab c"));
}

static void test_url_guard() {
    using ah::net::isSafeOutboundUrl;
    CHECK(!isSafeOutboundUrl("http://127.0.0.1:8080/x"));
    CHECK(!isSafeOutboundUrl("http://localhost/x"));
    CHECK(!isSafeOutboundUrl("http://foo.localhost/x"));
    CHECK(!isSafeOutboundUrl("http://10.0.0.1/"));
    CHECK(!isSafeOutboundUrl("http://172.16.5.5/"));
    CHECK(!isSafeOutboundUrl("http://192.168.1.1/"));
    CHECK(!isSafeOutboundUrl("http://169.254.1.1/"));
    CHECK(!isSafeOutboundUrl("http://0.0.0.0/"));
    CHECK(!isSafeOutboundUrl("http://[::1]/"));
    CHECK(!isSafeOutboundUrl("http://[fd00::1]/"));
    CHECK(!isSafeOutboundUrl("file:///etc/passwd"));
    CHECK(!isSafeOutboundUrl("ftp://example.com/x"));
    CHECK(!isSafeOutboundUrl("http://user@example.com/"));
    CHECK(isSafeOutboundUrl("https://example.com/page"));
    CHECK(isSafeOutboundUrl("http://93.184.216.34:8080/x"));
    // 非规范 IPv4 写法：系统解析器会认成真实地址，字面解析认不出来 —— 必须拒绝
    CHECK(!isSafeOutboundUrl("http://127.1/"));
    CHECK(!isSafeOutboundUrl("http://0177.0.0.1/"));
    CHECK(!isSafeOutboundUrl("http://0x7f.0.0.1/"));
    CHECK(!isSafeOutboundUrl("http://2130706433/"));
    // 正常域名不能误伤（含数字但含非十六进制字符）
    CHECK(isSafeOutboundUrl("https://api.github.com/x"));
    CHECK(isSafeOutboundUrl("http://abc.de/x"));
}

// 密钥比较必须是常量时间实现（std::string::operator== 会在首个不同字节短路）
static void test_constant_time_equals() {
    CHECK(ah::constantTimeEquals("", ""));
    CHECK(ah::constantTimeEquals("abc", "abc"));
    CHECK(!ah::constantTimeEquals("abc", "abd"));
    CHECK(!ah::constantTimeEquals("abc", "abcd"));
    CHECK(!ah::constantTimeEquals("", "a"));
    CHECK(!ah::constantTimeEquals("0000000000", "1000000000"));  // 首字节差异
    CHECK(!ah::constantTimeEquals("0000000000", "0000000001"));  // 末字节差异
}

// 版本比较：更新检查的唯一判据，必须严格（判错会导致漏升级或降级误报）
static void test_version_compare() {
    using ah::compareVersions;
    using ah::isNewerVersion;
    CHECK_EQ(compareVersions("1.0.0", "1.0.0"), 0);
    CHECK_EQ(compareVersions("v1.0.0", "1.0.0"), 0);   // v 前缀等价
    CHECK_EQ(compareVersions("V1.0.0", "v1.0.0"), 0);  // 前缀大小写不敏感（V/v 混用）
    CHECK_EQ(compareVersions("V1.0.0", "1.0.0"), 0);
    CHECK(isNewerVersion("V1.0.1", "v1.0.0"));         // 跨大小写比较新版本
    CHECK(!isNewerVersion("v1.0.0", "V1.0.0"));
    CHECK_EQ(compareVersions("1.2", "1.2.0"), 0);     // 缺位补 0
    CHECK(compareVersions("1.0.1", "1.0.0") > 0);
    CHECK(compareVersions("1.1.0", "1.0.9") > 0);
    CHECK(compareVersions("2.0.0", "1.99.99") > 0);
    CHECK(compareVersions("1.0.0", "1.0.1") < 0);
    CHECK(compareVersions("1.10.0", "1.9.0") > 0);     // 数值比较而非字符串比较
    CHECK(compareVersions("0.9.9", "1.0.0") < 0);
    // 预发布后缀：同号更旧；两个都是预发布按后缀比较
    CHECK(compareVersions("1.0.0-rc1", "1.0.0") < 0);
    CHECK(compareVersions("1.0.0", "1.0.0-rc1") > 0);
    CHECK(compareVersions("1.0.0-rc1", "1.0.0-rc2") < 0);
    CHECK(compareVersions("1.0.1-rc1", "1.0.0") > 0);  // 号更大优先于预发布后缀
    // isNewerVersion：更新提示的判据
    CHECK(isNewerVersion("1.0.1", "1.0.0"));
    CHECK(!isNewerVersion("1.0.0", "1.0.0"));
    CHECK(!isNewerVersion("0.9.0", "1.0.0"));
    CHECK(!isNewerVersion("1.0.0-rc1", "1.0.0"));      // 预发布不该提示正式版用户升级
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 去掉文件末尾换行，与 bootstrap 的 getline 读取一致
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

static void writeFileRaw(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

static void step(const char* s) {
    std::printf("  . %s\n", s);
    std::fflush(stdout);
}

static void test_platform_end_to_end() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_" + ah::randomHex(8));
    fs::create_directories(tmp);

    {
        step("bootstrap");
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));

        // 主密钥来自运行期生成的配置文件
        std::string masterKey = readFile((tmp / "config" / "master.key").string());
        CHECK(!masterKey.empty());
        CHECK(p.authenticateMaster(masterKey));

        // 注册 Agent
        step("register");
        std::string key, err2;
        CHECK(p.registerAgent(masterKey, "hermes", "member", key, err2));
        CHECK(!key.empty());
        CHECK(p.authenticate("hermes", key));
        CHECK(!p.authenticate("hermes", "wrong-key"));
        CHECK(!p.authenticateMaster("wrong-master"));
        CHECK(!p.isManager("hermes"));
        CHECK(p.isManager("zcode"));

        // 心跳与在线状态
        step("heartbeat");
        p.heartbeat("hermes", "写单元测试");
        std::vector<ah::AgentInfo> agents;
        CHECK(p.listAgents(agents, err));
        bool hermesOnline = false;
        for (const auto& a : agents)
            if (a.name == "hermes" && a.status == "online" && a.current_task == "写单元测试")
                hermesOnline = true;
        CHECK(hermesOnline);

        // 知识库：创建 + 关键词/语义搜索 + 版本只追加
        step("knowledge.create");
        ah::KnowledgeEntry e1;
        CHECK(p.knowledgeCreate("hermes", "sqlite-vec 接入指南",
                                "把 sqlite-vec 静态编译进进程，用 vec0 虚拟表做向量检索，配合 CMake FetchContent。",
                                {"cmake", "向量"}, "技术文档", {}, "", e1, err));
        ah::KnowledgeEntry e2;
        CHECK(p.knowledgeCreate("claude", "Qt 布局技巧",
                                "QSplitter 加 QTableWidget 做左右分栏，定时器刷新面板。",
                                {"qt"}, "技术文档", {}, "", e2, err));

        std::vector<ah::KnowledgeHit> hits;
        step("knowledge.search.semantic");
        CHECK(p.knowledgeSearch("向量检索", ah::SearchMode::Semantic, 10, "", hits, err));
        CHECK(!hits.empty());
        CHECK_EQ(hits[0].entry.title, "sqlite-vec 接入指南");

        std::vector<ah::KnowledgeHit> kws;
        CHECK(p.knowledgeSearch("Qt", ah::SearchMode::Keyword, 10, "", kws, err));
        CHECK_EQ(kws.size(), 1u);

        // 追加版本：旧版本保留
        ah::KnowledgeEntry e3;
        CHECK(p.knowledgeAddVersion("hermes", e1.uuid, "", "第二版内容：补充 Windows 下的编译选项说明。",
                                    {}, "", e3, err));
        CHECK_EQ(e3.version, 2);
        std::vector<ah::KnowledgeEntry> versions;
        CHECK(p.knowledgeVersions(e1.uuid, versions, err));
        CHECK_EQ(versions.size(), 2u);
        CHECK_EQ(versions[1].version, 1);  // v1 内容原样
        CHECK(versions[1].content.find("sqlite-vec 静态编译") != std::string::npos);

        // 技能：先注册后调用
        step("skills");
        std::string err3;
        CHECK(!p.skillInvoke("hermes", "ghost-skill", "{}", "", "success", 1, 0, 0, "", err3));
        ah::SkillInfo sk;
        CHECK(p.skillRegister("hermes", "code-review", "代码审查", "审查代码并给出意见",
                              "开发", "{\"type\":\"object\"}", sk, err));
        CHECK(p.skillInvoke("claude", "code-review", "{\"file\":\"a.cpp\"}", "通过", "success",
                            120, 3000, 2000, "msg-e2e-ref", err));

        // 记忆：两次 set 生成两个版本；base_version 冲突被拒
        step("memory");
        ah::MemoryEntry m1, m2;
        CHECK(p.memorySet("hermes", "project", "current", "正在开发多 Agent 平台", 0, m1, err));
        CHECK_EQ(m1.version, 1);
        // 乐观并发：基于 v1 写入应冲突（最新已是 v1？不，此时最新是 v1，基于 v9 冲突）
        std::string conflictErr;
        ah::MemoryEntry mc;
        CHECK(!p.memorySet("codex", "project", "current", "并发覆盖尝试", 9, mc, conflictErr));
        CHECK(conflictErr.rfind("version conflict", 0) == 0);
        CHECK(p.memorySet("claude", "project", "current", "正在开发多 Agent 平台（Qt 工作台）", 0, m2, err));
        CHECK_EQ(m2.version, 2);
        std::vector<ah::MemoryEntry> hist;
        CHECK(p.memoryHistory("project", "current", hist, err));
        CHECK_EQ(hist.size(), 2u);
        std::vector<ah::MemoryEntry> allMem;
        CHECK(p.memoryList("", allMem, err));
        CHECK(!allMem.empty());

        // 消息：发送 + 状态流转
        step("messages");
        ah::Message msg;
        CHECK(p.messageSend("task", "claude", "hermes", "修个 bug", "知识搜索返回空，请排查", msg, err));
        CHECK_EQ(msg.status, "pending");
        CHECK(p.messageSetStatus("hermes", msg.uuid, "accepted", msg, err));
        CHECK_EQ(msg.status, "accepted");
        CHECK(p.messageSetStatus("hermes", msg.uuid, "done", msg, err));
        CHECK_EQ(msg.status, "done");
        // 非法流转被拒绝
        std::string err4;
        CHECK(!p.messageSetStatus("hermes", msg.uuid, "accepted", msg, err4));

        // 错误：上报 + 解决（说明追加）
        step("errors");
        ah::ErrorReport er;
        CHECK(p.errorReport("hermes", "error", "search", "向量表未创建", "distance 查询报错", "", er, err));
        CHECK(p.errorResolve("hermes", er.uuid, "重建 vec 表后恢复", er, err));
        CHECK_EQ(er.status, "resolved");
        CHECK(er.resolution_notes.find("重建") != std::string::npos);
        // 非上报者不能解决
        ah::ErrorReport er2;
        CHECK(p.errorReport("claude", "warning", "gui", "布局警告", "占位", "", er2, err));
        std::string err5;
        CHECK(!p.errorResolve("hermes", er2.uuid, "越权", er2, err5));
        CHECK(p.errorResolve("zcode", er2.uuid, "管理者代解决", er2, err5));

        // Token 预算：80% 告警、95% 预警、超限
        // （此前技能调用已消耗 5000 Token；预算以 10 万为基准分段验证）
        step("usage");
        CHECK(p.usageSetBudget("user", 100000, err));
        ah::UsageSummary sum;
        CHECK(p.usageSummary(sum, err));
        CHECK_EQ(sum.total_tokens, static_cast<int64_t>(5000));
        CHECK_EQ(sum.alert_level, "none");
        bool dup = false;
        CHECK(p.usageReport("hermes", 500, 400, "skill", "", "x", "", dup, err));  // 5.9%
        CHECK(p.usageSummary(sum, err));
        CHECK_EQ(sum.alert_level, "none");
        // 幂等：同一 idempotency_key 重复上报不重复扣减
        CHECK(p.usageReport("hermes", 75000, 0, "llm", "", "task-1", "idem-1", dup, err));
        CHECK(!dup);
        CHECK(p.usageSummary(sum, err));
        int64_t afterFirst = sum.total_tokens;
        CHECK(p.usageReport("hermes", 75000, 0, "llm", "", "task-1", "idem-1", dup, err));
        CHECK(dup);
        CHECK(p.usageSummary(sum, err));
        CHECK_EQ(sum.total_tokens, afterFirst);  // 未重复扣减
        CHECK_EQ(sum.alert_level, "warn");       // 80.9%
        CHECK(p.usageReport("hermes", 15000, 0, "llm", "", "", "idem-2", dup, err));  // 95.9%
        CHECK(p.usageSummary(sum, err));
        CHECK_EQ(sum.alert_level, "critical");
        CHECK(p.usageReport("hermes", 10000, 0, "llm", "", "", "idem-3", dup, err));  // 105.9%
        CHECK(p.usageSummary(sum, err));
        CHECK_EQ(sum.alert_level, "over");

        // 用量统计：带模型上报 → 逐日趋势（连续日期补 0）与按模型累计
        CHECK(p.usageReport("hermes", 1200, 300, "llm", "glm-5.3-flash", "ref-m1",
                            "idem-m1", dup, err));
        std::vector<ah::UsageDailyPoint> daily;
        CHECK(p.usageDaily(7, daily, err));
        CHECK_EQ(daily.size(), static_cast<size_t>(7));
        CHECK(daily.front().day < daily.back().day);  // 旧 → 新排列
        int64_t dayTotal = 0;
        for (const auto& d : daily) dayTotal += d.tokens;
        CHECK(dayTotal >= 1500);  // 至少覆盖本次带模型的 1500
        std::vector<ah::UsageModelRow> models;
        CHECK(p.usageByModel(models, err));
        CHECK(models.size() >= 1);
        CHECK_EQ(models.front().model, std::string("glm-5.3-flash"));
        CHECK_EQ(models.front().tokens, static_cast<int64_t>(1500));
        // 多维用量切片：同一套 WHERE 下的合计 / 按 Agent / 按模型 / 逐日
        ah::UsageBreakdown bd;
        CHECK(p.usageBreakdown(7, "", "", bd, err));
        CHECK_EQ(bd.daily.size(), static_cast<size_t>(7));
        CHECK_EQ(bd.total_tokens, bd.total_in + bd.total_out);
        CHECK(bd.per_agent.size() >= 1);
        int64_t agentSum = 0;
        for (const auto& r : bd.per_agent) agentSum += r.tokens;
        CHECK_EQ(agentSum, bd.total_tokens);  // 按 Agent 汇总必须等于合计
        CHECK_EQ(bd.per_model.size(), static_cast<size_t>(1));  // 只有 glm-5.3-flash 带模型
        CHECK_EQ(bd.per_model.front().tokens, static_cast<int64_t>(1500));
        // agent 过滤：只剩 hermes，且合计等于未过滤结果里 hermes 那一行
        ah::UsageBreakdown onlyHermes;
        CHECK(p.usageBreakdown(7, "hermes", "", onlyHermes, err));
        CHECK_EQ(onlyHermes.per_agent.size(), static_cast<size_t>(1));
        CHECK_EQ(onlyHermes.per_agent.front().agent, std::string("hermes"));
        int64_t hermesRow = -1;
        for (const auto& r : bd.per_agent)
            if (r.agent == "hermes") hermesRow = r.tokens;
        CHECK(hermesRow >= 0);
        CHECK_EQ(onlyHermes.total_tokens, hermesRow);
        // model 过滤：只留带模型的那一笔
        ah::UsageBreakdown onlyModel;
        CHECK(p.usageBreakdown(7, "", "glm-5.3-flash", onlyModel, err));
        CHECK_EQ(onlyModel.total_tokens, static_cast<int64_t>(1500));
        CHECK_EQ(onlyModel.per_agent.size(), static_cast<size_t>(1));
        CHECK_EQ(onlyModel.per_agent.front().agent, std::string("hermes"));
        CHECK_EQ(onlyModel.calls, static_cast<int64_t>(1));
        // agent + model 同时过滤；不存在的模型 → 全 0 而不是报错
        ah::UsageBreakdown both;
        CHECK(p.usageBreakdown(7, "hermes", "glm-5.3-flash", both, err));
        CHECK_EQ(both.total_tokens, static_cast<int64_t>(1500));
        ah::UsageBreakdown none;
        CHECK(p.usageBreakdown(7, "ghost-agent", "", none, err));
        CHECK_EQ(none.total_tokens, static_cast<int64_t>(0));
        CHECK(none.per_agent.empty());
        // 预算读写（界面「调整预算」走的就是这条路径）
        CHECK(p.usageSetBudget("zcode", 4'242'000, err));
        CHECK_EQ(p.usageBudget(err), static_cast<int64_t>(4'242'000));
        CHECK(!p.usageSetBudget("zcode", 0, err));   // 非正数被拒
        CHECK(!p.usageSetBudget("hermes", 5'000'000, err));  // 非管理者被拒
        CHECK_EQ(p.usageBudget(err), static_cast<int64_t>(4'242'000));
        // 超额（105.9%）：仅告警升级，不拦截技能调用（用量是观测不是限制）
        std::vector<ah::SkillInvocation> invs;
        CHECK(p.skillInvoke("hermes", "code-review", "{}", "", "success", 1, 0, 0, "", err));
        CHECK(p.skillInvocations("code-review", 10, invs, err));
        CHECK(!invs.empty());  // 超额后调用仍被记录

        // 审计留痕
        step("audit");
        std::vector<ah::AuditRecord> audit;
        CHECK(p.auditList("", "", "", 500, audit, err));
        CHECK(!audit.empty());
        bool foundReg = false;
        for (const auto& r : audit)
            if (r.action == "agent.register") foundReg = true;
        CHECK(foundReg);

        // Agent 管理：非管理者拒删 / 管理者不可自删 / 管理者删除后列表收缩、凭据失效
        step("agents.remove");
        std::string droidKey;
        CHECK(p.registerAgent(masterKey, "droid", "member", droidKey, err));
        CHECK(!droidKey.empty());
        CHECK(!p.agentRemove("hermes", "droid", err));
        CHECK(!p.agentRemove("zcode", "zcode", err));
        CHECK(p.agentRemove("zcode", "droid", err));
        std::vector<ah::AgentInfo> afterRemove;
        CHECK(p.listAgents(afterRemove, err));
        bool droidGone = true;
        for (const auto& a : afterRemove)
            if (a.name == "droid") droidGone = false;
        CHECK(droidGone);
        CHECK(!p.authenticate("droid", droidKey));  // 凭据随之失效

        // ---- 加固项：保留身份 / role 白名单 / 点对点消息读隔离 ----
        step("hardening.identity");
        std::string hErr, hKey;
        // "user" 在鉴权里被当作人类用户（可解决任何错误、流转任何任务状态），
        // 若可被注册，任何持有主密钥的 Agent 都能冒充，且审计会记错主体
        CHECK(!p.registerAgent(masterKey, "user", "member", hKey, hErr));
        CHECK(hErr.find("reserved") != std::string::npos);
        CHECK(!p.registerAgent(masterKey, "zcode", "member", hKey, hErr));
        CHECK(!p.registerAgent(masterKey, "system", "member", hKey, hErr));
        // 角色白名单：此前 role 原样入库，可以自封任意角色（含管理者角色）
        CHECK(!p.registerAgent(masterKey, "roleprobe", "root", hKey, hErr));
        CHECK(hErr.find("invalid role") != std::string::npos);
        CHECK(!p.registerAgent(masterKey, "roleprobe", "zcode", hKey, hErr));
        CHECK(p.registerAgent(masterKey, "roleprobe", "member", hKey, hErr));

        step("messages.visibility");
        ah::Message pm;
        CHECK(p.messageSend("task", "hermes", "claude", "私密任务", "只给 claude", pm, err));
        auto seen = [&](const std::string& viewer, const std::string& uuid, bool& found) {
            std::vector<ah::Message> vis;
            std::string e;
            found = false;
            if (!p.messageList("", "", "", "", 50, vis, e, viewer)) return false;
            for (const auto& m : vis)
                if (m.uuid == uuid) found = true;
            return true;
        };
        bool f = false;
        CHECK(seen("roleprobe", pm.uuid, f));  // 无关 Agent
        CHECK(!f);
        CHECK(seen("claude", pm.uuid, f));     // 收件人
        CHECK(f);
        CHECK(seen("hermes", pm.uuid, f));     // 发件人
        CHECK(f);
        // 广播消息（recipient 为空）对所有人可见，不受 viewer 收敛影响
        ah::Message bm;
        CHECK(p.messageSend("note", "hermes", "", "广播", "全员可见", bm, err));
        CHECK(seen("roleprobe", bm.uuid, f));
        CHECK(f);

        // HTTP API 冒烟：启动服务，Agent 客户端访问
        step("http.start");
        int port = 0;
        for (int cand = 17890; cand < 17990 && port == 0; ++cand) {
            if (p.startHttpServer(cand, err)) port = p.httpPort();
        }
        CHECK(port > 0);
        step("http.client");
        // 注意：httplib 0.16.x 的 Client("host", port) 不剥 scheme，必须用单串构造
        httplib::Client cli("http://127.0.0.1:" + std::to_string(port));
        auto res = cli.Get("/api/agents", {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        if (!res) std::printf("  http err code=%d\n", static_cast<int>(res.error()));
        CHECK(res && res->status == 200);
        if (res) {
            auto body = json::parse(res->body);
            CHECK_EQ(body["code"], 0);
            CHECK(body["data"].is_array());
        }
        auto res2 = cli.Get("/api/memory?section=project",
                            {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(res2 && res2->status == 200);
        auto bad = cli.Get("/api/agents", {{"X-Agent-Name", "hermes"}, {"X-Api-Key", "nope"}});
        if (!bad)
            std::printf("  bad-key err code=%d\n", static_cast<int>(bad.error()));
        else if (bad->status != 401)
            std::printf("  bad-key status=%d body=%s\n", bad->status, bad->body.c_str());
        CHECK(bad && bad->status == 401);

        // ---- 加固项：HTTP 输入健壮性（异常参数必须返回 4xx 而非 500/崩溃）----
        auto badLimit = cli.Get("/api/knowledge?limit=abc",
                                {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(badLimit && badLimit->status == 400);
        auto badLimit2 = cli.Get("/api/messages?limit=-5",
                                 {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(badLimit2 && badLimit2->status == 400);
        {
            nlohmann::json badTags = {{"title", "t"}, {"content", "c"}, {"tags", json::array({1, 2})}};
            auto r = cli.Post("/api/knowledge", {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                              badTags.dump(), "application/json");
            CHECK(r && r->status == 400);
        }
        {
            nlohmann::json badBudget = {{"budget", "很多"}};
            auto r = cli.Put("/api/usage/budget", {{"X-Master-Key", masterKey}},
                             badBudget.dump(), "application/json");
            CHECK(r && r->status == 400);
        }
        // 用量统计端点：正常请求 200 + 数据形态；异常 days 400
        auto httpDaily = cli.Get("/api/usage/daily?days=7",
                                 {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(httpDaily && httpDaily->status == 200);
        if (httpDaily) {
            auto body = json::parse(httpDaily->body);
            CHECK_EQ(body["code"], 0);
            CHECK(body["data"]["days"].is_array());
            CHECK_EQ(body["data"]["days"].size(), 7);
        }
        auto badDays = cli.Get("/api/usage/daily?days=abc",
                               {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(badDays && badDays->status == 400);
        auto badDays2 = cli.Get("/api/usage/daily?days=-3",
                                {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(badDays2 && badDays2->status == 400);
        auto badModel = cli.Post("/api/usage/report",
                                 {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                 nlohmann::json{{"tokens_in", 1}, {"tokens_out", 1},
                                                {"model", 42}}.dump(),
                                 "application/json");
        CHECK(badModel && badModel->status == 400);
        auto httpModels = cli.Get("/api/usage/models",
                                  {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(httpModels && httpModels->status == 200);
        if (httpModels) {
            auto body = json::parse(httpModels->body);
            CHECK(body["data"]["models"].is_array());
        }

        // ---- 加固项：长度校验 ----
        std::string longTitle(300, 'x');
        std::string vErr;
        ah::KnowledgeEntry longEntry;
        CHECK(!p.knowledgeCreate("hermes", longTitle, "内容", {}, "", {}, "", longEntry, vErr));
        CHECK(vErr.find("too long") != std::string::npos);

        // ---- 加固项：管理性删除（仅 zcode）----
        std::string rmErr;
        CHECK(!p.knowledgeRemove("hermes", e1.uuid, rmErr));  // 非管理者被拒
        CHECK(p.knowledgeRemove("zcode", e1.uuid, rmErr));
        std::vector<ah::KnowledgeEntry> gone;
        CHECK(p.knowledgeVersions(e1.uuid, gone, rmErr));
        CHECK(gone.empty());                                  // 版本全部移除
        CHECK(p.memoryRemove("zcode", "project", "current", rmErr));
        std::vector<ah::MemoryEntry> memGone;
        CHECK(p.memoryList("project", memGone, rmErr));
        CHECK(memGone.empty());
        // 删除是连历史版本一起删（COUNT 与 DELETE 原子），不是只翻转 is_latest
        std::vector<ah::MemoryEntry> memHist;
        CHECK(p.memoryHistory("project", "current", memHist, rmErr));
        CHECK(memHist.empty());
        CHECK(!p.memoryRemove("zcode", "project", "nonexistent", rmErr));

        // ---- 加固项：备份与恢复 ----
        std::vector<ah::KnowledgeEntry> before;
        CHECK(p.knowledgeList(100, "", before, rmErr));
        std::string backupPath;
        CHECK(p.backupCreate(backupPath, rmErr));
        CHECK(fs::exists(backupPath));
        std::vector<std::string> backups;
        CHECK(p.backupList(backups, rmErr));
        CHECK(backups.size() >= 1);
        // 删除全部知识条目后从备份恢复
        for (const auto& e : before) CHECK(p.knowledgeRemove("zcode", e.uuid, rmErr));
        std::vector<ah::KnowledgeEntry> emptied;
        CHECK(p.knowledgeList(100, "", emptied, rmErr));
        CHECK(emptied.empty());
        CHECK(p.backupRestore(backups[0], rmErr));
        std::vector<ah::KnowledgeEntry> restored;
        CHECK(p.knowledgeList(100, "", restored, rmErr));
        CHECK_EQ(restored.size(), before.size());
        // 非法备份名被拒（路径穿越防护）
        std::string travErr;
        CHECK(!p.backupRestore("../platform.db", travErr));

        // ---- 加固项：维护轮转 ----
        std::string stats;
        CHECK(p.maintenanceRun("zcode", stats, rmErr));
        CHECK(stats.find("deleted_audit") != std::string::npos);

        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 直接对库文件统计行数：事务一致性（正文-向量、计数-删除）只能在存储层验证，
// 走 API 探测会漏掉"列表可见但语义检索永远命中不了"的静默残留。
// 平台对每个连接单独调用 sqlite3_vec_init 注册 vec0，第二个连接同样要先注册，
// 否则查不了 knowledge_vec 虚表。
static int64_t countRows(const fs::path& dbPath, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int64_t n = -1;
    if (sqlite3_vec_init(db, nullptr, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return n;
}

// 回归：知识的"正文-向量"两个存储必须同生同灭。
// create() 是"INSERT 正文 + INSERT 向量"两步，knowledgeRemove() 是
// "DELETE 向量 + DELETE 正文"两步；任一步失败若不回滚都会留下单侧残留：
//   孤儿正文（is_latest=1 但无向量）——列表/关键词检索可见，语义检索永远命中不了且无报错；
//   孤儿向量（正文已删但 vec0 行还在）——占用语义检索的 k 个召回名额，挤出正常结果。
// 两条不变式直接查库验证：每条正文都有向量、每条向量都有正文。
static void test_knowledge_vector_atomicity() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_vec_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        ah::KnowledgeEntry e;
        CHECK(p.knowledgeCreate("zcode", "正文向量原子性",
                                "正文与向量必须同生同灭，删除后不允许任何单侧残留。",
                                {"回归"}, "验证", {}, "", e, err));
        CHECK(!e.uuid.empty());
        // 两个版本：create 与 addVersion 各写一条向量，remove 要把两条向量一起删干净
        ah::KnowledgeEntry v2;
        CHECK(p.knowledgeAddVersion("zcode", e.uuid, "", "第二版：覆盖追加版本路径的向量写入。", {}, "",
                                    v2, err));
        CHECK_EQ(v2.version, 2);

        CHECK(p.knowledgeRemove("zcode", e.uuid, err));

        // 不变式一：没有"有正文无向量"的孤儿（对应 create 的两步写入）。
        // 向量按维度分表，内置嵌入器 384 → knowledge_vec_d384
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries WHERE id NOT IN "
                                   "(SELECT entry_id FROM knowledge_vec_d384)"),
                 0);
        // 不变式二：没有"有向量无正文"的残留（对应 remove 的两步删除）
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_vec_d384 WHERE entry_id NOT IN "
                                   "(SELECT id FROM knowledge_entries)"),
                 0);
        // 双版本的正文行全部删除
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries"), 0);
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 存量旧库升级：旧 schema（agents 无 salt、token_usage 无 idempotency_key）
// 直接跑新版本 bootstrap 必须成功，旧格式密钥仍可认证。
static void test_legacy_migration() {
    fs::path tmp = fs::temp_directory_path() / ("zcode_legacy_" + ah::randomHex(6));
    fs::create_directories(tmp);
    {
        ah::Database raw;
        std::string err;
        CHECK(raw.open((tmp / "platform.db").string(), err));
        const char* oldSchema =
            "CREATE TABLE agents(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE,"
            " role TEXT NOT NULL DEFAULT 'member', api_key_hash TEXT NOT NULL, status TEXT,"
            " current_task TEXT, last_seen_at TEXT, created_at TEXT, updated_at TEXT);"
            "CREATE TABLE token_usage(id INTEGER PRIMARY KEY AUTOINCREMENT, agent TEXT NOT NULL,"
            " week_start TEXT NOT NULL, tokens_in INTEGER NOT NULL DEFAULT 0,"
            " tokens_out INTEGER NOT NULL DEFAULT 0, call_type TEXT, reference_id TEXT,"
            " created_at TEXT NOT NULL);";
        CHECK(raw.execScript(oldSchema, err));
        std::string legacyHash = ah::sha256Hex("legacykey");
        CHECK(raw.execScript("INSERT INTO agents(name, role, api_key_hash, status, created_at,"
                             " updated_at) VALUES ('legacy','member','" +
                                 legacyHash + "','offline','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');",
                             err));
        raw.close();

        ah::Platform p(tmp.string());
        bool bootOk = p.bootstrap(err);
        if (!bootOk) std::printf("  legacy bootstrap err: %s\n", err.c_str());
        CHECK(bootOk);  // 旧库升级：修复前此处报 no such column: idempotency_key
        CHECK(p.authenticate("legacy", "legacykey"));       // 旧格式（无盐）兼容
        CHECK(!p.authenticate("legacy", "wrong"));
        bool dup = false;
        bool r1 = p.usageReport("legacy", 100, 50, "llm", "", "", "mig-1", dup, err);
        if (!r1) std::printf("  legacy report err: %s\n", err.c_str());
        CHECK(r1);
        CHECK(p.usageReport("legacy", 100, 50, "llm", "", "", "mig-1", dup, err));
        CHECK(dup);
        std::string key;
        std::string mk = readFile((tmp / "config" / "master.key").string());
        bool reg = p.registerAgent(mk, "newagent", "member", key, err);
        if (!reg) std::printf("  legacy register err: %s\n", err.c_str());
        CHECK(reg);
        CHECK(p.authenticate("newagent", key));             // 新 Agent 走加盐格式
        // 密钥轮换：旧密钥立即失效、新密钥生效，且明文缓存文件同步更新
        // （数据库只存加盐哈希，明文不可恢复，轮换是唯一恢复途径）
        std::string rotated;
        std::string rotErr;
        CHECK(p.agentRotateKey("zcode", "newagent", rotated, rotErr));
        CHECK(!rotated.empty());
        CHECK(rotated != key);
        CHECK(!p.authenticate("newagent", key));            // 旧密钥失效
        CHECK(p.authenticate("newagent", rotated));         // 新密钥生效
        std::string keyFile = readFile((tmp / "config" / "agents.json").string());
        CHECK(keyFile.find(rotated) != std::string::npos);
        CHECK(keyFile.find(key) == std::string::npos);      // 旧明文不再残留
        // 轮换权限：非管理者被拒；不存在的 Agent 被拒
        std::string denied;
        CHECK(!p.agentRotateKey("newagent", "newagent", denied, rotErr));
        CHECK(denied.empty());
        CHECK(!p.agentRotateKey("zcode", "ghost", denied, rotErr));
        // 一键接入（幂等）：不存在则注册，已存在则轮换；保留名照常拒绝
        std::string provided;
        CHECK(p.agentProvision("zcode", "codex", provided, rotErr));
        CHECK(p.authenticate("codex", provided));
        std::string provided2;
        CHECK(p.agentProvision("zcode", "codex", provided2, rotErr));
        CHECK(provided2 != provided);                        // 第二次调用=轮换
        CHECK(!p.authenticate("codex", provided));
        CHECK(p.authenticate("codex", provided2));
        CHECK(!p.agentProvision("zcode", "zcode", denied, rotErr));   // 保留名
        CHECK(denied.empty());
        CHECK(!p.agentProvision("codex", "claude", denied, rotErr));  // 非管理者
        // 密钥文件损坏（半截/垃圾）：持久化层必须干净失败（修复前 in >> j 直接抛异常
        // 穿透到 HTTP 层变成 400），且绝不能覆盖重写——那等于清掉全部明文条目。
        // API 层仍返回成功：库是事实源，明文缓存尽力而为（失败记审计）。
        writeFileRaw((tmp / "config" / "agents.json").string(), "{corrupt");
        std::string corruptKey;
        bool corruptOk = p.agentProvision("zcode", "claude", corruptKey, rotErr);
        if (!corruptOk) std::printf("  provision with corrupt keyfile err: %s\n", rotErr.c_str());
        CHECK(corruptOk);
        CHECK(p.authenticate("claude", corruptKey));            // 库侧已生效
        CHECK(readFile((tmp / "config" / "agents.json").string()) == "{corrupt");  // 未被覆盖
        // 修复文件后：明文缓存恢复正常写入
        writeFileRaw((tmp / "config" / "agents.json").string(), "{}");
        std::string repairedKey;
        CHECK(p.agentProvision("zcode", "claude", repairedKey, rotErr));
        CHECK(p.authenticate("claude", repairedKey));
        CHECK(readFile((tmp / "config" / "agents.json").string()).find(repairedKey) !=
              std::string::npos);
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ---------------- P0/P1 验收：配置生成与写入（core，Qt-free） ----------------

// 8 个工具 × 各自格式；含"反证"——旧实现的双引号 TOML 必须不再是合法写法
static void test_integrations_generate() {
    using namespace ah::integrations;
    const std::string exe = "C:/x/miderhive-mcp.exe";
    // JSON 家族：claude-code / droid 无 args，cursor 有 args 数组
    for (const bool cursor : {false, true}) {
        const std::string id = cursor ? "cursor" : "droid";
        auto j = nlohmann::json::parse(generateConfig(id, exe, "a1", "k1"));
        const auto srv = j["mcpServers"]["miderhive"];
        CHECK_EQ(srv["command"].get<std::string>(), exe);
        CHECK_EQ(srv["env"]["MIDERHIVE_AGENT_NAME"].get<std::string>(), "a1");
        CHECK_EQ(srv["env"]["MIDERHIVE_AGENT_KEY"].get<std::string>(), "k1");
        CHECK(cursor ? srv.contains("args") : !srv.contains("args"));
    }
    // TOML：字面量字符串（反斜杠不转义）；双引号基本字符串会让 config.toml 解析失败
    const std::string winExe = "C:\\Qt\\6.8.3\\bin\\miderhive-mcp.exe";
    const std::string toml = generateConfig("codex", winExe, "a1", "k1");
    CHECK(toml.find("[mcp_servers.miderhive]") != std::string::npos);
    CHECK(toml.find("command = '" + winExe + "'") != std::string::npos);
    CHECK(toml.find("command = \"") == std::string::npos);
    // DSH：密钥必须落在 env 块（DSH 会清洗子进程环境中的 *KEY*/*TOKEN*）
    const std::string dsh = generateConfig("dsh", winExe, "d1", "k1");
    CHECK(dsh.find("- insert:") != std::string::npos);
    CHECK(dsh.find("dsh-mcp-client") != std::string::npos);
    CHECK(dsh.find("MIDERHIVE_AGENT_KEY") != std::string::npos);
    // Hermes：mcp_servers 映射
    const std::string hermes = generateConfig("hermes", winExe, "h1", "k1");
    CHECK(hermes.find("mcp_servers:") != std::string::npos);
    CHECK(hermes.find("miderhive:") != std::string::npos);
    // ZCode 环境变量 / Copilot 指令块
    CHECK(generateConfig("zcode", winExe, "z1", "k1").find("MIDERHIVE_AGENT_NAME=z1") !=
          std::string::npos);
    CHECK(generateConfig("copilot", winExe, "p1", "k1").find("X-Agent-Name: p1") !=
          std::string::npos);
    // 注册表完整性：8 个 id、格式与默认名一致；hasWritableConfig 与格式匹配
    CHECK_EQ(toolRegistry().size(), static_cast<size_t>(8));
    for (const auto& t : toolRegistry()) {
        CHECK(toolById(t.id) != nullptr);
        CHECK(hasWritableConfig(t.id) ==
              (t.format != Format::InstructionsMd && t.format != Format::EnvVars));
    }
    CHECK(relativeConfigPath("codex") == ".codex/config.toml");
    CHECK(relativeConfigPath("zcode").empty());
}

// 写入器四条路径：新建 / 合并保留他人条目 / 坏 JSON 拒绝且不改 / 重复写入替换
static void test_integrations_write() {
    using namespace ah::integrations;
    const fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_cfg_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const std::string exe = "C:/x/miderhive-mcp.exe";

    // JSON（cursor → .cursor/mcp.json）：新建
    WriteResult r = writeConfigUnderRoot(tmp.string(), "cursor", exe, "c1", "k1");
    CHECK(r.ok);
    const std::string cursorPath = (tmp / ".cursor/mcp.json").string();
    std::string txt;
    CHECK(readFileUtf8(cursorPath, txt));
    nlohmann::json j = nlohmann::json::parse(txt, nullptr, false);
    CHECK(!j.is_discarded() && j["mcpServers"]["miderhive"]["command"] == exe);

    // 合并：已有其他 server 条目必须保留；覆盖前留 .miderhive.bak
    const std::string withOther =
        nlohmann::json{{"mcpServers",
                        nlohmann::json{{"other", nlohmann::json{{"command", "x"}}}}}}
            .dump();
    writeFileUtf8(cursorPath, withOther);
    r = writeConfigUnderRoot(tmp.string(), "cursor", exe, "c2", "k2");
    CHECK(r.ok);
    CHECK(readFileUtf8(cursorPath, txt));
    j = nlohmann::json::parse(txt, nullptr, false);
    CHECK(j["mcpServers"].contains("other"));
    CHECK_EQ(j["mcpServers"]["miderhive"]["env"]["MIDERHIVE_AGENT_NAME"].get<std::string>(), "c2");
    CHECK(std::filesystem::exists(cursorPath + ".miderhive.bak"));

    // 坏 JSON：拒绝写入，原文件一字未改
    const std::string garbage = "{ this is not json";
    writeFileUtf8(cursorPath, garbage);
    r = writeConfigUnderRoot(tmp.string(), "cursor", exe, "c3", "k3");
    CHECK(!r.ok);
    txt.clear();
    CHECK(readFileUtf8(cursorPath, txt));
    CHECK(txt == garbage);

    // TOML：重复写入替换整张表，不追加第二份
    CHECK(writeConfigUnderRoot(tmp.string(), "codex", "C:\\Qt\\mcp.exe", "x1", "k1").ok);
    r = writeConfigUnderRoot(tmp.string(), "codex", "C:\\Qt\\mcp2.exe", "x2", "k2");
    CHECK(r.ok);
    txt.clear();
    CHECK(readFileUtf8((tmp / ".codex/config.toml").string(), txt));
    {
        const size_t first = txt.find("[mcp_servers.miderhive]");
        CHECK(first != std::string::npos);
        CHECK(txt.find("[mcp_servers.miderhive]", first + 1) == std::string::npos);
        CHECK(txt.find("mcp2.exe") != std::string::npos);
    }

    // DSH 补丁：顶层裸 "[]" 摘掉后追加；重复写入替换整块
    const std::string dshPath = (tmp / ".dsh/profiles/web/cordis.patch.yml").string();
    writeFileUtf8(dshPath, "# patch layer\n[]\n");
    CHECK(writeConfigUnderRoot(tmp.string(), "dsh", exe, "d1", "k1").ok);
    txt.clear();
    CHECK(readFileUtf8(dshPath, txt));
    CHECK(txt.find("mcp-miderhive") != std::string::npos);
    CHECK(txt.find("dsh-mcp-client") != std::string::npos);
    CHECK(txt.find("\n[]") == std::string::npos);
    CHECK(writeConfigUnderRoot(tmp.string(), "dsh", exe, "d2", "k2").ok);
    txt.clear();
    CHECK(readFileUtf8(dshPath, txt));
    {
        const size_t first = txt.find("mcp-miderhive");
        CHECK(first != std::string::npos);
        CHECK(txt.find("mcp-miderhive", first + 1) == std::string::npos);
        CHECK(txt.find("d2") != std::string::npos);
        CHECK(txt.find("d1") == std::string::npos);
    }

    // Hermes：新建；已有 mcp_servers 段时合并且不破坏兄弟条目；重复写入替换
    CHECK(writeConfigUnderRoot(tmp.string(), "hermes", exe, "h1", "k1").ok);
    txt.clear();
    CHECK(readFileUtf8((tmp / "hermes/config.yaml").string(), txt));
    CHECK(txt.find("mcp_servers:") != std::string::npos);
    const std::string hermesWithOther =
        "model:\n  default: test\n\nmcp_servers:\n  github:\n    command: 'gh'\n    args: []\n"
        "\ntts:\n  enabled: true\n";
    writeFileUtf8((tmp / "hermes/config.yaml").string(), hermesWithOther);
    r = writeConfigUnderRoot(tmp.string(), "hermes", exe, "h2", "k2");
    CHECK(r.ok);
    txt.clear();
    CHECK(readFileUtf8((tmp / "hermes/config.yaml").string(), txt));
    // github 条目保留、miderhive 插入到 mcp_servers 段内、tts 顶层节未被动到
    CHECK(txt.find("github:") != std::string::npos);
    CHECK(txt.find("miderhive:") != std::string::npos);
    CHECK(txt.find("h2") != std::string::npos);
    {
        const size_t mcp = txt.find("mcp_servers:");
        const size_t gh = txt.find("github:");
        const size_t mid = txt.find("  miderhive:");
        CHECK(gh != std::string::npos && mid != std::string::npos && mcp < gh && mid > mcp);
        CHECK(txt.find("tts:") != std::string::npos);
        CHECK(txt.find("tts:") > mid);
    }
    CHECK(writeConfigUnderRoot(tmp.string(), "hermes", exe, "h3", "k3").ok);
    txt.clear();
    CHECK(readFileUtf8((tmp / "hermes/config.yaml").string(), txt));
    CHECK(txt.find("h3") != std::string::npos);
    CHECK(txt.find("h2") == std::string::npos);
    CHECK(txt.find("github:") != std::string::npos);

    // Claude Code 项目级写入：<projectRoot>/.mcp.json
    const std::string projDir = (tmp / "proj").string();
    r = writeProjectConfig(projDir, exe, "cc1", "k1");
    CHECK(r.ok);
    txt.clear();
    CHECK(readFileUtf8((fs::path(projDir) / ".mcp.json").string(), txt));
    j = nlohmann::json::parse(txt, nullptr, false);
    CHECK(j["mcpServers"]["miderhive"]["env"]["MIDERHIVE_AGENT_NAME"] == "cc1");

    // 无固定配置文件的工具必须明确失败
    CHECK(!writeConfigUnderRoot(tmp.string(), "zcode", exe, "z", "k").ok);
    CHECK(!writeProjectConfig("", exe, "z", "k").ok);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ---------------- P1 验收：维度分表 / 迁移 / FTS / tag 下推 ----------------

// Agent 自带 512 维向量与内置 384 维共存且互不污染
static void test_embedding_dims() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_dim_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));

        // 维度范围
        CHECK(ah::isValidEmbeddingDim(384));
        CHECK(ah::isValidEmbeddingDim(1536));
        CHECK(!ah::isValidEmbeddingDim(0));
        CHECK(!ah::isValidEmbeddingDim(63));
        CHECK(!ah::isValidEmbeddingDim(4097));

        // 内置 384 条目
        ah::KnowledgeEntry builtin;
        CHECK(p.knowledgeCreate("hermes", "builtin-dim",
                                "builtin vector entry alpha384 marker", {}, "tech", {}, "",
                                builtin, err));
        // Agent 自带 512 维条目
        std::vector<float> v512(512, 0.0f);
        for (size_t i = 0; i < v512.size(); ++i) v512[i] = 0.01f;
        ah::KnowledgeEntry custom;
        CHECK(p.knowledgeCreate("hermes", "custom-dim",
                                "agent supplied 512 dim entry beta512 marker", {"custom"},
                                "tech", v512, "test-model", custom, err));
        CHECK_EQ(custom.embedding_provider, std::string("test-model"));

        // 同维向量检索命中自带条目；异维互不污染
        std::vector<ah::KnowledgeHit> hits;
        CHECK(p.knowledgeSearchSemantic(v512, 10, "", hits, err));
        CHECK(!hits.empty());
        bool sawCustom = false, sawBuiltin = false;
        for (const auto& h : hits) {
            if (h.entry.uuid == custom.uuid) sawCustom = true;
            if (h.entry.uuid == builtin.uuid) sawBuiltin = true;
        }
        CHECK(sawCustom);
        CHECK(!sawBuiltin);  // 512 维查询在 512 分表里，不会命中 384 条目

        CHECK(p.knowledgeSearch("alpha384", ah::SearchMode::Semantic, 10, "", hits, err));
        sawCustom = sawBuiltin = false;
        for (const auto& h : hits) {
            if (h.entry.uuid == custom.uuid) sawCustom = true;
            if (h.entry.uuid == builtin.uuid) sawBuiltin = true;
        }
        CHECK(sawBuiltin);
        CHECK(!sawCustom);

        // A1 维度错配必须明确报错（而不是静默返回空数组）：错误里要点出请求维度
        // 与本库现有维度 + provider 标签，调用方才能自查"用 512 写、用 1536 查"。
        std::vector<float> v1536(1536, 0.02f);
        std::string dimErr;
        hits.clear();
        CHECK(!p.knowledgeSearchSemantic(v1536, 10, "", hits, dimErr));
        CHECK(dimErr.find("1536") != std::string::npos);
        CHECK(dimErr.find("384") != std::string::npos);
        CHECK(dimErr.find("512") != std::string::npos);
        CHECK(dimErr.find("test-model") != std::string::npos);  // 512 维那条的 provider 标签
        // 维度合法但越界（<64 / >4096）同样明确拒绝
        std::string badErr;
        CHECK(!p.knowledgeSearchSemantic(std::vector<float>(32, 0.f), 10, "", hits, badErr));
        CHECK(!p.knowledgeSearchSemantic(std::vector<float>(5000, 0.f), 10, "", hits, badErr));

        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 存量旧库（单张 knowledge_vec）启动时自动迁移到按维度分表，且数据不丢
static void test_legacy_vec_migration() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_vmig_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        ah::KnowledgeEntry migrated;
        CHECK(p.knowledgeCreate("hermes", "迁移样本", "legacy vector migration entry m1", {},
                                "tech", {}, "", migrated, err));
        p.shutdown();
    }
    // 把新表伪装回旧表名（vec0 不支持 RENAME：新建-复制-删除）
    {
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(dbPath.string().c_str(), &db) == SQLITE_OK);
        CHECK(sqlite3_vec_init(db, nullptr, nullptr) == SQLITE_OK);
        char* msg = nullptr;
        CHECK(sqlite3_exec(db,
                           "CREATE VIRTUAL TABLE knowledge_vec USING vec0(entry_id INTEGER "
                           "PRIMARY KEY, embedding float[384]);"
                           "INSERT INTO knowledge_vec SELECT entry_id, embedding FROM "
                           "knowledge_vec_d384;"
                           "DROP TABLE knowledge_vec_d384;",
                           nullptr, nullptr, &msg) == SQLITE_OK);
        sqlite3_free(msg);
        sqlite3_close(db);
    }
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));  // 迁移在这里发生
        std::vector<ah::KnowledgeEntry> out;
        CHECK(p.knowledgeList(10, "", out, err));
        CHECK_EQ(out.size(), static_cast<size_t>(1));
        std::vector<ah::KnowledgeHit> hits;
        CHECK(p.knowledgeSearch("migration", ah::SearchMode::Semantic, 10, "", hits, err));
        CHECK(!hits.empty());
        // 旧表已消失、新表回归；重复 bootstrap 幂等
        p.shutdown();
        CHECK(p.bootstrap(err));
        out.clear();
        CHECK(p.knowledgeList(10, "", out, err));
        CHECK_EQ(out.size(), static_cast<size_t>(1));
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// FTS5 关键词检索：中文子串、大小写不敏感、旧版本不命中、删除即不可检索
static void test_keyword_fts() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_fts_" + ah::randomHex(8));
    fs::create_directories(tmp);
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        ah::KnowledgeEntry a, b;
        CHECK(p.knowledgeCreate("hermes", "SQLite Trigram Guide",
                                "fts probe keyword zzzqqq unique marker", {"fts"}, "tech", {}, "",
                                a, err));
        CHECK(p.knowledgeCreate("hermes", "中文全文检索",
                                "中文内容检索探针条目，关键词是青枫浦不上不胜愁。", {"中文"}, "tech",
                                {}, "", b, err));

        // >=3 码点走 FTS：英文大小写不敏感（与 LIKE 行为一致）
        std::vector<ah::KnowledgeHit> out;
        CHECK(p.knowledgeSearch("zzzqqq", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK_EQ(out.size(), static_cast<size_t>(1));
        CHECK_EQ(out[0].entry.uuid, a.uuid);
        CHECK(p.knowledgeSearch("sqlite trigram", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(!out.empty() && out[0].entry.uuid == a.uuid);
        // 中文子串（>=3 字）
        CHECK(p.knowledgeSearch("青枫浦不上", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(!out.empty() && out[0].entry.uuid == b.uuid);
        // 引号/横杠等 FTS5 语法字符不得引发错误或语义漂移
        CHECK(p.knowledgeSearch("probe \"quoted\" entry-x", ah::SearchMode::Keyword, 10, "", out,
                                err));
        CHECK(out.empty());
        // <3 码点回退 LIKE
        CHECK(p.knowledgeSearch("中", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(!out.empty());

        // 追加新版本后，旧版本的独有内容不再可检索（is_latest 过滤）
        CHECK(p.knowledgeAddVersion("hermes", a.uuid, "", "second version content yyywww", {}, "",
                                    a, err));
        CHECK(p.knowledgeSearch("zzzqqq", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(out.empty());
        CHECK(p.knowledgeSearch("yyywww", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(!out.empty() && out[0].entry.uuid == a.uuid);

        // 删除即不可检索
        CHECK(p.knowledgeRemove("zcode", b.uuid, err));
        CHECK(p.knowledgeSearch("青枫浦不上", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(out.empty());
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// semantic + tag：先多召回再过滤，limit 能给满；不足时如实返回
static void test_semantic_tag_pushdown() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_tag_" + ah::randomHex(8));
    fs::create_directories(tmp);
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        // 50 条带 tag、5 条不带，共享同一召回关键词
        for (int i = 0; i < 50; ++i) {
            ah::KnowledgeEntry e;
            CHECK(p.knowledgeCreate(
                "hermes", "tagged " + std::to_string(i),
                "shared recall token kNNfeed tagged entry number " + std::to_string(i), {"t1"},
                "tech", {}, "", e, err));
        }
        for (int i = 0; i < 5; ++i) {
            ah::KnowledgeEntry e;
            CHECK(p.knowledgeCreate(
                "hermes", "untagged " + std::to_string(i),
                "shared recall token kNNfeed untagged entry number " + std::to_string(i), {},
                "tech", {}, "", e, err));
        }
        std::vector<ah::KnowledgeHit> hits;
        CHECK(p.knowledgeSearch("kNNfeed", ah::SearchMode::Semantic, 10, "t1", hits, err));
        // 旧行为："先取 10 条再过滤"，过滤后常常只剩零星几条；现在必须给满 10 条
        CHECK_EQ(hits.size(), static_cast<size_t>(10));
        for (const auto& h : hits) {
            bool has = false;
            for (const auto& t : h.entry.tags)
                if (t == "t1") has = true;
            CHECK(has);
        }
        // 匹配数不足 limit 时如实返回实际条数（3 条带 t1 的短内容）
        for (int i = 0; i < 3; ++i) {
            ah::KnowledgeEntry e;
            CHECK(p.knowledgeCreate("hermes", "solo " + std::to_string(i),
                                    "another token soloFeed entry " + std::to_string(i), {"t2"},
                                    "tech", {}, "", e, err));
        }
        CHECK(p.knowledgeSearch("soloFeed", ah::SearchMode::Semantic, 10, "t2", hits, err));
        CHECK_EQ(hits.size(), static_cast<size_t>(3));

        // A2 对抗分布：不带标签的条目与查询**更相似**（关键词出现 3 次），带标签的更远
        // （出现 1 次）。单次固定倍数召回的旧实现在这里只会返回零星几条；
        // 梯度扩 k 必须仍然给满 limit。
        for (int i = 0; i < 12; ++i) {
            ah::KnowledgeEntry e;
            CHECK(p.knowledgeCreate("hermes", "far-tagged " + std::to_string(i),
                                    "adversarialFeed once", {"t3"}, "tech", {}, "", e, err));
        }
        for (int i = 0; i < 30; ++i) {
            ah::KnowledgeEntry e;
            CHECK(p.knowledgeCreate("hermes", "near-untagged " + std::to_string(i),
                                    "adversarialFeed adversarialFeed adversarialFeed near " +
                                        std::to_string(i),
                                    {}, "tech", {}, "", e, err));
        }
        CHECK(p.knowledgeSearch("adversarialFeed", ah::SearchMode::Semantic, 10, "t3", hits, err));
        CHECK_EQ(hits.size(), static_cast<size_t>(10));
        for (const auto& h : hits) {
            bool has = false;
            for (const auto& t : h.entry.tags)
                if (t == "t3") has = true;
            CHECK(has);
        }
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 存量库 FTS 重建：正文已有数据、但没有全文索引（老 schema 没有 knowledge_fts 表）→
// 启动时 initSearchIndex 应一次性 rebuild，之后关键词检索可用。这条路径此前完全没覆盖。
static void test_fts_rebuild_legacy() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_frb_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        ah::KnowledgeEntry e;
        CHECK(p.knowledgeCreate("hermes", "老库正文",
                                "legacy content without fts index rebuild666 marker", {}, "tech",
                                {}, "", e, err));
        p.shutdown();
    }
    // 忠实模拟"升级前的旧库"：既没有全文索引表，也没有索引版本标记。
    // （只删表不删标记不叫旧库——那是手工破坏，恢复路径见 initSearchIndex 注释。）
    {
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(dbPath.string().c_str(), &db) == SQLITE_OK);
        char* msg = nullptr;
        CHECK(sqlite3_exec(db,
                           "DROP TABLE IF EXISTS knowledge_fts;"
                           "DELETE FROM settings WHERE key='fts_index_version';",
                           nullptr, nullptr, &msg) == SQLITE_OK);
        sqlite3_free(msg);
        sqlite3_close(db);
    }
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));  // 这里应触发 FTS rebuild 并打上版本标记
        std::vector<ah::KnowledgeHit> hits;
        CHECK(p.knowledgeSearch("rebuild666", ah::SearchMode::Keyword, 10, "", hits, err));
        CHECK(!hits.empty());
        // 重建后再插入的新条目也要能被索引到（触发器已恢复）
        ah::KnowledgeEntry e2;
        CHECK(p.knowledgeCreate("hermes", "新条目", "post rebuild entry fresh999 marker", {},
                                "tech", {}, "", e2, err));
        CHECK(p.knowledgeSearch("fresh999", ah::SearchMode::Keyword, 10, "", hits, err));
        CHECK(!hits.empty() && hits[0].entry.uuid == e2.uuid);
        // 重复 bootstrap 幂等（索引非空，不再 rebuild）
        p.shutdown();
        CHECK(p.bootstrap(err));
        CHECK(p.knowledgeSearch("rebuild666", ah::SearchMode::Keyword, 10, "", hits, err));
        CHECK(!hits.empty());
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

int main() {
    auto run = [](const char* name, void (*fn)()) {
        std::printf("== %s\n", name);
        std::fflush(stdout);
        fn();
    };
    run("sha256", test_sha256);
    run("week_start", test_week_start);
    run("embedder", test_embedder);
    run("url_guard", test_url_guard);
    run("constant_time", test_constant_time_equals);
    run("version_compare", test_version_compare);
    run("platform_e2e", test_platform_end_to_end);
    run("knowledge_vector_atomicity", test_knowledge_vector_atomicity);
    run("legacy_migration", test_legacy_migration);
    run("integrations_generate", test_integrations_generate);
    run("integrations_write", test_integrations_write);
    run("embedding_dims", test_embedding_dims);
    run("legacy_vec_migration", test_legacy_vec_migration);
    run("keyword_fts", test_keyword_fts);
    run("fts_rebuild_legacy", test_fts_rebuild_legacy);
    run("semantic_tag_pushdown", test_semantic_tag_pushdown);

    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
