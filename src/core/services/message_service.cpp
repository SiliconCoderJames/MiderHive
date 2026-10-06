#include "core/services/message_service.h"

#include "core/util.h"

namespace ah {

bool MessageService::send(const std::string& kind, const std::string& sender,
                          const std::string& recipient, const std::string& subject,
                          const std::string& body, const std::string& parentUuid, Message& out,
                          std::string& err, const InTxStep& inTx) {
    if (kind != "note" && kind != "question" && kind != "task") {
        err = "invalid message kind: " + kind;
        return false;
    }
    std::string uuid = uuid4();
    std::string initialStatus = kind == "task" ? "pending" : "unread";
    const std::string createdAt = nowIso();
    // inTx 要求"写入 + 该步"原子，因此事务必须**包住 INSERT 本身**：
    // 先 INSERT 再 BEGIN 是错的——INSERT 在自动提交下已经落库，之后的 rollback 是空操作。
    if (inTx && !db_.beginImmediate(err)) return false;
    if (!db_.query(
            "INSERT INTO messages(uuid, kind, sender, recipient, subject, body, status, parent_uuid, created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            [&](Stmt& st) {
                st.bind(1, uuid);
                st.bind(2, kind);
                st.bind(3, sender);
                // 空 recipient = 广播：按 schema 约定写 NULL（此前写空串，
                // 导致所有 recipient IS NULL 的查询都匹配不到广播消息）
                if (recipient.empty())
                    st.bindNull(4);
                else
                    st.bind(4, recipient);
                st.bind(5, subject);
                st.bind(6, body);
                st.bind(7, initialStatus);
                st.bind(8, parentUuid);
                st.bind(9, createdAt);
            },
            nullptr, err)) {
        if (inTx) db_.rollback();
        return false;
    }
    // 先填好返回值再执行事务内最后一步：调用方（审计）以 out.uuid 作留痕对象
    // （范式与 memory_service::set 一致；末尾的 get() 仍会回填完整字段）
    out.uuid = uuid;
    out.kind = kind;
    out.sender = sender;
    out.recipient = recipient;
    out.subject = subject;
    out.body = body;
    out.status = initialStatus;
    out.parent_uuid = parentUuid;
    out.created_at = createdAt;
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

bool MessageService::list(const std::string& recipientFilter, const std::string& kindFilter,
                          const std::string& statusFilter, const std::string& sinceIso, int limit,
                          std::vector<Message>& out, std::string& err, const std::string& viewer) {
    std::string sql =
        "SELECT id, uuid, kind, sender, recipient, subject, body, status, parent_uuid, created_at "
        "FROM messages WHERE 1=1";
    // 广播消息的历史行存的是空串、新行存 NULL，两种都要认
    if (!recipientFilter.empty())
        sql += " AND (recipient IS NULL OR recipient = '' OR recipient = ?)";
    if (!kindFilter.empty()) sql += " AND kind = ?";
    if (!statusFilter.empty()) sql += " AND status = ?";
    if (!sinceIso.empty()) sql += " AND created_at >= ?";
    // 可见性收敛：点对点消息只对收件人/发件人可见（广播对所有人可见）
    if (!viewer.empty())
        sql += " AND (recipient IS NULL OR recipient = '' OR recipient = ? OR sender = ?)";
    sql += " ORDER BY id DESC LIMIT ?";
    int idx = 1;
    out.clear();
    return db_.query(
        sql,
        [&](Stmt& st) {
            if (!recipientFilter.empty()) st.bind(idx++, recipientFilter);
            if (!kindFilter.empty()) st.bind(idx++, kindFilter);
            if (!statusFilter.empty()) st.bind(idx++, statusFilter);
            if (!sinceIso.empty()) st.bind(idx++, sinceIso);
            if (!viewer.empty()) {
                st.bind(idx++, viewer);
                st.bind(idx++, viewer);
            }
            st.bind(idx, static_cast<int64_t>(limit > 0 ? limit : 100));
        },
        [&](Stmt& st) {
            Message m;
            m.id = st.i64(0);
            m.uuid = st.text(1);
            m.kind = st.text(2);
            m.sender = st.text(3);
            m.recipient = st.isNull(4) ? std::string() : st.text(4);
            m.subject = st.isNull(5) ? std::string() : st.text(5);
            m.body = st.text(6);
            m.status = st.text(7);
            m.parent_uuid = st.isNull(8) ? std::string() : st.text(8);
            m.created_at = st.text(9);
            out.push_back(std::move(m));
        },
        err);
}

bool MessageService::get(const std::string& uuid, Message& out, std::string& err) {
    bool found = false;
    bool ok = db_.query(
        "SELECT id, uuid, kind, sender, recipient, subject, body, status, parent_uuid, created_at "
        "FROM messages WHERE uuid=?",
        [&](Stmt& st) { st.bind(1, uuid); },
        [&](Stmt& st) {
            out.id = st.i64(0);
            out.uuid = st.text(1);
            out.kind = st.text(2);
            out.sender = st.text(3);
            out.recipient = st.isNull(4) ? std::string() : st.text(4);
            out.subject = st.isNull(5) ? std::string() : st.text(5);
            out.body = st.text(6);
            out.status = st.text(7);
            out.parent_uuid = st.isNull(8) ? std::string() : st.text(8);
            out.created_at = st.text(9);
            found = true;
        },
        err);
    if (!ok) return false;
    if (!found) { err = "message not found: " + uuid; return false; }
    return true;
}

bool MessageService::reply(const std::string& parentUuid, const std::string& sender,
                           const std::string& body, Message& out, std::string& err,
                           const InTxStep& inTx) {
    Message parent;
    if (!get(parentUuid, parent, err)) return false;
    std::string recipient = parent.sender == sender ? parent.recipient : parent.sender;
    // "写入回复"与"把父消息标记为已读"是同一个用户可见动作的两步：必须同事务。
    // 此前是两次独立写入，第二步的返回值还被丢弃——回复写成功、标已读失败时，
    // 系统停在一个既没报错也没完成的半截状态。
    //
    // 说明：这里用嵌套事务（Platform::messageReply 在外面还包了一层用于覆盖审计）。
    // SQLite 不支持真正的嵌套事务，但 COMMIT 会延迟到最外层——因此内层的 commit
    // 只是"不提前提交"，语义正确。本函数若被单独调用（无外层事务），这一层就是
    // 真正的边界，行为不变。
    if (!db_.beginImmediate(err)) return false;
    if (!send("note", sender, recipient,
              std::string("Re: ") + (parent.subject.empty() ? "" : parent.subject), body, parentUuid,
              out, err)) {
        db_.rollback();
        return false;
    }
    if (!db_.query("UPDATE messages SET status='read' WHERE uuid=? AND kind!='task'",
                   [&](Stmt& st) { st.bind(1, parentUuid); }, nullptr, err)) {
        db_.rollback();
        return false;
    }
    // 提交前的最后一步（审计留痕）
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
bool MessageService::setStatus(const std::string& uuid, const std::string& newStatus, Message& out,
                               std::string& err, const InTxStep& inTx) {
    // 读旧状态 → 校验状态机 → 写新状态，全过程包在同一个 BEGIN IMMEDIATE 里：
    // 校验若基于事务外的旧读取，GUI 与 platformd 双进程并发流转同一条消息时，
    // 后提交的一方会按"过期校验"落库非法流转（如 accepted 之后改成 declined，
    // 而 declined 是终态，再无 API 能修复）。单进程有 Platform 大锁，跨进程只有
    // 事务能兜住（同族加固见 memory_service::set / knowledge_service::addVersion）。
    if (!db_.beginImmediate(err)) return false;
    Message cur;
    if (!get(uuid, cur, err)) {
        db_.rollback();
        return false;
    }

    bool allowed = false;
    if (cur.kind == "task") {
        if (cur.status == "pending" && (newStatus == "accepted" || newStatus == "declined"))
            allowed = true;
        else if (cur.status == "accepted" && newStatus == "done")
            allowed = true;
    } else {
        if (cur.status == "unread" && newStatus == "read") allowed = true;
    }
    if (!allowed) {
        err = "illegal status transition: " + cur.kind + " " + cur.status + " -> " + newStatus;
        db_.rollback();
        return false;
    }
    if (!db_.query("UPDATE messages SET status=? WHERE uuid=?",
                   [&](Stmt& st) { st.bind(1, newStatus); st.bind(2, uuid); }, nullptr, err)) {
        db_.rollback();
        return false;
    }
    // 提交前的最后一步（调用方注入，例如审计留痕）：失败即整体回滚
    if (inTx && !inTx()) {
        db_.rollback();
        if (err.empty()) err = "in-transaction step failed";
        return false;
    }
    if (!db_.commit(err)) {
        db_.rollback();
        return false;
    }
    return get(uuid, out, err);
}

}  // namespace ah
