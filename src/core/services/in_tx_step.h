#pragma once
// 服务层共享类型：必须在写事务**内部**（提交之前）执行的一步，返回 false = 整体回滚。
//
// 存在的理由（踩过一次）：SQLite 没有真正的嵌套事务——内层 COMMIT 会提交最外层。
// 于是"调用方在外层 BEGIN，服务内部自己 COMMIT"这种写法是错的：外层事务会永远
// 挂着，紧接着的下一条 BEGIN 直接报 "cannot start a transaction within a transaction"。
// 因此"业务写入 + 审计留痕"要原子，就必须让审计成为**服务自身事务里的一步**，
// 而不是在服务提交之后再补一次（那时 rollback 已经是空操作）。
#include <functional>

namespace ah {

using InTxStep = std::function<bool()>;

}  // namespace ah
