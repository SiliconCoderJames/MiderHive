#pragma once
// 共享知识库：向量化语义搜索 + 关键词搜索；内容只追加、以版本演进。
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/embed/embedder.h"
#include "core/types.h"

namespace ah {

// 允许 Agent 自带向量的维度范围。内置嵌入器固定 384；上限防滥用
// （vec0 每行固定 dim*sizeof(float)，无上限会被写成内存放大面）。
// 64 下限挡住"随手传个 3 维数组"。主流模型全在范围内：bge/m3e=1024、
// text-embedding-3-small=1536、OpenAI large=3072。
inline bool isValidEmbeddingDim(int dim) { return dim >= 64 && dim <= 4096; }

class KnowledgeService {
public:
    KnowledgeService(Database& db, Embedder& embedder) : db_(db), embedder_(embedder) {}

    // 启动初始化（向量侧）：旧库的单张 knowledge_vec 迁移到按维度分表，
    // 并保证内置维度的表存在。幂等，可重复调用。
    bool initVectorStore(std::string& err);

    // 启动初始化（检索侧）：存量库的 FTS 索引一次性 rebuild。幂等。
    bool initSearchIndex(std::string& err);

    bool create(const std::string& author, const std::string& title, const std::string& content,
                const std::string& tagsJson, const std::string& category,
                const std::vector<float>& embedding, const std::string& embeddingProvider,
                KnowledgeEntry& out, std::string& err);
    bool latest(const std::string& uuid, KnowledgeEntry& out, std::string& err);
    bool versions(const std::string& uuid, std::vector<KnowledgeEntry>& out, std::string& err);
    bool addVersion(const std::string& author, const std::string& uuid, const std::string& newTitle,
                    const std::string& newContent, const std::vector<float>& embedding,
                    const std::string& embeddingProvider, KnowledgeEntry& out, std::string& err);
    bool list(int limit, const std::string& tagFilter, std::vector<KnowledgeEntry>& out, std::string& err);
    bool searchKeyword(const std::string& query, int limit, const std::string& tagFilter,
                       std::vector<KnowledgeEntry>& out, std::string& err);
    bool searchSemantic(const std::vector<float>& queryVec, int limit, const std::string& tagFilter,
                        std::vector<KnowledgeHit>& out, std::string& err);

    // 删除某条目（全部版本）在所有维度表里的向量行。必须在写事务内调用，
    // 与删正文同生共死（否则留下"有向量无正文"的孤儿，挤占 kNN 召回名额）。
    bool deleteVecsForUuid(const std::string& uuid, std::string& err);

private:
    // vec0 每张表维度固定，因此**按维度分表**：knowledge_vec_d<dim>。
    // 这样 Agent 才能自带 1024/1536 维向量（否则 384 硬约束会把它们全部挡在门外），
    // 检索时按查询向量长度选表，维度天然隔离、互不污染。
    std::string vecTableFor(size_t dim) const;
    bool ensureVecTableFor(size_t dim, std::string& err);
    bool vecTableExists(size_t dim, bool& exists, std::string& err);
    // 现有维度表名（只认 CREATE VIRTUAL TABLE 的条目，避开 vec0 的 *_info/*_chunks 影子表）。
    // 失败通过**返回值**表达：err 是调用方复用的字符串，成功时不会被清空，
    // 不能用"err 非空"判断失败（仓库约定：err 仅在返回 false 时有意义）。
    bool vecTableNames(std::vector<std::string>& out, std::string& err);
    // "维度对不上"时的可自查提示：列出本库现有维度与该维度的 provider 标签，
    // 让调用方立刻看出"用 1024 写、用 1536 查"这类错配（而不是拿到一个空数组猜原因）
    std::string existingDimsHint();
    // 单次 kNN 查询（vec0：k 与 LIMIT 不能同时出现；k 隐含按 distance 升序）
    bool knnHits(size_t dim, const std::vector<float>& queryVec, int k,
                 std::vector<std::pair<int64_t, double>>& hits, std::string& err);
    bool insertVec(int64_t entryId, const std::vector<float>& vec, std::string& err);
    // 一次 IN (...) 查询取回全部条目（分批防触 SQLITE_MAX_VARIABLE_NUMBER），
    // 保持入参顺序。替换掉此前的逐行查询（N+1）。
    bool fetchByIds(const std::vector<int64_t>& ids, std::vector<KnowledgeEntry>& out, std::string& err);
    bool searchKeywordLike(const std::string& query, int limit, const std::string& tagFilter,
                           std::vector<KnowledgeEntry>& out, std::string& err);
    bool searchKeywordFts(const std::string& query, int limit, const std::string& tagFilter,
                          std::vector<KnowledgeEntry>& out, std::string& err);

    Database& db_;
    Embedder& embedder_;
};

}  // namespace ah
