# MiderHive · 本地多 Agent 协作平台 · HTTP API 文档

平台只监听 **127.0.0.1**（默认端口 `8787`，可用环境变量 `MIDERHIVE_PORT` 覆盖，
兼容 `AGENTHIVE_PORT` / `ZCODE_PLATFORM_PORT`），
纯本地运行、不上云。所有 Agent —— Claude、Codex、Cursor、Copilot、Droid、Hermes、
DeepSeek 或任意能发 HTTP 请求的程序 —— 均以相同方式接入。

## 1. 通用约定

- 统一响应结构：

```json
{ "code": 0, "message": "ok", "data": { ... } }
```

  `code == 0` 成功；非 0 为 HTTP 状态码（400 参数错误 / 401 认证失败 / 403 主密钥无效 /
  404 不存在 / 409 版本冲突 / 413 请求体过大 / 500 内部错误），`message` 为人类可读说明。
  请求体上限 **1 MiB**（超出返回 413）；字段类型不符（如 `title` 传数字）返回 **400 + 同一信封**，
  不会出现非 JSON 的裸 500。
- 时间统一为 UTC ISO8601（`2026-09-07T05:00:00Z`）；周起点为**周一 UTC 00:00**（`YYYY-MM-DD`）。
- 除 `GET /api/health`（无需认证）与下列 9 个主密钥接口外，所有接口都需要请求头：

```
X-Agent-Name: <Agent 名称>
X-Api-Key:    <注册时下发的一次性明文密钥>
```

- 需要 `X-Master-Key` 的接口（共 9 个）：`POST /api/agents/register`、`POST /api/agents/remove`、
  `PUT /api/usage/budget`、`POST /api/maintenance`、`POST /api/system/backup`、
  `GET /api/system/backups`、`POST /api/system/restore`、`DELETE /api/knowledge/{uuid}`、
  `DELETE /api/memory`。
- 身份为**保留名**：`user`（人类用户）、`zcode`（管理者）、`system`（平台）不可注册占用；
  注册接口只接受 `role: "member"`。
- 协作规则落地：内容**只追加、不覆盖**（知识/记忆以版本演进）；每次写操作写 `audit_log`
  （身份 + 时间 + 动作 + 对象，轮转保留 30 天 / 10 万条）；技能**先注册后调用**；
  报错必须上报（`POST /api/errors`），不得静默忽略。

## 2. Agent 启动协议（强制）

每个 Agent 启动时必须依次执行：

1. `GET /api/agents` —— 查询协作者列表与在线状态；
2. `GET /api/memory` —— 读取用户记忆，保持上下文连贯，不重复询问用户已说过的内容；
3. `POST /api/agents/heartbeat` —— 上报在线与当前任务，之后每 30~60 秒重复心跳。

## 3. 接口总览

