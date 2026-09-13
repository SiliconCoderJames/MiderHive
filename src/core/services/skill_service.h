#pragma once
// 技能库：先注册后调用；每次调用记录到 skill_invocations。
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/types.h"

namespace ah {

class SkillService {
public:
    explicit SkillService(Database& db) : db_(db) {}

    bool registerSkill(const std::string& name, const std::string& displayName,
                       const std::string& description, const std::string& category,
                       const std::string& ownerAgent, const std::string& paramSchema,
                       SkillInfo& out, std::string& err);
    bool get(const std::string& name, SkillInfo& out, std::string& err);
    bool list(const std::string& categoryFilter, const std::string& ownerFilter,
              std::vector<SkillInfo>& out, std::string& err);
    bool isRegistered(const std::string& name);
    bool recordInvocation(const std::string& skillName, const std::string& caller,
                          const std::string& paramsJson, const std::string& resultSummary,
                          const std::string& status, int64_t durationMs,
                          const std::string& referenceId, std::string& err);
    bool listInvocations(const std::string& skillName, int limit,
                         std::vector<SkillInvocation>& out, std::string& err);

private:
    Database& db_;
};

}  // namespace ah
