#include "core/platform.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <sqlite-vec.h>

#include "core/http/server.h"
#include "core/util.h"
#include "schema_sql.h"

namespace ah {

namespace fs = std::filesystem;

std::string defaultHomeDir() {
    if (auto env = envOr({"MIDERHIVE_HOME", "AGENTHIVE_HOME", "ZCODE_PLATFORM_HOME"}); !env.empty())
        return env;
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        std::string home = std::string(up) + "\\.miderhive";
        // 旧品牌数据目录平滑迁移：一次性改名，保留全部数据
        // （.agenthive 为上一代品牌，.zcode-platform 为更早的内部代号）
        std::error_code ec;
        if (!fs::exists(home)) {
            for (const char* legacyName : {".agenthive", ".zcode-platform"}) {
                std::string legacy = std::string(up) + "\\" + legacyName;
                if (fs::exists(legacy)) {
                    fs::rename(legacy, home, ec);
                    return home;
                }
            }
        }
        return home;
    }
#endif
    if (const char* home = std::getenv("HOME"); home && *home) {
        std::string homeDir = std::string(home) + "/.miderhive";
        std::error_code ec;
        if (!fs::exists(homeDir)) {
            for (const char* legacyName : {".agenthive", ".zcode-platform"}) {
                std::string legacy = std::string(home) + "/" + legacyName;
                if (fs::exists(legacy)) {
                    fs::rename(legacy, homeDir, ec);
                    return homeDir;
                }
            }
        }
        return homeDir;
    }
    return ".miderhive";
}

Platform::Platform(std::string homeDir)
    : home_dir_(std::move(homeDir)),
      embedder_(std::make_unique<NgramHashEmbedder>()),
      agents_(db_),
      knowledge_(db_, *embedder_, embedder_->dim()),
      skills_(db_),
      memory_(db_),
      messages_(db_),
      errors_(db_),
      usage_(db_),
      audit_(db_) {}

Platform::~Platform() { shutdown(); }

bool Platform::bootstrap(std::string& err) {
    std::lock_guard lock(mutex_);
    if (bootstrapped_) return true;

    std::error_code ec;
    fs::create_directories(fs::path(home_dir_) / "config", ec);
    fs::create_directories(fs::path(home_dir_) / "backup", ec);
    if (ec) { err = "cannot create home dir: " + ec.message(); return false; }

    std::string dbPath = (fs::path(home_dir_) / "platform.db").string();
    if (!db_.open(dbPath, err)) return false;
    if (!db_.execScript(kSchemaSql, err)) return false;
    // 存量库迁移：新增列（已存在则静默跳过）
    db_.tryExec("ALTER TABLE agents ADD COLUMN salt TEXT NOT NULL DEFAULT '';");
    db_.tryExec("ALTER TABLE token_usage ADD COLUMN idempotency_key TEXT;");
    db_.tryExec("ALTER TABLE token_usage ADD COLUMN model TEXT NOT NULL DEFAULT '';");
    db_.tryExec("CREATE UNIQUE INDEX IF NOT EXISTS idx_usage_idem "
                "ON token_usage(idempotency_key) "
                "WHERE idempotency_key IS NOT NULL AND idempotency_key != '';");
    // 启用 sqlite-vec（静态链接进本进程）
    if (sqlite3_vec_init(db_.handle(), nullptr, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_.handle());
        return false;
    }
    if (!knowledge_.ensureVecTable(err)) return false;

    // 预算默认值（可被 settings 覆盖）
    if (!db_.query("INSERT OR IGNORE INTO settings(key, value) VALUES('weekly_token_budget', ?)",
                   [](Stmt& st) { st.bind(1, kDefaultWeeklyBudget); }, nullptr, err))
        return false;

    // 主密钥：环境变量优先，其次运行期生成的文件；绝不写入源码
    std::string masterKey;
    if (auto env = envOr({"MIDERHIVE_MASTER_KEY", "AGENTHIVE_MASTER_KEY",
                          "ZCODE_PLATFORM_MASTER_KEY"}); !env.empty()) {
        masterKey = env;
    } else {
        std::string keyPath = (fs::path(home_dir_) / "config" / "master.key").string();
        std::ifstream in(keyPath);
        if (in) { std::getline(in, masterKey); in.close(); }
        if (masterKey.empty()) {
            masterKey = randomHex(32);
            std::ofstream out(keyPath, std::ios::trunc);
            out << masterKey << "\n";
            out.close();
        }
    }
    master_key_hash_ = sha256Hex(masterKey);

    // 管理者账号由主密钥引导注册（幂等）
    if (!agents_.nameExists(kManagerName)) {
        std::string zcodeKey = randomHex(32);
        std::string zcodeSalt = randomHex(16);
        if (!agents_.registerAgent(kManagerName, kManagerName, zcodeSalt,
                                   sha256Hex(zcodeSalt + zcodeKey), err))
            return false;
        if (!persistAgentKey(kManagerName, zcodeKey, err)) return false;
        audit_.log("system", "agent.register", kManagerName,
                   nlohmann::json{{"role", "zcode"}}.dump(), err);
    }

    // 明文密钥缓存文件的可观测性：库中已有管理者行，但文件缺失/不含该条目，说明明文已
    // 不可恢复（库中只有加盐哈希）。必须显式记入审计——否则用户只看到"Agent 一直离线"
    // 却没有任何线索。恢复途径：重新生成密钥（/api/agents/rotate 或界面上的"重新生成密钥"）。
    {
        std::string keyPath = (fs::path(home_dir_) / "config" / "agents.json").string();
        bool managerKeyCached = false;
        {
            std::ifstream in(keyPath);
            if (in) {
                nlohmann::json j;
                in >> j;
                if (j.is_object() && j.contains(kManagerName) && j[kManagerName].is_string() &&
                    !j[kManagerName].get<std::string>().empty())
                    managerKeyCached = true;
            }
        }
        if (!managerKeyCached) {
            std::string auditErr;
            audit_.log("system", "system.keyfile_missing", kManagerName,
                       nlohmann::json{{"path", keyPath},
                                      {"recover", "POST /api/agents/rotate"},
                                      {"hint", "plaintext keys are not recoverable from the database"}}
                           .dump(),
                       auditErr);
        }
    }

    // 数据增长维护：审计轮转 + 已解决错误清理（每次启动执行）
    std::string stats;
    maintenanceRun("system", stats, err);

    bootstrapped_ = true;
    return true;
}

