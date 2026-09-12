#pragma once
// Token 管控：每次调用上报消耗；按自然周（周一 UTC 起）聚合；
// 达到预算 80% 告警（warn）、95% 预警（critical）、超出标记 over。
#include <cstdint>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/types.h"

namespace ah {

constexpr int64_t kDefaultWeeklyBudget = 10'000'000;  // 每周 1000 万 Token

class UsageService {
public:
    explicit UsageService(Database& db) : db_(db) {}

    // idempotencyKey 非空时幂等：同一键重复上报只记一次，duplicate=true 标记
    // model 非空时按模型维度累计（设置界面「模型用量」）
    bool report(const std::string& agent, int64_t tokensIn, int64_t tokensOut,
                const std::string& callType, const std::string& model,
                const std::string& referenceId, const std::string& idempotencyKey,
                bool& duplicate, std::string& err);
    bool summary(UsageSummary& out, std::string& err);
    // 最近 days 天的逐日消耗（连续日期，缺失天补 0），days 夹取到 1..90
    bool daily(int days, std::vector<UsageDailyPoint>& out, std::string& err);
    // 按模型累计（全部历史，模型非空才计入），消耗降序，最多 20 行
    bool byModel(std::vector<UsageModelRow>& out, std::string& err);
    // 多维用量切片：最近 days 天（可选 agent/model 过滤）下的合计、按 Agent、
    // 按模型与逐日序列——同一条 WHERE 聚合，四个视图口径一致
    bool breakdown(int days, const std::string& agent, const std::string& model,
                   UsageBreakdown& out, std::string& err);
    int64_t budget(std::string& err);
    bool setBudget(int64_t budget, std::string& err);

private:
    Database& db_;
};

}  // namespace ah