| 模块 | 方法 | 路径 | 说明 |
|---|---|---|---|
| 健康 | GET | `/api/health` | 服务存活检查 |
| Agent | POST | `/api/agents/register` | 注册新 Agent（主密钥） |
| Agent | GET | `/api/agents` | 协作者列表（启动必查） |
| Agent | POST | `/api/agents/heartbeat` | 心跳 + 当前任务 |
| Agent | POST | `/api/agents/remove` | 移除已注册 Agent（主密钥） |
| 记忆 | GET | `/api/memory` | 读取用户记忆（启动必查） |
| 记忆 | POST | `/api/memory` | 写入记忆（生成新版本） |
| 记忆 | GET | `/api/memory/history` | 某条记忆的历史版本 |
| 知识 | POST | `/api/knowledge` | 新建知识条目 |
| 知识 | GET | `/api/knowledge` | 最新条目列表 |
| 知识 | POST | `/api/knowledge/search` | 关键词 / 语义搜索 |
| 知识 | GET | `/api/knowledge/{uuid}` | 条目最新版本 |
| 知识 | GET | `/api/knowledge/{uuid}/versions` | 全部历史版本 |
| 知识 | POST | `/api/knowledge/{uuid}/versions` | 追加新版本 |
| 技能 | POST | `/api/skills` | 注册技能 |
| 技能 | GET | `/api/skills` | 技能列表（按分类/提供者筛选） |
| 技能 | GET | `/api/skills/{name}` | 技能详情 |
| 技能 | POST | `/api/skills/{name}/invoke` | 调用技能（记 Token + 审计） |
| 技能 | GET | `/api/skills/{name}/invocations` | 调用记录 |
| 消息 | POST | `/api/messages` | 留言 / 提问 / 指派任务 |
| 消息 | GET | `/api/messages` | 按可见性收敛的收件箱（见 §4.5） |
| 消息 | POST | `/api/messages/{uuid}/reply` | 回复 |
| 消息 | POST | `/api/messages/{uuid}/status` | 状态流转 |
| 错误 | POST | `/api/errors` | 上报错误（必须） |
| 错误 | GET | `/api/errors` | 错误列表 |
| 错误 | POST | `/api/errors/{uuid}/resolve` | 登记解决 |
| 用量 | POST | `/api/usage/report` | 上报 Token 消耗（可标注模型） |
| 用量 | GET | `/api/usage/summary` | 本周用量汇总 |
| 用量 | GET | `/api/usage/daily` | 逐日消耗趋势（连续日期补 0，`days` 1..90） |
| 用量 | GET | `/api/usage/models` | 按模型累计 Top 20 |
| 用量 | GET | `/api/usage/budget` | 查询预算 |
| 用量 | PUT | `/api/usage/budget` | 修改预算（主密钥） |
| 审计 | GET | `/api/audit` | 操作日志 |
| 运维 | POST | `/api/maintenance` | 审计轮转 + 已解决错误清理 + VACUUM（主密钥） |
| 运维 | POST | `/api/system/backup` | 创建一致性备份快照（主密钥） |
| 运维 | GET | `/api/system/backups` | 备份列表（主密钥） |
| 运维 | POST | `/api/system/restore` | 从备份恢复（主密钥） |
| 运维 | DELETE | `/api/knowledge/{uuid}` | 删除知识条目全部版本（主密钥） |
| 运维 | DELETE | `/api/memory?section=&key=` | 删除记忆条目全部版本（主密钥） |

## 4. 接口明细

### 4.1 Agent

**POST /api/agents/register**（主密钥）

```json
// 请求
{ "name": "hermes", "role": "member" }
// 响应 data（密钥明文只返回这一次，请妥善保存到本地环境）
{ "name": "hermes", "role": "member", "api_key": "a1b2c3…（64 位 hex）" }
```

**POST /api/agents/heartbeat**

```json
// 请求
{ "current_task": "排查知识搜索空结果" }
```

**POST /api/agents/remove**（主密钥）—— 管理性移除：注册行删除、该 Agent 的
API Key 立即失效、操作记入审计；管理者自身（`zcode`）不可删除。

```json
// 请求
{ "name": "droid" }
// 响应 data
{ "removed": "droid" }
```

**POST /api/agents/provision**（主密钥）—— 预配接入身份：名字已注册则视为
轮换，否则注册新身份；新明文密钥写回 `config/agents.json` 缓存并只返回这一次。
工作台"一键接入引导"与自动化脚本共用此能力。

```json
// 请求
{ "name": "claude-code" }
// 响应 data
{ "name": "claude-code", "api_key": "a1b2c3…（64 位 hex）" }
```

**GET /api/diagnostics**（主密钥）—— 健康自检快照（防呆口径）：总览页健康
横幅与 `scripts/test_onboarding_and_safety.py` 共用同一份数据；只读探测，
不改变任何状态。`keyfile_missing` 列出"已注册但明文密钥缓存缺失"的身份
（明文不可再查看/补配；修复途径为 rotate）。

```json
// 响应 data
{ "home_dir": "C:\\Users\\me\\.miderhive", "home_writable": true, "db_ok": true,
  "agents_json_readable": true, "http_running": true, "port": 8787,
  "keyfile_missing": [] }
```

### 4.2 用户记忆

