#include "core/services/audit_service.h"

#include "core/util.h"

namespace ah {

bool AuditService::log(const std::string& actor, const std::string& action, const std::string& target,
                       const std::string& detailJson, std::string& err) {
    return db_.query(
        "INSERT INTO audit_log(actor, action, target, detail, created_at) VALUES (?,?,?,?,?)",
        [&](Stmt& st) {
            st.bind(1, actor);
            st.bind(2, action);
            st.bind(3, target);
            st.bind(4, detailJson);
            st.bind(5, nowIso());
        },
        nullptr, err);
}

bool AuditService::list(const std::string& actorFilter, const std::string& actionFilter,
                        const std::string& sinceIso, int limit, std::vector<AuditRecord>& out,
                        std::string& err, const std::string& viewer) {
    // 可见性收敛：审计 detail 会携带点对点消息的收件人/主题等元数据，全量开放
    // 等于绕过消息的可见性收敛。viewer 非空时强制只看自己作为主体的行
    // （此时 actorFilter 一并按 viewer 收敛——查别人直接得到空结果而非报错）。
    const std::string effectiveActor = viewer.empty() ? actorFilter : viewer;
    std::string sql =
        "SELECT id, actor, action, target, detail, created_at FROM audit_log WHERE 1=1";
    if (!effectiveActor.empty()) sql += " AND actor = ?";
    if (!actionFilter.empty()) sql += " AND action = ?";
    if (!sinceIso.empty()) sql += " AND created_at >= ?";
    sql += " ORDER BY id DESC LIMIT ?";
    int idx = 1;
    auto bind = [&](Stmt& st) {
        if (!effectiveActor.empty()) st.bind(idx++, effectiveActor);
        if (!actionFilter.empty()) st.bind(idx++, actionFilter);
        if (!sinceIso.empty()) st.bind(idx++, sinceIso);
        st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 100));
    };
    out.clear();
    return db_.query(
        sql, bind,
        [&](Stmt& st) {
            AuditRecord r;
            r.id = st.i64(0);
            r.actor = st.text(1);
            r.action = st.text(2);
            r.target = st.text(3);
            r.detail = st.text(4);
            r.created_at = st.text(5);
            out.push_back(std::move(r));
        },
        err);
}

}  // namespace ah