void Platform::shutdown() {
    // 顺序很关键：先取出并锁外停止 HTTP（join 在途请求，handler 需要拿
    // mutex_ 与可用的 db_）→ 再关闭数据库。持锁 join 会互相等待死锁。
    std::unique_ptr<HttpServer> http;
    {
        std::lock_guard lock(mutex_);
        http = std::move(http_);
    }
    if (http) http->stop();
    std::lock_guard lock(mutex_);
    db_.close();
    bootstrapped_ = false;
}

bool Platform::persistAgentKey(const std::string& name, const std::string& apiKey,
                               std::string& err) {
    // 密钥明文只存于此文件（数据库仅有加盐哈希，明文不可恢复），供 Agent 侧命令行取用。
    // 因此这里必须如实返回失败：此前结尾是 `return out.good() || true`——恒为 true，
    // 目录缺失/磁盘错误被静默吞掉，现场表现就是"注册过却永远离线"且毫无线索。
    std::string path = (fs::path(home_dir_) / "config" / "agents.json").string();
    std::error_code ec;
    fs::create_directories(fs::path(home_dir_) / "config", ec);
    nlohmann::json j;
    {
        std::ifstream in(path);
        if (in) {
            // 非抛出解析：文件损坏（半截/空/垃圾）时 operator>> 会抛异常，
            // 下面那道 fail 检查根本轮不到执行。这里转成如实的失败返回，
            // 绝不能当空对象继续走——否则合并写入会清掉其他 agent 的明文密钥。
            j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded()) { err = "agents.json is not valid JSON: " + path; return false; }
            in.close();
        }
    }
    if (!j.is_object()) j = nlohmann::json::object();
    if (apiKey.empty())
        j.erase(name);  // 空密钥 = 移除该条目（agentRemove 用）
    else
        j[name] = apiKey;
    // 先写临时文件再原子替换：中途失败不会留下半截文件（那等于再次丢失明文）
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { err = "cannot write " + tmp; return false; }
        out << j.dump(2) << "\n";
        out.flush();
        if (!out.good()) { err = "write failed: " + tmp; out.close(); return false; }
        out.close();
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Windows 的 rename 不覆盖既有文件：退化为先删再改名
        std::error_code ec2;
        fs::remove(path, ec2);
        fs::rename(tmp, path, ec);
        if (ec) { err = "cannot replace " + path + ": " + ec.message(); return false; }
    }
    return true;
}

bool Platform::authenticate(const std::string& agent, const std::string& apiKey) const {
    std::lock_guard lock(mutex_);
    std::string err;
    std::string salt, stored;
    if (!agents_.credentialOf(agent, salt, stored, err)) return false;
    if (stored.empty()) return false;
    // 盐为空 = 旧格式哈希；非空 = sha256(salt + 明钥)。常量时间比较，避免计时侧信道
    return constantTimeEquals(stored, sha256Hex(salt.empty() ? apiKey : salt + apiKey));
}

bool Platform::authenticateMaster(const std::string& masterKey) const {
    std::lock_guard lock(mutex_);
    return !master_key_hash_.empty() && constantTimeEquals(master_key_hash_, sha256Hex(masterKey));
}

bool Platform::isManager(const std::string& name) const { return name == kManagerName; }

