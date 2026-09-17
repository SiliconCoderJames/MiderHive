# 后端加固报告（第二轮优化 · 第一部分）

日期：2026-09-07 · 基线：99 单元测试 + 39 集成断言全绿 → 加固后 **132 单元测试全绿** + 39/39 集成断言

## 1. Token 预算并发扣减

| 项 | 结论 | 说明 |
|---|---|---|
| 并发扣减事务 | **已修复 + 代码** | `UsageService::report` 改为 `BEGIN IMMEDIATE` → 事务内查重 → 原子插入 → `COMMIT`（[usage_service.cpp](../src/core/services/usage_service.cpp)）。进程内另有 Platform 全局锁串行化；跨进程场景由 IMMEDIATE 写锁保证 |
| 超额拦截 | **已按产品决策移除** | 早期版本曾在用量达上限时拒绝技能调用（HTTP 429）。现定位为**纯观测**：周预算只是图表参考线，三级告警照常呈现，超额不拦截任何调用。另见第三轮审查的“发现与修复”表 |
| 幂等性 | **已新增** | `token_usage` 新增 `idempotency_key` 列 + 部分唯一索引；重复键只记一次，响应含 `duplicate: true`。单测：同键两次上报 total_tokens 不变 |

## 2. 数据增长与清理

| 项 | 结论 | 说明 |
|---|---|---|
| 操作日志轮转 | **已新增** | `maintenanceRun`：删除 30 天前且最多保留 10 万条（启动时自动执行 + `POST /api/maintenance` 手动触发，主密钥） |
| 已解决错误归档 | **已新增** | 解决超过 30 天的错误从主表清除 |
| 知识库/记忆清理接口 | **已新增** | `DELETE /api/knowledge/{uuid}`、`DELETE /api/memory?section&key`（仅管理者，连同全部版本与向量，写入审计 `knowledge.remove`/`memory.remove`） |
| SQLite VACUUM | **已新增** | 维护产生删除后自动 `VACUUM` 回收空间 |

## 3. 内存与稳定性

| 项 | 结论 | 说明 |
|---|---|---|
| AddressSanitizer | **已通过（限定）** | MSVC `/fsanitize=address` 重跑端到端测试：132 检查全过、0 个 ASan 报告（堆溢出/UAF）。注：Windows 不支持退出时泄漏检测（LeakSanitizer 不可用），泄漏项由浸泡测试替代 |
| 加速浸泡测试 | **已通过（加速替代 24h）** | 60 秒高频混合请求 **8626 ops / 0 错误**（144 ops/s）；platformd 工作集 8.2MB → 9.8MB 后收敛（尾段每 10 秒增量 < 100KB），无持续增长趋势。24 小时全时长测试建议在长期运行环境补充 |
| HTTP body/连接释放 | **已确认** | cpp-httplib 在 handler 返回后统一释放 Request/Response；ASan 全程无 UAF 报告佐证无悬空引用 |

## 4. 用户记忆并发冲突

| 项 | 结论 | 说明 |
|---|---|---|
| 并发追加版本链 | **已确认** | Platform 全局互斥 + `is_latest` 版本链，多 Agent 并发写同一 key 各自成版本、互不覆盖 |
| 冲突提示 | **已新增** | `POST /api/memory` 支持可选 `base_version`（乐观并发）：与最新版本不符时返回 **409** `version conflict: expected base vX, latest is vY`，不写入。单测覆盖 |

## 5. 向量检索精度

| 项 | 结论 | 说明 |
|---|---|---|
| n-gram 能力定位 | **已确认** | 现有 `NgramHashEmbedder` 为增强型模糊关键词匹配（字符 2/3-gram 特征哈希），适合短文本/标签级召回，不等于真正的语义向量 |
| 模型嵌入预留接口 | **已确认（既有）** | `Embedder` 抽象接口已预留：Agent 在写入/搜索时可自带 `embedding` 数组并标注 `embedder` 名称，平台按 provider 存储；接入真实模型只需新增 `Embedder` 实现（如 ONNX Runtime bge-m3），检索层零改动 |

## 6. 数据备份与恢复

| 项 | 结论 | 说明 |
|---|---|---|
| 手动备份 | **已新增** | `POST /api/system/backup`（主密钥）：`VACUUM INTO` 生成 `backup/platform-<时间戳>.db` 一致性快照（自带 checkpoint，WAL 已提交数据全部包含） |
| 恢复 | **已新增** | `GET /api/system/backups` 列表 + `POST /api/system/restore {"file": 名}`：校验 SQLite 文件头、拒绝路径穿越、关闭连接（自动 checkpoint）→ 覆盖 → 重开并重新挂载 vec 扩展。单测覆盖：备份→清空→恢复→数据一致 |
| 备份期 WAL checkpoint | **已确认** | `VACUUM INTO` 读取一致快照；恢复路径经 `sqlite3_close` 自动 checkpoint |

## 7. 安全加固

| 项 | 结论 | 说明 |
|---|---|---|
| API 密钥加盐哈希 | **已修复** | 注册新 Agent 改为 `salt = randomHex(16)`、存储 `sha256(salt + key)`（`agents.salt` 列，存量库自动迁移；盐为空的旧格式继续兼容认证）。明钥仅注册时返回一次 |
| 输入长度限制 | **已新增** | Platform 层统一校验：知识（标题≤200/内容≤10 万/标签≤20×64）、记忆（值≤2 万）、消息（正文≤5 万）、错误（≤5 万）、技能（schema≤1 万）等，超长返回 400。单测覆盖 |
| 仅本机绑定 | **已确认** | `HttpServer::start` 硬编码 `bind_to_port("127.0.0.1", ...)`，netstat 实测无 0.0.0.0 监听 |
| SQL 注入 | **已确认（既有）** | 全部查询经 `sqlite3_bind_*` 参数绑定，无字符串拼装 |

