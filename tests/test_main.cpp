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
#include "core/update_sign.h"
#include "core/util.h"
#include "core/version_util.h"
#include "sqlite-vec.h"  // 生成头：第二个连接查 knowledge_vec 前需注册 vec0

#ifdef _WIN32
#include <bcrypt.h>  // test_rsa_verify 动态段：CNG 生成密钥对并签名（windows.h 经 core/util.h 已就位）
#endif

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
    // 百分号编码的 host：解码后就是环回地址，不能让编码形态骗过字面量检查
    CHECK(!isSafeOutboundUrl("http://%31%32%37.0.0.1/"));
    CHECK(!isSafeOutboundUrl("http://local%68ost/"));
    // 尾点 FQDN 写法："localhost." 与 "localhost" 解析到同一处（Windows 实测环回）
    CHECK(!isSafeOutboundUrl("http://localhost./"));
    CHECK(!isSafeOutboundUrl("http://127.0.0.1./"));
    // 非法 % 序列：解码失败一律拒绝
    CHECK(!isSafeOutboundUrl("http://example.com%ZZ/"));
    CHECK(!isSafeOutboundUrl("http://ex%2mple.com/"));
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
        // 单次上报上限：防手滑/畸形客户端把周用量一次性打爆（上限=周预算默认值）
        CHECK(!p.usageReport("hermes", 10'000'001, 0, "llm", "", "", "", dup, err));
        CHECK(err.find("too large") != std::string::npos);
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
        // 回复权限：无关 Agent 不能回复别人的点对点消息（此前整条链路零校验，
        // 副作用还会把私密消息翻成已读）；收件人与发件人正常回复，广播人人可回
        ah::Message rp;
        std::string replyErr;
        CHECK(!p.messageReply("roleprobe", pm.uuid, "无关者想插话", rp, replyErr));
        CHECK(replyErr.find("not allowed") != std::string::npos);
        CHECK(p.messageReply("claude", pm.uuid, "收件人回复", rp, err));
        CHECK_EQ(rp.parent_uuid, pm.uuid);
        CHECK(p.messageReply("hermes", pm.uuid, "发件人补充", rp, err));
        CHECK(p.messageReply("roleprobe", bm.uuid, "广播下的回复", rp, err));
        // 空正文拒绝（与 messageSend 同一契约；此前会落库一条空回复）
        CHECK(!p.messageReply("claude", pm.uuid, "", rp, replyErr));
        CHECK(replyErr.find("body is required") != std::string::npos);

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

        // ---- 加固项：审计可见性收敛——普通 Agent 只能看自己作为主体的行 ----
        // （审计 detail 携带点对点消息的收件人/主题，全量开放会绕过消息可见性）
        p.heartbeat("roleprobe", "audit visibility probe");
        auto auditSelf = cli.Get("/api/audit?limit=500",
                                 {{"X-Agent-Name", "roleprobe"}, {"X-Api-Key", hKey}});
        CHECK(auditSelf && auditSelf->status == 200);
        if (auditSelf) {
            auto body = json::parse(auditSelf->body);
            CHECK_EQ(body["code"], 0);
            CHECK(!body["data"].empty());  // 至少能看到自己那条心跳
            bool onlySelf = true;
            for (const auto& r : body["data"])
                if (r["actor"] != "roleprobe") onlySelf = false;
            CHECK(onlySelf);
        }
        // 管理者（zcode）看全量：能看到别人的行
        std::string zcodeKey;
        {
            std::ifstream aj(tmp / "config" / "agents.json");
            json j = json::parse(aj);
            zcodeKey = j.at("zcode").get<std::string>();
        }
        auto auditMgr = cli.Get("/api/audit?limit=500",
                                {{"X-Agent-Name", "zcode"}, {"X-Api-Key", zcodeKey}});
        CHECK(auditMgr && auditMgr->status == 200);
        if (auditMgr) {
            auto body = json::parse(auditMgr->body);
            CHECK_EQ(body["code"], 0);
            bool sawHermes = false;
            for (const auto& r : body["data"])
                if (r["actor"] == "hermes") sawHermes = true;
            CHECK(sawHermes);
        }

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
        // ---- 加固项：损坏/非对象 JSON 统一 400（此前 reply/resolve 静默当空数据
        // 处理并真的落库：一条空正文回复、一次无说明的"已解决"）----
        {
            nlohmann::json replyOk = {{"body", "正常回复"}};
            auto rOk = cli.Post("/api/messages/" + pm.uuid + "/reply",
                                {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                replyOk.dump(), "application/json");
            CHECK(rOk && rOk->status == 200);
            auto rBad = cli.Post("/api/messages/" + pm.uuid + "/reply",
                                 {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                 "{ this is not json", "application/json");
            CHECK(rBad && rBad->status == 400);
            ah::ErrorReport rep;
            CHECK(p.errorReport("hermes", "info", "probe", "resolve probe", "detail", "", rep, err));
            auto eBad = cli.Post("/api/errors/" + rep.uuid + "/resolve",
                                 {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                 "not json at all", "application/json");
            CHECK(eBad && eBad->status == 400);
            // 损坏请求不得产生副作用：该错误仍是未解决状态
            std::vector<ah::ErrorReport> errs;
            CHECK(p.errorList("", "", 50, errs, err));
            bool stillOpen = false;
            for (const auto& e : errs)
                if (e.uuid == rep.uuid && e.status == "open") stillOpen = true;
            CHECK(stillOpen);
        }
        // ---- 加固项：预算**读**失败必须如实 500（曾经把 -1 当预算发出去）----
        // 注入方式：从第二个连接把 settings 表改名，制造真实的读失败
        //（与表被锁/损坏同构；平台空闲时无写锁，DDL 可拿到）。
        {
            const fs::path liveDb = tmp / "platform.db";
            sqlite3* raw = nullptr;
            CHECK(sqlite3_open(liveDb.string().c_str(), &raw) == SQLITE_OK);
            char* msg = nullptr;
            const bool renamed =
                sqlite3_exec(raw, "ALTER TABLE settings RENAME TO settings_hidden", nullptr,
                             nullptr, &msg) == SQLITE_OK;
            sqlite3_free(msg);
            sqlite3_close(raw);
            CHECK(renamed);
            auto failResp = cli.Get("/api/usage/budget",
                                    {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
            CHECK(failResp && failResp->status == 500);
            if (failResp) {
                auto body = json::parse(failResp->body);
                CHECK_EQ(body["code"], 500);  // 信封同步携带错误码，不是裸 500
            }
            raw = nullptr;
            CHECK(sqlite3_open(liveDb.string().c_str(), &raw) == SQLITE_OK);
            msg = nullptr;
            sqlite3_exec(raw, "ALTER TABLE settings_hidden RENAME TO settings", nullptr, nullptr,
                         &msg);
            sqlite3_free(msg);
            sqlite3_close(raw);
            auto okResp = cli.Get("/api/usage/budget",
                                  {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
            CHECK(okResp && okResp->status == 200);
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
        // token 数值必须如实校验：此前按 int 推导——5e9 静默回绕、浮点静默截断小数
        auto badTokensHuge = cli.Post("/api/usage/report",
                                      {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                      nlohmann::json{{"tokens_in", 5000000000}}.dump(),
                                      "application/json");
        CHECK(badTokensHuge && badTokensHuge->status == 400);  // 超单次上限
        auto badTokensType = cli.Post("/api/usage/report",
                                      {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                      nlohmann::json{{"tokens_in", 1}, {"tokens_out", "x"}}.dump(),
                                      "application/json");
        CHECK(badTokensType && badTokensType->status == 400);  // 字符串 → 400 而非全局兜底
        auto badTokensFloat = cli.Post("/api/usage/report",
                                       {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}},
                                       nlohmann::json{{"tokens_in", 1.5}}.dump(),
                                       "application/json");
        CHECK(badTokensFloat && badTokensFloat->status == 400);  // 浮点 → 400 而非静默截断
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

        // ---- 加固项：分页 limit 钳制（防 limit=1e9 把整库塞进一个响应）----
        // 播种 1001 条广播消息，超大 limit 的响应必须被钳到 ≤1000
        for (int i = 0; i < 1001; ++i) {
            ah::Message m;
            CHECK(p.messageSend("note", "zcode", "", "bulk " + std::to_string(i), "b",
                                m, err));
        }
        auto httpBulk = cli.Get("/api/messages?limit=99999999",
                                {{"X-Agent-Name", "hermes"}, {"X-Api-Key", key}});
        CHECK(httpBulk && httpBulk->status == 200);
        if (httpBulk) {
            auto body = json::parse(httpBulk->body);
            CHECK(body["data"].is_array());
            if (body["data"].is_array())
                CHECK(body["data"].size() <= 1000);
        }

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
    // TOML：基本字符串（双引号）。反斜杠按 TOML 语义双写为字面反斜杠；此前用
    // 单引号字面量字符串——它不支持 '' 转义，值含单引号时整份 config.toml 解析失败
    const std::string winExe = "C:\\Qt\\6.8.3\\bin\\miderhive-mcp.exe";
    const std::string toml = generateConfig("codex", winExe, "a1", "k1");
    CHECK(toml.find("[mcp_servers.miderhive]") != std::string::npos);
    CHECK(toml.find("command = \"C:\\\\Qt\\\\6.8.3\\\\bin\\\\miderhive-mcp.exe\"") !=
          std::string::npos);
    // 含单引号的名字也能安全表达（此前会生成非法 TOML）
    const std::string tomlQuote = generateConfig("codex", winExe, "O'Brien", "k'1");
    CHECK(tomlQuote.find("MIDERHIVE_AGENT_NAME = \"O'Brien\"") != std::string::npos);
    CHECK(tomlQuote.find("MIDERHIVE_AGENT_KEY = \"k'1\"") != std::string::npos);
    // cliCommand 的名字/密钥加引号：& 等 cmd.exe 元字符不能变成命令分隔符
    const std::string cc =
        cliCommand("claude-code", winExe, "R&D", "k1");
    CHECK(cc.find("-e MIDERHIVE_AGENT_NAME=\"R&D\"") != std::string::npos);
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
    CHECK_EQ(toolRegistry().size(), static_cast<size_t>(13));
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

    // TOML 非规范形状：miderhive 已被 [mcp_servers] 内联表占用时拒绝追加——
    // 硬追加会构成 TOML 重定义冲突，整份 config.toml 拒载。原文件一字未改。
    {
        const std::string tomlPath = (tmp / ".codex/config.toml").string();
        const std::string nonCanonical =
            "[mcp_servers]\nmiderhive = { command = \"x\" }\n\n[other]\nkey = 1\n";
        writeFileUtf8(tomlPath, nonCanonical);
        r = writeConfigUnderRoot(tmp.string(), "codex", exe, "x3", "k3");
        CHECK(!r.ok);
        txt.clear();
        CHECK(readFileUtf8(tomlPath, txt));
        CHECK(txt == nonCanonical);
    }

    // Gemini（settings.json）：mcpServers 合并必须保留用户的非 MCP 设置
    // （theme/model 等顶层键）——settings.json 是通用设置文件而不是专用 MCP 配置
    {
        const std::string geminiPath = (tmp / ".gemini/settings.json").string();
        writeFileUtf8(geminiPath,
                      "{\"theme\":\"dark\",\"model\":\"gemini-2.5-pro\",\"mcpServers\":{}}");
        r = writeConfigUnderRoot(tmp.string(), "gemini", exe, "g1", "k1");
        CHECK(r.ok);
        txt.clear();
        CHECK(readFileUtf8(geminiPath, txt));
        j = nlohmann::json::parse(txt, nullptr, false);
        CHECK(!j.is_discarded());
        CHECK_EQ(j["theme"], "dark");
        CHECK_EQ(j["model"], "gemini-2.5-pro");
        CHECK(j["mcpServers"]["miderhive"]["env"]["MIDERHIVE_AGENT_NAME"] == "g1");
        // 重复写入是替换而不是追加
        CHECK(writeConfigUnderRoot(tmp.string(), "gemini", exe, "g2", "k2").ok);
        txt.clear();
        CHECK(readFileUtf8(geminiPath, txt));
        j = nlohmann::json::parse(txt, nullptr, false);
        CHECK_EQ(j["mcpServers"]["miderhive"]["env"]["MIDERHIVE_AGENT_NAME"].get<std::string>(), "g2");
        CHECK_EQ(j["theme"], "dark");
    }

    // Hermes 父键带 flow 值（mcp_servers: {}）：缩进块插在 flow 值后面是结构性
    // 非法 YAML（宽容与严格解析器都拒载）。形状没把握就拒绝，原文件不动。
    {
        const std::string hermesPath = (tmp / "hermes/config.yaml").string();
        const std::string flowShape = "model:\n  default: t\n\nmcp_servers: {}\n\ntts:\n  enabled: true\n";
        writeFileUtf8(hermesPath, flowShape);
        r = writeConfigUnderRoot(tmp.string(), "hermes", exe, "h-flow", "k1");
        CHECK(!r.ok);
        txt.clear();
        CHECK(readFileUtf8(hermesPath, txt));
        CHECK(txt == flowShape);
        // 清掉污染现场，让后面"Hermes 新建"的用例从干净状态开始
        fs::remove(hermesPath);
    }

    // 备份失败必须中止写入：.miderhive.bak 位置放一个**非空目录**——remove 删不掉
    // （目录非空）、copy_file 也进不去，备份必败。此前错误码被吞、界面谎称"已备份"，
    // 用户被无备份覆盖还以为有安全网。
    {
        const std::string cursorPath = (tmp / ".cursor/mcp.json").string();
        const std::string before = "{\"mcpServers\":{}}";
        writeFileUtf8(cursorPath, before);
        fs::remove(cursorPath + ".miderhive.bak");  // 前面用例留下的 .bak 是普通文件，先清掉
        fs::create_directories(cursorPath + ".miderhive.bak");
        writeFileUtf8(cursorPath + ".miderhive.bak/keep.txt", "x");  // 非空：remove 必败
        r = writeConfigUnderRoot(tmp.string(), "cursor", exe, "cbk", "k1");
        CHECK(!r.ok);
        txt.clear();
        CHECK(readFileUtf8(cursorPath, txt));
        CHECK(txt == before);  // 原文件一字未改
        fs::remove_all(cursorPath + ".miderhive.bak");
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

    // B2 换行风格：原文件是 CRLF 就写回 CRLF（整份改成 LF 会产生满屏 diff）
    {
        const fs::path hermesPath = tmp / "hermes/config.yaml";
        std::string crlf = "model:\n  default: t\n\nmcp_servers:\n  github:\n    command: 'gh'\n";
        std::string crlfText;
        for (char c : crlf) {
            if (c == '\n') crlfText += '\r';
            crlfText += c;
        }
        writeFileUtf8(hermesPath.string(), crlfText);
        CHECK(writeConfigUnderRoot(tmp.string(), "hermes", exe, "crlf1", "k1").ok);
        std::string outText;
        CHECK(readFileUtf8(hermesPath.string(), outText));
        CHECK(outText.find("\r\n") != std::string::npos);
        // 不允许出现"裸 LF"（即每个 \n 前面都必须是 \r）
        bool bareLf = false;
        for (size_t i = 0; i < outText.size(); ++i)
            if (outText[i] == '\n' && (i == 0 || outText[i - 1] != '\r')) bareLf = true;
        CHECK(!bareLf);
        CHECK(outText.find("github:") != std::string::npos);
        CHECK(outText.find("crlf1") != std::string::npos);

        // LF 文件不得被反向污染成 CRLF
        writeFileUtf8(hermesPath.string(), "mcp_servers:\n  github:\n    command: 'gh'\n");
        CHECK(writeConfigUnderRoot(tmp.string(), "hermes", exe, "lf1", "k1").ok);
        CHECK(readFileUtf8(hermesPath.string(), outText));
        CHECK(outText.find("\r\n") == std::string::npos);
        CHECK(outText.find("lf1") != std::string::npos);
    }

    // B3 节尾顶格注释：属于本节、不当作边界；插入应在注释之前，注释视觉归属不变
    {
        const fs::path hermesPath = tmp / "hermes/config.yaml";
        writeFileUtf8(hermesPath.string(),
                      "mcp_servers:\n  github:\n    command: 'gh'\n# 节尾注释（归属 tts）\n"
                      "tts:\n  enabled: true\n");
        CHECK(writeConfigUnderRoot(tmp.string(), "hermes", exe, "cmt1", "k1").ok);
        std::string outText;
        CHECK(readFileUtf8(hermesPath.string(), outText));
        const size_t mcp = outText.find("mcp_servers:");
        const size_t mid = outText.find("  miderhive:");
        const size_t cmt = outText.find("# 节尾注释");
        const size_t tts = outText.find("tts:");
        CHECK(mcp != std::string::npos && mid != std::string::npos && cmt != std::string::npos &&
              tts != std::string::npos);
        CHECK(mcp < mid);       // 插在父节内
        CHECK(mid < cmt);       // 且排在注释之前（注释仍紧邻 tts）
        CHECK(cmt < tts);
        CHECK(outText.find("github:") != std::string::npos);
    }

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
    const fs::path dbPath = tmp / "platform.db";
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

        // 批次 3：证明是**索引层**就移除了历史版本（不是靠查询时的 is_latest 过滤）——
        // 直接对 knowledge_fts 做 MATCH，旧版本 token 必须一条都查不到。
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_fts "
                                   "WHERE knowledge_fts MATCH 'zzzqqq'"),
                 0);
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_fts "
                                   "WHERE knowledge_fts MATCH 'yyywww'"),
                 1);

        // 删除即不可检索
        CHECK(p.knowledgeRemove("zcode", b.uuid, err));
        CHECK(p.knowledgeSearch("青枫浦不上", ah::SearchMode::Keyword, 10, "", out, err));
        CHECK(out.empty());
        // 批次 3：删除后索引里也不留死条目
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_fts "
                                   "WHERE knowledge_fts MATCH '青枫浦不上'"),
                 0);
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

// B1 守卫：写入必须是"替换"语义——第二次写入内容为新值、不留 .miderhive.tmp、
// 目录不存在时能建出来。这也是 writeFileUtf8 去掉"先删目标"那次 remove 的依据。
static void test_write_replaces_atomically() {
    const fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_wr_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const std::string p = (tmp / "cfg.json").string();
    CHECK(ah::integrations::writeFileUtf8(p, "v1"));
    CHECK(ah::integrations::writeFileUtf8(p, "v2"));
    std::string t;
    CHECK(ah::integrations::readFileUtf8(p, t));
    CHECK_EQ(t, std::string("v2"));
    CHECK(!std::filesystem::exists(p + ".miderhive.tmp"));
    const std::string deep = (tmp / "a" / "b" / "c.json").string();
    CHECK(ah::integrations::writeFileUtf8(deep, "x"));
    CHECK(std::filesystem::exists(deep));
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 心跳审计降噪的回归锁定：同任务重复心跳**不**留痕，仅"首次/离线回归/任务变化"留痕。
// 该行为自 v0.1.0 起即存在（wasOffline || prevTask != currentTask 才写审计）；
// 此测试防止将来被"简化"成全量审计——30s 一次 × N 个 Agent 会把审计轮转的
// 10 万条上限在一周内耗尽，把文档承诺的 30 天窗口冲垮。
static void test_heartbeat_audit() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_hb_" + ah::randomHex(8));
    fs::create_directories(tmp);
    ah::Platform p(tmp.string());
    std::string err;
    CHECK(p.bootstrap(err));

    auto countHb = [&p](int64_t& n) {
        std::vector<ah::AuditRecord> recs;
        std::string e;
        if (!p.auditList("", "agent.heartbeat", "", 1000, recs, e)) return false;
        n = static_cast<int64_t>(recs.size());
        return true;
    };

    // zcode 由 bootstrap 预置，无需注册
    int64_t n0 = -1;
    CHECK(countHb(n0));
    p.heartbeat("zcode", "task one");  // 首次心跳：prevSeen 为空 → wasOffline → +1
    int64_t n1 = -1;
    CHECK(countHb(n1));
    CHECK_EQ(n1, n0 + 1);

    p.heartbeat("zcode", "task one");  // 同任务重复：不增
    p.heartbeat("zcode", "task one");
    int64_t n2 = -1;
    CHECK(countHb(n2));
    CHECK_EQ(n2, n1);

    p.heartbeat("zcode", "task two");  // 换任务：+1
    int64_t n3 = -1;
    CHECK(countHb(n3));
    CHECK_EQ(n3, n2 + 1);

    // 在线机制不受审计降噪影响：last_seen_at 已写、状态为 online
    std::vector<ah::AgentInfo> agents;
    CHECK(p.listAgents(agents, err));
    bool found = false;
    std::string seen, status;
    for (const auto& a : agents)
        if (a.name == "zcode") { found = true; seen = a.last_seen_at; status = a.status; }
    CHECK(found);
    CHECK(!seen.empty());
    CHECK_EQ(status, std::string("online"));
    p.shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 库结构版本门：全新库/旧库照常迁移并打上当前版本；来自更新版本的库（或备份）
// 必须明确拒绝，而不是带着错位的 schema 继续跑。
static void test_schema_version_gate() {
    // 直接对库文件读写 PRAGMA（第二个连接）
    auto readVersion = [](const fs::path& dbPath) -> int64_t {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
            SQLITE_OK) {
            if (db) sqlite3_close(db);
            return -1;
        }
        int64_t v = -1;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK &&
            st && sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        sqlite3_close(db);
        return v;
    };
    auto writeVersion = [](const fs::path& dbPath, int64_t v) -> bool {
        sqlite3* db = nullptr;
        if (sqlite3_open(dbPath.string().c_str(), &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return false;
        }
        char* msg = nullptr;
        const std::string sql = "PRAGMA user_version = " + std::to_string(v) + ";";
        const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) == SQLITE_OK;
        sqlite3_free(msg);
        sqlite3_close(db);
        return ok;
    };

    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_ver_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    {
        // 全新库：bootstrap 后版本被写上
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        p.shutdown();
        CHECK_EQ(readVersion(dbPath), static_cast<int64_t>(2));
    }
    {
        // 旧库（version=0）：重新 bootstrap 应照常迁移并置回当前版本，重复调用幂等
        CHECK(writeVersion(dbPath, 0));
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        p.shutdown();
        CHECK_EQ(readVersion(dbPath), static_cast<int64_t>(2));
        ah::Platform p2(tmp.string());
        CHECK(p2.bootstrap(err));
        p2.shutdown();
        CHECK_EQ(readVersion(dbPath), static_cast<int64_t>(2));
    }
    {
        // 来自更新版本的库：必须拒绝启动，且错误说明「更新版本」
        CHECK(writeVersion(dbPath, 99));
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(!p.bootstrap(err));
        CHECK(err.find("newer") != std::string::npos);
        p.shutdown();
    }
    {
        // 恢复路径：先做一份正常备份，再把备份文件伪造成更高版本 → 恢复必须被拒
        CHECK(writeVersion(dbPath, 2));
        std::string backupPath;
        {
            ah::Platform p(tmp.string());
            std::string err;
            CHECK(p.bootstrap(err));
            CHECK(p.backupCreate(backupPath, err));
            p.shutdown();
        }
        CHECK(writeVersion(fs::path(backupPath), 99));
        {
            ah::Platform p(tmp.string());
            std::string err;
            CHECK(p.bootstrap(err));
            const std::string backupName = fs::path(backupPath).filename().string();
            CHECK(!p.backupRestore(backupName, err));
            CHECK(err.find("newer") != std::string::npos);
            // 原库未被覆盖：版本仍是当前值（2）
            p.shutdown();
            CHECK_EQ(readVersion(dbPath), static_cast<int64_t>(2));
        }
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 批次 6：embedding_dim 记录 + 精确删除 + 旧行回填。
// 关键陷阱：同一 uuid 的不同版本可能落在**不同**维度表（v1 内置 384、v2 自带 1024），
// 删除必须覆盖全部，否则留下"有向量无正文"的孤儿。
static void test_embedding_dim_tracking() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_dim_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    std::string crossUuid;
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        // 单维度版本链：v1/v2 都用内置 384 → 走精确删除路径
        ah::KnowledgeEntry a;
        CHECK(p.knowledgeCreate("hermes", "same dim", "same dim content alpha", {}, "", {}, "",
                                a, err));
        ah::KnowledgeEntry a2;
        CHECK(p.knowledgeAddVersion("hermes", a.uuid, "", "same dim content beta", {}, "", a2,
                                    err));
        // 跨维度版本链：v1 内置 384 → v2 自带 1024（必须走安全回退，两张表都要清）
        ah::KnowledgeEntry b;
        std::vector<float> v1024(1024, 0.5f);
        CHECK(p.knowledgeCreate("hermes", "cross dim", "cross dim version one", {}, "", {}, "",
                                b, err));
        ah::KnowledgeEntry b2;
        CHECK(p.knowledgeAddVersion("hermes", b.uuid, "", "cross dim version two", v1024,
                                    "model-1024", b2, err));
        crossUuid = b.uuid;
        p.shutdown();
    }
    // 维度已落库：内置 384 共 3 行（同维度链的 v1/v2 + 跨维度链的 v1），跨维度那条 v2 记 1024
    CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries "
                               "WHERE embedding_dim = 384"),
             3);
    CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries "
                               "WHERE embedding_dim = 1024"),
             1);
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));
        CHECK(p.knowledgeRemove("zcode", crossUuid, err));  // 跨维度：两张表都要清
        const std::string inUuid =
            "(SELECT id FROM knowledge_entries WHERE uuid='" + crossUuid + "')";
        CHECK_EQ(countRows(dbPath, ("SELECT COUNT(*) FROM knowledge_vec_d384 WHERE entry_id IN " +
                                    inUuid)
                                       .c_str()),
                 0);
        CHECK_EQ(countRows(dbPath, ("SELECT COUNT(*) FROM knowledge_vec_d1024 WHERE entry_id IN " +
                                    inUuid)
                                       .c_str()),
                 0);
        // 不变式：不存在"有向量无正文"的孤儿（含跨维度场景）
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_vec_d384 WHERE entry_id NOT IN "
                                   "(SELECT id FROM knowledge_entries)"),
                 0);
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_vec_d1024 WHERE entry_id NOT IN "
                                   "(SELECT id FROM knowledge_entries)"),
                 0);
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 批次 5：维度↔provider 绑定。同一维度的向量共用一张 vec 表，不同模型的向量会互相
// 污染 kNN 结果（查询时无法按 provider 过滤）；因此首次写入登记 provider，之后同维度
// 换模型必须被明确拒绝，而不是产出"检索结果莫名其妙"。
static void test_embed_provider_binding() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_bind_" + ah::randomHex(8));
    fs::create_directories(tmp);
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));

        // 内置写入：384 维已由 bootstrap 登记为内置 provider
        ah::KnowledgeEntry a;
        CHECK(p.knowledgeCreate("hermes", "builtin", "builtin content one", {}, "", {}, "", a,
                                err));

        // 128 维 + model-A：首次写入登记成功
        std::vector<float> v128a(128, 0.1f), v128b(128, 0.2f);
        ah::KnowledgeEntry b;
        CHECK(p.knowledgeCreate("hermes", "customA", "custom content two", {}, "", v128a,
                                "model-A", b, err));

        // 同维度换模型：拒绝，且错误点明维度与两个 provider
        ah::KnowledgeEntry c;
        std::string bindErr;
        CHECK(!p.knowledgeCreate("hermes", "customB", "custom content three", {}, "", v128b,
                                 "model-B", c, bindErr));
        CHECK(bindErr.find("128") != std::string::npos);
        CHECK(bindErr.find("model-A") != std::string::npos);
        CHECK(bindErr.find("model-B") != std::string::npos);

        // 同维度同模型：放行
        CHECK(p.knowledgeCreate("hermes", "customA2", "custom content four", {}, "", v128a,
                                "model-A", c, err));

        // 另一维度另一模型：放行（维度之间互不干扰）
        std::vector<float> v256(256, 0.3f);
        ah::KnowledgeEntry d;
        CHECK(p.knowledgeCreate("hermes", "other256", "custom content five", {}, "", v256,
                                "model-B", d, err));

        // agent 用 384 维自有模型 → 与内置的 384 表冲突，拒绝
        std::vector<float> v384(384, 0.4f);
        ah::KnowledgeEntry e;
        CHECK(!p.knowledgeCreate("hermes", "conflict384", "custom content six", {}, "", v384,
                                 "my-model", e, bindErr));

        // 追加版本路径同样受校验（防止绕过 create 混入异模型向量）
        CHECK(!p.knowledgeAddVersion("hermes", b.uuid, "", "v2 content", v128b, "model-B", e,
                                     bindErr));
        CHECK(bindErr.find("model-A") != std::string::npos);

        // 自带向量但漏报来源：拒绝（此前会冒用内置 ngram-hash-v2 标签登记维度，
        // 架空绑定防线并误导后续如实声明的写入）
        std::vector<float> v512(512, 0.5f);
        ah::KnowledgeEntry f;
        CHECK(!p.knowledgeCreate("hermes", "anon-vec", "anonymous content", {}, "", v512, "", f,
                                 bindErr));
        CHECK(bindErr.find("embedder is required") != std::string::npos);
        // 被拒之后，同一维度如实声明的写入仍然可用（假登记没有留下任何痕迹）
        CHECK(p.knowledgeCreate("hermes", "declared", "declared content", {}, "", v512, "model-C",
                                f, err));
        // 追加版本路径同样拒绝漏报；如实声明则放行
        CHECK(!p.knowledgeAddVersion("hermes", f.uuid, "", "v2 content", v512, "", e, bindErr));
        CHECK(bindErr.find("embedder is required") != std::string::npos);
        CHECK(p.knowledgeAddVersion("hermes", f.uuid, "", "v2 content", v512, "model-C", e, err));
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ---- 失败面测试：注入失败后必须"如实报错 + 不留半截状态" ----
// 注入手段是**真实的数据库触发器**（RAISE(ABORT)），不是测试替身：
//   * 只在注入的那一条语句上触发，其他路径完全不受影响；
//   * 错误经由 sqlite3 真实回传，走的是产品代码原样的错误分支；
//   * 不需要为测试改动任何生产 API。
// 这补的是此前 21 个测试函数共同的盲区：它们全都只断言正常路径，
// 于是"辅助写入失败被静默吞掉"这类缺陷可以永久绿灯（例如维护统计只断言字段名存在）。
static bool installAbortTrigger(const fs::path& dbPath, const std::string& name,
                                const std::string& ddl) {
    ah::Database raw;
    std::string err;
    if (!raw.open(dbPath.string(), err)) return false;
    bool ok = raw.execScript("CREATE TRIGGER IF NOT EXISTS " + name + " " + ddl, err) &&
              raw.execScript("DROP TRIGGER IF EXISTS " + name, err) &&
              raw.execScript("CREATE TRIGGER " + name + " " + ddl, err);
    raw.close();
    return ok;
}