// 保留身份：这些名字在鉴权里被当作特权主体（"user" = 人类用户，"zcode" = 管理者，
// "system" = 平台自身），必须禁止注册占用。否则任何持有主密钥的 Agent 只要注册一个
// 叫 "user" 的账号，就能以"用户"身份解决他人错误、流转他人任务状态，而且审计日志会
// 把操作记成人类用户做的——恰好打穿"全程可追溯"这条核心承诺。
bool Platform::isReservedName(const std::string& name) {
    const std::string n = toLower(name);
    return n == kManagerName || n == "user" || n == "system" || n == "master";
}

bool Platform::registerAgent(const std::string& masterKey, const std::string& name,
                             const std::string& role, std::string& outApiKey, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!authenticateMaster(masterKey)) { err = "invalid master key"; return false; }
    if (name.empty() || name.size() > 64 || name.find_first_of(" \t\r\n") != std::string::npos) {
        err = "invalid agent name (1..64 chars, no whitespace)";
        return false;
    }
    // 保留名不可注册（否则可冒充人类用户/管理者，且审计会记错主体）
    if (isReservedName(name)) {
        err = "reserved agent name: " + name;
        return false;
    }
    // 角色白名单：此前 role 原样入库、且从不参与鉴权，等于可以自封任意角色。
    // 管理者的 zcode 角色只由 bootstrap 内部写入，不通过注册接口下发。
    std::string actualRole = role.empty() ? std::string(kDefaultRole) : role;
    if (actualRole != kDefaultRole) {
        err = "invalid role (only '" + std::string(kDefaultRole) + "' may be registered)";
        return false;
    }
    if (agents_.nameExists(name)) { err = "agent already registered: " + name; return false; }
    outApiKey = randomHex(32);
    std::string salt = randomHex(16);
    if (!agents_.registerAgent(name, actualRole, salt, sha256Hex(salt + outApiKey), err))
        return false;
    std::string persistErr;
    std::string auditErr;
    if (!persistAgentKey(name, outApiKey, persistErr))
        audit_.log("system", "system.keyfile_write_failed", name,
                   nlohmann::json{{"error", persistErr}}.dump(), auditErr);
    audit_.log("master", "agent.register", name,
               nlohmann::json{{"role", actualRole}}.dump(), auditErr);
    return true;
}

bool Platform::agentRemove(const std::string& actor, const std::string& name, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can remove agents"; return false; }
    if (name == kManagerName) { err = "cannot remove the manager agent"; return false; }
    if (!agents_.nameExists(name)) { err = "agent not found: " + name; return false; }
    if (!agents_.removeAgent(name, err)) return false;
    // 密钥缓存文件同步移除该条目（尽力而为，失败不回滚删除）
    std::string persistErr;
    (void)persistAgentKey(name, "", persistErr);
    audit_.log(actor, "agent.remove", name, nlohmann::json{{"removed", name}}.dump(), err);
    return true;
}

bool Platform::agentRotateKey(const std::string& actor, const std::string& name,
                              std::string& outApiKey, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can rotate agent keys"; return false; }
    if (!agents_.nameExists(name)) { err = "agent not found: " + name; return false; }
    outApiKey = randomHex(32);
    std::string salt = randomHex(16);
    if (!agents_.rotateKey(name, salt, sha256Hex(salt + outApiKey), err)) return false;
    // 明文落盘供 Agent 侧取用。写失败不回滚轮换：新密钥已经返回给调用方，
    // 回滚反而会让新旧密钥同时失效。失败记入审计，便于排查"密钥文件写不进去"。
    std::string persistErr;
    std::string auditErr;
    if (!persistAgentKey(name, outApiKey, persistErr))
        audit_.log("system", "system.keyfile_write_failed", name,
                   nlohmann::json{{"error", persistErr}}.dump(), auditErr);
    audit_.log(actor, "agent.rotate_key", name, nlohmann::json{{"rotated", name}}.dump(),
               auditErr);
    return true;
}

bool Platform::agentProvision(const std::string& actor, const std::string& name,
                              std::string& outApiKey, std::string& err) {
    // 单一锁域内完成判断+写入：mutex_ 不可重入，不能转调持锁的公开方法
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can provision agents"; return false; }
    if (name.empty() || name.size() > 64 || name.find_first_of(" \t\r\n") != std::string::npos) {
        err = "invalid agent name (1..64 chars, no whitespace)";
        return false;
    }
    if (isReservedName(name)) { err = "reserved agent name: " + name; return false; }
    const bool existed = agents_.nameExists(name);
    outApiKey = randomHex(32);
    std::string salt = randomHex(16);
    if (existed) {
        if (!agents_.rotateKey(name, salt, sha256Hex(salt + outApiKey), err)) return false;
    } else {
        if (!agents_.registerAgent(name, kDefaultRole, salt, sha256Hex(salt + outApiKey), err))
            return false;
    }
    std::string persistErr;
    std::string auditErr;
    if (!persistAgentKey(name, outApiKey, persistErr))
        audit_.log("system", "system.keyfile_write_failed", name,
                   nlohmann::json{{"error", persistErr}}.dump(), auditErr);
    audit_.log(actor, "agent.provision", name,
               nlohmann::json{{"mode", existed ? "rotate" : "register"}}.dump(), auditErr);
    return true;
}

