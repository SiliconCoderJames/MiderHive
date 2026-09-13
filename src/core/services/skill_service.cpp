#include "core/services/skill_service.h"

#include "core/util.h"

namespace ah {

bool SkillService::registerSkill(const std::string& name, const std::string& displayName,
                                 const std::string& description, const std::string& category,
                                 const std::string& ownerAgent, const std::string& paramSchema,
                                 SkillInfo& out, std::string& err) {
    if (isRegistered(name)) { err = "skill already registered: " + name; return false; }
    std::string now = nowIso();
    if (!db_.query(
            "INSERT INTO skills(name, display_name, description, category, owner_agent, param_schema, "
            "version, status, created_at, updated_at) VALUES (?,?,?,?,?,?,1,'active',?,?)",
            [&](Stmt& st) {
                st.bind(1, name);
                st.bind(2, displayName);
                st.bind(3, description);
                st.bind(4, category);
                st.bind(5, ownerAgent);
                st.bind(6, paramSchema);
                st.bind(7, now);
                st.bind(8, now);
            },
            nullptr, err))
        return false;
    return get(name, out, err);
}

bool SkillService::get(const std::string& name, SkillInfo& out, std::string& err) {
    bool found = false;
    bool ok = db_.query(
        "SELECT name, display_name, description, category, owner_agent, param_schema, version, status, "
        "created_at, updated_at FROM skills WHERE name=?",
        [&](Stmt& st) { st.bind(1, name); },
        [&](Stmt& st) {
            out.name = st.text(0);
            out.display_name = st.isNull(1) ? std::string() : st.text(1);
            out.description = st.text(2);
            out.category = st.isNull(3) ? std::string() : st.text(3);
            out.owner_agent = st.text(4);
            out.param_schema = st.text(5);
            out.version = static_cast<int>(st.i64(6));
            out.status = st.text(7);
            out.created_at = st.text(8);
            out.updated_at = st.text(9);
            found = true;
        },
        err);
    if (!ok) return false;
    if (!found) { err = "skill not found: " + name; return false; }
    return true;
}

bool SkillService::list(const std::string& categoryFilter, const std::string& ownerFilter,
                        std::vector<SkillInfo>& out, std::string& err) {
    std::string sql =
        "SELECT name, display_name, description, category, owner_agent, param_schema, version, status, "
        "created_at, updated_at FROM skills WHERE 1=1";
    if (!categoryFilter.empty()) sql += " AND category = ?";
    if (!ownerFilter.empty()) sql += " AND owner_agent = ?";
    sql += " ORDER BY name";
    int idx = 1;
    out.clear();
    return db_.query(
        sql,
        [&](Stmt& st) {
            if (!categoryFilter.empty()) st.bind(idx++, categoryFilter);
            if (!ownerFilter.empty()) st.bind(idx++, ownerFilter);
        },
        [&](Stmt& st) {
            SkillInfo s;
            s.name = st.text(0);
            s.display_name = st.isNull(1) ? std::string() : st.text(1);
            s.description = st.text(2);
            s.category = st.isNull(3) ? std::string() : st.text(3);
            s.owner_agent = st.text(4);
            s.param_schema = st.text(5);
            s.version = static_cast<int>(st.i64(6));
            s.status = st.text(7);
            s.created_at = st.text(8);
            s.updated_at = st.text(9);
            out.push_back(std::move(s));
        },
        err);
}

bool SkillService::isRegistered(const std::string& name) {
    std::string err;
    bool found = false;
    db_.query("SELECT 1 FROM skills WHERE name=?",
              [&](Stmt& st) { st.bind(1, name); },
              [&](Stmt&) { found = true; }, err);
    return found;
}

bool SkillService::recordInvocation(const std::string& skillName, const std::string& caller,
                                    const std::string& paramsJson, const std::string& resultSummary,
                                    const std::string& status, int64_t durationMs,
                                    const std::string& referenceId, std::string& err) {
    return db_.query(
        "INSERT INTO skill_invocations(skill_name, caller_agent, params, result_summary, status, "
        "duration_ms, reference_id, created_at) VALUES (?,?,?,?,?,?,?,?)",
        [&](Stmt& st) {
            st.bind(1, skillName);
            st.bind(2, caller);
            st.bind(3, paramsJson);
            st.bind(4, resultSummary);
            st.bind(5, status);
            st.bind(6, durationMs);
            if (referenceId.empty())
                st.bindNull(7);
            else
                st.bind(7, referenceId);
            st.bind(8, nowIso());
        },
        nullptr, err);
}

bool SkillService::listInvocations(const std::string& skillName, int limit,
                                   std::vector<SkillInvocation>& out, std::string& err) {
    std::string sql =
        "SELECT id, skill_name, caller_agent, params, result_summary, status, duration_ms, "
        "reference_id, created_at FROM skill_invocations";
    if (!skillName.empty()) sql += " WHERE skill_name = ?";
    sql += " ORDER BY id DESC LIMIT ?";
    int idx = 1;
    out.clear();
    return db_.query(
        sql,
        [&](Stmt& st) {
            if (!skillName.empty()) st.bind(idx++, skillName);
            st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 100));
        },
        [&](Stmt& st) {
            SkillInvocation inv;
            inv.id = st.i64(0);
            inv.skill_name = st.text(1);
            inv.caller_agent = st.text(2);
            inv.params = st.text(3);
            inv.result_summary = st.isNull(4) ? std::string() : st.text(4);
            inv.status = st.text(5);
            inv.duration_ms = st.i64(6);
            inv.reference_id = st.isNull(7) ? std::string() : st.text(7);
            inv.created_at = st.text(8);
            out.push_back(std::move(inv));
        },
        err);
}

}  // namespace ah
