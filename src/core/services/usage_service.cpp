#include "core/services/usage_service.h"

#include <algorithm>
#include <map>

#include "core/util.h"

namespace ah {

bool UsageService::report(const std::string& agent, int64_t tokensIn, int64_t tokensOut,
                          const std::string& callType, const std::string& model,
                          const std::string& referenceId, const std::string& idempotencyKey,
                          bool& duplicate, std::string& err) {
    duplicate = false;
    // BEGIN IMMEDIATE + 事务内查重：并发/重复上报不会双重扣减
    if (!db_.beginImmediate(err)) return false;
    if (!reportInTx(agent, tokensIn, tokensOut, callType, model, referenceId, idempotencyKey,
                    duplicate, err)) {
        db_.rollback();
        return false;
    }
    // COMMIT 失败同样要回滚：否则事务悬挂,后续所有写操作都会报
    // "cannot start a transaction within a transaction"
    if (!db_.commit(err)) { db_.rollback(); return false; }
    return true;
}

bool UsageService::reportInTx(const std::string& agent, int64_t tokensIn, int64_t tokensOut,
                              const std::string& callType, const std::string& model,
                              const std::string& referenceId, const std::string& idempotencyKey,
                              bool& duplicate, std::string& err) {
    duplicate = false;
    if (tokensIn < 0 || tokensOut < 0) { err = "token counts must be >= 0"; return false; }
    if (idempotencyKey.size() > 200) { err = "idempotency_key too long (max 200)"; return false; }
    if (model.size() > 100) { err = "model too long (max 100)"; return false; }

    if (!idempotencyKey.empty()) {
        bool exists = false;
        if (!db_.query("SELECT 1 FROM token_usage WHERE idempotency_key = ?",
                       [&](Stmt& st) { st.bind(1, idempotencyKey); },
                       [&](Stmt&) { exists = true; }, err)) {
            return false;
        }
        if (exists) {
            duplicate = true;
            return true;
        }
    }
    return db_.query(
        "INSERT INTO token_usage(agent, week_start, tokens_in, tokens_out, call_type, model, reference_id, idempotency_key, created_at) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        [&](Stmt& st) {
            st.bind(1, agent);
            st.bind(2, weekStartIso());
            st.bind(3, tokensIn);
            st.bind(4, tokensOut);
            st.bind(5, callType);
            st.bind(6, model);
            st.bind(7, referenceId);
            st.bind(8, idempotencyKey);
            st.bind(9, nowIso());
        },
        nullptr, err);
}

bool UsageService::daily(int days, std::vector<UsageDailyPoint>& out, std::string& err) {
    out.clear();
    days = std::clamp(days, 1, 90);
    // created_at 为 UTC ISO8601，截前 10 位即日期；字典序与时间序一致
    std::string cutoff = isoDaysAgo(days - 1).substr(0, 10);
    std::map<std::string, int64_t> byDay;
    if (!db_.query(
            "SELECT substr(created_at,1,10) AS day, SUM(tokens_in + tokens_out) "
            "FROM token_usage WHERE substr(created_at,1,10) >= ? GROUP BY day",
            [&](Stmt& st) { st.bind(1, cutoff); },
            [&](Stmt& st) { byDay[st.text(0)] = st.i64(1); }, err))
        return false;
    for (int i = days - 1; i >= 0; --i) {  // 旧 → 新，缺失天补 0
        UsageDailyPoint p;
        p.day = isoDaysAgo(i).substr(0, 10);
        if (auto it = byDay.find(p.day); it != byDay.end()) p.tokens = it->second;
        out.push_back(std::move(p));
    }
    return true;
}

bool UsageService::byModel(std::vector<UsageModelRow>& out, std::string& err) {
    out.clear();
    return db_.query(
        "SELECT model, SUM(tokens_in + tokens_out) AS t FROM token_usage "
        "WHERE model <> '' GROUP BY model ORDER BY t DESC LIMIT 20",
        nullptr,
        [&](Stmt& st) { out.push_back({st.text(0), st.i64(1)}); }, err);
}