void Platform::heartbeat(const std::string& name, const std::string& currentTask) {
    std::lock_guard lock(mutex_);
    std::string prevTask, prevSeen;
    bool found = false;
    std::string err;
    db_.query("SELECT current_task, last_seen_at FROM agents WHERE name=?",
              [&](Stmt& st) { st.bind(1, name); },
              [&](Stmt& st) {
                  prevTask = st.isNull(0) ? std::string() : st.text(0);
                  prevSeen = st.isNull(1) ? std::string() : st.text(1);
                  found = true;
              },
              err);
    if (!found) return;
    agents_.heartbeat(name, currentTask);
    // 状态/任务变化才留痕，避免高频心跳淹没审计表
    bool wasOffline = prevSeen.empty();
    std::time_t t = 0;
    if (!prevSeen.empty() && parseIso(prevSeen, t) && (std::time(nullptr) - t) > 120) wasOffline = true;
    if (wasOffline || prevTask != currentTask) {
        audit_.log(name, "agent.heartbeat", name,
                   nlohmann::json{{"task", currentTask}}.dump(), err);
    }
}

bool Platform::listAgents(std::vector<AgentInfo>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return agents_.listAgents(out, err);
}

std::vector<float> Platform::resolveEmbedding(const std::string& content,
                                              const std::vector<float>* provided,
                                              bool& isProvided) {
    if (provided && !provided->empty() && static_cast<int>(provided->size()) == embedder_->dim()) {
        isProvided = true;
        return *provided;
    }
    isProvided = false;
    return embedder_->embed(content);
}

std::vector<float> Platform::embedText(const std::string& text) {
    std::lock_guard lock(mutex_);
    return embedder_->embed(text);
}

int Platform::embeddingDim() const { return embedder_->dim(); }

// ---------------- 知识库 ----------------

bool Platform::knowledgeCreate(const std::string& author, const std::string& title,
                               const std::string& content, const std::vector<std::string>& tags,
                               const std::string& category, const std::vector<float>& embedding,
                               const std::string& embeddingProvider, KnowledgeEntry& out,
                               std::string& err) {
    std::lock_guard lock(mutex_);
    if (title.empty() || content.empty()) { err = "title and content are required"; return false; }
    if (title.size() > 200 || content.size() > 100000 || category.size() > 64 || tags.size() > 20) {
        err = "field too long (title<=200, content<=100000, category<=64, tags<=20)";
        return false;
    }
    for (const auto& t : tags)
        if (t.size() > 64) { err = "tag too long (<=64)"; return false; }
    bool provided = false;
    std::vector<float> vec = resolveEmbedding(content, &embedding, provided);
    std::string provider = provided ? embeddingProvider : embedder_->name();
    if (provider.empty()) provider = embedder_->name();
    std::string tagsJson = nlohmann::json(tags).dump();
    if (!knowledge_.create(author, title, content, tagsJson, category, vec, provider, out, err))
        return false;
    audit_.log(author, "knowledge.create", out.uuid,
               nlohmann::json{{"title", title}, {"tags", tags}, {"category", category}}.dump(), err);
    return true;
}

bool Platform::knowledgeList(int limit, const std::string& tagFilter,
                             std::vector<KnowledgeEntry>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return knowledge_.list(limit, tagFilter, out, err);
}

bool Platform::knowledgeLatest(const std::string& uuid, KnowledgeEntry& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return knowledge_.latest(uuid, out, err);
}

bool Platform::knowledgeVersions(const std::string& uuid, std::vector<KnowledgeEntry>& out,
                                 std::string& err) {
    std::lock_guard lock(mutex_);
    return knowledge_.versions(uuid, out, err);
}

bool Platform::knowledgeAddVersion(const std::string& author, const std::string& uuid,
                                   const std::string& newTitle, const std::string& newContent,
                                   const std::vector<float>& embedding,
                                   const std::string& embeddingProvider, KnowledgeEntry& out,
                                   std::string& err) {
    std::lock_guard lock(mutex_);
    if (newContent.empty()) { err = "new content is required"; return false; }
    bool provided = false;
    std::vector<float> vec = resolveEmbedding(newContent, &embedding, provided);
    std::string provider = provided ? embeddingProvider : embedder_->name();
    if (provider.empty()) provider = embedder_->name();
    if (!knowledge_.addVersion(author, uuid, newTitle, newContent, vec, provider, out, err))
        return false;
    audit_.log(author, "knowledge.version.add", uuid,
               nlohmann::json{{"version", out.version}}.dump(), err);
    return true;
}