// 拆掉注入，让后续步骤回到正常路径（否则审计失败会掩盖真正要测的那条失败）
static bool dropTrigger(const fs::path& dbPath, const std::string& name) {
    ah::Database raw;
    std::string err;
    if (!raw.open(dbPath.string(), err)) return false;
    const bool ok = raw.execScript("DROP TRIGGER IF EXISTS " + name, err);
    raw.close();
    return ok;
}

static bool execOnDb(const fs::path& dbPath, const std::string& sql) {
    ah::Database raw;
    std::string err;
    if (!raw.open(dbPath.string(), err)) return false;
    const bool ok = raw.execScript(sql, err);
    raw.close();
    return ok;
}

static void test_failure_paths_report_errors() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_fail_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));

        // ---- 1. 审计写不进去 → 写操作必须失败，且业务数据不得落库 ----
        // 先造一条待解决的错误（errorReport 也走审计，必须赶在装触发器之前）
        ah::ErrorReport seed;
        CHECK(p.errorReport("claude", "error", "seed", "待解决的错误", "正文", "", seed, err));
        CHECK(!seed.uuid.empty());
        CHECK(installAbortTrigger(dbPath, "trig_audit_insert",
                                  "BEFORE INSERT ON audit_log BEGIN "
                                  "SELECT RAISE(ABORT,'injected: audit unavailable'); END"));
        {
            ah::KnowledgeEntry e;
            std::string e1;
            CHECK(!p.knowledgeCreate("hermes", "审计失败不得入库", "正文", {}, "", {}, "", e, e1));
            CHECK(!e1.empty());
            // 副作用不存在：不是"返回了 false 但数据还在"
            CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries"), 0);
        }
        {
            ah::MemoryEntry m;
            std::string e2;
            CHECK(!p.memorySet("hermes", "project", "status", "x", 0, m, e2));
            CHECK(!e2.empty());
            CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM memory_entries"), 0);
        }
        {
            ah::Message out;
            std::string e3;
            CHECK(!p.messageSend("note", "hermes", "claude", "s", "body", out, e3));
            CHECK(!e3.empty());
            CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM messages"), 0);
        }
        {
            // 解决错误同样受审计硬契约约束：失败时不得留下"已解决但无留痕"的半截状态
            ah::ErrorReport resolved;
            std::string e4;
            CHECK(!p.errorResolve("zcode", seed.uuid, "第一次解决说明", resolved, e4));
            CHECK(!e4.empty());
            const std::string stillOpen = "SELECT COUNT(*) FROM errors WHERE uuid='" + seed.uuid +
                                          "' AND status='open'";
            CHECK_EQ(countRows(dbPath, stillOpen.c_str()),
                     1);  // 回滚生效：仍是 open，说明也没写进去
        }

        // ---- 2. 消息回复：写回复与标已读必须同事务 ----
        // 先拆掉审计注入：下面要测的是"正文写入失败导致整体回滚"，不该被审计失败掩盖
        CHECK(dropTrigger(dbPath, "trig_audit_insert"));
        ah::Message parent;
        CHECK(p.messageSend("note", "claude", "hermes", "问题", "父消息正文", parent, err));
        CHECK(installAbortTrigger(dbPath, "trig_msg_update",
                                  "BEFORE UPDATE ON messages BEGIN "
                                  "SELECT RAISE(ABORT,'injected: mark-read failed'); END"));
        {
            ah::Message reply;
            std::string e4;
            CHECK(!p.messageReply("hermes", parent.uuid, "回复正文", reply, e4));
            CHECK(!e4.empty());
            // 回滚生效：父消息之外没有多出回复行
            CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM messages"), 1);
        }
        CHECK(dropTrigger(dbPath, "trig_msg_update"));

        // ---- 3. 维护统计：删除失败必须如实返回失败 ----
        // 注意：SQLite 的行级触发器**只在真的匹配到行时才触发**。审计行都是刚写的
        // （30 天以内），所以三条 DELETE 本来匹配 0 行、触发器根本不响——必须先塞一条
        // 过期审计行，这条用例才真的在测"删除失败"。
        // 这条也是实测教训：第一版用例因此假绿（返回 true 被当成"没触发"）。
        CHECK(execOnDb(dbPath,
                       "INSERT INTO audit_log(actor, action, target, detail, created_at) "
                       "VALUES ('tester','test.old','x','{}','2000-01-01T00:00:00Z')"));
        CHECK(installAbortTrigger(dbPath, "trig_audit_delete",
                                  "BEFORE DELETE ON audit_log BEGIN "
                                  "SELECT RAISE(ABORT,'injected: rotate failed'); END"));
        {
            std::string stats, e5;
            CHECK(!p.maintenanceRun("zcode", stats, e5));
            CHECK(!e5.empty());
            // 失败时不得留下"维护成功"的统计与审计
            CHECK(stats.empty());
        }
        CHECK(dropTrigger(dbPath, "trig_audit_delete"));

        // ---- 4. 预算读不出来 → summary 必须失败，且 err 不能被后续查询覆盖 ----
        // SQLite 没有 SELECT 触发器，用"把表改名"制造真实的读失败（等价于表被锁/损坏）
        CHECK(execOnDb(dbPath, "ALTER TABLE settings RENAME TO settings_hidden"));
        {
            ah::UsageSummary sum;
            std::string e6;
            CHECK(!p.usageSummary(sum, e6));
            CHECK(e6.find("weekly_token_budget") != std::string::npos);  // 是预算读失败，不是别的
            std::string e7;
            CHECK_EQ(p.usageBudget(e7), static_cast<int64_t>(-1));  // 调用方拿得到"读失败"
        }
        CHECK(execOnDb(dbPath, "ALTER TABLE settings_hidden RENAME TO settings"));
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 库结构版本读写（第二个连接直接操作 PRAGMA）：bootstrap 的版本门与恢复路径共用同一判据
static int64_t readSchemaVersion(const fs::path& dbPath) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    int64_t v = -1;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && st &&
        sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

