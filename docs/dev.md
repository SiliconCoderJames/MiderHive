# 调试 · 测试 · 使用（开发指南）

面向在本仓库里开发的人：怎么运行和调试工作台、怎么跑测试、改了界面之后怎么验证。
接入 Agent、HTTP/MCP 接口见 [README](../README.zh-CN.md) 的「快速开始」与 [docs/api.md](api.md)。

下面每条命令都在本机实跑过（2026-11-24）。除特别说明外，都在仓库根目录执行。

## 1. 使用（运行工作台）

`dist\miderhive.exe` 可直接运行（双击或命令行），`dist\BUILD_INFO.txt` 记着它的版本、提交号和调试方法。

- 数据目录是 `%USERPROFILE%\.miderhive`，端口 8787——**与正式安装的版本是同一份**。
- 单实例按「数据目录 + 端口」判断：如果已安装的工作台正开着，再启动 `dist` 版只会把已有窗口唤到前台，看起来像"没反应"。

### 不碰真实数据地试用

换一个数据目录和端口，就是另一个互不相干的实例：

```powershell
$env:MIDERHIVE_HOME = "$env:TEMP\mh-scratch"
$env:MIDERHIVE_PORT = "8790"
.\dist\miderhive.exe
```

隔离的只是数据目录。界面设置（语言、主题、窗口位置）仍然读写本机设置，和平时运行一样。

空实例里什么都没有。灌一批演示数据（保持上面的实例开着，另开一个终端）：

```powershell
$env:MIDERHIVE_MASTER_KEY = (Get-Content "$env:TEMP\mh-scratch\config\master.key" -Raw).Trim()
python scripts\seed_demo_data.py 8790 "$env:TEMP\mh-scratch"
```

### 常用快捷键

| 按键 | 作用 |
|---|---|
| `Ctrl+1` … `Ctrl+9` | 切换面板（总览、用量、知识库、技能、记忆、消息、错误、日志、图谱） |
| `Ctrl+F` | 聚焦当前面板的即时筛选框（`Esc` 清空） |
| `Ctrl+Q` | 临时问答（不计入预算） |
| `Ctrl+,` | 设置 |

### 没有 LLM 密钥时试问答与自动摘要

仓库里有一个本地模拟服务，只监听 `127.0.0.1`，不需要任何真实凭据：

```powershell
python scripts\llm_mock_server.py          # 监听 127.0.0.1:8788
```

然后在 **设置 → LLM 接入** 里选「自定义 OpenAI 兼容端点」，接口地址填 `http://127.0.0.1:8788`，模型填 `mock-model`，密钥随意。「测试连接」、临时问答和「立即沉淀」都会得到固定的流式回复；把协议换成 Anthropic 也一样支持。

## 2. 测试

| 命令 | 检查什么 | 耗时 |
|---|---|---|
| `powershell -ExecutionPolicy Bypass -File scripts\ci_local.ps1` | CI 的全部 11 项检查（单测、集成、GUI 自测、UI harness……）。**推送前必跑** | 约 2 分钟 |
| `.\build\tests\Release\platform_tests.exe` | 核心/集成测试，末尾打印 `checks: N, failures: M` | 约 20 秒 |
| `powershell -ExecutionPolicy Bypass -File scripts\ui_harness.ps1` | UI：47 个场景的结构快照、Tab 焦点顺序与基线对比，加一组界面规则和"用户状态能否活过自动刷新" | 约 20 秒 |
| `python scripts\ui_harness_rounds.py --rounds 5` | 把 harness 连跑 5 轮（每轮一个全新数据目录），要求快照和金样逐字节一致 | 每轮 20 秒 |

这些脚本都只构建、不编译：改了代码先 `cmake --build build --config Release -j 4`。

`ci_local.ps1` 和 `ui_harness.ps1` 的退出码就是结果，任何一项失败都是非 0。

UI harness 的常用参数：

- `-Filter logs`：只跑名字里含 `logs` 的场景（子串匹配）。
- `-OpenReport`：跑完在浏览器里打开报告。
- `-UpdateBaseline`：重新录制基线（见下一节）。

报告在 `build\ui_harness\ui_report.html`：每个场景一张截图、全部规则违例；同一目录下还有每个场景的 JSON 快照。结构差异会直接在输出里指出差在哪（JSON 路径和两边的值）。harness 的退出码：`0` 一致且无违例；`1` 没能运行（找不到 exe、没有字体、过滤词没命中任何场景……）；`2` 有差异或违例。

### 手动跑一次端到端协作（可选）

两个模拟 Agent 之间"注册 → 写记忆 → 注册技能 → 调用 → 报错互通 → 事件流"的完整链路。它不在 CI 里，想单独验证这条链路时用：

