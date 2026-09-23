#include "core/services/memory_service.h"

#include "core/util.h"

namespace ah {

bool MemoryService::list(const std::string& sectionFilter, std::vector<MemoryEntry>& out,
                         std::string& err) {
    std::string sql =
        "SELECT section, key, value, author, version, created_at FROM memory_entries WHERE is_latest=1";
    if (!sectionFilter.empty()) sql += " AND section = ?";
    sql += " ORDER BY section, key";
    out.clear();
    return db_.query(
        sql,
        [&](Stmt& st) {
            if (!sectionFilter.empty()) st.bind(1, sectionFilter);
        },
        [&](Stmt& st) {
            MemoryEntry m;
            m.section = st.text(0);
            m.key = st.text(1);
            m.value = st.text(2);
            m.author = st.text(3);
            m.version = static_cast<int>(st.i64(4));
            m.created_at = st.text(5);
            out.push_back(std::move(m));
        },
        err);
}

bool MemoryService::set(const std::string& author, const std::string& section, const std::string& key,
                        const std::string& value, int baseVersion, MemoryEntry& out,
                        std::string& err, const InTxStep& inTx) {
    // BEGIN IMMEDIATE 包裹查-改-插：GUI 与 platformd 双进程并发写同库时，
    // 不会产生两条 is_latest=1 的同版本记录（进程内另有 Platform 大锁）
    if (!db_.beginImmediate(err)) return false;

    int64_t latestId = 0;
    int latestVersion = 0;
    bool found = false;
    if (!db_.query(
            "SELECT id, version FROM memory_entries WHERE section=? AND key=? AND is_latest=1",
            [&](Stmt& st) { st.bind(1, section); st.bind(2, key); },
            [&](Stmt& st) { latestId = st.i64(0); latestVersion = static_cast<int>(st.i64(1)); found = true; },
            err)) {
        db_.rollback();
        return false;
    }

    // 乐观并发：调用方基于旧版本写入时拒绝，避免静默覆盖他人更新
    if (baseVersion > 0 && (!found || latestVersion != baseVersion)) {
        err = "version conflict: expected base v" + std::to_string(baseVersion) +
              ", latest is v" + std::to_string(found ? latestVersion : 0);
        db_.rollback();
        return false;
    }

    int newVersion = found ? latestVersion + 1 : 1;
    if (found) {
        // 仅翻转版本标记，旧内容原样保留（append-only）
        if (!db_.query("UPDATE memory_entries SET is_latest=0 WHERE id=?",
                       [&](Stmt& st) { st.bind(1, latestId); }, nullptr, err)) {
            db_.rollback();
            return false;
        }
    }
    if (!db_.query(
            "INSERT INTO memory_entries(section, key, value, author, version, is_latest, created_at) "
            "VALUES (?,?,?,?,?,1,?)",
            [&](Stmt& st) {
                st.bind(1, section);
                st.bind(2, key);
                st.bind(3, value);
                st.bind(4, author);
                st.bind(5, static_cast<int64_t>(newVersion));
                st.bind(6, nowIso());
            },
            nullptr, err)) {
        db_.rollback();
        return false;
    }

    // 先填好返回值再执行事务内最后一步：调用方（审计）需要新版本号
    out.section = section;
    out.key = key;
    out.value = value;
    out.author = author;
    out.version = newVersion;
    out.created_at = nowIso();

    if (inTx && !inTx()) {
        db_.rollback();
        if (err.empty()) err = "in-transaction step failed";
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    return true;
}

bool MemoryService::remove(const std::string& section, const std::string& key, int64_t& removed,
                           std::string& err, const InTxStep& inTx) {
    removed = 0;
    // 事务包裹计数与删除：GUI 与 platformd 双进程并发写同库时，
    // COUNT 与 DELETE 两步之间可能被插入新版本行，
    // 令返回的 removed 与实际删除数不一致（该数字会写进审计 versions_removed）
    if (!db_.beginImmediate(err)) return false;
    if (!db_.query("SELECT COUNT(*) FROM memory_entries WHERE section=? AND key=?",
                   [&](Stmt& st) { st.bind(1, section); st.bind(2, key); },
                   [&](Stmt& st) { removed = st.i64(0); }, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.query("DELETE FROM memory_entries WHERE section=? AND key=?",
                   [&](Stmt& st) { st.bind(1, section); st.bind(2, key); }, nullptr, err)) {
        db_.rollback();
        return false;
    }
    // 提交前的最后一步（审计需要 removed 计数，因此必须在计数之后、提交之前）
    if (inTx && !inTx()) {
        db_.rollback();
        if (err.empty()) err = "in-transaction step failed";
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    return true;
}

bool MemoryService::history(const std::string& section, const std::string& key,
                            std::vector<MemoryEntry>& out, std::string& err) {
    out.clear();
    return db_.query(
        "SELECT section, key, value, author, version, created_at FROM memory_entries "
        "WHERE section=? AND key=? ORDER BY version DESC",
        [&](Stmt& st) { st.bind(1, section); st.bind(2, key); },
        [&](Stmt& st) {
            MemoryEntry m;
            m.section = st.text(0);
            m.key = st.text(1);
            m.value = st.text(2);
            m.author = st.text(3);
            m.version = static_cast<int>(st.i64(4));
            m.created_at = st.text(5);
            out.push_back(std::move(m));
        },
        err);
}

}  // namespace ah