static bool writeSchemaVersion(const fs::path& dbPath, int64_t v) {
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    char* msg = nullptr;
    const std::string sql = "PRAGMA user_version = " + std::to_string(v) + ";";
    const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) == SQLITE_OK;
    sqlite3_free(msg);
    sqlite3_close(db);
    return ok;
}

// ---- C 批：恢复备份的失败路径与迁移重放 ----
// 两个此前真实存在的问题：
//   1) 恢复成功后**不重放迁移链**，所以恢复出来的旧库不会重新打上 user_version——
//      "拒绝来自更新版本的库"那道门在恢复路径上是空的（bootstrap 打了，restore 没打）。
//   2) db_.close() 之后有 5 条裸 return false：磁盘满/权限/文件被占时，Platform 被留在
//      "库已关"状态，而调用方只看到一个提示，继续跑则每个操作都静默失效。
static void test_backup_restore_failure_paths() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_restore_" + ah::randomHex(8));
    fs::create_directories(tmp);
    const fs::path dbPath = tmp / "platform.db";
    std::string backupName;
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));

        // ---- 1. 恢复"旧 schema"的备份：必须重新应用迁移并打上当前 user_version ----
        std::string bp;
        CHECK(p.backupCreate(bp, err));
        backupName = fs::path(bp).filename().string();
        // 把备份伪装成旧版本库（user_version=1）。当前版本是 2，因此它不会被版本门拒绝，
        // 恢复后必须由迁移链重新打上 2；若恢复路径漏了迁移，这里会停在 1。
        CHECK(writeSchemaVersion(fs::path(bp), 1));
        CHECK(p.backupRestore(backupName, err));
        p.shutdown();
        CHECK_EQ(readSchemaVersion(dbPath), static_cast<int64_t>(2));
    }
    {
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));

        // ---- 2. 版本门：拒绝来自更新版本的备份，且原库必须原样 ----
        std::string bp;
        CHECK(p.backupCreate(bp, err));
        const std::string newerName = fs::path(bp).filename().string();
        CHECK(writeSchemaVersion(fs::path(bp), 99));
        std::string newerErr;
        CHECK(!p.backupRestore(newerName, newerErr));
        CHECK(newerErr.find("newer") != std::string::npos);
        CHECK(p.isUsable());  // 拒绝不是失败：平台必须还能用

        // ---- 3. 恢复中途失败（备份文件头合法、内容损坏）----
        // 真实故障形态：copy 成功、sqlite3_open 也成功（惰性），但初始化里的第一条
        // 查询就报 "database disk image is malformed" —— 此时库文件已被换掉、连接已关。
        // 期望：补偿把旧库放回去并重开，平台**仍然可用**，且 err 不宣称需要重启。
        // 注意顺序：先写标记条目、再备份，这样"补偿后的旧库"本身就含该条目——
        // 断言才同时证明了两件事（放回的是旧库，且内容完整）。
        ah::KnowledgeEntry marker;
        CHECK(p.knowledgeCreate("hermes", "恢复前写入", "这条必须活过失败的恢复。", {}, "", {}, "",
                                marker, err));
        std::string bp2;
        CHECK(p.backupCreate(bp2, err));
        const std::string badName = fs::path(bp2).filename().string();
        {
            std::ifstream in(bp2, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            in.close();
            CHECK(bytes.size() > 200);
            // 保留前 100 字节（SQLite 头：magic/页大小/页数），其余清零。
            // 页数不变 → 头仍然合法，因此能 open，但读第一页数据即判定损坏。
            for (size_t i = 100; i < bytes.size(); ++i) bytes[i] = '\0';
            std::ofstream out(bp2, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        std::string badErr;
        CHECK(!p.backupRestore(badName, badErr));
        CHECK(!badErr.empty());
        // 补偿成功时明确告诉用户"旧库没被动过"，不该把人吓去重启
        CHECK(badErr.find("unusable") == std::string::npos);
        CHECK(p.isUsable());
        // 库连接确实活着：写一条新的必须成功（若停在"库已关"，这里必然失败）
        ah::KnowledgeEntry after;
        std::string afterErr;
        CHECK(p.knowledgeCreate("hermes", "恢复失败后仍可写", "补偿把旧库放回来了。", {}, "", {}, "",
                                after, afterErr));
        CHECK(!after.uuid.empty());
        CHECK(p.diagnostics().store_unusable == false);

        // ---- 4. 留底轮转：重试恢复不得删掉上一次失败留下的救援副本 ----
        // 场景：上次恢复失败且补偿也没成功（usable_=false），prePath 是用户原始数据的
        // 最后一份；用户重启后重试恢复——旧行为是无条件 fs::remove(prePath)，把它删了。
        const fs::path preStash = tmp / "platform.db.before-restore";
        const fs::path prevStash = tmp / "platform.db.before-restore.previous";
        const std::string stashMarker = "RESCUE-COPY-MARKER-v1";
        {
            std::ofstream out(preStash, std::ios::binary | std::ios::trunc);
            out.write(stashMarker.data(), static_cast<std::streamsize>(stashMarker.size()));
        }
        std::string bp3;
        CHECK(p.backupCreate(bp3, err));
        CHECK(p.backupRestore(fs::path(bp3).filename().string(), err));  // 成功的恢复
        // 救援副本未消失：被轮转到了 .previous，内容原样
        CHECK(fs::exists(prevStash));
        {
            std::ifstream in(prevStash, std::ios::binary);
            std::string got((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
            CHECK_EQ(got, stashMarker);
        }
        // 本次留底在成功后被清理（.previous 不受成功路径影响，那是上一代的）
        CHECK(!fs::exists(preStash));
        CHECK(p.isUsable());
        p.shutdown();
        // 旧库原样回来了：标记条目仍在（若是被损坏文件覆盖，这里会是 0 或读不出来）
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries WHERE title='恢复前写入'"),
                 1);
        CHECK_EQ(readSchemaVersion(dbPath), static_cast<int64_t>(2));
    }

    // ---- 5. 恢复中断自愈：主库缺失 + 留底还在 → 启动时自动改回，绝不静默新建空库 ----
    // 真实故障形态：恢复流程在"关库 → 留底改名"与"拷入备份"之间被杀。此时磁盘上
    // 没有 platform.db，SQLite 默认 CREATE 会静默新建空库照常运行——数据看似全丢，
    // 留底却完好地躺在旁边。bootstrap 必须在开库前发现并把它改回去。
    {
        const fs::path leftover = tmp / "platform.db.before-restore";
        // 制造中断现场：把当前库改名留底（正是恢复流程第二步之后、被杀瞬间的状态）
        fs::rename(dbPath, leftover);
        CHECK(fs::exists(leftover));
        CHECK(!fs::exists(dbPath));
        ah::Platform p(tmp.string());
        std::string err;
        CHECK(p.bootstrap(err));  // 自愈：留底改回原名后正常打开
        // 恢复回来的就是留底那份库：第 4 节断言过的标记条目仍在（若被静默新建
        // 空库顶替，这里会是 0）
        CHECK_EQ(countRows(dbPath, "SELECT COUNT(*) FROM knowledge_entries WHERE title='恢复前写入'"),
                 1);
        CHECK_EQ(readSchemaVersion(dbPath), static_cast<int64_t>(2));
        // 留底已消费（改回原名），不再是悬空文件
        CHECK(!fs::exists(leftover));
        p.shutdown();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// 审计留痕的字段真实性：target 必须指认得了被操作的对象，detail 里的 version 必须是
// 实际写入的版本号。此前三类写路径（knowledge.create / message.send / error.report）
// 的审计行 target 恒为空串——服务在提交后才回填 out，审计却在服务事务内执行；
// knowledge.version.add 与 memory.set 的审计 detail 里 version 恒为调用方默认值 1
// （detail 字符串在服务调用前就拼好了）。留痕存在却指认不了对象，违背"身份+时间+
// 动作+对象"的硬契约。
static void test_audit_target_and_version() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_audit_" + ah::randomHex(8));
    fs::create_directories(tmp);
    ah::Platform p(tmp.string());
    std::string err;
    CHECK(p.bootstrap(err));

    ah::KnowledgeEntry k;
    CHECK(p.knowledgeCreate("hermes", "audit probe", "content v1", {}, "", {}, "", k, err));
    CHECK(p.knowledgeAddVersion("hermes", k.uuid, "", "content v2", {}, "", k, err));
    CHECK_EQ(k.version, 2);

    ah::Message m;
    CHECK(p.messageSend("note", "hermes", "claude", "audit subject", "audit body", m, err));
    ah::ErrorReport e;
    CHECK(p.errorReport("hermes", "warning", "test", "audit error", "detail", "", e, err));
    ah::MemoryEntry mem;
    CHECK(p.memorySet("hermes", "project", "audit_key", "v1", 0, mem, err));
    CHECK(p.memorySet("hermes", "project", "audit_key", "v2", mem.version, mem, err));
    CHECK_EQ(mem.version, 2);

    std::vector<ah::AuditRecord> recs;
    CHECK(p.auditList("", "", "", 1000, recs, err));
    auto findLatest = [&recs](const std::string& action) -> const ah::AuditRecord* {
        const ah::AuditRecord* hit = nullptr;
        for (const auto& r : recs)
            if (r.action == action && (!hit || r.id > hit->id)) hit = &r;
        return hit;
    };

    const ah::AuditRecord* kc = findLatest("knowledge.create");
    CHECK(kc != nullptr);
    if (kc) CHECK_EQ(kc->target, k.uuid);
    const ah::AuditRecord* ms = findLatest("message.send");
    CHECK(ms != nullptr);
    if (ms) CHECK_EQ(ms->target, m.uuid);
    const ah::AuditRecord* er = findLatest("error.report");
    CHECK(er != nullptr);
    if (er) CHECK_EQ(er->target, e.uuid);

    const ah::AuditRecord* va = findLatest("knowledge.version.add");
    CHECK(va != nullptr);
    if (va) {
        auto j = nlohmann::json::parse(va->detail, nullptr, false);
        CHECK(!j.is_discarded());
        if (!j.is_discarded()) CHECK_EQ(j.value("version", 0), 2);
    }
    const ah::AuditRecord* mset = findLatest("memory.set");
    CHECK(mset != nullptr);
    if (mset) {
        auto j = nlohmann::json::parse(mset->detail, nullptr, false);
        CHECK(!j.is_discarded());
        if (!j.is_discarded()) CHECK_EQ(j.value("version", 0), 2);
    }
    p.shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// LIKE 通配符必须按字面量匹配：用户查询与标签里的 % / _ 是普通字符，不是匹配语法。
// 此前查询 "100%" 会当通配符扫出一串无关条目、tag "_" 会让标签过滤形同虚设
//（三处 LIKE：短查询回退路径、列表标签过滤、FTS 路径的标签过滤）。
static void test_like_wildcard_escaping() {
    fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_like_" + ah::randomHex(8));
    fs::create_directories(tmp);
    ah::Platform p(tmp.string());
    std::string err;
    CHECK(p.bootstrap(err));

    ah::KnowledgeEntry a, b;
    CHECK(p.knowledgeCreate("hermes", "progress 100%", "含字面量百分号的正文", {"a_b"}, "", {},
                            "", a, err));
    CHECK(p.knowledgeCreate("hermes", "unrelated", "完全无关的条目", {"other"}, "", {}, "", b,
                            err));

    // 查询 "100%"（<3 码点走 LIKE 回退路径）：只命中字面量包含 100% 的那条
    std::vector<ah::KnowledgeHit> hits;
    CHECK(p.knowledgeSearch("100%", ah::SearchMode::Keyword, 20, "", hits, err));
    CHECK_EQ(hits.size(), static_cast<size_t>(1));
    if (hits.size() == 1) CHECK_EQ(hits[0].entry.uuid, a.uuid);
    // 查询 "_"（LIKE 路径）：字面量匹配，标题与正文都不含下划线 → 空
    hits.clear();
    CHECK(p.knowledgeSearch("_", ah::SearchMode::Keyword, 20, "", hits, err));
    CHECK(hits.empty());
    // ≥3 码点走 FTS 路径：标签过滤同样按字面量
    hits.clear();
    CHECK(p.knowledgeSearch("unrelated", ah::SearchMode::Keyword, 20, "other", hits, err));
    CHECK_EQ(hits.size(), static_cast<size_t>(1));
    // 列表的标签过滤："a_b" 按字面量命中（含通配符语义时同样命中，作对照）；
    // "_" 按字面量不存在（没有恰好叫 _ 的标签）→ 空——若 %/_ 仍是通配符，
    // 这里会扫出全部条目
    std::vector<ah::KnowledgeEntry> listed;
    CHECK(p.knowledgeList(50, "a_b", listed, err));
    CHECK_EQ(listed.size(), static_cast<size_t>(1));
    if (listed.size() == 1) CHECK_EQ(listed[0].uuid, a.uuid);
    listed.clear();
    CHECK(p.knowledgeList(50, "_", listed, err));
    CHECK(listed.empty());
    // "a%b" 按字面量不存在 → 空（若 % 仍是通配符，这里会误命中 a_b）
    listed.clear();
    CHECK(p.knowledgeList(50, "a%b", listed, err));
    CHECK(listed.empty());
    p.shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// RSA-2048 PKCS1v15/SHA256 验签原语（更新清单签名校验的底层）。
// 两层覆盖：固定已知向量（一次性测试密钥签发，私钥已销毁；篡改任意一端必须
// 失败）+ CNG 生成密钥对自签自验（确认与 .NET/openssl 签名口径互通）。
static void test_rsa_verify() {
    // ---- a) 固定向量 ----
    const char* kMod =
        "856F1A21ADFB2982A09B7449850572B5BE5DA4C67668446A6F0D22F3B75658B1BBB7CDC41558417EF858A3F7E6AE4B3D871C"
        "6EDE6ABF85DF249EA6927BA2F9DD5405B0FCAECFF674987ACD8EB681BE63C9C4027C9A40498FDE04B1517460F36CB2E17599"
        "C73E87E0962B1404197F1AFFB0AA9E7758E59E027BEF91335D035E8DEB110F1EA31AC646AB6BC9B098D7FF1E63B3F56DBBEC"
        "2E07F483922F02FC95C5A600181AED50FF04F2662DDE21F193BC4471651D9151F0EB36694D9B6B7C77B0CA5239FE755BAC99"
        "4A8657A733D93576F2E5F1E538AA816F7C95905BDF1E53AD86FBA4DF9437B737C3E40FDF38B0433D0640DC018374857764DD"
        "96B96DEAF06D";
    const char* kMsg = "miderhive-update-manifest-test-vector";
    const char* kSig =
        "WGiuVVo+9AUuIc6p0PZD0tkinqK0KM2b+xWIAf1NeDZV2zHGfRSR2yTB1jlyfFqfIBZ31RDGNcoYOw7etFRdkn+3PdA12Aj4dEPU"
        "Wk1HlN1LDwxDMD5hN/sk5U2MO6TEol/20Aem/+jq17UI6j3f5Sx3FKDEflF6IWWJR9jvo9K+QdjGWGeo3eKR2mGdYbMxGqIKj8n1"
        "bqZqJcmUpfSh07srWs7Z+BwurBo4BvW9tyRa74OHBK3QUoHv6oWklPbGSw24JAcQJtLPLottiDDNNx56usxrSFl/v73d1IkNgkqH"
        "AzYiupWrSQl7luE0m7IaAeJj0aVpG6efn8jvwHjWiQ==";
    CHECK(ah::rsaVerifySha256Pkcs1(kMod, "010001", kMsg, kSig));
    CHECK(!ah::rsaVerifySha256Pkcs1(kMod, "010001", "tampered payload", kSig));
    CHECK(!ah::rsaVerifySha256Pkcs1(kMod, "010001", kMsg, "AAAA"));   // 长度不足的坏签名
    CHECK(!ah::rsaVerifySha256Pkcs1("ZZ", "010001", kMsg, kSig));     // 非法 hex
    CHECK(!ah::rsaVerifySha256Pkcs1(kMod, "010001", kMsg, "!!!!"));   // 非法 base64

#ifdef _WIN32
    // ---- b) 动态：CNG 生成 RSA-2048 → 签 SHA256（PKCS1v15）→ 用待测函数验 ----
    BCRYPT_ALG_HANDLE keyAlg = nullptr, hashAlg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    do {
        if (BCryptOpenAlgorithmProvider(&keyAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) { std::fprintf(stderr, "[dyn-debug] open key alg\n"); break; }
        if (BCryptGenerateKeyPair(keyAlg, &key, 2048, 0) != 0) { std::fprintf(stderr, "[dyn-debug] generate\n"); break; }
        if (BCryptFinalizeKeyPair(key, 0) != 0) { std::fprintf(stderr, "[dyn-debug] finalize\n"); break; }
        // 导出公钥 blob，取模数/指数转 hex
        ULONG blobLen = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &blobLen, 0) != 0) { std::fprintf(stderr, "[dyn-debug] export size\n"); break; }
        std::string blob(blobLen, '\0');
        if (BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                            reinterpret_cast<PUCHAR>(blob.data()), blobLen, &blobLen, 0) != 0)
            { std::fprintf(stderr, "[dyn-debug] export\n"); break; }
        auto rdU32 = [&blob](size_t off) {
            return static_cast<uint32_t>(static_cast<uint8_t>(blob[off])) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(blob[off + 1])) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(blob[off + 2])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(blob[off + 3])) << 24);
        };
        const uint32_t cbExp = rdU32(8), cbMod = rdU32(12);
        std::string expHex, modHex;
        for (uint32_t i = 0; i < cbExp; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02X",
                          static_cast<uint8_t>(blob[24 + i]));
            expHex += buf;
        }
        for (uint32_t i = 0; i < cbMod; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02X",
                          static_cast<uint8_t>(blob[24 + cbExp + i]));
            modHex += buf;
        }
        // 签名固定消息（PKCS1v15：BCryptSignHash 无 padding flags）
        const std::string msg = "dynamic round-trip vector";
        if (BCryptOpenAlgorithmProvider(&hashAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) { std::fprintf(stderr, "[dyn-debug] open hash alg\n"); break; }
        BCRYPT_HASH_HANDLE hash = nullptr;
        uint8_t sha[32];
        if (BCryptCreateHash(hashAlg, &hash, nullptr, 0, nullptr, 0, 0) != 0) { std::fprintf(stderr, "[dyn-debug] create hash\n"); break; }
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())),
                           static_cast<ULONG>(msg.size()), 0) != 0)
            { std::fprintf(stderr, "[dyn-debug] hash data\n"); break; }
        if (BCryptFinishHash(hash, sha, sizeof(sha), 0) != 0) { std::fprintf(stderr, "[dyn-debug] finish hash\n"); break; }
        ULONG sigLen = 0;
        BCRYPT_PKCS1_PADDING_INFO padInfo;
        padInfo.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        if (BCryptSignHash(key, reinterpret_cast<PUCHAR>(&padInfo), sha, sizeof(sha), nullptr, 0,
                           &sigLen, BCRYPT_PAD_PKCS1) != 0) { std::fprintf(stderr, "[dyn-debug] sign size\n"); break; }
        std::string sig(sigLen, '\0');
        if (BCryptSignHash(key, reinterpret_cast<PUCHAR>(&padInfo), sha, sizeof(sha),
                           reinterpret_cast<PUCHAR>(sig.data()), sigLen, &sigLen,
                           BCRYPT_PAD_PKCS1) != 0)
            { std::fprintf(stderr, "[dyn-debug] sign\n"); break; }
        // base64 编码（测试内联，签名侧交付格式）
        static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string sigB64;
        for (size_t i = 0; i < sig.size(); i += 3) {
            const uint32_t n = (static_cast<uint8_t>(sig[i]) << 16) |
                               (i + 1 < sig.size() ? static_cast<uint8_t>(sig[i + 1]) << 8 : 0) |
                               (i + 2 < sig.size() ? static_cast<uint8_t>(sig[i + 2]) : 0);
            sigB64 += kB64[(n >> 18) & 63];
            sigB64 += kB64[(n >> 12) & 63];
            sigB64 += i + 1 < sig.size() ? kB64[(n >> 6) & 63] : '=';
            sigB64 += i + 2 < sig.size() ? kB64[n & 63] : '=';
        }
        CHECK(ah::rsaVerifySha256Pkcs1(modHex, expHex, msg, sigB64));
        CHECK(!ah::rsaVerifySha256Pkcs1(modHex, expHex, msg + "!", sigB64));
        BCryptDestroyHash(hash);
    } while (false);
    if (hashAlg) BCryptCloseAlgorithmProvider(hashAlg, 0);
    if (key) BCryptDestroyKey(key);
    if (keyAlg) BCryptCloseAlgorithmProvider(keyAlg, 0);
