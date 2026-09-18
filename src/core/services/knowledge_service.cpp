#include "core/services/knowledge_service.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <nlohmann/json.hpp>

#include "core/util.h"

namespace ah {

namespace {
std::string vecToBytes(const std::vector<float>& v) {
    std::string bytes(v.size() * sizeof(float), '\0');
    std::memcpy(bytes.data(), v.data(), bytes.size());
    return bytes;
}
// tags_json -> tags 数组（tags_json 始终是合法 JSON，异常时静默保留空列表）
void parseTags(KnowledgeEntry& e) {
    try {
        auto tags = nlohmann::json::parse(e.tags_json);
        if (tags.is_array())
            for (const auto& t : tags) e.tags.push_back(t.get<std::string>());
    } catch (...) {
    }
}
}  // namespace

std::string KnowledgeService::vecTableFor(size_t dim) const {
    return "knowledge_vec_d" + std::to_string(dim);
}

bool KnowledgeService::vecTableExists(size_t dim, bool& exists, std::string& err) {
    exists = false;
    return db_.query(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
        [&](Stmt& st) { st.bind(1, vecTableFor(dim)); },
        [&](Stmt&) { exists = true; }, err);
}

bool KnowledgeService::ensureVecTableFor(size_t dim, std::string& err) {
    // 维度是内部整数（来自向量长度），不构成注入面
    const std::string ddl = "CREATE VIRTUAL TABLE IF NOT EXISTS " + vecTableFor(dim) +
                            " USING vec0(entry_id INTEGER PRIMARY KEY, embedding float[" +
                            std::to_string(dim) + "])";
    return db_.execScript(ddl, err);
}

bool KnowledgeService::initVectorStore(std::string& err) {
    // 旧库迁移：老版本只有一张固定维度的 knowledge_vec；现在按维度分表。
    // vec0 虚拟表不支持 ALTER/RENAME，只能"新建-复制-删除"。
    // 幂等且并发安全（工作台与 platformd 可能同时首启）：
    //   * INSERT 带 NOT IN 守卫——两边都复制也不会重复；
    //   * DROP 用 IF EXISTS——对方先删了也不再报错；
    //   * 复制后旧表随即删除，后续启动 hasLegacy 恒为 false。
    bool hasLegacy = false;
    if (!db_.query("SELECT 1 FROM sqlite_master WHERE type='table' AND name='knowledge_vec'",
                   nullptr,
                   [&](Stmt&) { hasLegacy = true; }, err))
        return false;
    if (hasLegacy) {
        const size_t builtinDim = static_cast<size_t>(embedder_.dim());
        if (!ensureVecTableFor(builtinDim, err)) return false;
        if (!db_.execScript("INSERT INTO " + vecTableFor(builtinDim) +
                                "(entry_id, embedding) SELECT entry_id, embedding FROM knowledge_vec "
                                "WHERE entry_id NOT IN (SELECT entry_id FROM " +
                                vecTableFor(builtinDim) + ")",
                            err))
            return false;
        if (!db_.execScript("DROP TABLE IF EXISTS knowledge_vec", err)) return false;
    }
    return ensureVecTableFor(static_cast<size_t>(embedder_.dim()), err);
}

bool KnowledgeService::initSearchIndex(std::string& err) {
    // 用 settings 里的索引版本标记判断要不要重建全文索引。
    //   * 旧库（升级前）：既没有 knowledge_fts 表也没有标记 → 这里一次性 rebuild 并打标记；
    //   * 之后：标记命中直接返回，启动零成本。
    //   * 需要强制重建时（例如手工删过索引表），删掉这行标记即可：
    //       DELETE FROM settings WHERE key='fts_index_version';
    //
    // **不能**用 SELECT COUNT(*) FROM knowledge_fts 判断：外部内容表的无 MATCH 查询会被
    // FTS5 转发到正文表，空索引也照样返回正文行数（实测），据此判断必然漏掉重建。
    const char* kKey = "fts_index_version";
    // '2'：索引改为只覆盖最新版本（旧版索引了全部历史版本）。
    // 递增即强制存量库重建一次，把旧语义的索引冲掉。
    const char* kVersion = "2";
    std::string cur;
    bool found = false;
    if (!db_.query("SELECT value FROM settings WHERE key=?",
                   [&](Stmt& st) { st.bind(1, std::string(kKey)); },
                   [&](Stmt& st) {
                       cur = st.text(0);
                       found = true;
                   },
                   err))
        return false;
    if (found && cur == kVersion) return true;

    // 重建：**不能**用 FTS5 的 'rebuild'——它直接读内容表，会把全部历史版本也灌进索引，
    // 与"只索引最新版本"的新语义矛盾（且未入索引的行再被删除时会留下死条目）。
    // 改为 delete-all 清空 + 只插 is_latest=1 的行。
    if (!db_.execScript("INSERT INTO knowledge_fts(knowledge_fts) VALUES('delete-all')", err))
        return false;
    if (!db_.execScript("INSERT INTO knowledge_fts(rowid, title, content) "
                        "SELECT id, title, content FROM knowledge_entries WHERE is_latest=1",
                        err))
        return false;
    if (!db_.query("INSERT INTO settings(key, value) VALUES(?,?) "
                   "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                   [&](Stmt& st) {
                       st.bind(1, std::string(kKey));
                       st.bind(2, std::string(kVersion));
                   },
                   nullptr, err))
        return false;
    return true;
}

