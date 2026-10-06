#include "core/platform.h"

#include <algorithm>
#include <cstdio>
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

// 库结构版本：必须与 schema.sql 末尾的 `PRAGMA user_version` 保持一致。
// 改 schema 时两处一起递增，并在 bootstrap 的迁移链里补上对应步骤。
constexpr int64_t kSchemaVersion = 2;

// ---- 审计留痕是硬契约：审计写不进去，这次写操作就算失败 ----
// 理由：schema/README 承诺"所有写操作在 audit_log 留痕"。若让业务数据落库而审计行写不进去，
// 产出的是一条**无法追溯**的记录——比直接拒绝这次写更糟。
//
// 关键约束（实测踩过两次，两次都是"失败被静默吞掉"本身）：
//   1) SQLite **没有真正的嵌套事务**，内层 COMMIT 会提交最外层。所以
//      "调用方在外层 BEGIN、服务内部自己 COMMIT"是错的——外层事务会永远挂着，
//      紧接着的下一条 BEGIN 直接报 "cannot start a transaction within a transaction"。
//      需要留痕的业务必须让审计成为**服务自身事务里的一步**（见 InTxStep / auditStep）。
//   2) 调用方必须把**自己的 err 传进来**。曾写成"写进局部 auditErr"，
//      结果返回 false 而 err 为空——调用方拿到一个没有理由的失败，正是本规则要消灭的问题。
#define AUDIT_OR_FAIL(call, sink)                                  \
    do {                                                           \
        if (!(call)) {                                             \
            db_.rollback();                                        \
            if ((sink).empty()) (sink) = "audit log write failed";  \
            return false;                                          \
        }                                                          \
    } while (0)

// 提交前执行的审计步骤：专供服务层 InTxStep 使用（事务由服务拥有，审计在其内部执行）。
// 审计失败时把原因写进 err，由调用方并入自己的 err 上报。
bool Platform::auditStep(const std::string& actor, const std::string& action,
                         const std::string& target, const std::string& detailJson,
                         std::string& err) {
    return audit_.log(actor, action, target, detailJson, err);
}

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
      knowledge_(db_, *embedder_),
      skills_(db_),
      memory_(db_),
      messages_(db_),
      errors_(db_),
      usage_(db_),
      audit_(db_) {}

Platform::~Platform() { shutdown(); }

bool Platform::initStoreLocked(std::string& err) {
    // ---- 库结构版本门（schema.sql 末尾 PRAGMA user_version 写入）----
    // 必须在执行 schema **之前**比对：更高版本的库不能被本程序按旧 schema 解读。
    int64_t dbVersion = 0;
    if (!db_.query("PRAGMA user_version", nullptr,
                   [&](Stmt& st) { dbVersion = st.i64(0); }, err))
        return false;
    if (dbVersion > kSchemaVersion) {
        err = "database schema version " + std::to_string(dbVersion) +
              " is newer than this build supports (max " + std::to_string(kSchemaVersion) +
              "); upgrade MiderHive or restore an older backup";
        return false;
    }
    if (!db_.execScript(kSchemaSql, err)) return false;
    // 存量库迁移：新增列（已存在则静默跳过）
    db_.tryExec("ALTER TABLE agents ADD COLUMN salt TEXT NOT NULL DEFAULT '';");
    db_.tryExec("ALTER TABLE token_usage ADD COLUMN idempotency_key TEXT;");
    db_.tryExec("ALTER TABLE token_usage ADD COLUMN model TEXT NOT NULL DEFAULT '';");
    db_.tryExec("ALTER TABLE skill_invocations ADD COLUMN reference_id TEXT;");
    // v2：记录每个版本向量所在的维度（精确清理向量行 + 诊断信息）
    db_.tryExec("ALTER TABLE knowledge_entries ADD COLUMN embedding_dim INTEGER;");
    db_.tryExec("CREATE UNIQUE INDEX IF NOT EXISTS idx_usage_idem "
                "ON token_usage(idempotency_key) "
                "WHERE idempotency_key IS NOT NULL AND idempotency_key != '';");
    // 启用 sqlite-vec（静态链接进本进程）：旧库的单张向量表迁移到按维度分表
    if (sqlite3_vec_init(db_.handle(), nullptr, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_.handle());
        return false;
    }
    if (!knowledge_.initVectorStore(err)) return false;
    // v2 迁移：给没有维度记录的旧行按"向量实际所在的表"回填 embedding_dim
    if (!knowledge_.backfillEmbeddingDim(err)) return false;
    // 关键词检索的 FTS 索引：存量库在这里一次性 rebuild
    if (!knowledge_.initSearchIndex(err)) return false;

    // 预算默认值（可被 settings 覆盖）
    if (!db_.query("INSERT OR IGNORE INTO settings(key, value) VALUES('weekly_token_budget', ?)",
                   [](Stmt& st) { st.bind(1, kDefaultWeeklyBudget); }, nullptr, err))
        return false;

    // 内置嵌入器的维度预先登记为它的 provider：384 维不准其它模型的向量混入
    // （同一维度共用一张 vec 表，混入会污染 kNN 结果，见 bindEmbeddingProvider）
    if (!db_.query("INSERT OR IGNORE INTO settings(key, value) VALUES(?,?)",
                   [&](Stmt& st) {
                       st.bind(1, std::string("embed_provider_") +
                                      std::to_string(embedder_->dim()));
                       st.bind(2, embedder_->name());
                   },
                   nullptr, err))
        return false;
    return true;
}