bool Platform::knowledgeRemove(const std::string& actor, const std::string& uuid,
                               std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can remove knowledge entries"; return false; }
    KnowledgeEntry cur;
    if (!knowledge_.latest(uuid, cur, err)) return false;
    // 事务包裹删向量与删正文两步：后半步失败先回滚再返回失败，
    // 避免留下无向量的正文（列表仍可见、语义检索失效的静默残留）
    if (!db_.beginImmediate(err)) return false;
    // 删除全部版本与向量行（vec0 虚拟表按 entry_id 关联）
    if (!db_.query("DELETE FROM knowledge_vec WHERE entry_id IN "
                   "(SELECT id FROM knowledge_entries WHERE uuid=?)",
                   [&](Stmt& st) { st.bind(1, uuid); }, nullptr, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.query("DELETE FROM knowledge_entries WHERE uuid=?",
                   [&](Stmt& st) { st.bind(1, uuid); }, nullptr, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    // 审计写在业务事务提交之后：审计失败只记入 err，不回滚已提交的删除
    audit_.log(actor, "knowledge.remove", uuid,
               nlohmann::json{{"title", cur.title}}.dump(), err);
    return true;
}

bool Platform::knowledgeSearch(const std::string& query, SearchMode mode, int limit,
                               const std::string& tagFilter, std::vector<KnowledgeHit>& out,
                               std::string& err) {
    std::lock_guard lock(mutex_);
    if (query.empty()) { err = "query is required"; return false; }
    if (mode == SearchMode::Semantic) {
        std::vector<float> qv = embedder_->embed(query);
        return knowledge_.searchSemantic(qv, limit, tagFilter, out, err);
    }
    std::vector<KnowledgeEntry> entries;
    if (!knowledge_.searchKeyword(query, limit, tagFilter, entries, err)) return false;
    out.clear();
    for (auto& e : entries) out.push_back({std::move(e), 0.0});
    return true;
}

// ---------------- 技能库 ----------------

bool Platform::skillRegister(const std::string& author, const std::string& name,
                             const std::string& displayName, const std::string& description,
                             const std::string& category, const std::string& paramSchema,
                             SkillInfo& out, std::string& err) {
    std::lock_guard lock(mutex_);
    if (name.empty() || description.empty()) { err = "name and description are required"; return false; }
    if (name.size() > 64 || description.size() > 2000 || category.size() > 64 ||
        paramSchema.size() > 10000) {
        err = "field too long (name<=64, description<=2000, category<=64, schema<=10000)";
        return false;
    }
    if (!skills_.registerSkill(name, displayName, description, category, author, paramSchema, out,
                               err))
        return false;
    audit_.log(author, "skill.register", name,
               nlohmann::json{{"description", description}, {"category", category}}.dump(), err);
    return true;
}

bool Platform::skillList(const std::string& categoryFilter, const std::string& ownerFilter,
                         std::vector<SkillInfo>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return skills_.list(categoryFilter, ownerFilter, out, err);
}

bool Platform::skillGet(const std::string& name, SkillInfo& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return skills_.get(name, out, err);
}

namespace {
// param_schema 的最小校验：只看顶层 required 与 properties 的 type。
// 设计取舍：param_schema 是给调用方看的"约定"，平台此前完全不校验（错误参数静默入库）；
// 这里拦明显违规（缺必填键 / 顶层类型错），不做嵌套递归与 format 校验，避免误伤合法调用。
// schema 为空、缺失或本身损坏时一律放行——宁放行勿误杀（注册方写了坏 schema 不该堵死调用方）。
bool jsonTypeMatches(const nlohmann::json& v, const std::string& t) {
    if (t == "string") return v.is_string();
    if (t == "number") return v.is_number();
    if (t == "integer") return v.is_number_integer();
    if (t == "boolean") return v.is_boolean();
    if (t == "array") return v.is_array();
    if (t == "object") return v.is_object();
    if (t == "null") return v.is_null();
    return true;  // 未知类型标注放行
}
bool validateParamsAgainstSchema(const std::string& paramsJson, const std::string& schemaJson,
                                 std::string& err) {
    nlohmann::json params = nlohmann::json::parse(paramsJson, nullptr, false);
    if (params.is_discarded() || !params.is_object()) {
        err = "params must be a JSON object";
        return false;
    }
    nlohmann::json schema = nlohmann::json::parse(schemaJson, nullptr, false);
    if (schema.is_discarded() || !schema.is_object()) return true;
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& r : schema["required"]) {
            if (!r.is_string()) continue;
            if (!params.contains(r.get<std::string>())) {
                err = "missing required param: " + r.get<std::string>();
                return false;
            }
        }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            if (!params.contains(it.key())) continue;
            const nlohmann::json& spec = it.value();
            if (!spec.is_object() || !spec.contains("type") || !spec["type"].is_string()) continue;
            if (!jsonTypeMatches(params[it.key()], spec["type"].get<std::string>())) {
                err = "param '" + it.key() + "' must be " + spec["type"].get<std::string>();
                return false;
            }
        }
    }
    return true;
}
}  // namespace