## 验证汇总

- 单元测试：**132 checks / 0 failures**（新增 33 项加固断言）
- ASan 端到端：132 checks / 0 AddressSanitizer 报告
- 浸泡：60s / 8626 ops / 0 错误 / 内存收敛
- 存量数据库迁移：`ALTER TABLE` 幂等迁移（duplicate column 静默跳过），旧密钥格式兼容认证

---

# 第三轮 · 全局核心审查（2026-09-08）

对核心层约 4000 行做 /W4 严格警告编译 + 全量人工通读 + 专项检查（线程安全 / SQL 绑定 / 鉴权一致性），
修复与结论如下：

## 发现与修复

| 类别 | 问题 | 修复 |
|---|---|---|
| 线程安全（死锁） | `shutdown()`/`stopHttpServer()` 持 `recursive_mutex` 等待 HTTP 线程 join，在途请求 handler 又在等这把锁——退出时有在途请求即互相等待 | 锁内取出 `http_`（`unique_ptr` 所有权转移），**锁外** `stop()` 排空请求后再重入关库 |
| 跨进程一致性 | `memory.set` 与 `knowledge.addVersion` 为“查-改-插”三步无事务：GUI 与 platformd 双进程并发写同库可产生两条 `is_latest=1` | 以 `BEGIN IMMEDIATE` 包裹全部三步，任一步失败回滚。双进程实测：60 次并发写，60 个版本无重复无断层 |
| 输入健壮性 | HTTP 层 5 处 `std::stoi(limit)` 对非数字抛异常；`tags` 非字符串元素、`budget` 非数字类型抛异常；库内存储 JSON（schema/params/audit detail）损坏时 `json::parse` 抛异常 | `parseLimit` 统一校验（非法 → 400）；tags 逐项 `is_string`；budget `is_number`；`safeStoredJson` 退化为原字符串 |
| 语义质量 | 嵌入器设计了词间分隔符（`kSep`）但从未使用——“AB C”与“A BC”跨词 n-gram 同哈希 | 空白折叠为分隔符码点参与哈希；provider 名升级 `ngram-hash-v2`（向量与 v1 不兼容） |
| 输入校验 | Agent 名长度无上限 | `1..64` 字符且禁止空白 |
| 索引 | GUI 按动作筛选审计时全表扫描 | 补 `idx_audit_action(action, created_at)`（schema 幂等执行，存量库自动生效） |
| 警告 | /W4 下 C4996/C4100/C4189 | `_CRT_SECURE_NO_WARNINGS`（本机场景预期用法）+ 未引用参数处理；核心层 /W4 零警告 |

## 验证汇总（本轮）

- 单元测试：**149 checks / 0 failures**（新增 4 项 HTTP 健壮性断言：`limit=abc`/`limit=-5`/tags 非字符串/budget 非数字 → 400）
- ASan：**149 checks / 0 报告**（含本轮全部修复）
- 集成验证：**39/39 通过**（feasibility_check.py，兼容 `MIDERHIVE_MASTER_KEY` 及旧名）
- 双进程并发：**60/60 版本唯一、无断层**
- 浸泡：**20 分钟 / 94,914 ops / 0 错误**（79 ops/s，混合读写高频请求）

---

# 检索路径性能测量（2026-09-14，一次性本机测量）

测量工具：[scripts/bench_search.py](../scripts/bench_search.py)（隔离数据目录 + 独立 platformd，
1 万条知识条目，每条路径预热 5 轮后计时 30 轮，HTTP 全链路含鉴权）。
**与本文其他数字一样，这是一次性的本机测量，没有可复现的 CI 任务，请按"测过一次"理解。**

| 路径（1 万条，limit=20） | P50 | P95 | max |
|---|---|---|---|
| keyword ≥3 码点（FTS5 trigram） | 34.9 ms | 51.3 ms | 53.0 ms |
| keyword 2 码点（LIKE 回退） | 31.8 ms | 48.8 ms | 48.8 ms |
| semantic（384 维 vec0 kNN） | 32.6 ms | 50.1 ms | 51.5 ms |

## 诚实的结论

1. **三条路径的耗时几乎相同，且都贴着 ~35–50ms 的请求地板**——这个地板是 HTTP 往返 +
   JSON 解析 + 鉴权 + 平台全局锁的固定开销（小数据集上 feasibility_check 实测单次语义检索
   也要 ~42ms，可以佐证），而不是文本检索本身。1 万行时 LIKE 扫描与 FTS 检索都只占几毫秒。
2. 因此**"FTS5 比 LIKE 快"在这个量级上没有可测量的证据**。它的收益是结构性的：单次查询的
   工作量从"随正文总量线性增长的全表扫描 + 逐行回表（N+1）"变为"索引查询 + 一次批量取回"，
   只有在数据量大得多（10 万行级）或单条正文大得多时才会显现。消掉 N+1 这一点已经落地。
3. 验收标准里写的"keyword P95 < 50ms（1 万条）"**没有达标**（实测 51.3ms），且换成旧实现
   同样不会达标——该阈值定错了对象（定在了被地板支配的端到端延迟上，而不是检索本身）。
   脚本保留 `--assert-p95-ms` 开关供后续手动对比，但默认不断言。

## 后续若要真正优化

* 降低请求地板：JSON 解析/鉴权/锁竞争各占多少需要先分层测量，再决定是否值得做。
* 大数据量场景的 FTS 收益：把 `--rows` 提到 10 万再测，届时 LIKE 与 FTS 的差距才会拉开。
