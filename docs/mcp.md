# 通过 MCP 连接 MiderHive（miderhive-mcp）

`miderhive-mcp` 是一个 **MCP（Model Context Protocol）stdio 服务器**。支持 MCP 的 AI
客户端把它作为子进程拉起后，agent 就能用 **27 个工具**直接参与蜂巢协作：确认自己是
否已连上、读写共享记忆、检索/新增/迭代知识库、收发消息（点对点与广播）、上报并闭环
错误、注册与调用技能、上报与查看 token 用量。

协议传输：stdin/stdout 上每行一条 JSON-RPC 2.0 消息（MCP stdio 约定）。数据通道只经
过 `127.0.0.1` 的 HTTP API——本程序不直接碰数据库，身份就是一个普通 agent。

> **最省事的接入方式**：打开工作台 → **设置 → Agent 管理 → 一键接入常用 Agent**。
> 选好工具后它会签发凭据、生成该工具真正能读的配置，并在可以安全合并时**直接写进
> 配置文件**（原文件备份为 `.miderhive.bak`）；该 agent 一上线，右侧就显示
> 「已连接」。下面的手工步骤是同一件事的展开，供排障和自动化使用。

## 前置条件

- 平台在运行：启动工作台（`miderhive.exe`）或后台服务 `platformd`。
- 默认端口 `8787`，可用环境变量 `MIDERHIVE_PORT` 覆盖（所有进程必须一致）。
- `miderhive-mcp.exe` 在安装目录（MSI 会把该目录加入用户 PATH）。

## 身份（环境变量）

| 变量 | 说明 |
|---|---|
| `MIDERHIVE_AGENT_NAME` | agent 名字（显示在协作列表/审计里） |
| `MIDERHIVE_AGENT_KEY` | 该 agent 的 API key |
| `MIDERHIVE_MASTER_KEY` | 可选。给出且未提供 KEY 时自动注册并落盘到 `config/agents.json` |
| `MIDERHIVE_PORT` | 平台端口，默认 `8787` |
| `MIDERHIVE_HOME` | 数据目录，默认 `%USERPROFILE%\.miderhive`（密钥落盘位置） |

身份解析顺序：显式 KEY → `config/agents.json` 里的同名密钥 → 用 MASTER_KEY 注册一次。
`zcode` / `user` / `system` / `master` 是保留身份，工具侧请另起名字（如 `claude`）。

## 各客户端接入

### Claude Code（项目根 `.mcp.json`，推荐）

在项目根新建/编辑 `.mcp.json`：

```json
{
  "mcpServers": {
    "miderhive": {
      "command": "C:\\Program Files\\MiderHive\\miderhive-mcp.exe",
      "args": [],
      "env": {
        "MIDERHIVE_AGENT_NAME": "claude",
        "MIDERHIVE_AGENT_KEY": "<粘贴 key>"
      }
    }
  }
}
```

然后运行一次 `claude`，它会把 `miderhive` 列为待批准并请求你确认（一次即可）。
已实测：Claude Code v2.1.250 能识别该文件、完成 MCP 握手并显示 `√ Connected`。

也可以走命令行（**请粘进 cmd.exe**）：

```bat
claude mcp add miderhive -e MIDERHIVE_AGENT_NAME=claude -e MIDERHIVE_AGENT_KEY=<粘贴 key> -- "C:\Program Files\MiderHive\miderhive-mcp.exe"
```

> **PowerShell 用户注意**：`claude` 是 npm 的 `claude.ps1` 包装脚本，PowerShell 调用它时会把
> `--` 之后的命令吞掉，报 `error: missing required argument 'commandOrUrl'`
> （已实测，与 `-e` / `--env` 无关）。要么用 cmd.exe，要么就让 claude.exe 直接收到参数
> （例如在 PowerShell 里用参数数组 `& claude @args`）。走上面的 `.mcp.json` 则完全没有这个问题。

### Claude Desktop / Cursor（JSON）

Claude Desktop：`%APPDATA%\Claude\claude_desktop_config.json`；
Cursor：`%USERPROFILE%\.cursor\mcp.json`。

```json
{
  "mcpServers": {
    "miderhive": {
      "command": "C:\\Program Files\\MiderHive\\miderhive-mcp.exe",
      "env": {
        "MIDERHIVE_AGENT_NAME": "claude",
        "MIDERHIVE_AGENT_KEY": "<粘贴 key>"
      }
    }
  }
}
```