bool Platform::refuseIfUnusable(std::string& err) const {
    if (usable_) return false;
    err = "the data store is unusable after a failed backup restore; restart MiderHive";
    return true;
}

bool Platform::isUsable() const {
    std::lock_guard lock(mutex_);
    return usable_;
}

bool Platform::bootstrap(std::string& err) {
    std::lock_guard lock(mutex_);
    if (bootstrapped_) return true;

    std::error_code ec;
    fs::create_directories(fs::path(home_dir_) / "config", ec);
    fs::create_directories(fs::path(home_dir_) / "backup", ec);
    if (ec) { err = "cannot create home dir: " + ec.message(); return false; }

    std::string dbPath = (fs::path(home_dir_) / "platform.db").string();
    if (!db_.open(dbPath, err)) return false;
    // 库初始化（版本门/schema/迁移/vec/FTS/默认设置）——与恢复备份路径共用同一套
    if (!initStoreLocked(err)) return false;

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
        // IGNORE: 引导期一次性留痕。此处 schema 刚建好、数据目录刚通过自检，
        // 审计写失败意味着"整套初始化已经不可信"（由后续 diagnostics 暴露），
        // 不值得在这里让首次启动直接失败（用户将完全无法进入程序）。
        std::string bootAuditErr;
        audit_.log("system", "agent.register", kManagerName,
                   nlohmann::json{{"role", "zcode"}}.dump(), bootAuditErr);
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
            // IGNORE: 启动期诊断留痕（"明文密钥文件缺失"这条线索本身），不是用户发起的
            // 写操作；它写不进去时启动不该失败——那会让"密钥文件丢了"升级成"打不开程序"。
            audit_.log("system", "system.keyfile_missing", kManagerName,
                       nlohmann::json{{"path", keyPath},
                                      {"recover", "POST /api/agents/rotate"},
                                      {"hint", "plaintext keys are not recoverable from the database"}}
                           .dump(),
                       auditErr);
        }
    }

    // 数据增长维护：审计轮转 + 已解决错误清理（每次启动执行）。
    // 定位是"尽力而为的家务"：库被占用/磁盘暂时写不进去时，不该让整个工作台启动失败
    // （下次启动会再来一次）。但**不能静默**：失败原文写进启动日志，且不能污染 err。
    std::string stats;
    std::string maintErr;
    // IGNORE: 有意不改启动判定——见上；失败原因已在下面显式输出。
    if (!maintenanceRun("system", stats, maintErr))
        std::fprintf(stderr, "[miderhive] startup maintenance skipped: %s\n", maintErr.c_str());

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
    // 注册与留痕同事务
    if (!db_.beginImmediate(err)) return false;
    if (!agents_.registerAgent(name, actualRole, salt, sha256Hex(salt + outApiKey), err)) {
        db_.rollback();
        return false;
    }
    std::string auditErr;
    AUDIT_OR_FAIL(audit_.log("master", "agent.register", name,
                             nlohmann::json{{"role", actualRole}}.dump(), auditErr),
                  auditErr);
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    std::string persistErr;
    if (!persistAgentKey(name, outApiKey, persistErr)) {
        std::string keyfileAuditErr;
        // IGNORE: 元审计（"审计写失败"本身的留痕），写不进去时无处可报，不影响注册结果
        audit_.log("system", "system.keyfile_write_failed", name,
                   nlohmann::json{{"error", persistErr}}.dump(), keyfileAuditErr);
    }
    return true;
}

bool Platform::agentRemove(const std::string& actor, const std::string& name, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can remove agents"; return false; }
    if (name == kManagerName) { err = "cannot remove the manager agent"; return false; }
    if (!agents_.nameExists(name)) { err = "agent not found: " + name; return false; }
    // 删除与留痕同事务：不能出现"Agent 没了、审计里查不到谁删的"
    if (!db_.beginImmediate(err)) return false;
    if (!agents_.removeAgent(name, err)) {
        db_.rollback();
        return false;
    }
    std::string auditErr;
    AUDIT_OR_FAIL(audit_.log(actor, "agent.remove", name,
                             nlohmann::json{{"removed", name}}.dump(), auditErr),
                  auditErr);
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    // 密钥缓存文件同步移除该条目（文件操作不进数据库事务：失败不回滚删除）
    std::string persistErr;
    (void)persistAgentKey(name, "", persistErr);
    return true;
}

