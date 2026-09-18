#pragma once
// Platform：平台门面。Qt 工作台（进程内直调）与 HTTP API 层共用同一实例，
// 内部以互斥锁串行化，保证 SQLite 与各服务线程安全。
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/embed/embedder.h"
#include "core/services/agent_service.h"
#include "core/services/audit_service.h"
#include "core/services/error_service.h"
#include "core/services/knowledge_service.h"
#include "core/services/memory_service.h"
#include "core/services/message_service.h"
#include "core/services/skill_service.h"
#include "core/services/usage_service.h"
#include "core/types.h"

namespace ah {

class HttpServer;

// 数据目录：环境变量 MIDERHIVE_HOME 优先（兼容 AGENTHIVE_HOME / ZCODE_PLATFORM_HOME），
// 否则 %USERPROFILE%\.miderhive
std::string defaultHomeDir();

class Platform {
public:
    explicit Platform(std::string homeDir);
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    const std::string& homeDir() const { return home_dir_; }
    bool bootstrap(std::string& err);
    void shutdown();

    // ---- 认证（仅比对哈希，密钥明文只存于运行期生成的配置文件）----
    bool authenticate(const std::string& agent, const std::string& apiKey) const;
    bool authenticateMaster(const std::string& masterKey) const;
    bool registerAgent(const std::string& masterKey, const std::string& name, const std::string& role,
                       std::string& outApiKey, std::string& err);
    bool isManager(const std::string& name) const;
    // 保留身份（user/zcode/system/master）：在鉴权里是特权主体，禁止注册占用
    static bool isReservedName(const std::string& name);

    // ---- Agent ----
    void heartbeat(const std::string& name, const std::string& currentTask);
    bool listAgents(std::vector<AgentInfo>& out, std::string& err);
    // 管理性移除（仅管理者；管理者自身不可删）；密钥随之失效，记入审计
    bool agentRemove(const std::string& actor, const std::string& name, std::string& err);
    // 重新生成某 Agent 的 API Key（管理者权限）。数据库只存加盐哈希，明文不可恢复，
    // 因此密钥丢失/泄露时这是唯一恢复与轮换途径；新明文同时写入 config/agents.json 并返回一次。
    bool agentRotateKey(const std::string& actor, const std::string& name, std::string& outApiKey,
                        std::string& err);
    // 一键接入：为给定名称签发可用凭据并返回明文密钥（幂等）。
    // 不存在则注册（member 角色），已存在则轮换（库里只有哈希，明文不可恢复）；
    // 保留名照常拒绝。供 GUI 接入向导在进程内以管理者身份调用。
    bool agentProvision(const std::string& actor, const std::string& name, std::string& outApiKey,
                        std::string& err);

    // ---- 知识库 ----
    bool knowledgeCreate(const std::string& author, const std::string& title,
                         const std::string& content, const std::vector<std::string>& tags,
                         const std::string& category, const std::vector<float>& embedding,
                         const std::string& embeddingProvider, KnowledgeEntry& out, std::string& err);
    bool knowledgeList(int limit, const std::string& tagFilter, std::vector<KnowledgeEntry>& out,
                       std::string& err);
    bool knowledgeLatest(const std::string& uuid, KnowledgeEntry& out, std::string& err);
    bool knowledgeVersions(const std::string& uuid, std::vector<KnowledgeEntry>& out, std::string& err);
    bool knowledgeAddVersion(const std::string& author, const std::string& uuid,
                             const std::string& newTitle, const std::string& newContent,
                             const std::vector<float>& embedding, const std::string& embeddingProvider,
                             KnowledgeEntry& out, std::string& err);
    bool knowledgeSearch(const std::string& query, SearchMode mode, int limit,
                         const std::string& tagFilter, std::vector<KnowledgeHit>& out,
                         std::string& err);
    // 语义检索（Agent 自带模型向量）：queryVec 的维度决定在哪个分表里检索，
    // 需与写入该条目时的维度一致。不走内置嵌入器。
    bool knowledgeSearchSemantic(const std::vector<float>& queryVec, int limit,
                                 const std::string& tagFilter, std::vector<KnowledgeHit>& out,
                                 std::string& err);
    // 管理性硬删除（仅管理者/主密钥），连同全部版本与向量；记入审计
    bool knowledgeRemove(const std::string& actor, const std::string& uuid, std::string& err);

    // ---- 技能库 ----
    bool skillRegister(const std::string& author, const std::string& name,
                       const std::string& displayName, const std::string& description,
                       const std::string& category, const std::string& paramSchema, SkillInfo& out,
                       std::string& err);
    bool skillList(const std::string& categoryFilter, const std::string& ownerFilter,
                   std::vector<SkillInfo>& out, std::string& err);
    bool skillGet(const std::string& name, SkillInfo& out, std::string& err);
    // 先注册后调用；记录调用 + Token + 审计（同一事务）。referenceId 为可选的
    // 协作上下文追溯（发起调用的消息/任务/错误 uuid），随调用记录一并落库。
    bool skillInvoke(const std::string& caller, const std::string& skillName,
                     const std::string& paramsJson, const std::string& resultSummary,
                     const std::string& status, int64_t durationMs, int64_t tokensIn,
                     int64_t tokensOut, const std::string& referenceId, std::string& err);
    bool skillInvocations(const std::string& skillName, int limit,
                          std::vector<SkillInvocation>& out, std::string& err);