Cursor 的条目额外接受 `"args": []`。

### ChatGPT / Codex CLI（TOML）

`%USERPROFILE%\.codex\config.toml`：

```toml
[mcp_servers.miderhive]
command = 'C:\Program Files\MiderHive\miderhive-mcp.exe'
args = []
env = { MIDERHIVE_AGENT_NAME = 'codex', MIDERHIVE_AGENT_KEY = '<粘贴 key>' }
```

路径用**单引号字面量字符串**：TOML 基本字符串里 `\Q` 之类是非法转义，而 Windows 路径
全是反斜杠，用双引号会让整份 `config.toml` 解析失败。

ChatGPT 桌面端目前在 **设置 → 连接器** 里添加自定义 MCP 服务器，填同样的命令与环境变量。

### Factory Droid（JSON 或命令行）

`%USERPROFILE%\.factory\mcp.json`：

```json
{
  "mcpServers": {
    "miderhive": {
      "command": "C:\\Program Files\\MiderHive\\miderhive-mcp.exe",
      "args": [],
      "env": { "MIDERHIVE_AGENT_NAME": "droid", "MIDERHIVE_AGENT_KEY": "<粘贴 key>" }
    }
  }
}
```

或：

```bash
droid mcp add miderhive "C:\Program Files\MiderHive\miderhive-mcp.exe" \
  --env MIDERHIVE_AGENT_NAME=droid --env MIDERHIVE_AGENT_KEY=<粘贴 key>
```

Droid 会在 `mcp.json` 变化后自动重载；在 Droid 里输入 `/mcp` 可看连接状态。

### DSH / DeepSeek Harness（profile 补丁层）

DSH 用官方 `dsh-mcp-client` 插件把外部 MCP 工具桥接成原生工具。**不要改 bundles 自带的
`cordis.yml`**，把下面这段加进 profile 的补丁层
`%USERPROFILE%\.dsh\profiles\<profile>\cordis.patch.yml`（向导会自动定位真实 profile）：

```yaml
- insert:
    - id: mcp-miderhive
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: miderhive
        transport: stdio
        command: 'C:\Program Files\MiderHive\miderhive-mcp.exe'
        args: []
        env:
          MIDERHIVE_AGENT_NAME: 'dsh'
          MIDERHIVE_AGENT_KEY: '<粘贴 key>'
```

DSH 的工具会以 `mcp__miderhive__<工具名>` 出现（例如 `mcp__miderhive__knowledge_search`）。
注意 **DSH 会清洗子进程环境**（名字含 `KEY`/`TOKEN`/`SECRET` 的一律删除），所以密钥必须
写在上面这个 `env` 块里，指望继承父进程环境是无效的。

### Hermes（config.yaml）

`%LOCALAPPDATA%\hermes\config.yaml`，加一段 `mcp_servers:`：

```yaml
mcp_servers:
  miderhive:
    command: 'C:\Program Files\MiderHive\miderhive-mcp.exe'
    args: []
    env:
      MIDERHIVE_AGENT_NAME: 'hermes'
      MIDERHIVE_AGENT_KEY: '<粘贴 key>'
```

若该文件里**已经有** `mcp_servers:` 段，请把 `miderhive:` 这一项手工加到那段下面
（向导检测到这种情况会拒绝自动写入，以免弄坏你的配置）。

### ZCode

ZCode 的执行器不走 MCP，通过环境变量接入（`agent-cli` / `platformd` 已内置支持）：

```
MIDERHIVE_AGENT_NAME=zcode-agent
MIDERHIVE_AGENT_KEY=<粘贴 key>
MIDERHIVE_PORT=8787
```

### GitHub Copilot

Copilot 不支持 MCP。向导会生成一段 HTTP 指令块，粘进仓库的
`.github/copilot-instructions.md`，Copilot 就会按同样的凭据调用本工作台的 HTTP API。

## 工具一览（27 个）

### 自检与在场

| 工具 | 说明 |
|---|---|
| `hive_status` | **建议第一个调**：平台是否可达、我是谁、谁在线、本周用量，一次问清 |
| `agents_list` | 谁在蜂巢里：角色/在线状态/当前任务 |
| `heartbeat` | 汇报自己正在做什么（协作列表可见），建议 30–60 秒一次 |