bool UsageService::breakdown(int days, const std::string& agentFilter,
                             const std::string& modelFilter, UsageBreakdown& out,
                             std::string& err) {
    out = UsageBreakdown{};
    days = std::clamp(days, 1, 90);
    // 与 daily() 同口径：created_at 为 UTC ISO8601，截前 10 位即日期
    const std::string cutoff = isoDaysAgo(days - 1).substr(0, 10);
    std::string where = "substr(created_at,1,10) >= ?";
    if (!agentFilter.empty()) where += " AND agent = ?";
    if (!modelFilter.empty()) where += " AND model = ?";
    auto bind = [&](Stmt& st) {
        int idx = 1;
        st.bind(idx++, cutoff);
        if (!agentFilter.empty()) st.bind(idx++, agentFilter);
        if (!modelFilter.empty()) st.bind(idx++, modelFilter);
    };

    if (!db_.query(
            ("SELECT COALESCE(SUM(tokens_in),0), COALESCE(SUM(tokens_out),0), COUNT(*) "
             "FROM token_usage WHERE "
             + where).c_str(),
            bind,
            [&](Stmt& st) {
                out.total_in = st.i64(0);
                out.total_out = st.i64(1);
                out.calls = st.i64(2);
                out.total_tokens = out.total_in + out.total_out;
            },
            err))
        return false;

    if (!db_.query(
            ("SELECT agent, SUM(tokens_in), SUM(tokens_out), SUM(tokens_in + tokens_out), COUNT(*) "
             "FROM token_usage WHERE " + where +
             " GROUP BY agent ORDER BY SUM(tokens_in + tokens_out) DESC").c_str(),
            bind,
            [&](Stmt& st) {
                UsageAgentRow r;
                r.agent = st.text(0);
                r.in = st.i64(1);
                r.out = st.i64(2);
                r.tokens = st.i64(3);
                r.calls = st.i64(4);
                out.per_agent.push_back(std::move(r));
            },
            err))
        return false;

    if (!db_.query(
            ("SELECT model, SUM(tokens_in + tokens_out), COUNT(*) FROM token_usage WHERE " +
             where + " AND model <> '' GROUP BY model ORDER BY 2 DESC LIMIT 20").c_str(),
            bind,
            [&](Stmt& st) {
                out.per_model.push_back({st.text(0), st.i64(1), st.i64(2)});
            },
            err))
        return false;

    std::map<std::string, int64_t> byDay;
    if (!db_.query(
            ("SELECT substr(created_at,1,10) AS day, SUM(tokens_in + tokens_out) "
             "FROM token_usage WHERE " + where + " GROUP BY day").c_str(),
            bind,
            [&](Stmt& st) { byDay[st.text(0)] = st.i64(1); }, err))
        return false;
    for (int i = days - 1; i >= 0; --i) {  // 旧 → 新，缺失天补 0
        UsageDailyPoint p;
        p.day = isoDaysAgo(i).substr(0, 10);
        if (auto it = byDay.find(p.day); it != byDay.end()) p.tokens = it->second;
        out.daily.push_back(std::move(p));
    }
    return true;
}

int64_t UsageService::budget(std::string& err) {
    int64_t value = kDefaultWeeklyBudget;
    db_.query("SELECT value FROM settings WHERE key='weekly_token_budget'", nullptr,
              [&](Stmt& st) { value = st.i64(0); }, err);
    return value;
}

bool UsageService::setBudget(int64_t newBudget, std::string& err) {
    if (newBudget <= 0) { err = "budget must be positive"; return false; }
    return db_.query(
        "INSERT INTO settings(key, value) VALUES('weekly_token_budget', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        [&](Stmt& st) { st.bind(1, newBudget); }, nullptr, err);
}

bool UsageService::summary(UsageSummary& out, std::string& err) {
    out = UsageSummary{};
    out.week_start = weekStartIso();
    out.budget = budget(err);

    if (!db_.query(
            "SELECT COALESCE(SUM(tokens_in),0), COALESCE(SUM(tokens_out),0) FROM token_usage WHERE week_start=?",
            [&](Stmt& st) { st.bind(1, out.week_start); },
            [&](Stmt& st) {
                out.total_in = st.i64(0);
                out.total_out = st.i64(1);
            },
            err))
        return false;
    out.total_tokens = out.total_in + out.total_out;

    if (!db_.query(
            "SELECT agent, SUM(tokens_in + tokens_out) AS t FROM token_usage WHERE week_start=? "
            "GROUP BY agent ORDER BY t DESC",
            [&](Stmt& st) { st.bind(1, out.week_start); },
            [&](Stmt& st) { out.per_agent.emplace_back(st.text(0), st.i64(1)); }, err))
        return false;

    double ratio = out.budget > 0 ? static_cast<double>(out.total_tokens) / out.budget : 0.0;
    if (ratio > 1.0) out.alert_level = "over";
    else if (ratio >= 0.95) out.alert_level = "critical";
    else if (ratio >= 0.80) out.alert_level = "warn";
    else out.alert_level = "none";
    return true;
}

}  // namespace ah
