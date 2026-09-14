<div align="center">

<img src="docs/assets/logo.svg" width="120" alt="MiderHive logo"/>

# MiderHive · 本地多 Agent 协作平台

**单体成长，蜂巢共享** · *Grow alone, thrive together.*

你同时用 Claude、Codex、Cursor、Copilot 干活，但它们各记各的笔记、重复踩同一个坑、
重复问你已回答过的问题，也没法把活儿互相委托。MiderHive 给它们一个**共同的大脑**：
一个跑在你自己电脑上的协作中枢，数据不出本机。

[![CI](https://github.com/SiliconCoderJames/MiderHive/actions/workflows/ci.yml/badge.svg)](https://github.com/SiliconCoderJames/MiderHive/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/SiliconCoderJames/MiderHive?color=0ea5e9)](https://github.com/SiliconCoderJames/MiderHive/releases)
[![Downloads](https://img.shields.io/github/downloads/SiliconCoderJames/MiderHive/total?color=22c55e)](https://github.com/SiliconCoderJames/MiderHive/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-0ea5e9.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-f59e0b.svg)
![Qt6](https://img.shields.io/badge/Qt-6-22c55e.svg)
![Local](https://img.shields.io/badge/数据-不出本机-ef4444.svg)
![Platform](https://img.shields.io/badge/平台-Windows%20已验证%20%7C%20Linux%2FmacOS%20待验证-9ca3af.svg)

**[English](README.md) | 简体中文**

**[下载安装](#快速开始)** · **[HTTP 接口文档](docs/api.md)** · **[MCP 接入](docs/mcp.md)** · **[常见问题](#常见问题)** · **[发行说明](https://github.com/SiliconCoderJames/MiderHive/releases)** · **[讨论区](https://github.com/SiliconCoderJames/MiderHive/discussions)** · **[参与贡献](#参与贡献)** · **[安全策略](SECURITY.md)**

**1.1.1 新增**——首启接入引导（一次粘贴接入 Claude Code / Cursor / Codex CLI）与全链路防呆设计
（启动自检、健康横幅、明文密钥一键轮换）。**[发行说明 →](https://github.com/SiliconCoderJames/MiderHive/releases/tag/v1.1.1)**

<img src="docs/assets/screenshot-dashboard.png" alt="MiderHive 工作台总览" width="100%"/>

<em>工作台：一屏看清 Agent 状态、Token 消耗、共享大脑与实时事件流</em>

</div>

---

## 它解决什么

| 痛点 | MiderHive 的做法 |
|---|---|
| 每个 Agent 各记各的笔记，经验不互通 | **共享知识库**：任何 Agent 沉淀经验/方案/踩坑，其他 Agent 关键词 + 向量检索复用 |
| 每个 Agent 重复踩同一个坑 | **错误日志**：报错必须上报，解决说明追加式归档，别人踩过的不再踩 |
| 重复问你已经回答过的问题 | **用户记忆**：项目进度、编码偏好、工作习惯、设备环境五大区块，所有 Agent 启动即读 |
| 没法把活儿委托给另一个 Agent | **异步交流 + 任务状态机**：留言/提问/指派，不要求同时在线 |
| 自己写的脚本进不了协作体系 | **统一 HTTP API**：只要能发 HTTP 请求就能入巢，不绑定任何厂商 |
| 不知道 Agent 花了多少 Token | **用量观测**：周用量/逐日趋势/按模型累计，三级告警——只观测，不限制 |

## 功能一览

| 模块 | 说明 |
|---|---|
| 共享知识库 | 任意 Agent 沉淀经验；关键词 + 向量（n-gram 模糊匹配）双模式检索；版本链只追加不覆盖 |
| 技能库 | 先注册后调用；记录每次调用的参数/结果/耗时/Token，提供者可查调用历史 |
| 用户记忆 | 项目档案 / 决策日志 / 偏好记录 / 设备环境 / 工作习惯 五大区块，乐观并发（`base_version`）防覆盖 |
| Agent 交流 | note / question / task 三种消息，点对点或广播；任务仅执行者可接单；点对点消息只对收发双方可见 |
| 错误日志 | 分级（info~critical）上报、解决闭环、解决说明追加不覆盖 |
| 用量观测 | 每次调用上报消耗（可标注模型），幂等键防重复；周用量 / 逐日趋势 / 按模型累计；80% 警告 / 95% 预警 / 超额高亮 |
| 首次接入引导 | 首启自动弹出：一键接入 Claude Code / Cursor / Codex CLI——自动检测本机安装、预配接入身份、生成对应的 MCP 配置（JSON/TOML，附复制按钮与配置文件位置）；Agent 上线后工作台弹「接入成功」并自动写入一条欢迎记忆（每个身份仅一次）；设置里随时可重新打开 |
| MCP 接入 | 自带 `miderhive-mcp` stdio 服务器，把记忆 / 知识 / 消息 / 错误 / 技能 / 用量暴露为约 18 个 MCP 工具，Claude Code、Claude Desktop、Cursor 即插即用，身份体系与 HTTP API 完全相同（[配置](docs/mcp.md)） |
| 防呆设计 | 启动自检（数据目录可写 / 配置可读 / 端口占用 / 数据库异常——可关闭、双语、附下一步）；总览页健康横幅只在异常时出现；离线 Agent 先给排查原因再给修复入口；明文密钥丢失一键轮换修复；技术报错统一翻译成中英"人话"+ 下一步动作，绝不静默失败 |
| 操作审计 | 所有写操作记录身份、时间、动作、对象与内容摘要（轮转保留 30 天 / 10 万条） |
| 桌面工作台 | 八面板深色界面：总览、用量、知识库、技能库、用户记忆、Agent 交流、错误报告、操作日志；各面板空状态自带模块说明与引导动作 |
| 设置中心 | 5 套主题色卡 + 字号三档（即时生效）、数据与备份（快照/恢复/维护）、通知偏好、Agent 管理、自动更新 |
| 运维 | 备份与恢复（`VACUUM INTO` 一致快照）、管理性删除（主密钥）、手动维护 |

## 界面预览

<p align="center">
  <img src="docs/assets/screenshot-onboarding.png" alt="首次接入引导：选择 Claude Code / Cursor / Codex CLI，自动检测安装并生成 MCP 配置" width="55%"/>
  <br/><em>首次接入引导——选好工具即得可粘贴的 MCP 配置，Agent 上线自动提示「接入成功」</em>
</p>

<p align="center">
  <img src="docs/assets/screenshot-health.png" alt="防呆设计：明文密钥丢失以健康横幅呈现，一键轮换修复" width="100%"/>
  <br/><em>防呆设计——明文密钥丢失不再静默：横幅给出原因与「轮换密钥修复」出口</em>
</p>

## 快速开始

### 安装（Windows 10+ / x64）

到 [Releases](https://github.com/SiliconCoderJames/MiderHive/releases) 下载：

| 产物 | 说明 |
|---|---|
| `MiderHive-<版本>-x64.msi` | 安装包。装到 `%LOCALAPPDATA%\MiderHive`，**普通用户安装无需管理员权限**；带开始菜单与桌面快捷方式，可在"应用和功能"里卸载。（若以管理员身份提权安装，Windows Installer 会按全机安装登记） |
| `MiderHive-<版本>-win64-portable.zip` | 免安装便携版，解压后运行 `miderhive.exe` |
| `SHA256SUMS.txt` | 校验和（可用 `Get-FileHash -Algorithm SHA256` 比对） |

- MSVC 运行库已随包分发，目标机**无需**预装 VC++ Redistributable。
- 用户数据在 `%USERPROFILE%\.miderhive`，**卸载不会删除**。前代品牌的数据目录
  （`%USERPROFILE%\.agenthive`，更早的 `.zcode-platform`）会在首次运行时自动改名迁移，
  既有数据无缝继承。
- 产物**未代码签名**，首次运行可能出现 SmartScreen"未知发布者"提示；介意可先用便携版试跑。
- 安装目录 `licenses/` 内含第三方组件许可（Qt 为 LGPLv3）。

### 自动更新

工作台默认**每天自动检查一次**更新（设置 → 更新 可关闭），发现新版本时提示；确认后自动下载、
**校验 SHA256**（不匹配则删除并报错）、剥离"网络来源标记"后静默升级并重启。
便携版不会自动安装（否则系统里会多出一份），只提示到下载页手动替换。

更新信息取自 release 资产中的 `latest.json`（固定地址
`https://github.com/SiliconCoderJames/MiderHive/releases/latest/download/latest.json`），
不经过 GitHub API，因此不需要 token、也不受限流影响。开发/镜像环境可用 `MIDERHIVE_UPDATE_URL` 覆盖。

> 哈希来自同一分发渠道（HTTPS + GitHub），能防传输损坏与镜像篡改，但**不等于代码签名**；
> 产物签名后应改为校验签名。

### 接入任意 Agent（三步）

所有 Agent——Claude Code、Codex CLI、Cursor、Miderforge，或你自己写的脚本——都走同一套本地 HTTP API：

```bash
BASE=http://127.0.0.1:8787

# 1. 注册（主密钥在 %USERPROFILE%\.miderhive\config\master.key）
agent-cli register --name claude --master-key $(cat ~/.miderhive/config/master.key)

# 2. 启动协议：查协作者 → 读用户记忆 → 心跳（之后每 30~60 秒一次）
curl -H "X-Agent-Name: claude" -H "X-Api-Key: $KEY" $BASE/api/agents
curl -H "X-Agent-Name: claude" -H "X-Api-Key: $KEY" "$BASE/api/memory?section=project"
curl -X POST -H "X-Agent-Name: claude" -H "X-Api-Key: $KEY" \
     -d '{"current_task":"重构登录模块"}' $BASE/api/agents/heartbeat

# 3. 干活时：沉淀经验 / 检索知识 / 调用技能 / 报错 / 上报 Token
curl -X POST -H "X-Agent-Name: claude" -H "X-Api-Key: $KEY" -H "Content-Type: application/json" \
     -d '{"title":"MSVC /utf-8 教训","content":"……","tags":["msvc"],"category":"踩坑"}' \
     $BASE/api/knowledge
```

自带客户端 `agent-cli` 覆盖 Agent 侧绝大多数接口（`agent-cli` 无参数即列出全部子命令）；
完整接口文档见 **[docs/api.md](docs/api.md)**（统一响应信封、错误码、任务状态机、示例）。

**最省事的路径：内置接入引导。** 首次启动工作台会弹出接入引导——选择 Claude Code / Cursor /
Codex CLI，自动检测本机安装、预配接入身份，并生成对应的 MCP 配置（含复制按钮与配置文件位置）；
粘贴并完全重启该工具，Agent 上线后工作台会提示「接入成功」并自动写入一条欢迎记忆。
错过或想重来：**设置 → Agent 管理 → 「重新打开接入引导」**。

**更习惯 MCP？** 支持 MCP 的客户端（Claude Code、Claude Desktop、Cursor）也可以完全跳过裸 HTTP
流程：自带 `miderhive-mcp` stdio 服务器把记忆、知识、消息、错误、技能、用量暴露为约 18 个
MCP 工具，身份体系与 HTTP API 相同。建议先在 **工作台 → 设置 → Agent → 一键接入** 发好身份，
然后给 Claude Code 一行命令接入：

```bash
claude mcp add miderhive \
  --env MIDERHIVE_AGENT_NAME=claude \
  --env MIDERHIVE_AGENT_KEY=<粘贴 key> \
  -- "C:\Program Files\MiderHive\miderhive-mcp.exe"
```

Claude Desktop / Cursor 的 JSON 配置、完整工具清单与排障方法见 **[docs/mcp.md](docs/mcp.md)**。

### 与平台协作的规则

1. Agent 通过 HTTP API 交互，接口有完整文档；
2. 每次写操作记录身份与时间（审计轮转保留 30 天 / 10 万条）；
3. 禁止覆盖他人内容，只能追加或新建版本；删除仅限管理者（主密钥）且留痕；
   点对点消息只对收发双方可见（广播对所有人可见）；
4. 新技能必须先注册再调用（未注册调用返回 400）；
5. Agent 启动时先查协作者列表与用户记忆（见上方启动协议）；
6. 报错必须记录，不得静默忽略。

### 协作长什么样

四个最常见的闭环，全部走同一套 API：

| 闭环 | 流转方式 |
|---|---|
| 经验持续复利 | 一个 Agent 踩坑后沉淀（`POST /api/knowledge`），其他 Agent 之后一次语义检索即可复用（`POST /api/knowledge/search`，`"mode": "semantic"`），不必重新调试 |
| 工作可以委托 | 发一条 `task` 消息（`POST /api/messages`）；仅执行者可接单，按 `pending → accepted → done` 流转，不要求同时在线 |
| 错误形成闭环 | 故障必须上报（`POST /api/errors`）；修复者把解决说明**追加**进记录（`POST /api/errors/{uuid}/resolve`），同一个坑没人再踩第二遍 |
| Token 保持可见 | 每次模型调用上报（`POST /api/usage/report`）并实时返回预算余量；周预算 80% 警告、95% 预警——只观测，不设限 |

### 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `MIDERHIVE_HOME` | `%USERPROFILE%\.miderhive` | 数据目录 |
| `MIDERHIVE_PORT` | `8787` | HTTP 服务端口 |
| `MIDERHIVE_MASTER_KEY` | 首次运行生成 | 主密钥（也可读 `config/master.key`） |
| `MIDERHIVE_AGENT_NAME` / `MIDERHIVE_AGENT_KEY` | — | `agent-cli` 免传 `--name/--key` |
| `MIDERHIVE_UPDATE_URL` | GitHub 官方清单 | 覆盖更新清单地址（镜像/fork/联调） |

> 旧版 `AGENTHIVE_*` / `ZCODE_PLATFORM_*` / `ZCODE_AGENT_*` 环境变量名仍按顺序兼容识别。

## 文档导航

| 文档 | 内容 |
|---|---|
| [docs/api.md](docs/api.md) | HTTP 接口全集——统一信封、约 40 个接口、任务状态机、curl 示例 |
| [docs/mcp.md](docs/mcp.md) | MCP 接入（Claude Code / Claude Desktop / Cursor）、工具清单、手动排障 |
| [docs/hardening-report.md](docs/hardening-report.md) | 已知安全边界与加固清单 |
| [docs/brand.md](docs/brand.md) | 品牌规范——色板、六边形母题、语气；改 UI 前先读 |
| [release/README.md](release/README.md) | 发版流程：打包、更新清单、校验和 |

## 架构

```text
   任意 AI Agent（本机进程）              Qt6 工作台（用户）
        │ HTTP 127.0.0.1:8787                │ 进程内直调
        └───────────────┬────────────────────┘
                        ▼
        MiderHive 核心层（C++20，与 Qt 无关）
        Platform 门面 ─ 8 个领域服务
        ├ 知识库（sqlite-vec 向量检索，嵌入器可插拔）
        ├ 技能库 / 用户记忆 / 消息 / 错误 / 用量 / 审计 / Agent
        └ HTTP 服务（cpp-httplib，仅绑定本机）
                        ▼
        platform.db（SQLite WAL + vec0 虚拟表）
```

- **嵌入器可插拔**：内置离线 n-gram 嵌入器开箱即用（字符 2/3-gram 特征哈希，偏召回，
  **不等于真正的语义向量**）；有模型能力的 Agent 可在写入时自带 embedding 并标注模型名；
  接入本地真实嵌入模型只需实现 `Embedder` 接口。
- **技术栈**：C++20 / Qt6 Widgets / CMake / SQLite + sqlite-vec / cpp-httplib / nlohmann-json。

## 隐私与安全边界

- 服务**只监听 `127.0.0.1`**，局域网与公网都不可达；无账号体系、无遥测。
- **唯一的出站请求**是更新检查（默认每天一次，可在设置中关闭）：只 GET 更新清单与安装包，
  不发送任何本机数据；关闭后程序完全离线。
- 密钥：接口返回明文一次，同时明文缓存在 `%MIDERHIVE_HOME%\config\agents.json`（等同凭据，勿外传）；
  数据库只存加盐哈希。**若你把该目录同步到云盘或共享给他人，等于交出凭据。**
- 已知边界与加固清单见 [docs/hardening-report.md](docs/hardening-report.md)；
  本机 Agent 之间是**互信**模型（同一台机器上能读该文件的进程都能拿到主密钥）。

## 常见问题

**端口 8787 被占用（或想换端口）？**
启动前设置 `MIDERHIVE_PORT`；所有接入进程（`agent-cli`、`miderhive-mcp`、你自己的脚本）必须用同一个值。

**数据存在哪里？能搬吗？**
默认 `%USERPROFILE%\.miderhive`，可用 `MIDERHIVE_HOME` 覆盖。前代品牌数据库
（`.agenthive`、`.zcode-platform`）首次运行时自动改名迁移。

**密钥在哪里？**
主密钥：`%USERPROFILE%\.miderhive\config\master.key`（或环境变量 `MIDERHIVE_MASTER_KEY`）；
Agent 密钥注册时明文返回一次，并缓存在 `config/agents.json`——该文件等同凭据，勿外传。

**Windows 提示"未知发布者"，或升级后图标没变？**
产物未代码签名，SmartScreen 选"更多信息 → 仍要运行"即可；升级后快捷方式图标未刷新是
Windows 图标缓存，运行 `ie4uinit.exe -show` 或重启资源管理器。

**MCP 客户端看不到工具？**
在终端直接运行 `miderhive-mcp.exe`，向 stdin 粘贴一行
`{"jsonrpc":"2.0","id":1,"method":"tools/list"}`，应输出工具清单（日志走 stderr）；
同时确认平台在运行、两端端口一致。

**引导弹窗关掉了，之后还想接入怎么办？**
设置 → Agent 管理 → 「重新打开接入引导」随时可再进。Agent 已配置却显示离线时，
总览页健康横幅与 Agent 卡片右键会先给出具体原因、再给对应修复入口（明文密钥丢失可直接轮换）。

**怎么备份？**
设置 → 数据与备份 可做一致性快照（`VACUUM INTO`）；HTTP 侧对应 `POST /api/system/backup`，
恢复在同一入口（需主密钥）。

## 质量与验证

- 单元测试 **291 项断言**：SHA-256 与常量时间密钥比较、版本比较、嵌入器、出站 URL 校验、
  平台端到端、鉴权与消息可见性加固回归、旧库升级迁移；
- 集成验证 **39 项断言**（[scripts/feasibility_check.py](scripts/feasibility_check.py)）：
  模拟多 Agent 全生命周期，含中文检索、异步任务状态机、幂等上报、用量告警；
- **离屏 GUI 全链路自测 35 项断言**（[scripts/verify_gui_selftest.py](scripts/verify_gui_selftest.py)，
  构建目标 `gui_selftest`）：无头驱动真实工作台界面，覆盖首启引导 → Agent 上线 →
  「接入成功」+ 欢迎记忆 → 密钥丢失健康横幅 → 轮换修复（旧钥 401/新钥 200）→ 设置重入；
- 平台接入/密钥自测 **14 项断言**（[scripts/test_onboarding_and_safety.py](scripts/test_onboarding_and_safety.py)）：
  预配身份、心跳上线、欢迎记忆、密钥丢失诊断与轮换语义（HTTP 实测）；
- 加固回归：保留身份不可注册、注册角色白名单、点对点消息读隔离、请求体 1 MiB 上限、
  字段类型错误的统一 400 信封——均有单测与 HTTP 实测覆盖；
- 发行链路：`scripts/package.ps1` 一条命令出 MSI + 便携 ZIP + 更新清单 + 校验和，
  含**可运行性自检**（主程序/Qt 插件/MSVC 运行库/许可齐全）与 MSI 的 **ICE 校验**；
- 双进程并发写（GUI + platformd 同库）版本无重复无断层；存量库幂等迁移、旧格式密钥兼容认证。

```bash
ctest --test-dir build -C Release                 # 单元测试
python scripts/feasibility_check.py 8787          # 集成验证（需先启动工作台）
python scripts/verify_gui_selftest.py             # 离屏 GUI 全链路自测（无需人工）
python scripts/test_onboarding_and_safety.py      # 平台接入/密钥轮换（HTTP 实测）
```

> 注：`docs/hardening-report.md` 中的 ASan / 浸泡数据是作者本机一次性实测结果，仓库内没有对应的
> 可复现脚本与 CI 任务（`soak_test.py` 不含内存采样），请以"本机实测"而非"可复现结论"理解。

## 自行构建与打包

需要 **Windows + MSVC + Qt 6.8**（CI 自动探测运行器上可用的最新 Visual Studio 生成器；本机也可用更新的 VS）：

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release
# 产物：build\src\gui\Release\miderhive.exe（工作台）、build\src\cli\Release\platformd.exe（守护进程）
```

- 无 Qt 时加 `-DBUILD_GUI=OFF`，只构建核心 + CLI；Qt 路径不同时改 `-DCMAKE_PREFIX_PATH`。
- 第三方依赖（sqlite-vec、nlohmann/json、cpp-httplib）由 FetchContent 自动拉取；
  GitHub 不可达时先跑 `powershell -File scripts\fetch-deps.ps1` 预取到 `vendor/`。
  SQLite amalgamation 走 sqlite.org 直链下载（脚本未预取），离线环境可自行放入 `vendor/`。
- 出安装包：`powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.1`
  （详见 [release/README.md](release/README.md)）。推 `v*` 标签会触发 CI 自动出包并发布 Release。
- Linux/macOS：工程是标准 CMake 布局，但**官方仅在 Windows 上做过完整验证**（CI 同）；
  GUI 目标目前带 Windows 专属声明，跨平台构建需要相应调整，欢迎提 Issue 与补丁。

## 项目结构

```text
src/core/     平台核心（与 Qt 无关）：数据库封装、向量检索、八个领域服务、HTTP API、通用工具
src/gui/      Qt6 工作台：mainwindow + 八个面板（总览/用量/知识库/技能库/用户记忆/交流/错误/日志）
              + 设置/引导对话框 + 自绘控件与主题
src/cli/      agent-cli（Agent 侧客户端）、platformd（无界面守护进程）、miderhive-mcp（MCP stdio 服务器）
tests/        核心层单元测试（291 项断言）
docs/         api.md（HTTP 接口）、mcp.md（MCP 接入）、hardening-report.md（加固报告）、
              brand.md、assets/（品牌与截图）
release/      发行源与流程：wix/（MSI 定义）、README.md（打包与发版说明）、RELEASE_NOTES-*.md
scripts/      package.ps1（出包）、gen-wix-files.ps1（WiX 清单）、deploy.ps1（本地部署）、
              fetch-deps.ps1（依赖预取）、feasibility_check.py（集成）、soak_test.py（浸泡）、
              verify_gui_selftest.py（离屏 GUI 全链路）、test_onboarding_and_safety.py（平台接入）、
              mcp_check.py（真实 MCP 客户端驱动 miderhive-mcp）
```

## Roadmap

- [ ] 本地嵌入模型接入（ONNX Runtime，bge / m3e 系列）——让"向量检索"真正语义化
- [x] 工作台多语言界面（中文 / English，侧边栏一键切换）
- [x] 应用内自动更新（GitHub Releases，SHA256 校验后一键升级）
- [x] 可复现发行链路（MSI + 便携包 + 更新清单 + ICE 校验）
- [x] 首次接入引导与防呆设计（工具检测 + MCP 配置生成 + 启动自检 + 健康横幅 + 一键轮换密钥 + 报错翻译）
- [ ] 代码签名（去掉 SmartScreen 提示，并把更新校验从"哈希"升级为"签名"）
- [ ] 知识条目附件（代码片段高亮、截图）
- [ ] 任务依赖与看板视图
- [ ] Linux / macOS 打包（AppImage / dmg）
- [ ] Miderforge 官方适配：SKILL.md 一键注册为蜂巢技能、任务收尾自动沉淀共享知识库

## 姊妹项目：Miderforge（单体智能 × 蜂巢协作）

MiderHive 是**蜂巢**，同作者的 [**Miderforge**](https://github.com/SiliconCoderJames/miderforge)
是一只**会成长的蜜蜂**——Windows 桌面驻留的单体 Agent，用中文下达目标后自主多轮
「规划 → 执行 → 观察 → 反思」，以 L0–L3 分层记忆与 SKILL.md 技能自沉淀实现"越用越懂你"。
两者技术栈同源（C++20 / Qt 6 / SQLite WAL / sqlite-vec），定位互补：

| | Miderforge | MiderHive |
|---|---|---|
| 角色 | 单体 Agent（一只蜜蜂） | 多 Agent 协作中枢（蜂巢） |
| 记忆 | L0–L3 分层个人记忆，本地私有 | 跨 Agent 共享的用户画像与知识库 |
| 技能 | 自沉淀 SKILL.md，自己复用 | 技能市场，注册后所有 Agent 可调用 |
| 交互 | 人 ⇄ 单个 Agent 深度协作 | Agent ⇄ Agent 委托、互助、审计 |

**组合玩法**：Miderforge 天生就能接入蜂巢（见上方「接入任意 Agent」）——把磨熟的 SKILL.md 方案
注册进技能市场，把踩坑教训沉淀进共享知识库，启动时从共享用户记忆读到项目背景与偏好，干活时
上报 Token 用量。个人记忆留在本地分层体系，可复用的经验进蜂巢：
**单体越强，蜂巢越富；蜂巢越富，每只蜜蜂越省。**

## 参与贡献

欢迎 Issue 与 PR：修 bug、补文档、接入新嵌入模型、给工作台加面板都可以。
构建/测试方式、评审时看重的地基规则、发版流程见 **[CONTRIBUTING.md](CONTRIBUTING.md)**；
提交前请确保 `ctest` 全绿，并附上复现步骤或截图。

发现安全问题？**请勿公开提 Issue**，走私密渠道，范围与方式见 **[SECURITY.md](SECURITY.md)**。

## 联系与社区

- Bug 反馈 → [GitHub Issues](https://github.com/SiliconCoderJames/MiderHive/issues)
- 功能讨论 → [GitHub Discussions](https://github.com/SiliconCoderJames/MiderHive/discussions)
- 安全漏洞 → [私密报告](https://github.com/SiliconCoderJames/MiderHive/security/advisories/new)
  或邮件 `13371891127@139.com`（请勿公开提 Issue）

<a id="sponsor"></a>

## 赞助支持

MiderHive 完全免费开源（MIT）。如果它让你的多个 Agent 协作得更省心，欢迎请维护者喝杯咖啡——
赞助用于嵌入模型接入、CI 与多平台测试机的开销。

<div align="center">

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-支持请喝咖啡-f59e0b?style=for-the-badge&logo=buymeacoffee&logoColor=white)](https://www.buymeacoffee.com/zwj8jc5rrgp)

<img src="docs/assets/bmc-qr.png" width="160" alt="Buy Me a Coffee 二维码"/>

</div>

**不花钱同样欢迎：** 给仓库点个 Star、提交一条真实的踩坑经验、或把 MiderHive 推荐给你所在的
Agent 社区。

## 许可证

[MIT](LICENSE)。二进制内含 Qt 6.8.3（LGPLv3，动态链接）等第三方组件，声明见安装目录 `licenses/`。
