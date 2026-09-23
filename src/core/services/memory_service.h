#pragma once
// 用户记忆：section + key 定位条目；修改生成新版本，历史完整保留。
#include <functional>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/services/in_tx_step.h"
#include "core/types.h"

namespace ah {

class MemoryService {
public:
    explicit MemoryService(Database& db) : db_(db) {}

    bool list(const std::string& sectionFilter, std::vector<MemoryEntry>& out, std::string& err);
    // baseVersion > 0 时做乐观并发校验：与最新版本不符则报 version conflict（不写入）
    // inTx（可空）在提交之前执行：返回 false 则整体回滚
    bool set(const std::string& author, const std::string& section, const std::string& key,
             const std::string& value, int baseVersion, MemoryEntry& out, std::string& err,
             const InTxStep& inTx = {});
    bool remove(const std::string& section, const std::string& key, int64_t& removed,
                std::string& err, const InTxStep& inTx = {});
    bool history(const std::string& section, const std::string& key,
                 std::vector<MemoryEntry>& out, std::string& err);

private:
    Database& db_;
};

}  // namespace ah