#endif
}

// 安装检测下沉 core 的原语：PATH 查找（含 Windows 后缀补全与 PATH 注入/撤除）、
// 注册表 exeName 完整性、UTF-8（中文）路径的读/写/备份回环（fsPath 宽转换）。
static void test_install_detection() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("MiderHive_test_probe_" + ah::randomHex(8));
    fs::create_directories(tmp);
#ifdef _WIN32
    const std::string probe = "mhtestprobe";
    const std::string probeFile = (tmp / "mhtestprobe.cmd").string();
#else
    const std::string probe = "mhtestprobe";
    const std::string probeFile = (tmp / "mhtestprobe").string();
#endif
    { std::ofstream o(probeFile, std::ios::binary); o << "probe"; }
    const std::string oldPath = ah::envOr({"PATH"});
#ifdef _WIN32
    _putenv_s("PATH", (tmp.string() + ";" + oldPath).c_str());
#else
    setenv("PATH", (tmp.string() + ":" + oldPath).c_str(), 1);
#endif
    CHECK(ah::integrations::exeOnPath(probe));
    CHECK(!ah::integrations::exeOnPath("mhtestprobe-definitely-missing"));
#ifdef _WIN32
    _putenv_s("PATH", oldPath.c_str());
#else
    setenv("PATH", oldPath.c_str(), 1);