### 共享记忆

| 工具 | 说明 |
|---|---|
| `memory_list` | 读取共享记忆（仅最新版本），可按 section 过滤 |
| `memory_write` | 写入/更新一条记忆；支持 `base_version` 乐观并发（冲突返回 409 语义） |
| `memory_history` | 某条记忆的全部历史版本 |
| `memory_remove` | 删除某条记忆的全部版本（仅管理者） |

### 知识库

| 工具 | 说明 |
|---|---|
| `knowledge_add` | 新增一条知识。不带 `embedding` 时平台用内置离线嵌入器算（384 维，偏召回，**不等于真语义**）；带 `embedding`（64..4096 维）+ `embedder`（产出该向量的模型名）则存真语义 |
| `knowledge_search` | 检索：`mode=keyword` 子串 / `mode=semantic` 向量相似；可带 `embedding` 查询向量（64..4096 维，与写入条目时的模型一致） |
| `knowledge_list` | 浏览最新条目 |
| `knowledge_get` | 按 uuid 读最新版本 |
| `knowledge_versions` | 按 uuid 列出全部历史版本 |
| `knowledge_add_version` | **追加新版本**（旧版保留）——迭代已有知识而不是造重复条目；同样可带 `embedding` + `embedder` |

> **怎么拿到真语义向量？** 平台本体零出站，不会替你调模型。用 `agent-cli` 对接你已有的
> OpenAI 兼容嵌入服务（Ollama / LM Studio / vLLM / 自建网关）最省事：
> `agent-cli knowledge add --embed-url http://127.0.0.1:11434/v1/embeddings --model M …`。
> MCP 侧的 Agent 若自己就能调模型，直接把向量填进 `embedding` 并标注 `embedder` 即可，
> 效果等价。**同一维度只能绑一个模型**（同宽向量共用一张表），第二个模型请换一个维度，
> 否则平台会返回 400 并在错误里点明已绑定的 provider。详见
> [api.md §4.3.1](api.md#431-用真实嵌入模型外接端点agent-cli)。

### 消息与任务

| 工具 | 说明 |
|---|---|
| `message_send` | `kind=note/task/question`；省略 `recipient` 即广播 |
| `message_list` | 按可见性收敛的收件箱（广播 + 发给我的 + 我发出的） |
| `message_reply` | 回复并自动把父消息标记为已读 |
| `message_set_status` | 状态流转：`pending→accepted→done/declined`（task）；`unread→read` |

### 错误闭环

| 工具 | 说明 |
|---|---|
| `error_report` | 上报错误/阻塞（进工作台告警） |
| `error_list` | 查询（默认只看未解决） |
| `error_resolve` | 登记解决说明（仅管理者） |

### 技能市场

| 工具 | 说明 |
|---|---|
| `skill_list` | 发现技能及其参数 schema |
| `skill_register` | **发布技能**到市场，供任何 agent 发现与调用 |
| `skill_invoke` | 调用留痕（参数/结果/时长/token 计入审计与预算；实际执行由调用方完成） |

### Token 用量

| 工具 | 说明 |
|---|---|
| `usage_summary` | 本周总量、预算余量、告警级别、按 agent 分摊 |
| `usage_report` | **上报一次模型调用消耗**；带 `idempotency_key` 时重试不会重复计数 |
| `usage_daily` | 最近 N 天逐日消耗（缺失天补 0） |
| `usage_breakdown` | 按时间/agent/模型多维切片（与用量面板同口径） |

## 安全与边界

- 服务器只监听 `127.0.0.1`：MCP 进程与本机平台通信，不产生任何外联。
- MCP 的身份就是一个普通 agent：非管理者调用 `memory_remove` / `error_resolve`
  会被平台拒绝（403 语义）。
- key 与 agent-cli 共用同一套约定（`MIDERHIVE_AGENT_*`，旧 `AGENTHIVE_*` 兼容）。
- 手动排障：在终端直接运行 `miderhive-mcp.exe`，向 stdin 粘贴一行
  `{"jsonrpc":"2.0","id":1,"method":"tools/list"}`，应输出工具清单；日志走 stderr。
  粘贴 `{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"hive_status"}}`
  可以直接验证"我到底连上没有"。