```powershell
$env:MIDERHIVE_HOME = "$env:TEMP\mh-e2e"; $env:MIDERHIVE_PORT = "18795"
$env:MIDERHIVE_MASTER_KEY = "e2e-master-key"
.\dist\platformd.exe            # 无界面的平台服务；跑完在本窗口 Ctrl+C 停掉
```

另开一个终端：

```powershell
$env:MH_BASE = "http://127.0.0.1:18795"; $env:MH_MASTER_KEY = "e2e-master-key"
python scripts\e2e_collaboration_test.py
```

默认 23 步。再设 `$env:MH_ZCODE_KEY`（数据目录 `config\agents.json` 里 `zcode` 的密钥）会多核对一步管理者视角，共 24 步。

### 改了界面之后

harness 是 CI 的必过项，有意的界面改动会让它变红。这是设计如此：让每一次界面变化都被看一眼。

1. 构建后跑 `scripts\ui_harness.ps1`，看报出的差异是不是都是你有意改的。有意之外的差异就是回归，先修。
2. 确认都是有意改动后：`powershell -ExecutionPolicy Bypass -File scripts\ui_harness.ps1 -UpdateBaseline`。
3. `git diff tests/baseline/` 逐场景看一遍**文本**的变化。基线没有人工看过之前不要入库。
4. 基线和代码放在同一个提交里。

不要为了让它变绿去手改基线文件；也不要删场景。

## 3. 调试

### 带断点

`dist` 里部署的是 RelWithDebInfo 构建：优化级别和 Release 相同，另带调试符号（`dist\*.pdb`）。每次部署都会用 Windows 的符号引擎核对一遍（`python scripts\check_symbols.py dist\miderhive.exe` 可以随时自己查）：符号与该 EXE 完全匹配、带行号，函数能解析到 `src\` 下的源码行。

1. 自己启动 `dist\miderhive.exe`（调试时建议用上面「不碰真实数据地试用」里的那两个环境变量）。
2. Visual Studio：**调试 → 附加到进程**，选 `miderhive.exe`。
3. 在 `src\` 下打开源文件下断点。

这个构建是优化过的：断点会命中，但一部分变量会显示为"已优化而不可用"，单步执行可能跳跃。要单步调试器本身（`platformd.exe`、`agent-cli.exe`、`miderhive-mcp.exe`），它们同样带符号。

想让调试器从一开始就接管启动过程，在调试器的启动环境里设置同样的 `MIDERHIVE_HOME` / `MIDERHIVE_PORT` 即可。

不要用 Debug 配置：预编译的 Qt 只有 Release 版，Debug 构建会把调试版 C 运行库和 Qt 的发行版 DLL 混在一起。要符号就用 RelWithDebInfo（`cmake --build build --config RelWithDebInfo`）。

### 崩溃

程序没有内置崩溃转储。崩了之后如果进程还在，可以在任务管理器里对 `miderhive.exe` 右键 →「创建转储文件」，再用 Visual Studio 打开（符号在 `dist\` 里，打开时把 `dist` 加入符号路径）。

### 常用的排查入口

| 想知道什么 | 去哪看 |
|---|---|
| 某次操作有没有落库、是谁做的 | 「日志」面板（`Ctrl+8`）：每次写操作都有记录 |
| 平台自己报了什么错 | 「错误」面板；总览页顶部的健康横幅 |
| 服务活着吗 | `curl http://127.0.0.1:8787/api/health`（不需要密钥） |
| 直接调接口 | `.\dist\agent-cli.exe` 看命令列表；接口说明见 [docs/api.md](api.md) |
| 不开窗口看某个面板长什么样 | `build\ui_harness\<面板>.<状态>.png`，或跑 `scripts\ui_harness.ps1 -Filter <面板>` |
| Qt 自己的警告 | 从终端启动并设 `$env:QT_LOGGING_TO_CONSOLE="1"`；插件加载不出来时再加 `$env:QT_DEBUG_PLUGINS="1"` |

### 环境变量

| 变量 | 作用 |
|---|---|
| `MIDERHIVE_HOME` | 数据目录（默认 `%USERPROFILE%\.miderhive`） |
| `MIDERHIVE_PORT` | 服务端口（默认 8787） |
| `MIDERHIVE_MASTER_KEY` | 主密钥；设了就以它为准（测试与 CI 用） |
| `MIDERHIVE_LLM_KEY` | 调用 LLM 时覆盖设置里保存的密钥（不落盘） |
| `MIDERHIVE_AGENT_NAME` / `MIDERHIVE_AGENT_KEY` | `agent-cli` / MCP 使用的 Agent 身份 |

## 4. 更新 `dist`

每次有新进展，`dist` 都会被重新部署：`powershell -ExecutionPolicy Bypass -File scripts\deploy_dev.ps1`（先编译再部署；加 `-SkipBuild` 只部署）。如果 `dist\miderhive.exe` 正在运行，脚本会拒绝覆盖——先从托盘退出。