#endif
    CHECK(!ah::integrations::exeOnPath(probe));  // 注入撤除后不再命中

    for (const auto& t : ah::integrations::toolRegistry()) {
        CHECK(t.exeName != nullptr);
        CHECK(!ah::integrations::candidatePaths(t.id).empty() || (t.exeName && *t.exeName));
        CHECK(!ah::integrations::candidatePaths(t.id).empty());
    }
    CHECK(ah::integrations::candidatePaths("no-such-tool").empty());

    // UTF-8（中文）路径：写/读/备份回环——窄字符串路径在 Windows 按 ACP 解释，
    // 中文用户名下会全部错位（fsPath 的 char8_t 宽转换是修法）
    const fs::path cdir = fs::temp_directory_path() / "MiderHive_test_中文路径";
    fs::create_directories(cdir);
    auto u8str = [](const fs::path& q) {
        auto u8 = q.u8string();  // UTF-8 字节串（char8_t -> char 保真拷贝）
        return std::string(u8.begin(), u8.end());
    };
    const std::string cfile = u8str(cdir / "cfg.json");  // .string() 会转 ACP，中文目录名错位
    const std::string payload = "{\"mcpServers\":{}}";
    CHECK(ah::integrations::writeFileUtf8(cfile, payload));
    std::string got;
    CHECK(ah::integrations::readFileUtf8(cfile, got));
    CHECK_EQ(got, payload);
    CHECK(ah::integrations::backupFile(cfile));
    CHECK(fs::exists(cdir / "cfg.json.miderhive.bak"));
    std::error_code ec;
    fs::remove_all(cdir, ec);
    fs::remove_all(tmp, ec);
}

int main() {
    auto run = [](const char* name, void (*fn)()) {
        std::printf("== %s\n", name);
        std::fflush(stdout);
        fn();
    };
    run("sha256", test_sha256);
    run("rsa_verify", test_rsa_verify);
    run("install_detection", test_install_detection);
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
    run("write_replaces_atomically", test_write_replaces_atomically);
    run("embedding_dims", test_embedding_dims);
    run("legacy_vec_migration", test_legacy_vec_migration);
    run("keyword_fts", test_keyword_fts);
    run("like_wildcard_escaping", test_like_wildcard_escaping);
    run("fts_rebuild_legacy", test_fts_rebuild_legacy);
    run("semantic_tag_pushdown", test_semantic_tag_pushdown);
    run("heartbeat_audit", test_heartbeat_audit);
    run("audit_target_and_version", test_audit_target_and_version);
    run("schema_version_gate", test_schema_version_gate);
    run("embed_provider_binding", test_embed_provider_binding);
    run("embedding_dim_tracking", test_embedding_dim_tracking);
    run("failure_paths_report_errors", test_failure_paths_report_errors);
    run("backup_restore_failure_paths", test_backup_restore_failure_paths);

    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