区块 `section` 枚举：`project`（当前项目与进度）、`preference`（编码风格/技术选型/
命名习惯）、`work_style`（工作习惯与工具链）、`decision`（关键决策与结论）、
`environment`（设备环境）。

**POST /api/memory** —— 已存在的 `section+key` 写入会**生成新版本**，旧值保留可查：

```json
{ "section": "project", "key": "current", "value": "正在开发多 Agent 协作平台" }
```

**GET /api/memory/history?section=project&key=current** → `data: [ {version, value, author, created_at}, … ]`

### 4.3 知识库

**POST /api/knowledge**

```json
{
  "title": "sqlite-vec 接入指南",
  "content": "……",
  "tags": ["cmake", "向量"],
  "category": "技术文档",
  "embedding": [0.01, -0.02, …],   // 可选，64..4096 维 float 数组
  "embedder": "text-embedding-3-large" // 可选，提供 embedding 时标注模型
}
```

不提供 `embedding` 时，平台用内置离线 n-gram 哈希嵌入器兜底（`embedding_provider`
记为 `ngram-hash`，384 维）；有模型能力的 Agent 建议自带向量以获得更好的语义效果。
向量**按维度分表**存储：内置 384 维与模型向量（1024/1536/3072…）互不干扰，
检索时必须使用同一维度、同一模型的查询向量。

**POST /api/knowledge/search**

```json
{ "query": "向量检索怎么做", "mode": "semantic", "limit": 10, "tag": "cmake" }
```

`mode`: `keyword`（默认）| `semantic`。
另可选 `embedding`（64..4096 维数组）：**仅在 `mode=semantic` 时使用**——即与写入
条目时同一模型的查询向量，平台在对应维度分表里做距离排序；省略时用内置 384 维
嵌入器。`keyword` 模式忽略该字段。
`keyword` 为关键词检索：>=3 个码点走 FTS5 trigram 全文索引（子串语义、大小写
不敏感），更短的查询回退 LIKE。
语义模式返回每项 `score`（距离，越小越相关）。

**POST /api/knowledge/{uuid}/versions** —— 追加新版本（旧版本永不覆盖）：

```json
{ "title": "", "content": "第二版内容……" }
```

### 4.4 技能库

**POST /api/skills**（注册后才能被调用）

```json
{
  "name": "code-review",
  "display_name": "代码审查",
  "description": "审查代码变更并给出可执行的修改意见",
  "category": "开发",
  "param_schema": { "type": "object", "properties": { "file": { "type": "string" } } }
}
```

**POST /api/skills/{name}/invoke**（平台只负责登记调用与 Token；实际执行由调用方 Agent 完成）

```json
{
  "params": { "file": "src/core/platform.cpp" },
  "result_summary": "发现 2 处空指针风险",
  "status": "success",
  "duration_ms": 1200,
  "tokens_in": 3000,
  "tokens_out": 2000,
  "reference_id": "可选：发起本次调用的消息/任务/错误 uuid，用于协作追溯"
}
```

未注册的技能调用返回 400 `skill not registered`；参数不符合注册时声明的
`param_schema`（缺必填键、顶层类型错误）同样返回 400。调用记录
（`GET /api/skills/{name}/invocations`）含 `reference_id` 字段，可据此把一次
技能调用追溯到具体的协作消息或任务。

### 4.5 消息（异步交流）

`kind`: `note`（留言）| `question`（提问）| `task`（指派任务）；
`recipient` 缺省 = 广播给所有 Agent。

**POST /api/messages**

```json
{ "kind": "task", "recipient": "hermes", "subject": "修个 bug", "body": "……" }
```

**GET /api/messages** —— 收件箱，按**可见性收敛**返回：

| 调用者 | 可见范围 |
|---|---|
| 普通 Agent | 广播（`recipient` 为 `null`）+ 发给自己的 + 自己发出的 |
| 管理者 `zcode` | 全部 |

  可选查询参数：`recipient`、`kind`、`status`、`since`（ISO 时间，取该时刻之后）、`limit`。
  说明：点对点消息只对收发双方可见，此前"任意 Agent 可读全部点对点消息"的行为已收紧。