bool Platform::agentRotateKey(const std::string& actor, const std::string& name,
                              std::string& outApiKey, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can rotate agent keys"; return false; }
    if (!agents_.nameExists(name)) { err = "agent not found: " + name; return false; }
    outApiKey = randomHex(32);
    std::string salt = randomHex(16);
    // 轮换与留痕同事务：失败时旧密钥继续有效，不会出现"换了但查不到谁换的"
    if (!db_.beginImmediate(err)) return false;
    if (!agents_.rotateKey(name, salt, sha256Hex(salt + outApiKey), err)) {
        db_.rollback();
        return false;
    }
    std::string auditErr;
    AUDIT_OR_FAIL(audit_.log(actor, "agent.rotate_key", name,
                             nlohmann::json{{"rotated", name}}.dump(), auditErr),
                  auditErr);
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    // 明文落盘供 Agent 侧取用。写失败不回滚轮换：新密钥已经生效并返回给调用方，
    // 回滚反而会让新旧密钥同时失效。失败记入审计，便于排查"密钥文件写不进去"。
    std::string persistErr;
    if (!persistAgentKey(name, outApiKey, persistErr)) {
        std::string keyfileAuditErr;
        // IGNORE: 这是"审计写失败"本身的审计（元审计），它自己写不进去时无处可报；
        // 不让它影响轮换结果。
        audit_.log("system", "system.keyfile_write_failed", name,
                   nlohmann::json{{"error", persistErr}}.dump(), keyfileAuditErr);
    }
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
    // 签发/轮换与留痕同事务（理由同 agentRotateKey）
    if (!db_.beginImmediate(err)) return false;
    if (existed) {
        if (!agents_.rotateKey(name, salt, sha256Hex(salt + outApiKey), err)) {
            db_.rollback();
            return false;
        }
    } else {
        if (!agents_.registerAgent(name, kDefaultRole, salt, sha256Hex(salt + outApiKey), err)) {
            db_.rollback();
            return false;
        }
    }
    std::string auditErr;
    AUDIT_OR_FAIL(audit_.log(actor, "agent.provision", name,
                             nlohmann::json{{"mode", existed ? "rotate" : "register"}}.dump(),
                             auditErr),
                  auditErr);
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    std::string persistErr;
    if (!persistAgentKey(name, outApiKey, persistErr)) {
        std::string keyfileAuditErr;
        // IGNORE: 元审计（"审计写失败"本身的留痕），写不进去时无处可报，不影响签发结果
        audit_.log("system", "system.keyfile_write_failed", name,
                   nlohmann::json{{"error", persistErr}}.dump(), keyfileAuditErr);
    }
    return true;
}

void Platform::heartbeat(const std::string& name, const std::string& currentTask) {
    std::lock_guard lock(mutex_);
    std::string prevTask, prevSeen;
    bool found = false;
    std::string err;
    // IGNORE: 只读探测——用于判断"是否首次上线/任务是否变化"以决定要不要留痕；
    // 查不到就按 found=false 继续（心跳本身由 AgentService::heartbeat 负责）。
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
        // IGNORE: 心跳是 30-60s 一次的高频上报，审计失败不该让心跳失败
        // （那会把"暂时写不进审计表"放大成"Agent 集体离线"）；心跳本身仍由审计覆盖，
        // 只是允许这条留痕尽力而为。
        std::string heartbeatAuditErr;
        audit_.log(name, "agent.heartbeat", name,
                   nlohmann::json{{"task", currentTask}}.dump(), heartbeatAuditErr);
    }
}

bool Platform::listAgents(std::vector<AgentInfo>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    return agents_.listAgents(out, err);
}

