#pragma once
// 操作日志：每次操作记录身份 + 时间 + 动作 + 对象 + 内容摘要。
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/types.h"

namespace ah {

class AuditService {
public:
    explicit AuditService(Database& db) : db_(db) {}

    bool log(const std::string& actor, const std::string& action, const std::string& target,
             const std::string& detailJson, std::string& err);
    // viewer 非空时只返回 actor == viewer 的行（普通 Agent 的可见性收敛）；
    // viewer 为空 = 进程内 GUI/管理视角，看全量
    bool list(const std::string& actorFilter, const std::string& actionFilter,
              const std::string& sinceIso, int limit, std::vector<AuditRecord>& out, std::string& err,
              const std::string& viewer = {});

private:
    Database& db_;
};

}  // namespace ah