bool Platform::skillInvoke(const std::string& caller, const std::string& skillName,
                           const std::string& paramsJson, const std::string& resultSummary,
                           const std::string& status, int64_t durationMs, int64_t tokensIn,
                           int64_t tokensOut, std::string& err) {
    std::lock_guard lock(mutex_);
    // 协作规则：新技能必须先注册再调用
    SkillInfo info;
    if (!skills_.get(skillName, info, err)) {
        err = "skill not registered: " + skillName;
        return false;
    }
    // 参数须符合注册时声明的 param_schema（最小校验：必填键 + 顶层类型）
    std::string schemaErr;
    if (!validateParamsAgainstSchema(paramsJson, info.param_schema, schemaErr)) {
        err = "params do not match param_schema of '" + skillName + "': " + schemaErr;
        return false;
    }
    if (status != "success" && status != "failed") { err = "status must be success|failed"; return false; }
    // 用量仅做统计与告警（80%/95%/超额三级），不做硬性拦截——
    // 平台定位是协作与观测，Agent 的消耗策略由调用方自行决定
    if (!skills_.recordInvocation(skillName, caller, paramsJson, resultSummary, status, durationMs,
                                  err))
        return false;
    if (tokensIn > 0 || tokensOut > 0) {
        bool invDup = false;
        if (!usage_.report(caller, tokensIn, tokensOut, "skill", "", skillName, "", invDup, err))
            return false;
    }
    audit_.log(caller, "skill.invoke", skillName,
               nlohmann::json{{"status", status}, {"duration_ms", durationMs}}.dump(), err);
    return true;
}

bool Platform::skillInvocations(const std::string& skillName, int limit,
                                std::vector<SkillInvocation>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return skills_.listInvocations(skillName, limit, out, err);
}

// ---------------- 用户记忆 ----------------

bool Platform::memoryList(const std::string& section, std::vector<MemoryEntry>& out,
                          std::string& err) {
    std::lock_guard lock(mutex_);
    return memory_.list(section, out, err);
}

bool Platform::memorySet(const std::string& author, const std::string& section,
                         const std::string& key, const std::string& value, int baseVersion,
                         MemoryEntry& out, std::string& err) {
    std::lock_guard lock(mutex_);
    if (section.empty() || key.empty()) { err = "section and key are required"; return false; }
    if (section.size() > 32 || key.size() > 128 || value.size() > 20000) {
        err = "field too long (section<=32, key<=128, value<=20000)";
        return false;
    }
    if (!memory_.set(author, section, key, value, baseVersion, out, err)) return false;
    audit_.log(author, "memory.set", section + "/" + key,
               nlohmann::json{{"version", out.version}, {"value", value}}.dump(), err);
    return true;
}

bool Platform::memoryRemove(const std::string& actor, const std::string& section,
                            const std::string& key, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can remove memory entries"; return false; }
    int64_t removed = 0;
    if (!memory_.remove(section, key, removed, err)) return false;
    if (removed == 0) { err = "memory entry not found: " + section + "/" + key; return false; }
    audit_.log(actor, "memory.remove", section + "/" + key,
               nlohmann::json{{"versions_removed", removed}}.dump(), err);
    return true;
}

bool Platform::memoryHistory(const std::string& section, const std::string& key,
                             std::vector<MemoryEntry>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return memory_.history(section, key, out, err);
}

// ---------------- 消息 ----------------

bool Platform::messageSend(const std::string& kind, const std::string& sender,
                           const std::string& recipient, const std::string& subject,
                           const std::string& body, Message& out, std::string& err) {
    std::lock_guard lock(mutex_);
    if (body.empty()) { err = "body is required"; return false; }
    if (body.size() > 50000 || subject.size() > 200) {
        err = "field too long (subject<=200, body<=50000)";
        return false;
    }
    if (!messages_.send(kind, sender, recipient, subject, body, std::string(), out, err))
        return false;
    audit_.log(sender, "message.send", out.uuid,
               nlohmann::json{{"kind", kind}, {"recipient", recipient}, {"subject", subject}}.dump(),
               err);
    return true;
}

bool Platform::messageList(const std::string& recipientFilter, const std::string& kindFilter,
                           const std::string& statusFilter, const std::string& sinceIso, int limit,
                           std::vector<Message>& out, std::string& err, const std::string& viewer) {
    std::lock_guard lock(mutex_);
    return messages_.list(recipientFilter, kindFilter, statusFilter, sinceIso, limit, out, err,
                          viewer);
}

bool Platform::messageGet(const std::string& uuid, Message& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return messages_.get(uuid, out, err);
}

bool Platform::messageReply(const std::string& sender, const std::string& parentUuid,
                            const std::string& body, Message& out, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!messages_.reply(parentUuid, sender, body, out, err)) return false;
    audit_.log(sender, "message.reply", out.uuid,
               nlohmann::json{{"parent", parentUuid}}.dump(), err);
    return true;
}