    // ---- 用户记忆 ----
    bool memoryList(const std::string& section, std::vector<MemoryEntry>& out, std::string& err);
    bool memorySet(const std::string& author, const std::string& section, const std::string& key,
                   const std::string& value, int baseVersion, MemoryEntry& out, std::string& err);
    bool memoryHistory(const std::string& section, const std::string& key,
                       std::vector<MemoryEntry>& out, std::string& err);
    // 管理性删除某条记忆的全部版本；记入审计
    bool memoryRemove(const std::string& actor, const std::string& section,
                      const std::string& key, std::string& err);

    // ---- 消息 ----
    bool messageSend(const std::string& kind, const std::string& sender,
                     const std::string& recipient, const std::string& subject,
                     const std::string& body, Message& out, std::string& err);
    // viewer 非空时按可见性收敛：只返回广播 + 发给 viewer + viewer 自己发出的消息
    // （viewer 为空 = 进程内 GUI/管理视角，可见全部）。此前 HTTP 层不传任何约束，
    // 任意 Agent 都能读到别人的点对点消息。
    bool messageList(const std::string& recipientFilter, const std::string& kindFilter,
                     const std::string& statusFilter, const std::string& sinceIso, int limit,
                     std::vector<Message>& out, std::string& err, const std::string& viewer = {});
    bool messageGet(const std::string& uuid, Message& out, std::string& err);
    bool messageReply(const std::string& sender, const std::string& parentUuid,
                      const std::string& body, Message& out, std::string& err);
    bool messageSetStatus(const std::string& actor, const std::string& uuid,
                          const std::string& newStatus, Message& out, std::string& err);

    // ---- 错误日志 ----
    bool errorReport(const std::string& reporter, const std::string& severity,
                     const std::string& source, const std::string& title, const std::string& detail,
                     const std::string& stackTrace, ErrorReport& out, std::string& err);
    bool errorList(const std::string& statusFilter, const std::string& severityFilter, int limit,
                   std::vector<ErrorReport>& out, std::string& err);
    bool errorResolve(const std::string& actor, const std::string& uuid, const std::string& notes,
                      ErrorReport& out, std::string& err);

    // ---- Token 用量 ----
    // 纯观测：只统计与分级告警，不因超预算拒绝调用（历史注释曾写"超过即拒绝"，已废弃）
    bool usageReport(const std::string& agent, int64_t tokensIn, int64_t tokensOut,
                     const std::string& callType, const std::string& model,
                     const std::string& referenceId, const std::string& idempotencyKey,
                     bool& duplicate, std::string& err);
    bool usageSummary(UsageSummary& out, std::string& err);
    bool usageDaily(int days, std::vector<UsageDailyPoint>& out, std::string& err);
    bool usageByModel(std::vector<UsageModelRow>& out, std::string& err);
    bool usageBreakdown(int days, const std::string& agent, const std::string& model,
                        UsageBreakdown& out, std::string& err);
    int64_t usageBudget(std::string& err);
    bool usageSetBudget(const std::string& actor, int64_t budget, std::string& err);

    // ---- 操作日志 ----
    bool auditList(const std::string& actorFilter, const std::string& actionFilter,
                   const std::string& sinceIso, int limit, std::vector<AuditRecord>& out,
                   std::string& err);

    // ---- 运维：维护 / 备份恢复 ----
    // 审计轮转（30 天或 10 万条）+ 已解决错误归档清理；有删除时 VACUUM。
    // 返回统计 JSON（deleted_audit/deleted_errors/vacuumed）。
    bool maintenanceRun(const std::string& actor, std::string& statsJson, std::string& err);
    // 备份：VACUUM INTO 到 home/backup/platform-<时间戳>.db（WAL 一致性快照）
    bool backupCreate(std::string& outPath, std::string& err);
    bool backupList(std::vector<std::string>& out, std::string& err);
    // 恢复：name 为 backupList 中的文件名（拒绝路径分隔符）；恢复后重开数据库
    bool backupRestore(const std::string& name, std::string& err);

    // ---- HTTP 服务（仅绑定 127.0.0.1）----
    bool startHttpServer(int port, std::string& err);
    void stopHttpServer();
    int httpPort() const;
    bool httpRunning() const;

    // ---- 健康自检（防呆）：总览页健康横幅与 GET /api/diagnostics 共用 ----
    // 只读探测：数据目录可写、数据库可读且核心表存在、agents.json 可读、
    // HTTP 服务状态、已注册但明文密钥缺失的 Agent 清单。不改变任何状态。
    Diagnostics diagnostics();

    // ---- 向量工具 ----
    std::vector<float> embedText(const std::string& text);
    int embeddingDim() const;

private:
    bool persistAgentKey(const std::string& name, const std::string& apiKey, std::string& err);
    std::vector<float> resolveEmbedding(const std::string& content,
                                        const std::vector<float>* provided, bool& isProvided);
    // 维度↔provider 绑定校验（同维度共用 vec 表，不同模型会互相污染）；
    // 调用方须持有 mutex_
    bool bindEmbeddingProvider(size_t dim, const std::string& provider, std::string& err);

    std::string home_dir_;
    Database db_;
    std::unique_ptr<Embedder> embedder_;
    AgentService agents_;
    KnowledgeService knowledge_;
    SkillService skills_;
    MemoryService memory_;
    MessageService messages_;
    ErrorService errors_;
    UsageService usage_;
    AuditService audit_;
    std::unique_ptr<HttpServer> http_;
    std::string master_key_hash_;
    bool bootstrapped_ = false;
    mutable std::recursive_mutex mutex_;
};

}  // namespace ah