std::vector<float> Platform::resolveEmbedding(const std::string& content,
                                              const std::vector<float>* provided,
                                              bool& isProvided) {
    // Agent 自带向量：维度落在合法范围内就原样采用（按维度分表，互不干扰）。
    // 不再要求等于内置 384——那是"语义检索其实接不了真模型"的根源。
    if (provided && !provided->empty() &&
        isValidEmbeddingDim(static_cast<int>(provided->size()))) {
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

int Platform::embeddingDim() const { return embedder_->dim(); }  // 内置嵌入器的维度（384）

// ---------------- 知识库 ----------------

// 维度↔provider 绑定：同一维度的向量在 vec0 里**共用一张表**，不同模型的向量因此会
// 互相污染 kNN 结果（查询时无法区分来源维度相同但语义空间不同的向量）。
// 首次写入某维度时登记其 provider；之后同维度不同模型的写入明确拒绝——把"检索结果
// 莫名其妙"变成"一眼可读的用法错误"。调用方须持有 mutex_。
bool Platform::bindEmbeddingProvider(size_t dim, const std::string& provider, std::string& err) {
    const std::string key = "embed_provider_" + std::to_string(dim);
    std::string bound;
    bool found = false;
    if (!db_.query("SELECT value FROM settings WHERE key=?",
                   [&](Stmt& st) { st.bind(1, key); },
                   [&](Stmt& st) {
                       bound = st.text(0);
                       found = true;
                   },
                   err))
        return false;
    if (!found) {
        return db_.query("INSERT OR IGNORE INTO settings(key, value) VALUES(?,?)",
                         [&](Stmt& st) {
                             st.bind(1, key);
                             st.bind(2, provider);
                         },
                         nullptr, err);
    }
    if (bound != provider) {
        err = "dimension " + std::to_string(dim) + " is bound to embedding provider '" + bound +
              "' (got '" + provider +
              "'); vectors from different models share one table and would pollute each other — "
              "use the same model for this dimension, or pick another dimension";
        return false;
    }
    return true;
}

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
    // 自带向量必须声明来源模型：空串一旦回落成内置标签，任何模型的向量都能顶着同一个
    // 假登记混进对应维度表——"维度↔provider 绑定"防线被整体架空，之后如实声明的
    // 合法写入反而被这条假登记拒绝。MCP 侧同规则：给了 embedding 就必须说清是谁算的。
    if (provided && embeddingProvider.empty()) {
        err = "embedder is required when embedding is provided";
        return false;
    }
    std::string provider = provided ? embeddingProvider : embedder_->name();
    if (!bindEmbeddingProvider(vec.size(), provider, err)) return false;
    std::string tagsJson = nlohmann::json(tags).dump();
    // 审计作为服务事务内的一步：知识条目与它的留痕要么都在、要么都不在。
    // 审计用独立的 err——审计失败时本函数即将返回 false，绝不能把审计的错误文本
    // 写进调用方的 err 之后又返回 true（那会让"成功"带着一条错误信息出去）。
    std::string auditErr;
    const std::string createDetail =
        nlohmann::json{{"title", title}, {"tags", tags}, {"category", category}}.dump();
    const bool ok = knowledge_.create(
        author, title, content, tagsJson, category, vec, provider, out, err, [&]() {
            return auditStep(author, "knowledge.create", out.uuid, createDetail, auditErr);
        });
    if (!ok && err.empty()) err = auditErr;  // 失败原因来自审计时要如实上报
    return ok;
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
    // 自带向量必须声明来源模型（理由同 knowledgeCreate：空串兜底会架空维度绑定）
    if (provided && embeddingProvider.empty()) {
        err = "embedder is required when embedding is provided";
        return false;
    }
    std::string provider = provided ? embeddingProvider : embedder_->name();
    if (!bindEmbeddingProvider(vec.size(), provider, err)) return false;
    // 审计作为服务事务内的一步（InTxStep）：SQLite 无嵌套事务，不能在服务外面再包 BEGIN
    // detail 必须在 lambda 内构造：审计执行时服务才刚把新版本号填进 out（在调用前拼串
    // 拿到的是调用方传入的旧对象，version 恒为默认值——审计留痕就此失真）
    std::string auditErr;
    const bool ok = knowledge_.addVersion(
        author, uuid, newTitle, newContent, vec, provider, out, err, [&]() {
            return auditStep(author, "knowledge.version.add", uuid,
                             nlohmann::json{{"version", out.version}}.dump(), auditErr);
        });
    if (!ok && err.empty()) err = auditErr;
    return ok;
}

bool Platform::knowledgeSearchSemantic(const std::vector<float>& queryVec, int limit,
                                       const std::string& tagFilter,
                                       std::vector<KnowledgeHit>& out, std::string& err) {
    std::lock_guard lock(mutex_);
    if (queryVec.empty()) { err = "embedding is required for semantic search"; return false; }
    if (!isValidEmbeddingDim(static_cast<int>(queryVec.size()))) {
        err = "embedding dimension " + std::to_string(queryVec.size()) +
              " not supported (expected 64..4096)";
        return false;
    }
    return knowledge_.searchSemantic(queryVec, limit, tagFilter, out, err);
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
    // 删除全部版本与向量行（向量在按维度分表里，由 KnowledgeService 统一清理）
    if (!knowledge_.deleteVecsForUuid(uuid, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.query("DELETE FROM knowledge_entries WHERE uuid=?",
                   [&](Stmt& st) { st.bind(1, uuid); }, nullptr, err)) {
        db_.rollback();
        return false;
    }
    // 审计放在提交之前：留痕是硬契约，写不进去就整体回滚——不能出现
    // "条目已删除、审计里查不到谁删的"（数据与留痕必须同生同灭）。
    std::string auditErr;
    AUDIT_OR_FAIL(audit_.log(actor, "knowledge.remove", uuid,
                             nlohmann::json{{"title", cur.title}}.dump(), auditErr),
                  auditErr);
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
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
    // 审计作为服务事务内的一步：技能注册与留痕同生同灭
    std::string auditErr;
    const std::string skillDetail =
        nlohmann::json{{"description", description}, {"category", category}}.dump();
    const bool ok = skills_.registerSkill(name, displayName, description, category, author,
                                          paramSchema, out, err, [&]() {
                                              return auditStep(author, "skill.register", name,
                                                               skillDetail, auditErr);
                                          });
    if (!ok && err.empty()) err = auditErr;
    return ok;
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
                           int64_t tokensOut, const std::string& referenceId, std::string& err) {
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
    if (referenceId.size() > 128) { err = "reference_id too long (<=128)"; return false; }
    // 记录调用、记账、审计是同一逻辑事件的三次写入：绑进一个事务，任一失败全部回滚。
    // 此前三次写入各自独立——记账失败会留下"有调用记录、无 token 账目"的半截状态，
    // 且审计写失败被静默忽略（违背 schema 的留痕承诺）。
    if (!db_.beginImmediate(err)) return false;
    if (!skills_.recordInvocation(skillName, caller, paramsJson, resultSummary, status, durationMs,
                                  referenceId, err)) {
        db_.rollback();
        return false;
    }
    if (tokensIn > 0 || tokensOut > 0) {
        // 用量仅做统计与告警（80%/95%/超额三级），不做硬性拦截——
        // 平台定位是协作与观测，Agent 的消耗策略由调用方自行决定
        bool invDup = false;
        if (!usage_.reportInTx(caller, tokensIn, tokensOut, "skill", "", skillName, "", invDup,
                               err)) {
            db_.rollback();
            return false;
        }
    }
    if (!audit_.log(caller, "skill.invoke", skillName,
                    nlohmann::json{{"status", status}, {"duration_ms", durationMs}}.dump(), err)) {
        db_.rollback();
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
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
    // 审计作为服务事务内的一步：记忆写入与留痕同生同灭（SQLite 无嵌套事务）
    // detail 必须在 lambda 内构造：服务在事务内最后一步之前才把 out.version 填好，
    // 在调用前拼串拿到的是默认值 1（memory.remove 的 versions_removed 同理已如此）
    std::string auditErr;
    const bool ok = memory_.set(author, section, key, value, baseVersion, out, err, [&]() {
        return auditStep(author, "memory.set", section + "/" + key,
                         nlohmann::json{{"version", out.version}, {"value", value}}.dump(),
                         auditErr);
    });
    if (!ok && err.empty()) err = auditErr;
    return ok;
}

bool Platform::memoryRemove(const std::string& actor, const std::string& section,
                            const std::string& key, std::string& err) {
    std::lock_guard lock(mutex_);
    if (!isManager(actor)) { err = "only zcode can remove memory entries"; return false; }
    int64_t removed = 0;
    // 审计作为服务事务内的一步（需要 versions_removed 计数）
    std::string auditErr;
    const bool ok = memory_.remove(section, key, removed, err, [&]() {
        return auditStep(actor, "memory.remove", section + "/" + key,
                         nlohmann::json{{"versions_removed", removed}}.dump(), auditErr);
    });
    if (!ok) {
        if (err.empty()) err = auditErr;
        return false;
    }
    if (removed == 0) { err = "memory entry not found: " + section + "/" + key; return false; }
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
    // 审计作为服务事务内的一步：消息与它的留痕同生同灭
    // （不要在服务外面再包一层 BEGIN：SQLite 的 COMMIT 会提交最外层事务）
    std::string auditErr;
    const std::string sendDetail =
        nlohmann::json{{"kind", kind}, {"recipient", recipient}, {"subject", subject}}.dump();
    const bool ok = messages_.send(kind, sender, recipient, subject, body, std::string(), out, err,
                                   [&]() {
                                       return auditStep(sender, "message.send", out.uuid, sendDetail,
                                                        auditErr);
                                   });
    if (!ok && err.empty()) err = auditErr;
    return ok;
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
    // 审计作为服务事务内的一步（reply 自身把"写回复 + 标已读"绑在一个事务里）
    std::string auditErr;
    const std::string replyDetail = nlohmann::json{{"parent", parentUuid}}.dump();
    const bool ok = messages_.reply(parentUuid, sender, body, out, err, [&]() {
        return auditStep(sender, "message.reply", out.uuid, replyDetail, auditErr);
    });
    if (!ok && err.empty()) err = auditErr;
    return ok;
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
    // 与审计同事务（messages_.setStatus 是单条 UPDATE，自动提交下落库后回滚无效）
    std::string auditErr;
    const std::string statusDetail = nlohmann::json{{"status", newStatus}}.dump();
    const bool ok = messages_.setStatus(uuid, newStatus, out, err, [&]() {
        return auditStep(actor, "message.status", uuid, statusDetail, auditErr);
    });
    if (!ok && err.empty()) err = auditErr;
    return ok;
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
    // 与审计同事务：协作规则是"错误必须记录"，留痕失败就不该留下这条记录。
    // errors_.report 自带事务，审计用 InTxStep 注入（在外面再包 BEGIN 是错的：
    // SQLite 的 COMMIT 会提交最外层，外层 BEGIN 会永远挂着）。
    std::string auditErr;
    const std::string errDetail = nlohmann::json{{"severity", sev}, {"title", title}}.dump();
    const bool ok = errors_.report(reporter, sev, source, title, detail, stackTrace, out, err,
                                   [&]() {
                                       return auditStep(reporter, "error.report", out.uuid, errDetail,
                                                        auditErr);
                                   });
    if (!ok && err.empty()) err = auditErr;
    return ok;
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
    // 审计作为服务事务内的一步：解决记录与留痕同生同灭（此前是 A 批策略唯一
    // 标注 IGNORE 的写路径边界，现与其它服务一致）
    std::string auditErr;
    const std::string notesJson = nlohmann::json{{"notes", notes}}.dump();
    const bool ok = errors_.resolve(uuid, actor, notes, out, err, [&]() {
        return auditStep(actor, "error.resolve", uuid, notesJson, auditErr);
    });
    if (!ok && err.empty()) err = auditErr;
    return ok;
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
        // IGNORE: token 上报是高频遥测，账目本身（token_usage 行）已是权威来源；
        // 审计失败不该让一次合法的用量上报失败（那会让客户端重试并放大流量）。
        // 用量明细仍逐条落库，这里只是放弃"再记一条审计"。
        std::string usageAuditErr;
        audit_.log(agent, "usage.report", referenceId,
                   nlohmann::json{{"tokens_in", tokensIn}, {"tokens_out", tokensOut},
                                  {"call_type", callType}, {"model", model}}.dump(),
                   usageAuditErr);
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
    // 与审计同事务：预算变更属于管理操作，必须留下是谁改的
    if (!db_.beginImmediate(err)) return false;
    if (!usage_.setBudget(budget, err)) {
        db_.rollback();
        return false;
    }
    std::string auditErr;
    AUDIT_OR_FAIL(audit_.log(actor, "budget.set", std::string(),
                             nlohmann::json{{"budget", budget}}.dump(), auditErr),
                  auditErr);
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
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
    // 每条清理都必须接住返回值，并在**该语句成功之后立刻**取变更数：
    // sqlite3_changes64() 反映的是"最近一条语句"，此前三条 DELETE 的返回值全被丢弃，
    // 于是"删了 0 条"和"删除失败"给出完全相同的统计（0）且照样返回 true。
    auto cleanup = [&](const std::string& sql, const std::function<void(Stmt&)>& bind,
                       int64_t& counter) -> bool {
        if (!db_.query(sql, bind, nullptr, err)) return false;
        counter += sqlite3_changes64(db_.handle());
        return true;
    };
    // 审计轮转：保留 30 天且最多 10 万条（created_at 与 nowIso 同格式，直接字符串比较）
    if (!cleanup("DELETE FROM audit_log WHERE created_at < ?",
                 [&](Stmt& st) { st.bind(1, isoDaysAgo(30)); }, deletedAudit))
        return false;
    if (!cleanup("DELETE FROM audit_log WHERE id <= (SELECT COALESCE(MAX(id),0) FROM audit_log) - ?",
                 [](Stmt& st) { st.bind(1, static_cast<int64_t>(100000)); }, deletedAudit))
        return false;
    // 已解决错误归档清理：解决超过 30 天的移出主表
    if (!cleanup(
            "DELETE FROM errors WHERE status='resolved' AND resolved_at IS NOT NULL AND resolved_at < ?",
            [&](Stmt& st) { st.bind(1, isoDaysAgo(30)); }, deletedErrors))
        return false;

    bool vacuumed = false;
    if (deletedAudit > 0 || deletedErrors > 0) {
        vacuumed = db_.execScript("VACUUM;", err);
    }
    statsJson = nlohmann::json{{"deleted_audit", deletedAudit},
                               {"deleted_errors", deletedErrors},
                               {"vacuumed", vacuumed}}.dump();
    if (deletedAudit > 0 || deletedErrors > 0) {
        // IGNORE: 维护自身的留痕失败不改判定——清理已经完成且统计已如实返回；
        // 这里返回 false 会让调用方以为"没清掉"，而事实上已经清掉了。
        std::string maintAuditErr;
        audit_.log(actor, "system.maintenance", std::string(), statsJson, maintAuditErr);
    }
    return true;
}

bool Platform::backupCreate(std::string& outPath, std::string& err) {
    std::lock_guard lock(mutex_);
    // VACUUM INTO 生成一致性好、含 WAL 已提交数据的独立快照文件。
    // 名字粒度是**秒**，而 VACUUM INTO 遇到已存在的输出文件会直接失败
    // （"output file already exists"）——同一秒内连点两次"立即备份"就会撞名。
    // 撞名时补一个短后缀保证唯一（用户视角是"第二次备份也能成功"，而不是一句
    // 语焉不详的 SQLite 报错）。
    std::string name = "platform-" + nowIso() + ".db";
    for (auto& ch : name)
        if (ch == ':') ch = '-';
    auto pathFor = [&](const std::string& n) {
        return (fs::path(home_dir_) / "backup" / n).string();
    };
    std::error_code ec;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const std::string candidate =
            attempt == 0 ? name
                         : name.substr(0, name.size() - 3) + "-" + randomHex(3) + ".db";
        const std::string candidatePath = pathFor(candidate);
        if (fs::exists(candidatePath, ec)) continue;
        outPath = candidatePath;
        name = candidate;
        break;
    }
    if (outPath.empty()) {
        err = "cannot allocate a unique backup file name";
        return false;
    }
    std::string escaped;
    for (char ch : outPath) {
        escaped += ch;
        if (ch == '\'') escaped += '\'';
    }
    if (!db_.execScript("VACUUM INTO '" + escaped + "';", err)) return false;
    // IGNORE: 元操作留痕——备份**文件已经落盘**，此时审计失败再返回 false 会让调用方
    // 以为"没备份成功"（实际有），反而诱导重复备份；失败原因保留在局部变量里。
    // 备份文件本身在 backup/ 目录可自证，不依赖审计行。
    std::string backupAuditErr;
    audit_.log("zcode", "system.backup", name, nlohmann::json{{"file", name}}.dump(),
               backupAuditErr);
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
    // 恢复前先看备份文件的库结构版本：与 bootstrap 同一道门——来自更新版本的备份
    // 拒绝恢复（覆盖当前库之后本程序无法正确解读它，等于把好库换成读不了的库）。
    {
        sqlite3* probe = nullptr;
        if (sqlite3_open_v2(src.string().c_str(), &probe, SQLITE_OPEN_READONLY, nullptr) !=
            SQLITE_OK) {
            err = "cannot open backup file: " + name;
            if (probe) sqlite3_close(probe);
            return false;
        }
        int64_t backupVersion = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(probe, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK &&
            st != nullptr && sqlite3_step(st) == SQLITE_ROW)
            backupVersion = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        sqlite3_close(probe);
        if (backupVersion > kSchemaVersion) {
            err = "backup was created by a newer MiderHive (schema version " +
                  std::to_string(backupVersion) + " > " + std::to_string(kSchemaVersion) +
                  "); refusing to restore";
            return false;
        }
    }
    // 关闭连接（自动 checkpoint WAL）→ 覆盖 → 重开
    //
    // 这一段是"库已关、必须重开"的危险窗口：任何一步失败，如果就这么返回，调用方
    // 会继续拿着一个 db_ 已关闭的 Platform 跑——面板刷新、心跳、HTTP 全部静默失效，
    // 而界面上只有一个"恢复失败"的提示。因此在窗口内失败时按优先级做两件事：
    //   1) 尝试把旧的库文件放回去并重开（补偿），失败也无妨，只要重新 open 成功；
    //   2) 都失败就把平台标记为不可用（usable_ = false），HTTP 统一 503、界面强制提示重启。
    const fs::path livePath = fs::path(home_dir_) / "platform.db";
    const fs::path prePath = fs::path(home_dir_) / "platform.db.before-restore";
    const fs::path prevPath = fs::path(home_dir_) / "platform.db.before-restore.previous";
    std::error_code ec;
    // 顺序是硬约束：**必须先关连接**。Windows 上 SQLite 还持有 platform.db 句柄时
    // fs::rename 会直接失败（实测："The process cannot access the file because it is
    // being used by another process"），而关连接同时会把 WAL checkpoint 回主库。
    db_.close();
    // 旧库改名留底：copy_file(overwrite) 中途失败会留下半截文件，那时旧库还在
    // .before-restore 里，能原样放回去。
    //
    // 留底轮转（而不是删除）：prePath 若已存在，说明上一次恢复失败且补偿也没成功
    // （usable_=false 那条路径会**故意**留下它）——那是用户原始数据的最后一份救援副本。
    // 用户重启后重试恢复是最常见的动作，无条件 fs::remove(prePath) 会把这份副本静默
    // 删掉。因此先把旧留底转存为 .previous（保持至多两代：本次 + 上次），再留新的底。
    fs::remove(prevPath, ec);   // 丢弃更老的一代（保底两代，不无限增长）
    ec.clear();
    fs::rename(prePath, prevPath, ec);  // prePath 不存在属正常（上次恢复成功已清理）
    ec.clear();
    fs::rename(livePath, prePath, ec);
    if (ec) {
        // 连留底都做不到：没动过任何文件，直接把连接开回去即可（平台仍可用）。
        // 注意必须重新注册 vec0——sqlite3_vec_init 是按**连接**生效的，新连接上
        // 不认识 vec0，随后任何语义检索/向量写入都会报 "no such module: vec0"。
        std::string reopenErr;
        if (db_.open(livePath.string(), reopenErr)) {
            sqlite3_vec_init(db_.handle(), nullptr, nullptr);
        } else {
            usable_ = false;
        }
        err = "cannot stash current database: " + ec.message();
        return false;
    }

    auto bail = [&](const std::string& why) -> bool {
        // 关键：先关掉"可能已经打开但不可信"的连接。失败点可能是 open 之后
        // （initStoreLocked 失败），此时句柄还开着——不关就 rename 不动（Windows 占用）
        // 且随后的 open 也会失败，补偿等于白做（实测：平台被误判为不可用）。
        db_.close();
        // 补偿：把旧库放回原位并重开；成功则平台仍可用（数据回到恢复前的样子）。
        // 同时清掉同名的 -wal/-shm：库文件是被整体换掉的，旧 WAL 的头部序列号与新主库
        // 对不上，留着只会让 SQLite 走一次不确定的 WAL 恢复。
        std::error_code rec;
        fs::remove(livePath, rec);
        rec.clear();
        fs::remove(fs::path(livePath.string() + "-wal"), rec);
        fs::remove(fs::path(livePath.string() + "-shm"), rec);
        rec.clear();
        fs::rename(prePath, livePath, rec);
        std::string reopenErr;
        if (!rec && db_.open(livePath.string(), reopenErr)) {
            // sqlite3_vec_init 是按连接生效的：补偿用的新连接必须重新注册 vec0，
            // 否则平台"看起来可用"，而向量写入与语义检索全都报 no such module: vec0。
            sqlite3_vec_init(db_.handle(), nullptr, nullptr);
            err = why + " (your previous database was left untouched)";
            return false;
        }
        usable_ = false;
        // 指明留底位置：这是用户原始数据的手工救援路径，不能让人猜文件在哪
        err = why + "; the data store is now unusable - restart MiderHive "
              "(your previous database was kept as " + prePath.filename().string() + ")";
        return false;
    };

    ec.clear();
    fs::copy_file(src, livePath, fs::copy_options::overwrite_existing, ec);
    if (ec) return bail("copy failed: " + ec.message());
    if (!db_.open(livePath.string(), err)) return bail("cannot reopen database: " + err);
    // 与启动完全同一套初始化：版本门、schema、迁移、vec/FTS。
    // 少了这一步，恢复出来的旧库不会重新打上 user_version——"拒绝更新版本库"那道门
    // 在恢复路径上就是空的（此前只跑了 initVectorStore/initSearchIndex）。
    if (!initStoreLocked(err)) return bail("initializing the restored database failed: " + err);
    // 成功：丢弃**本次**留底。不动 .previous——那可能是更早一次失败留下的救援副本，
    // 用户此刻刚恢复成功，删掉它没有任何收益，轮转逻辑会在下次恢复时自然处理。
    fs::remove(prePath, ec);
    // IGNORE: 元操作留痕——库已被替换并重开，此时审计失败不能再"撤销恢复"
    // （把旧库换回去是更危险的操作）；失败原因留在局部变量里。
    std::string restoreAuditErr;
    audit_.log("zcode", "system.restore", name, nlohmann::json{{"file", name}}.dump(),
               restoreAuditErr);
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

Diagnostics Platform::diagnostics() {
    // 全程持锁：探测只读（临时探针文件写完即删），避免并发下互相踩踏
    std::lock_guard lock(mutex_);
    Diagnostics d;
    d.home_dir = home_dir_;
    d.store_unusable = !usable_;
    d.port = http_ ? http_->port() : 0;
    d.http_running = http_ && http_->running();

    // 数据目录可写：写探针文件再删（目录不存在则先补建——bootstrap 本应建好）
    {
        std::error_code ec;
        fs::create_directories(home_dir_, ec);
        const auto probe = fs::path(home_dir_) / ".diag-probe";
        bool ok = false;
        {
            std::ofstream out(probe, std::ios::trunc);
            ok = static_cast<bool>(out);
        }
        if (ok) fs::remove(probe, ec);
        d.home_writable = ok;
    }

    // 数据库可读且核心表存在（bootstrapped_ 之外再做一次实际查询，防"假在跑"）
    {
        std::string qerr;
        int64_t n = -1;
        const bool ok = db_.query("SELECT COUNT(*) FROM settings", nullptr,
                                  [&](Stmt& st) { n = st.i64(0); }, qerr);
        d.db_ok = bootstrapped_ && ok && n >= 0;
    }

    // 明文密钥缓存：可读性 + 已注册 Agent 的条目覆盖（缺失=密钥不可恢复，修复=轮换）
    {
        std::vector<AgentInfo> agents;
        std::string aerr;
        if (!agents_.listAgents(agents, aerr)) agents.clear();
        const auto keyPath = fs::path(home_dir_) / "config" / "agents.json";
        nlohmann::json j;
        bool readable = false;
        {
            std::ifstream in(keyPath);
            if (in) {
                j = nlohmann::json::parse(in, nullptr, false);
                readable = !j.is_discarded() && j.is_object();
            }
        }
        d.agents_json_readable = readable;
        for (const auto& a : agents) {
            const bool hasKey = readable && j.contains(a.name) && j[a.name].is_string() &&
                                !j[a.name].get<std::string>().empty();
            if (!hasKey) d.keyfile_missing.push_back(a.name);
        }
    }
    return d;
}

}  // namespace ah