bool Platform::messageSetStatus(const std::string& actor, const std::string& uuid,
                                const std::string& newStatus, Message& out, std::string& err) {
    std::lock_guard lock(mutex_);
    Message cur;
    if (!messages_.get(uuid, cur, err)) return false;
    // 任务类消息只有收件人（执行者）、用户或管理者可流转，发件人不能代为接单
    if (cur.kind == "task" && actor != cur.recipient && actor != "user" && !isManager(actor)) {
        err = "only the task recipient can change task status";
        return false;
    }
    // 其余消息仅收件人、发件人、用户或管理者可流转
    if (actor != "user" && actor != cur.recipient && actor != cur.sender && !isManager(actor)) {
        err = "not allowed to change this message's status";
        return false;
    }
    if (!messages_.setStatus(uuid, newStatus, out, err)) return false;
    audit_.log(actor, "message.status", uuid, nlohmann::json{{"status", newStatus}}.dump(), err);
    return true;
}

// ---------------- 错误日志 ----------------

bool Platform::errorReport(const std::string& reporter, const std::string& severity,
                           const std::string& source, const std::string& title,
                           const std::string& detail, const std::string& stackTrace,
                           ErrorReport& out, std::string& err) {
    std::lock_guard lock(mutex_);
    if (title.empty() || detail.empty()) { err = "title and detail are required"; return false; }
    if (title.size() > 200 || detail.size() > 50000 || stackTrace.size() > 20000 ||
        source.size() > 128) {
        err = "field too long (title<=200, detail<=50000, stack<=20000, source<=128)";
        return false;
    }
    std::string sev = severity;
    if (sev != "info" && sev != "warning" && sev != "error" && sev != "critical") sev = "error";
    if (!errors_.report(reporter, sev, source, title, detail, stackTrace, out, err)) return false;
    audit_.log(reporter, "error.report", out.uuid,
               nlohmann::json{{"severity", sev}, {"title", title}}.dump(), err);
    return true;
}

bool Platform::errorList(const std::string& statusFilter, const std::string& severityFilter,
                         int limit, std::vector<ErrorReport>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return errors_.list(statusFilter, severityFilter, limit, out, err);
}

bool Platform::errorResolve(const std::string& actor, const std::string& uuid,
                            const std::string& notes, ErrorReport& out, std::string& err) {
    std::lock_guard lock(mutex_);
    ErrorReport cur;
    if (!errors_.get(uuid, cur, err)) return false;
    if (actor != "user" && actor != cur.reporter && !isManager(actor)) {
        err = "only the reporter or zcode can resolve this error";
        return false;
    }
    if (!errors_.resolve(uuid, actor, notes, out, err)) return false;
    audit_.log(actor, "error.resolve", uuid, nlohmann::json{{"notes", notes}}.dump(), err);
    return true;
}

// ---------------- Token 用量 ----------------

bool Platform::usageReport(const std::string& agent, int64_t tokensIn, int64_t tokensOut,
                           const std::string& callType, const std::string& model,
                           const std::string& referenceId, const std::string& idempotencyKey,
                           bool& duplicate, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!usage_.report(agent, tokensIn, tokensOut, callType, model, referenceId, idempotencyKey,
                       duplicate, err))
        return false;
    if (!duplicate) {
        audit_.log(agent, "usage.report", referenceId,
                   nlohmann::json{{"tokens_in", tokensIn}, {"tokens_out", tokensOut},
                                  {"call_type", callType}, {"model", model}}.dump(),
                   err);
    }
    return true;
}

bool Platform::usageDaily(int days, std::vector<UsageDailyPoint>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return usage_.daily(days, out, err);
}

bool Platform::usageByModel(std::vector<UsageModelRow>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return usage_.byModel(out, err);
}

bool Platform::usageBreakdown(int days, const std::string& agent, const std::string& model,
                              UsageBreakdown& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return usage_.breakdown(days, agent, model, out, err);
}

bool Platform::usageSummary(UsageSummary& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return usage_.summary(out, err);
}

int64_t Platform::usageBudget(std::string& err) {
    std::lock_guard lock(mutex_);
    return usage_.budget(err);
}

bool Platform::usageSetBudget(const std::string& actor, int64_t budget, std::string& err) {
    std::lock_guard lock(mutex_);
    if (actor != "user" && !isManager(actor)) { err = "only the user or zcode can change the budget"; return false; }
    if (!usage_.setBudget(budget, err)) return false;
    audit_.log(actor, "budget.set", std::string(),
               nlohmann::json{{"budget", budget}}.dump(), err);
    return true;
}

// ---------------- 操作日志 ----------------

bool Platform::auditList(const std::string& actorFilter, const std::string& actionFilter,
                         const std::string& sinceIso, int limit, std::vector<AuditRecord>& out,
                         std::string& err) {
    std::lock_guard lock(mutex_);
    return audit_.list(actorFilter, actionFilter, sinceIso, limit, out, err);
}

// ---------------- 运维：维护 / 备份恢复 ----------------

