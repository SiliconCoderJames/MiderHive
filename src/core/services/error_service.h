#pragma once
// 错误日志：报错必须记录；解决时只追加说明，不覆盖原始内容。
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/services/in_tx_step.h"
#include "core/types.h"

namespace ah {

class ErrorService {
public:
    explicit ErrorService(Database& db) : db_(db) {}

    // report 是单条 INSERT；inTx（可空）使它带事务：INSERT 与该步同生同灭。
    // 注意报告必须是原子的——"错误记录了但没有留痕"违背协作规则。
    bool report(const std::string& reporter, const std::string& severity, const std::string& source,
                const std::string& title, const std::string& detail, const std::string& stackTrace,
                ErrorReport& out, std::string& err, const InTxStep& inTx = {});
    bool list(const std::string& statusFilter, const std::string& severityFilter, int limit,
              std::vector<ErrorReport>& out, std::string& err);
    bool get(const std::string& uuid, ErrorReport& out, std::string& err);
    // resolve 是"读旧说明 → 合并 → 写回"三步，必须整体原子：除了审计留痕（inTx），
    // 也封住并发窗口——两个进程同时 resolve 同一条时，后写者会覆盖先写者的说明。
    bool resolve(const std::string& uuid, const std::string& actor, const std::string& notes,
                 ErrorReport& out, std::string& err, const InTxStep& inTx = {});

private:
    Database& db_;
};

}  // namespace ah
