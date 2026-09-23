#pragma once
// Agent 异步交流：留言 / 提问 / 指派任务（不要求同时在线）。
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/services/in_tx_step.h"
#include "core/types.h"

namespace ah {

class MessageService {
public:
    explicit MessageService(Database& db) : db_(db) {}

    // send 是单条 INSERT（自动提交）；inTx（可空）在该 INSERT 之后、提交之前执行，
    // 返回 false 则整体回滚（调用方用来把审计绑进同一个原子动作）。
    bool send(const std::string& kind, const std::string& sender, const std::string& recipient,
              const std::string& subject, const std::string& body, const std::string& parentUuid,
              Message& out, std::string& err, const InTxStep& inTx = {});
    // viewer 非空时按可见性收敛：广播（recipient 为空）+ 发给 viewer + viewer 发出的。
    // viewer 为空表示进程内 GUI/管理视角，不做收敛。
    bool list(const std::string& recipientFilter, const std::string& kindFilter,
              const std::string& statusFilter, const std::string& sinceIso, int limit,
              std::vector<Message>& out, std::string& err, const std::string& viewer = {});
    bool get(const std::string& uuid, Message& out, std::string& err);
    // 两步（写回复 + 标父消息已读）同事务；inTx 在提交前执行
    bool reply(const std::string& parentUuid, const std::string& sender, const std::string& body,
               Message& out, std::string& err, const InTxStep& inTx = {});
    // 状态流转（调用方先做权限校验）：
    //   note/question: unread -> read
    //   task: pending -> accepted/declined ; accepted -> done
    // inTx（可空）在提交前执行，返回 false 则整体回滚
    bool setStatus(const std::string& uuid, const std::string& newStatus, Message& out,
                   std::string& err, const InTxStep& inTx = {});

private:
    Database& db_;
};

}  // namespace ah