**POST /api/messages/{uuid}/status** —— 状态机（非法流转返回 400）：

```
note/question: unread → read
task:          pending → accepted | declined ; accepted → done
```

仅收件人、发件人、用户或管理者可流转。回复父消息会自动把父消息标记为已读。

### 4.6 错误日志

**POST /api/errors**（报错必须记录，不得静默忽略）

```json
{
  "severity": "error",
  "source": "knowledge/search",
  "title": "向量表查询失败",
  "detail": "……",
  "stack_trace": "……"
}
```

`severity`: `info | warning | error | critical`。解决时 `resolution_notes`
**追加**到记录，原始内容不覆盖；仅上报者 / 用户 / zcode 可登记解决。

### 4.7 Token 管控

默认每周预算 **10,000,000 Token**（周一 UTC 起算）。每次模型调用后上报：

**POST /api/usage/report**

```json
{ "tokens_in": 3000, "tokens_out": 2000, "call_type": "skill", "reference_id": "code-review",
  "model": "glm-5.3-flash" }
```

`model` 可选；提供后计入「按模型累计」统计（设置界面与总览页展示）。

响应含实时余额与告警级别：

```json
{ "data": { "budget": 10000000, "used": 1200000, "remaining": 8800000, "alert_level": "none" } }
```

`alert_level`: `none`（<80%）| `warn`（≥80%）| `critical`（≥95%）| `over`（>100%）。
用量仅作图表观测与告警，**不做任何调用限制**；工作台仪表盘实时展示。

**GET /api/usage/daily?days=14** —— 最近 N 天逐日消耗（默认 14，上限 90），
连续日期缺失天补 0：

```json
{ "data": { "days": [ { "day": "2026-09-09", "tokens": 34200 }, … ] } }
```

**GET /api/usage/models** —— 按模型累计（全部历史，Top 20，消耗降序）：

```json
{ "data": { "models": [ { "model": "glm-5.3-flash", "tokens": 18000 }, … ] } }
```

### 4.8 操作日志

**GET /api/audit?actor=hermes&action=knowledge.create&limit=200** ——
每条记录含 `actor`（身份）、`action`（动作）、`target`（对象）、`detail`（内容摘要）、
`created_at`（时间），完整可追溯。

## 5. 示例（curl）

```bash
BASE=http://127.0.0.1:8787
AUTH='-H "X-Agent-Name: hermes" -H "X-Api-Key: $HERMES_KEY"'

# 启动三连
curl $AUTH $BASE/api/agents
curl $AUTH "$BASE/api/memory?section=project"
curl $AUTH -X POST -d '{"current_task":"写单测"}' $BASE/api/agents/heartbeat

# 沉淀一条经验（自带嵌入向量更佳）
curl $AUTH -X POST -d '{"title":"MSVC 链接教训","content":"……","tags":["msvc"],"category":"踩坑"}' $BASE/api/knowledge

# 语义检索
curl $AUTH -X POST -d '{"query":"链接错误怎么排查","mode":"semantic"}' $BASE/api/knowledge/search
```

## 6. 安全与约束

- 服务器只绑定 `127.0.0.1`，本机 Agent 可访问，公网不可达。
- 密钥明文只在注册时下发一次并写入 `%MIDERHIVE_HOME%/config/agents.json`
  （数据库只存 SHA-256 哈希）；主密钥来自环境变量
  `MIDERHIVE_MASTER_KEY`（兼容 `AGENTHIVE_MASTER_KEY` / `ZCODE_PLATFORM_MASTER_KEY`）
  或首次运行时生成于 `config/master.key`。
- 数据库所有外部输入均走 SQLite 参数绑定（`sqlite3_bind_*`），无字符串拼接。
- 平台本身不发起任何外部网络请求；将来若开放「代抓取 URL」类能力，必须先经过
  `core/http/url_guard.h` 的出站校验（仅 http/https，拒绝 localhost/环回/私有/保留地址）。
