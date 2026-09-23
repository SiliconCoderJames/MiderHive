#include "core/services/error_service.h"

#include "core/util.h"

namespace ah {

bool ErrorService::report(const std::string& reporter, const std::string& severity,
                          const std::string& source, const std::string& title,
                          const std::string& detail, const std::string& stackTrace,
                          ErrorReport& out, std::string& err, const InTxStep& inTx) {
    std::string uuid = uuid4();
    // 需要注入步骤（审计）时才开事务：单条 INSERT 本身已是原子的
    if (inTx && !db_.beginImmediate(err)) return false;
    if (!db_.query(
            "INSERT INTO errors(uuid, reporter, severity, source, title, detail, stack_trace, status, created_at) "
            "VALUES (?,?,?,?,?,?,?,'open',?)",
            [&](Stmt& st) {
                st.bind(1, uuid);
                st.bind(2, reporter);
                st.bind(3, severity);
                st.bind(4, source);
                st.bind(5, title);
                st.bind(6, detail);
                st.bind(7, stackTrace);
                st.bind(8, nowIso());
            },
            nullptr, err)) {
        if (inTx) db_.rollback();
        return false;
    }
    if (inTx) {
        if (!inTx()) {
            db_.rollback();
            if (err.empty()) err = "in-transaction step failed";
            return false;
        }
        if (!db_.commit(err)) {
            db_.rollback();
            return false;
        }
    }
    return get(uuid, out, err);
}

bool ErrorService::list(const std::string& statusFilter, const std::string& severityFilter, int limit,
                        std::vector<ErrorReport>& out, std::string& err) {
    std::string sql =
        "SELECT id, uuid, reporter, severity, source, title, detail, stack_trace, status, "
        "resolution_notes, resolved_by, created_at, resolved_at FROM errors WHERE 1=1";
    if (!statusFilter.empty()) sql += " AND status = ?";
    if (!severityFilter.empty()) sql += " AND severity = ?";
    sql += " ORDER BY id DESC LIMIT ?";
    int idx = 1;
    out.clear();
    return db_.query(
        sql,
        [&](Stmt& st) {
            if (!statusFilter.empty()) st.bind(idx++, statusFilter);
            if (!severityFilter.empty()) st.bind(idx++, severityFilter);
            st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 200));
        },
        [&](Stmt& st) {
            ErrorReport e;
            e.id = st.i64(0);
            e.uuid = st.text(1);
            e.reporter = st.text(2);
            e.severity = st.text(3);
            e.source = st.isNull(4) ? std::string() : st.text(4);
            e.title = st.text(5);
            e.detail = st.text(6);
            e.stack_trace = st.isNull(7) ? std::string() : st.text(7);
            e.status = st.text(8);
            e.resolution_notes = st.isNull(9) ? std::string() : st.text(9);
            e.resolved_by = st.isNull(10) ? std::string() : st.text(10);
            e.created_at = st.text(11);
            e.resolved_at = st.isNull(12) ? std::string() : st.text(12);
            out.push_back(std::move(e));
        },
        err);
}

bool ErrorService::get(const std::string& uuid, ErrorReport& out, std::string& err) {
    bool found = false;
    bool ok = db_.query(
        "SELECT id, uuid, reporter, severity, source, title, detail, stack_trace, status, "
        "resolution_notes, resolved_by, created_at, resolved_at FROM errors WHERE uuid=?",
        [&](Stmt& st) { st.bind(1, uuid); },
        [&](Stmt& st) {
            out.id = st.i64(0);
            out.uuid = st.text(1);
            out.reporter = st.text(2);
            out.severity = st.text(3);
            out.source = st.isNull(4) ? std::string() : st.text(4);
            out.title = st.text(5);
            out.detail = st.text(6);
            out.stack_trace = st.isNull(7) ? std::string() : st.text(7);
            out.status = st.text(8);
            out.resolution_notes = st.isNull(9) ? std::string() : st.text(9);
            out.resolved_by = st.isNull(10) ? std::string() : st.text(10);
            out.created_at = st.text(11);
            out.resolved_at = st.isNull(12) ? std::string() : st.text(12);
            found = true;
        },
        err);
    if (!ok) return false;
    if (!found) { err = "error not found: " + uuid; return false; }
    return true;
}

bool ErrorService::resolve(const std::string& uuid, const std::string& actor, const std::string& notes,
                           ErrorReport& out, std::string& err) {
    // 解决说明是追加而非覆盖
    std::string merged = notes;
    bool ok = db_.query(
        "SELECT resolution_notes, status FROM errors WHERE uuid=?",
        [&](Stmt& st) { st.bind(1, uuid); },
        [&](Stmt& st) {
            if (!st.isNull(0) && !st.text(0).empty()) merged = st.text(0) + "\n---\n" + notes;
        },
        err);
    if (!ok) return false;
    ok = db_.query(
        "UPDATE errors SET status='resolved', resolved_by=?, resolved_at=?, resolution_notes=? WHERE uuid=?",
        [&](Stmt& st) {
            st.bind(1, actor);
            st.bind(2, nowIso());
            st.bind(3, merged);
            st.bind(4, uuid);
        },
        nullptr, err);
    if (!ok) return false;
    return get(uuid, out, err);
}

}  // namespace ah
