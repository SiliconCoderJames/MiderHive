#pragma once
// Agent 注册 / 认证 / 心跳 / 在线状态。
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/types.h"

namespace ah {

class AgentService {
public:
    explicit AgentService(Database& db) : db_(db) {}

    bool registerAgent(const std::string& name, const std::string& role,
                       const std::string& salt, const std::string& keyHash, std::string& err);
    // 取凭据：salt 为空表示旧格式（hash = sha256(明钥)），否则 hash = sha256(salt + 明钥)
    bool credentialOf(const std::string& name, std::string& salt, std::string& hash,
                      std::string& err) const;
    void heartbeat(const std::string& name, const std::string& currentTask);
    bool listAgents(std::vector<AgentInfo>& out, std::string& err);
    // 管理性移除：删除注册行（密钥随之失效）；调用方负责管理者校验与审计
    bool removeAgent(const std::string& name, std::string& err);
    // v4 接入开关：置位/清除 disabled 标志（平台侧拒接语义，密钥与工具配置不动）；
    // 调用方负责管理者校验与审计
    bool setDisabled(const std::string& name, bool disabled, std::string& err);
    // 该身份当前是否被禁用（查询失败按"未禁用"处理，见实现内注释）
    bool isDisabled(const std::string& name) const;
    // 重新生成密钥：保留注册行（审计主体、创建时间、历史统计不变），仅替换 salt 与哈希
    bool rotateKey(const std::string& name, const std::string& salt, const std::string& keyHash,
                   std::string& err);
    bool nameExists(const std::string& name);

private:
    Database& db_;
};

}  // namespace ah