bool KnowledgeService::create(const std::string& author, const std::string& title,
                              const std::string& content, const std::string& tagsJson,
                              const std::string& category, const std::vector<float>& embedding,
                              const std::string& embeddingProvider, KnowledgeEntry& out,
                              std::string& err) {
    std::string uuid = uuid4();
    const size_t dim = embedding.empty() ? static_cast<size_t>(embedder_.dim()) : embedding.size();
    // 建表放在事务外：幂等 DDL，不必混进写事务（也避免 vec0 在事务内建表的未知行为）
    if (!ensureVecTableFor(dim, err)) return false;
    // 事务包裹正文与向量两步写入：向量一步失败时先回滚再返回失败，
    // 否则会留下 is_latest=1 但无向量的孤儿记录——列表与关键词检索都能看到它，
    // 语义检索却永远命中不了，且没有任何报错（范式与 addVersion 一致）
    if (!db_.beginImmediate(err)) return false;
    if (!db_.query(
            "INSERT INTO knowledge_entries(uuid, title, content, tags_json, category, author, version, "
            "parent_version_id, is_latest, embedding_provider, embedding_dim, created_at) "
            "VALUES (?,?,?,?,?,?,1,NULL,1,?,?,?)",
            [&](Stmt& st) {
                st.bind(1, uuid);
                st.bind(2, title);
                st.bind(3, content);
                st.bind(4, tagsJson);
                st.bind(5, category);
                st.bind(6, author);
                st.bind(7, embeddingProvider);
                st.bind(8, static_cast<int64_t>(dim));
                st.bind(9, nowIso());
            },
            nullptr, err)) {
        db_.rollback();
        return false;
    }
    int64_t id = db_.lastInsertId();
    if (!insertVec(id, embedding, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    // 提交后再读回：读回失败只代表本次调用失败，已提交的数据保持一致
    return latest(uuid, out, err);
}

bool KnowledgeService::vecTableNames(std::vector<std::string>& out, std::string& err) {
    out.clear();
    // sqlite_master 里 vec0 还会登记 *_info / *_chunks 等影子表，只认虚表本身
    return db_.query(
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'knowledge_vec_d%' "
        "AND sql LIKE 'CREATE VIRTUAL TABLE%'",
        nullptr, [&](Stmt& st) { out.push_back(st.text(0)); }, err);
}

std::string KnowledgeService::existingDimsHint() {
    // 表名形如 knowledge_vec_d<dim>：前缀 + 纯数字才认（与 deleteVecsForUuid 同一白名单）
    static constexpr const char* kPrefix = "knowledge_vec_d";
    const size_t kPrefixLen = std::strlen(kPrefix);
    std::vector<std::string> names;
    std::string ignored;  // 提示是尽力而为：查不到就返回空提示，不改变主流程
    if (!vecTableNames(names, ignored)) return "";
    if (names.empty()) return " (this hive has no embeddings yet)";

    std::string hint = " (existing: ";
    bool first = true;
    for (const auto& n : names) {
        if (n.size() <= kPrefixLen) continue;
        const std::string dimStr = n.substr(kPrefixLen);
        if (!std::all_of(dimStr.begin(), dimStr.end(),
                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            continue;
        // 该维度最新一条的 provider 标签；查不到就标 unknown（老数据可能为空串）
        std::string provider = "unknown", perr;
        db_.query("SELECT embedding_provider FROM knowledge_entries WHERE id IN "
                  "(SELECT entry_id FROM " + n + ") ORDER BY id DESC LIMIT 1",
                  nullptr,
                  [&](Stmt& st) {
                      if (!st.isNull(0) && !st.text(0).empty()) provider = st.text(0);
                  },
                  perr);
        if (!first) hint += ", ";
        hint += dimStr + " [" + provider + "]";
        first = false;
    }
    hint += ")";
    return first ? std::string(" (this hive has no embeddings yet)") : hint;
}

bool KnowledgeService::knnHits(size_t dim, const std::vector<float>& queryVec, int k,
                               std::vector<std::pair<int64_t, double>>& hits, std::string& err) {
    hits.clear();
    const std::string bytes = vecToBytes(queryVec);
    return db_.query(
        "SELECT entry_id, distance FROM " + vecTableFor(dim) +
            " WHERE embedding MATCH ? AND k = ?",
        [&](Stmt& st) {
            st.bindBlob(1, bytes.data(), bytes.size());
            st.bind(2, static_cast<int64_t>(k));
        },
        [&](Stmt& st) { hits.emplace_back(st.i64(0), st.dbl(1)); }, err);
}

bool KnowledgeService::insertVec(int64_t entryId, const std::vector<float>& vec, std::string& err) {
    std::string bytes = vecToBytes(vec);
    return db_.query("INSERT INTO " + vecTableFor(vec.size()) + "(entry_id, embedding) VALUES (?,?)",
                     [&](Stmt& st) {
                         st.bind(1, entryId);
                         st.bindBlob(2, bytes.data(), bytes.size());
                     },
                     nullptr, err);
}

bool KnowledgeService::latest(const std::string& uuid, KnowledgeEntry& out, std::string& err) {
    bool found = false;
    bool ok = db_.query(
        "SELECT id, uuid, title, content, tags_json, category, author, version, embedding_provider, created_at "
        "FROM knowledge_entries WHERE uuid=? AND is_latest=1",
        [&](Stmt& st) { st.bind(1, uuid); },
        [&](Stmt& st) {
            out.id = st.i64(0);
            out.uuid = st.text(1);
            out.title = st.text(2);
            out.content = st.text(3);
            out.tags_json = st.text(4);
            out.category = st.isNull(5) ? std::string() : st.text(5);
            out.author = st.text(6);
            out.version = static_cast<int>(st.i64(7));
            out.embedding_provider = st.isNull(8) ? std::string() : st.text(8);
            out.created_at = st.text(9);
            found = true;
        },
        err);
    if (!ok) return false;
    if (!found) { err = "knowledge not found: " + uuid; return false; }
    try {
        auto tags = nlohmann::json::parse(out.tags_json);
        if (tags.is_array())
            for (const auto& t : tags) out.tags.push_back(t.get<std::string>());
    } catch (...) {
    }
    return true;
}

bool KnowledgeService::versions(const std::string& uuid, std::vector<KnowledgeEntry>& out,
                                std::string& err) {
    out.clear();
    return db_.query(
        "SELECT id, uuid, title, content, tags_json, category, author, version, embedding_provider, created_at "
        "FROM knowledge_entries WHERE uuid=? ORDER BY version DESC",
        [&](Stmt& st) { st.bind(1, uuid); },
        [&](Stmt& st) {
            KnowledgeEntry e;
            e.id = st.i64(0);
            e.uuid = st.text(1);
            e.title = st.text(2);
            e.content = st.text(3);
            e.tags_json = st.text(4);
            e.category = st.isNull(5) ? std::string() : st.text(5);
            e.author = st.text(6);
            e.version = static_cast<int>(st.i64(7));
            e.embedding_provider = st.isNull(8) ? std::string() : st.text(8);
            e.created_at = st.text(9);
            out.push_back(std::move(e));
        },
        err);
}

bool KnowledgeService::addVersion(const std::string& author, const std::string& uuid,
                                  const std::string& newTitle, const std::string& newContent,
                                  const std::vector<float>& embedding,
                                  const std::string& embeddingProvider, KnowledgeEntry& out,
                                  std::string& err) {
    const size_t dim = embedding.empty() ? static_cast<size_t>(embedder_.dim()) : embedding.size();
    if (!ensureVecTableFor(dim, err)) return false;  // 事务外建表（幂等），理由同 create()

    // BEGIN IMMEDIATE 包裹查-改-插：双进程并发追加同一 uuid 时不会产生两条 is_latest=1
    if (!db_.beginImmediate(err)) return false;

    // 追加新版本：旧版本内容原样保留，仅翻转 is_latest 标记
    int64_t oldId = 0;
    int oldVersion = 0;
    std::string oldTitle, oldTags, oldCategory;
    bool found = false;
    if (!db_.query(
            "SELECT id, version, title, tags_json, category FROM knowledge_entries WHERE uuid=? AND is_latest=1",
            [&](Stmt& st) { st.bind(1, uuid); },
            [&](Stmt& st) {
                oldId = st.i64(0);
                oldVersion = static_cast<int>(st.i64(1));
                oldTitle = st.text(2);
                oldTags = st.text(3);
                oldCategory = st.isNull(4) ? std::string() : st.text(4);
                found = true;
            },
            err)) {
        db_.rollback();
        return false;
    }
    if (!found) {
        err = "knowledge not found: " + uuid;
        db_.rollback();
        return false;
    }

    if (!db_.query("UPDATE knowledge_entries SET is_latest=0 WHERE id=?",
                   [&](Stmt& st) { st.bind(1, oldId); }, nullptr, err)) {
        db_.rollback();
        return false;
    }

    std::string title = newTitle.empty() ? oldTitle : newTitle;
    if (!db_.query(
            "INSERT INTO knowledge_entries(uuid, title, content, tags_json, category, author, version, "
            "parent_version_id, is_latest, embedding_provider, embedding_dim, created_at) "
            "VALUES (?,?,?,?,?,?,?,?,1,?,?,?)",
            [&](Stmt& st) {
                st.bind(1, uuid);
                st.bind(2, title);
                st.bind(3, newContent);
                st.bind(4, oldTags);
                st.bind(5, oldCategory);
                st.bind(6, author);
                st.bind(7, static_cast<int64_t>(oldVersion + 1));
                st.bind(8, oldId);
                st.bind(9, embeddingProvider);
                st.bind(10, static_cast<int64_t>(dim));
                st.bind(11, nowIso());
            },
            nullptr, err)) {
        db_.rollback();
        return false;
    }
    int64_t id = db_.lastInsertId();
    if (!insertVec(id, embedding, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    return latest(uuid, out, err);
}

bool KnowledgeService::backfillEmbeddingDim(std::string& err) {
    // 旧行没有维度记录：按"向量实际所在的表"回填（每张维度表一条 UPDATE）。
    // embedding_dim IS NULL 守卫保证幂等——跑过一次后就是空操作。
    std::vector<std::string> tables;
    if (!vecTableNames(tables, err)) return false;
    static constexpr const char* kPrefix = "knowledge_vec_d";
    for (const auto& t : tables) {
        const std::string dimStr = t.substr(std::strlen(kPrefix));
        const bool numeric = !dimStr.empty() &&
                             std::all_of(dimStr.begin(), dimStr.end(), [](char c) {
                                 return std::isdigit(static_cast<unsigned char>(c));
                             });
        if (!numeric) {
            err = "unexpected vector table name: " + t;
            return false;
        }
        if (!db_.execScript("UPDATE knowledge_entries SET embedding_dim=" + dimStr +
                                " WHERE embedding_dim IS NULL AND id IN "
                                "(SELECT entry_id FROM " + t + ")",
                            err))
            return false;
    }
    return true;
}

bool KnowledgeService::deleteVecsForUuid(const std::string& uuid, std::string& err) {
    // 精确删除：条目自 v2 起记录自己的维度（旧行由 backfillEmbeddingDim 回填）。
    // 但**同一 uuid 的不同版本可能落在不同维度表**（v1 用内置 384、v2 用自带 1024），
    // 所以只有"所有版本都记了维度且维度唯一"才能只清那一张表；否则退回遍历全部维度表
    // ——漏清会留下"有向量无正文"的孤儿，挤占 kNN 召回名额（有专门的不变式测试把守）。
    int64_t total = 0, recorded = 0, minDim = 0, maxDim = 0;
    if (!db_.query("SELECT COUNT(*), COUNT(embedding_dim), COALESCE(MIN(embedding_dim),0), "
                   "COALESCE(MAX(embedding_dim),0) FROM knowledge_entries WHERE uuid=?",
                   [&](Stmt& st) { st.bind(1, uuid); },
                   [&](Stmt& st) {
                       total = st.i64(0);
                       recorded = st.i64(1);
                       minDim = st.i64(2);
                       maxDim = st.i64(3);
                   },
                   err))
        return false;
    if (total > 0 && recorded == total && minDim > 0 && minDim == maxDim) {
        return db_.query("DELETE FROM " + vecTableFor(static_cast<size_t>(minDim)) +
                             " WHERE entry_id IN (SELECT id FROM knowledge_entries WHERE uuid=?)",
                         [&](Stmt& st) { st.bind(1, uuid); }, nullptr, err);
    }

    // 退回遍历（含未回填的旧行 / 跨维度的版本链）
    std::vector<std::string> tables;
    if (!vecTableNames(tables, err)) return false;    static constexpr const char* kPrefix = "knowledge_vec_d";
    for (const auto& t : tables) {
        const bool numeric = t.size() > std::strlen(kPrefix) &&
                             std::all_of(t.begin() + std::strlen(kPrefix), t.end(),
                                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (!numeric) {
            err = "unexpected vector table name: " + t;
            return false;
        }
        if (!db_.query("DELETE FROM " + t + " WHERE entry_id IN "
                       "(SELECT id FROM knowledge_entries WHERE uuid=?)",
                       [&](Stmt& st) { st.bind(1, uuid); }, nullptr, err))
            return false;
    }
    return true;
}

bool KnowledgeService::fetchByIds(const std::vector<int64_t>& ids, std::vector<KnowledgeEntry>& out,
                                  std::string& err) {
    out.clear();
    // 分批 IN 查询：变量数按保守上限切块；结果顺序靠"按块顺序拼回"保持
    // （kNN 的距离序 / 列表的时间序都不能乱）。替换掉此前的逐行查询（N+1）。
    constexpr size_t kChunk = 500;
    for (size_t begin = 0; begin < ids.size(); begin += kChunk) {
        const size_t end = std::min(ids.size(), begin + kChunk);
        std::string sql =
            "SELECT id, uuid, title, content, tags_json, category, author, version, "
            "embedding_provider, created_at FROM knowledge_entries WHERE is_latest=1 AND id IN (";
        for (size_t i = begin; i < end; ++i) sql += (i == begin ? "?" : ",?");
        sql += ") ORDER BY id DESC";
        std::map<int64_t, KnowledgeEntry> got;
        if (!db_.query(
                sql,
                [&](Stmt& st) {
                    for (size_t i = begin; i < end; ++i)
                        st.bind(static_cast<int>(i - begin + 1), ids[i]);
                },
                [&](Stmt& st) {
                    KnowledgeEntry e;
                    e.id = st.i64(0);
                    e.uuid = st.text(1);
                    e.title = st.text(2);
                    e.content = st.text(3);
                    e.tags_json = st.text(4);
                    e.category = st.isNull(5) ? std::string() : st.text(5);
                    e.author = st.text(6);
                    e.version = static_cast<int>(st.i64(7));
                    e.embedding_provider = st.isNull(8) ? std::string() : st.text(8);
                    e.created_at = st.text(9);
                    parseTags(e);
                    got.emplace(e.id, std::move(e));
                },
                err))
            return false;
        for (size_t i = begin; i < end; ++i) {
            auto it = got.find(ids[i]);
            if (it != got.end()) out.push_back(std::move(it->second));
        }
    }
    return true;
}

bool KnowledgeService::list(int limit, const std::string& tagFilter,
                            std::vector<KnowledgeEntry>& out, std::string& err) {
    std::string sql =
        "SELECT id FROM knowledge_entries WHERE is_latest=1";
    if (!tagFilter.empty()) sql += " AND tags_json LIKE ?";
    sql += " ORDER BY id DESC LIMIT ?";
    std::vector<int64_t> ids;
    if (!db_.query(
            sql,
            [&](Stmt& st) {
                int idx = 1;
                if (!tagFilter.empty()) st.bind(idx++, "%\"" + tagFilter + "\"%");
                st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 100));
            },
            [&](Stmt& st) { ids.push_back(st.i64(0)); }, err))
        return false;
    return fetchByIds(ids, out, err);
}

bool KnowledgeService::searchKeyword(const std::string& query, int limit,
                                     const std::string& tagFilter,
                                     std::vector<KnowledgeEntry>& out, std::string& err) {
    // trigram 分词最少 3 个码点；更短的查询（中文两字词很常见）回退 LIKE，保证不漏
    if (static_cast<int>(utf8Codepoints(query).size()) >= 3)
        return searchKeywordFts(query, limit, tagFilter, out, err);
    return searchKeywordLike(query, limit, tagFilter, out, err);
}

// 老路径：LIKE 子串扫描。仅用于 <3 码点的短查询（trigram 无法切词）。
bool KnowledgeService::searchKeywordLike(const std::string& query, int limit,
                                         const std::string& tagFilter,
                                         std::vector<KnowledgeEntry>& out, std::string& err) {
    std::string like = "%" + query + "%";
    std::string sql =
        "SELECT id FROM knowledge_entries WHERE is_latest=1 AND (title LIKE ? OR content LIKE ?)";
    if (!tagFilter.empty()) sql += " AND tags_json LIKE ?";
    sql += " ORDER BY id DESC LIMIT ?";
    std::vector<int64_t> ids;
    if (!db_.query(
            sql,
            [&](Stmt& st) {
                st.bind(1, like);
                st.bind(2, like);
                int idx = 3;
                if (!tagFilter.empty()) st.bind(idx++, "%\"" + tagFilter + "\"%");
                st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 20));
            },
            [&](Stmt& st) { ids.push_back(st.i64(0)); }, err))
        return false;
    return fetchByIds(ids, out, err);
}

// 主路径：FTS5 trigram 全文索引（外部内容表，正文不重复存储）。
// 用户查询包成**短语**并转义内部引号：既避免 "-" "(" 等被当成 FTS5 查询语法，
// 在 trigram 下又等价于子串语义；大小写不敏感与 LIKE 行为一致。
bool KnowledgeService::searchKeywordFts(const std::string& query, int limit,
                                        const std::string& tagFilter,
                                        std::vector<KnowledgeEntry>& out, std::string& err) {
    std::string escaped;
    for (char c : query) {
        escaped += c;
        if (c == '"') escaped += '"';
    }
    const std::string ftsQuery = "\"" + escaped + "\"";
    std::string sql =
        "SELECT ke.id FROM knowledge_entries ke WHERE ke.is_latest=1 AND ke.id IN "
        "(SELECT rowid FROM knowledge_fts WHERE knowledge_fts MATCH ?)";
    if (!tagFilter.empty()) sql += " AND ke.tags_json LIKE ?";
    sql += " ORDER BY ke.id DESC LIMIT ?";
    std::vector<int64_t> ids;
    if (!db_.query(
            sql,
            [&](Stmt& st) {
                st.bind(1, ftsQuery);
                int idx = 2;
                if (!tagFilter.empty()) st.bind(idx++, "%\"" + tagFilter + "\"%");
                st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 20));
            },
            [&](Stmt& st) { ids.push_back(st.i64(0)); }, err))
        return false;
    return fetchByIds(ids, out, err);
}

bool KnowledgeService::searchSemantic(const std::vector<float>& queryVec, int limit,
                                      const std::string& tagFilter, std::vector<KnowledgeHit>& out,
                                      std::string& err) {
    out.clear();
    if (queryVec.empty()) {
        err = "empty query vector";
        return false;
    }
    const size_t dim = queryVec.size();
    bool exists = false;
    if (!vecTableExists(dim, exists, err)) return false;
    if (!exists) {
        // 明确报错而不是静默返回空：维度错配（用 1024 写、用 1536 查）是最容易发生、
        // 又最难看出的用法错误，一条"现有维度 + provider"提示就能自证。
        err = "no entries with " + std::to_string(dim) + "-dim embeddings" + existingDimsHint();
        return false;
    }

    const int base = limit > 0 ? limit : 20;
    // 单次召回的硬上限：梯度扩 k 直到凑够 limit 或该维度取尽（hits.size() < k），
    // 这个上限只防病态内存放大。5 万行 × 16B ≈ 800KB 峰值，而表行数本身天然封顶；
    // 设得偏低会让"标签稀疏的大库"系统性少给结果，因此取一个远大于常见库规模的值。
    constexpr int kMax = 50000;

    // vec0 不支持在 MATCH 里做标量过滤，带 tag 时只能"多召回再过滤"。
    // 单次固定倍数（原实现 limit×4）在"未打标签的条目向量更近"时会系统性少给，
    // 因此按梯度扩 k 重查，直到：凑够 base 条 / 本次召回断档（行数 < k，该维度已取尽）/ 到上限。
    int k = tagFilter.empty() ? base : std::min(base * 4, 200);
    std::vector<std::pair<int64_t, double>> hits;
    std::vector<KnowledgeEntry> entries;
    for (;;) {
        if (!knnHits(dim, queryVec, k, hits, err)) return false;
        std::vector<int64_t> ids;
        ids.reserve(hits.size());
        for (const auto& h : hits) ids.push_back(h.first);
        if (!fetchByIds(ids, entries, err)) return false;  // 顺序 = kNN 距离序

        if (tagFilter.empty() || k >= kMax) break;
        int tagged = 0;
        for (const auto& e : entries)
            if (e.tags_json.find("\"" + tagFilter + "\"") != std::string::npos) ++tagged;
        if (tagged >= base) break;
        if (static_cast<int>(hits.size()) < k) break;  // 已取尽，再扩 k 也没用
        k = std::min(k * 4, kMax);
    }

    // 按距离序组装：hits 已按 distance 升序，用 id→entry 查表同时拿到正文与距离
    std::map<int64_t, KnowledgeEntry> byId;
    for (auto& e : entries) byId.emplace(e.id, std::move(e));
    int kept = 0;
    for (const auto& h : hits) {
        if (limit > 0 && kept >= limit) break;
        auto it = byId.find(h.first);
        if (it == byId.end()) continue;  // 非最新版本（fetchByIds 只取 is_latest=1）
        if (!tagFilter.empty() &&
            it->second.tags_json.find("\"" + tagFilter + "\"") == std::string::npos)
            continue;
        out.push_back({std::move(it->second), h.second});
        ++kept;
    }
    return true;
}

}  // namespace ah