bool Platform::maintenanceRun(const std::string& actor, std::string& statsJson, std::string& err) {
    std::lock_guard lock(mutex_);
    int64_t deletedAudit = 0, deletedErrors = 0;
    // 审计轮转：保留 30 天且最多 10 万条（created_at 与 nowIso 同格式，直接字符串比较）
    db_.query("DELETE FROM audit_log WHERE created_at < ?",
              [&](Stmt& st) { st.bind(1, isoDaysAgo(30)); },
              nullptr, err);
    deletedAudit = sqlite3_changes64(db_.handle());
    db_.query("DELETE FROM audit_log WHERE id <= (SELECT COALESCE(MAX(id),0) FROM audit_log) - ?",
              [](Stmt& st) { st.bind(1, static_cast<int64_t>(100000)); },
              nullptr, err);
    deletedAudit += sqlite3_changes64(db_.handle());
    // 已解决错误归档清理：解决超过 30 天的移出主表
    db_.query("DELETE FROM errors WHERE status='resolved' AND resolved_at IS NOT NULL AND resolved_at < ?",
              [&](Stmt& st) { st.bind(1, isoDaysAgo(30)); },
              nullptr, err);
    deletedErrors = sqlite3_changes64(db_.handle());

    bool vacuumed = false;
    if (deletedAudit > 0 || deletedErrors > 0) {
        vacuumed = db_.execScript("VACUUM;", err);
    }
    statsJson = nlohmann::json{{"deleted_audit", deletedAudit},
                               {"deleted_errors", deletedErrors},
                               {"vacuumed", vacuumed}}.dump();
    if (deletedAudit > 0 || deletedErrors > 0) {
        audit_.log(actor, "system.maintenance", std::string(), statsJson, err);
    }
    return true;
}

bool Platform::backupCreate(std::string& outPath, std::string& err) {
    std::lock_guard lock(mutex_);
    // VACUUM INTO 生成一致性好、含 WAL 已提交数据的独立快照文件
    std::string name = "platform-" + nowIso() + ".db";
    for (auto& ch : name)
        if (ch == ':') ch = '-';
    outPath = (fs::path(home_dir_) / "backup" / name).string();
    std::string escaped;
    for (char ch : outPath) {
        escaped += ch;
        if (ch == '\'') escaped += '\'';
    }
    if (!db_.execScript("VACUUM INTO '" + escaped + "';", err)) return false;
    // 备份是一次完整的数据库落盘，属于必须留痕的管理操作（此前漏记）
    audit_.log("zcode", "system.backup", name, nlohmann::json{{"file", name}}.dump(), err);
    return true;
}

bool Platform::backupList(std::vector<std::string>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    out.clear();
    std::error_code ec;
    fs::path dir = fs::path(home_dir_) / "backup";
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".db")
            out.push_back(entry.path().filename().string());
    }
    if (ec) { err = "cannot list backup dir: " + ec.message(); return false; }
    std::sort(out.rbegin(), out.rend());  // 新的在前
    return true;
}

bool Platform::backupRestore(const std::string& name, std::string& err) {
    std::lock_guard lock(mutex_);
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
        err = "invalid backup name";
        return false;
    }
    fs::path src = fs::path(home_dir_) / "backup" / name;
    if (!fs::exists(src)) { err = "backup not found: " + name; return false; }
    {
        std::ifstream in(src, std::ios::binary);
        char header[16] = {0};
        in.read(header, 15);
        if (std::string(header) != "SQLite format 3") {
            err = "not a valid sqlite backup file";
            return false;
        }
    }
    // 关闭连接（自动 checkpoint WAL）→ 覆盖 → 重开
    db_.close();
    std::error_code ec;
    fs::copy_file(src, fs::path(home_dir_) / "platform.db",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "copy failed: " + ec.message(); return false; }
    if (!db_.open((fs::path(home_dir_) / "platform.db").string(), err)) return false;
    if (sqlite3_vec_init(db_.handle(), nullptr, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_.handle());
        return false;
    }
    if (!knowledge_.ensureVecTable(err)) return false;
    audit_.log("zcode", "system.restore", name, nlohmann::json{{"file", name}}.dump(), err);
    return true;
}

// ---------------- HTTP ----------------

bool Platform::startHttpServer(int port, std::string& err) {
    std::lock_guard lock(mutex_);
    if (http_ && http_->running()) return true;
    http_ = std::make_unique<HttpServer>(*this);
    return http_->start(port, err);
}

void Platform::stopHttpServer() {
    // 同 shutdown：取出后锁外停止，避免与在途请求互相等待
    std::unique_ptr<HttpServer> http;
    {
        std::lock_guard lock(mutex_);
        http = std::move(http_);
    }
    if (http) http->stop();
}

int Platform::httpPort() const {
    std::lock_guard lock(mutex_);
    return http_ ? http_->port() : 0;
}

bool Platform::httpRunning() const {
    std::lock_guard lock(mutex_);
    return http_ && http_->running();
}

}  // namespace ah
