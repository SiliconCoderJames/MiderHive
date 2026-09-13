#pragma once
// 平台各域的结构化数据类型，core / http / gui 共用。
#include <cstdint>
#include <string>
#include <vector>

#include "core/version.h"  // CMake 生成：MIDERHIVE_VERSION 等

namespace ah {

struct AgentInfo {
    std::string name;
    std::string role;           // member | zcode
    std::string status;         // online | offline（由心跳时间推断）
    std::string current_task;
    std::string last_seen_at;
    std::string created_at;
};

struct KnowledgeEntry {
    int64_t id = 0;
    std::string uuid;
    std::string title;
    std::string content;
    std::string tags_json;      // 原始 JSON 数组
    std::string category;
    std::string author;
    int version = 1;
    std::string embedding_provider;
    std::string created_at;
    std::vector<std::string> tags;
};

struct KnowledgeHit {
    KnowledgeEntry entry;
    double score = 0.0;         // 语义距离（越小越相似）
};

struct SkillInfo {
    std::string name;
    std::string display_name;
    std::string description;
    std::string category;
    std::string owner_agent;
    std::string param_schema;   // JSON Schema
    int version = 1;
    std::string status;         // active | deprecated
    std::string created_at;
    std::string updated_at;
};

struct SkillInvocation {
    int64_t id = 0;
    std::string skill_name;
    std::string caller_agent;
    std::string params;
    std::string result_summary;
    std::string status;
    int64_t duration_ms = 0;
    std::string reference_id;   // 协作上下文追溯：发起本次调用的消息/任务/错误 uuid（弱关联，可空）
    std::string created_at;
};

struct MemoryEntry {
    std::string section;        // project | preference | work_style | decision | environment
    std::string key;
    std::string value;
    std::string author;
    int version = 1;
    std::string created_at;
};

struct Message {
    int64_t id = 0;
    std::string uuid;
    std::string kind;           // note | question | task
    std::string sender;
    std::string recipient;      // 空 = 广播
    std::string subject;
    std::string body;
    std::string status;         // unread|read ; task: pending|accepted|done|declined
    std::string parent_uuid;
    std::string created_at;
};

struct ErrorReport {
    int64_t id = 0;
    std::string uuid;
    std::string reporter;
    std::string severity;       // info | warning | error | critical
    std::string source;
    std::string title;
    std::string detail;
    std::string stack_trace;
    std::string status;         // open | investigating | resolved
    std::string resolution_notes;
    std::string resolved_by;
    std::string created_at;
    std::string resolved_at;
};

struct AuditRecord {
    int64_t id = 0;
    std::string actor;
    std::string action;
    std::string target;
    std::string detail;         // JSON
    std::string created_at;
};

struct UsageSummary {
    std::string week_start;
    int64_t budget = 0;
    int64_t total_tokens = 0;
    int64_t total_in = 0;
    int64_t total_out = 0;
    std::vector<std::pair<std::string, int64_t>> per_agent;  // name, tokens
    std::string alert_level;    // none | warn | critical | over
};

struct UsageDailyPoint {
    std::string day;            // UTC 日期 YYYY-MM-DD
    int64_t tokens = 0;
};

struct UsageModelRow {
    std::string model;
    int64_t tokens = 0;
};

// 用量分析的多维切片（时间窗 + 可选 agent/model 筛选）下的聚合行
struct UsageAgentRow {
    std::string agent;
    int64_t in = 0;
    int64_t out = 0;
    int64_t tokens = 0;
    int64_t calls = 0;
};

struct UsageModelStat {
    std::string model;
    int64_t tokens = 0;
    int64_t calls = 0;
};

// 一次筛选（最近 N 天 + 可选 agent/model）下的完整用量视图：
// 同一套 WHERE 聚合出合计、按 Agent、按模型与逐日序列，保证四者口径一致
struct UsageBreakdown {
    int64_t total_in = 0;
    int64_t total_out = 0;
    int64_t total_tokens = 0;
    int64_t calls = 0;
    std::vector<UsageAgentRow> per_agent;
    std::vector<UsageModelStat> per_model;
    std::vector<UsageDailyPoint> daily;
};

enum class SearchMode { Keyword, Semantic };

constexpr const char* kDefaultRole = "member";
constexpr const char* kManagerName = "zcode";
// 版本号来自 CMake 生成的 version.h（单一来源，界面/健康检查/安装包共用）
constexpr const char* kPlatformVersion = MIDERHIVE_VERSION;

}  // namespace ah
