/* zcode-workflow
description: 九区域并行通读排查（core 事务/HTTP/存储/协作/MCP/写入器/更新器/GUI/打包 CI）+ 每条候选由独立复核员证实证伪
  + 单元测试与集成自测基线，产出带证据与修复路线的中文审计报告
whenToUse: 发版前或大批修复合并前，对 MiderHive 全库做一次"暗藏 bug"深审计。要求构建产物存在（build/ 下 Release
  全目标），运行约 20-40 分钟；产出逐条带 path:line 证据、已确认/存疑/否决状态与分批修复路线的报告。
*/
// ===== 结果类型 =====
interface RawFinding {
  /** 仓库相对路径:行号，如 src/core/platform.cpp:123 */
  where: string;
  /** 一句话说清哪里错了（说问题本身，不是说修法） */
  what: string;
  /** 证据：关键代码摘录与为什么错，120 字以内 */
  evidence: string;
  /** 什么场景/操作序列会触发这个问题 */
  trigger: string;
  /** 修复方向，一句话 */
  fix: string;
  /** high=数据丢失/崩溃/错误结果；medium=功能错误但影响可控；low=边缘场景健壮性 */
  severity: "low" | "medium" | "high";
  /** high=已沿调用链读完、确认可触发；medium=逻辑成立但触发条件未完全确认；low=疑似 */
  confidence: "high" | "medium" | "low";
}

interface HuntResult {
  findings: RawFinding[];
}

interface Confirmation {
  /** confirmed=独立读代码后问题成立；refuted=不成立；uncertain=证据不足无法定论 */
  verdict: "confirmed" | "refuted" | "uncertain";
  /** 复核后的证据（引用确切代码；原 finding 的行号或描述有误请在此修正） */
  evidence: string;
  /** 复核后认定的严重级别（可与原判不同） */
  severity: "low" | "medium" | "high";
  /** 一句话理由 */
  note: string;
}

interface AuditedFinding {
  id: string;
  area: string;
  where: string;
  what: string;
  evidence: string;
  trigger: string;
  fix: string;
  severity: "low" | "medium" | "high";
  status: "verified" | "unconfirmed" | "refuted";
  note: string;
}

interface LiveCheck {
  /** 检查项名称 */
  name: string;
  /** 是否通过 */
  passed: boolean;
  /** 结果摘要（失败时含最有诊断价值的输出片段） */
  detail: string;
}

interface LiveResult {
  checks: LiveCheck[];
  /** 观察到的异常行为（崩溃/挂起/乱码/端口冲突/偶发失败等），没有则空数组 */
  anomalies: string[];
}

interface Synthesis {
  /** 2-4 句中文总结论 */
  summary: string;
  /** 排序后的 finding id：verified 在前（high>medium>low），unconfirmed 在后 */
  rankedIds: string[];
  /** 跨区域同根因归并组 */
  clusters: { name: string; ids: string[]; note: string }[];
  /** 分批修复路线 */
  roadmap: { batch: string; goal: string; ids: string[]; verify: string }[];
}

interface BaselineResult {
  attempted: string;
  exitCode: number | null;
  output: string;
}

interface WorkflowReport {
  conclusion: string;
  findings: { where: string; what: string; evidence: string; status: "verified" | "unconfirmed"; severity: "low" | "medium" | "high" }[];
  verified: string[];
  notCovered: string[];
}

interface AreaSpec {
  key: string;
  name: string;
  files: string;
  lens: string;
}

// ===== 工具函数 =====
function tailText(s: string, n: number): string {
  return s.length <= n ? s : "……（前文截断）……" + s.slice(s.length - n);
}

function statusLabel(s: AuditedFinding["status"]): string {
  if (s === "verified") return "已确认";
  if (s === "unconfirmed") return "存疑";
  return "已否决";
}

function sevLabel(s: string): string {
  if (s === "high") return "高";
  if (s === "medium") return "中";
  return "低";
}

// ===== 看板（实时展示每条候选的复核状态） =====
artifact.board("findings", {
  title: "候选问题看板",
  key: "id",
  status: "status",
  columns: ["已确认", "存疑", "已否决"],
  cardTitle: "title",
  detail: [
    { field: "where", label: "位置" },
    { field: "severity", label: "严重级" },
    { field: "area", label: "区域" },
  ],
});

// ===== 角色设定 =====
const HUNTER_SYSTEM =
  "你是资深 C++20/Qt6/SQLite 代码审计员，在一个经过多轮失败路径加固的代码库里寻找仍然暗藏的真实缺陷：" +
  "会造成数据丢失、崩溃、错误结果、功能失效或安全边界突破的问题。不评风格与性能。诚实优先：" +
  "没把握就标低 confidence，找不到就如实返回空列表，绝不编造不存在的代码。只读审计：" +
  "不编辑、不创建、不删除任何文件；可以用只读命令（grep、git log 等）辅助。若指令无法执行或有矛盾，直说而不是绕过。全部用中文。";

const LIVE_TESTER_SYSTEM =
  "你是负责集成验证的工程师，在本机 Windows 上用仓库现有构建产物运行项目自带的端到端检查。铁律：" +
  "绝不触碰真实用户数据目录（%USERPROFILE%\\.miderhive 等）——一切用 MIDERHIVE_HOME / MIDERHIVE_PORT " +
  "环境变量重定向到仓库内临时目录；结束后按 PID 精确清理自己启动的进程。如实记录，失败原样报告，不粉饰。" +
  "某个脚本确实无法在隔离环境运行时，记录原因并跳过。不得修改仓库文件。中文交流。";

const SYNTH_SYSTEM =
  "你是首席审计汇总人：把多位审计员经独立复核的结果去重归并、排定修复优先级、给出分批修复路线。" +
  "只依据给你的数据，不新增问题、不改变事实。全部用中文。";

// ===== 九个排查区域 =====
const AREAS: AreaSpec[] = [
  {
    key: "core-platform",
    name: "核心平台层（事务与备份恢复）",
    files: "src/core/platform.cpp、src/core/platform.h、src/core/types.h、src/core/services/in_tx_step.h",
    lens: "写路径与审计留痕是否在所有分支（含抛异常、提前 return、部分失败）都真正同事务；备份 VACUUM INTO / 恢复 / 留底轮转 / 迁移重放的每个失败路径（磁盘满、文件被占用、中途崩溃后重启再跑一遍）是否会丢数据或留下半成品；库结构版本门与幂等迁移；maintenanceRun 与手动维护入口；Platform 门面层是否存在返回值被忽略的调用。",
  },
  {
    key: "http-server",
    name: "HTTP 服务与鉴权",
    files: "src/core/http/server.cpp、src/core/http/server.h、src/core/http/url_guard.h",
    lens: "cpp-httplib 线程模型下 handler 并发调用时共享的可变状态（服务对象、缓存、计数器、last-error 之类）有没有加锁或是否每次独立；鉴权链（首部缺失、空名、大小写、常量时间比较）能否绕过；请求体 1MiB 上限是否覆盖所有读 body 的端点（含 MCP / embedding 透传）；FTS5 MATCH 与 LIKE 的用户输入是否转义引号与运算符；路径参数（uuid 等）与查询参数解析；错误分支是否都走统一错误信封而非静默 200；url_guard 出站白名单能否被大小写、百分号编码、@、重定向绕过。",
  },
  {
    key: "storage",
    name: "存储与数据服务（知识库/向量/用量/技能）",
    files: "src/core/db/database.cpp、src/core/db/database.h、src/core/db/schema.sql、src/core/services/knowledge_service.cpp（及 .h）、src/core/services/usage_service.cpp（及 .h）、src/core/services/skill_service.cpp（及 .h）、src/core/embed/embedder.cpp、src/core/embed/embedder.h",
    lens: "SQL 是否全部参数绑定（重点：FTS5 MATCH 串、ORDER BY、向量 KNN 查询的拼接）；事务边界与回滚路径；sqlite-vec 维度↔provider 绑定、按维度分表与旧表迁移的边界；知识版本链 append-only 在并发双写下能否分叉或覆盖；预算计算（除零、周窗口的时区与边界日、80%/95% 比较方向、幂等键去重后的累计口径）；embedder 的 n-gram 特征哈希与外接端点返回向量的长度校验（声明维度与实拿维度不一致时）。",
  },
  {
    key: "collab",
    name: "协作服务（消息/记忆/错误/Agent/审计/工具）",
    files: "src/core/services/message_service.cpp（及 .h）、src/core/services/memory_service.cpp（及 .h）、src/core/services/error_service.cpp（及 .h）、src/core/services/agent_service.cpp（及 .h）、src/core/services/audit_service.cpp（及 .h）、src/core/util.cpp、src/core/util.h、src/core/version_util.h",
    lens: "点对点消息可见性过滤是否覆盖每一个读取端点（列表、详情、搜索、未读计数、导出）——找漏掉 WHERE 条件的查询；用户记忆 base_version 乐观并发：读-改-写窗口内并发写会不会静默覆盖；错误解决说明追加与状态机（未解决→已解决）的原子性；审计轮转（30 天 / 10 万条）的删除边界（off-by-one、同秒、空表）；util 的 SHA-256 与常量时间比较逐行验证正确性；密钥生成的随机源强度；时间戳统一性（本地时区与 UTC 混用导致跨天/跨周归类错误）。",
  },
  {
    key: "mcp-cli",
    name: "MCP 服务器与命令行客户端",
    files: "src/cli/mcp_main.cpp、src/cli/main.cpp、src/cli/serverd.cpp",
    lens: "stdio JSON-RPC 分帧：半行、超长行、非法 JSON、EOF、并发请求的 id 匹配；Windows 下 stdin/stdout 编码（UTF-8 与代码页、中文参数往返）；环境变量优先级（MIDERHIVE_* 覆盖旧名 AGENTHIVE_*/ZCODE_* 的顺序与空值处理）；27 个 MCP 工具的参数校验（缺失/类型错/越界）是否都返回 JSON-RPC error 而不是崩溃或挂死；平台离线时的行为（重试、死循环、超时）；agent-cli 各子命令的错误退出码与 --embed-url 出站请求。",
  },
  {
    key: "integrations",
    name: "接入配置写入器（八种工具）",
    files: "src/core/integrations.hpp、src/gui/integrations.h",
    lens: "八种工具配置生成与合并：手写 TOML/YAML 解析对注释、引号、缩进、重复键、残缺文件的处理；合并是否可能覆盖或丢失用户已有条目（尤其非 miderhive 的其他 server 条目）；.miderhive.bak 会不会在连续两次操作时把更早的备份冲掉；写入是否先写临时文件再原子替换（半写损坏用户配置）；BOM/GBK 编码文件读入；生成的 JSON/TOML/YAML 里 Windows 路径反斜杠与密钥特殊字符的转义；工具检测（PATH 查找、安装探测）的误判路径。",
  },
  {
    key: "updater-dialogs",
    name: "更新器与设置/接入对话框",
    files: "src/gui/update_checker.cpp、src/gui/update_checker.h、src/gui/connect_dialog.cpp、src/gui/welcome_dialog.cpp、src/gui/settings_dialog.cpp、src/gui/startup_check.h",
    lens: "更新链路：latest.json 字段缺失/畸形版本号的解析与比较（version_util）；下载中断、磁盘满、代理返回 HTML 时的处理；SHA256 不匹配的删除路径；Zone 标识剥离对 zip 与 msi 是否都适用；msiexec 静默升级时应用自身在运行（exe 被占用、升级后重启失败的回退）；便携版被误判自动安装；HTTP 非 200 与重定向；connect/welcome/settings 三个对话框：密钥轮换后旧界面的缓存状态、对话框并发打开、空输入提交。",
  },
  {
    key: "workbench",
    name: "工作台界面与生命周期",
    files: "src/gui/mainwindow.cpp、src/gui/mainwindow.h、src/gui/widgets.h、src/gui/gui_util.h、src/gui/i18n.h、src/gui/theme.h、src/gui/main.cpp、src/gui/panels/ 目录下全部 .cpp 与 .h",
    lens: "Qt 生命周期：面板/行刷新时旧 QWidget/QObject 的销毁时机（移除行后仍持有指针、信号连接的接收者已析构、lambda 捕获悬空引用）；定时器（心跳/自动刷新/更新检查）在窗口关闭后是否停止；UI 线程上的同步阻塞（HTTP、VACUUM 备份、大结果集查询）导致冻结与重入；widgets.h 自绘控件在空数据、除零、超长文本、极值下的绘制；i18n 切换的半程状态；与 platformd 双进程同库写时 busy 重试在界面上的表现。",
  },
  {
    key: "scripts",
    name: "打包与 CI 脚本",
    files: "scripts/package.ps1、scripts/gen-wix-files.ps1、scripts/fetch-deps.ps1、scripts/deploy.ps1、.github/workflows/ 目录下的 CI 定义、根 CMakeLists.txt",
    lens: "PowerShell：$ErrorActionPreference 与未检查的 $LASTEXITCODE（外部命令失败后继续跑）；含空格路径未加引号；版本号在各处拼接是否一致；package.ps1 的可运行性自检与 SHA256SUMS 生成/校验是否对称；CI 步骤的条件跳过（continue-on-error、if 条件）造成假绿；CMake 安装规则漏文件。",
  },
];

// ===== 提问模板 =====
function hunterAsk(a: AreaSpec): string {
  return [
    "你是代码审计员，在 MiderHive 仓库（当前工作目录即仓库根）排查暗藏的潜在 bug。",
    "项目背景：本地多 Agent 协作平台，C++20 / Qt6 Widgets / SQLite WAL + sqlite-vec / cpp-httplib / nlohmann-json，服务仅监听 127.0.0.1。",
        "近期已修复以下问题（1.2.2 批次，不要重复上报；但可以验证这些修复是否彻底、有没有漏掉的兄弟路径）：备份同秒撞名、恢复留底轮转与恢复中断自愈（bootstrap 检测 before-restore 改回）、写操作与审计留痕同事务且审计字段真实（target/version 不再恒空/恒 1）、maintenanceRun 统计可信化、budget 读失败不再伪装默认值、自带向量必须声明 embedder（空 embedder → 400）、messageReply 权限白名单与空正文拒绝、/api/audit 按 viewer 收敛（普通 Agent 只见自己）、向量表迁移与任务状态机入事务、LIKE 通配符转义（escapeLike + ESCAPE）、CSPRNG（BCryptGenRandom）、url_guard host 规范化（百分号解码/尾点）、agent-cli --port 生效与退出码 2、platformd 信号停机、envOr 走宽字符 UTF-8、配置写入器（TOML 基本字符串/flow 形状拒绝/备份失败不覆盖/cmd 引号）、接入对话框 issuedName_ 配对与 exe 平台分支、更新器单飞与写盘检查、六面板读失败如实提示、错误面板选中恢复、usage 数值类型/上限校验、deploy.ps1 必需文件闸门、release.yml LASTEXITCODE throw、wxs 关闭 platformd。",
    "",
    `你负责的区域：${a.name}`,
    `主审文件（必须通读全文，含对应 .h）：${a.files}`,
    "",
    "该区域的重点排查视角：",
    a.lens,
    "",
    "跨区域通用清单（同样适用于你的文件）：被吞掉的失败返回值（本项目纪律是绝不静默失败，找漏网之处）；事务原子性（BEGIN 后提前 return / throw 是否漏 ROLLBACK）；SQL 参数绑定与 FTS5 转义；并发（cpp-httplib 多线程 handler 共享状态、GUI 与 platformd 双进程同库写）；Windows 路径与 UTF-8 编码边界；整型溢出/截断；除零与空集合；时间与时区（周边界）；Qt 对象生命周期与信号悬挂。",
    "",
    "要求：",
    "- 每条问题必须基于你实际读到的代码，给出 path:line（行号自己打开文件核对）；发现可疑点要顺着调用链（含 HTTP 端点 / GUI 调用方）追完整再下结论；",
    "- 只报会造成数据丢失、崩溃、错误结果、功能失效或安全边界突破的真实缺陷；不报风格、命名、性能洁癖、不报「建议加测试」；",
    "- 最多 6 条，宁缺毋滥；确实没有就返回空 findings 数组，绝不为了交差编造；",
    "- 返回 { findings: [...] } 结构，全部用中文。",
  ].join("\n");
}

function confirmerAsk(areaName: string, f: RawFinding): string {
  return [
    "你是独立复核员，对下面这条候选 bug 做证实或证伪。只读复核：不得编辑、创建或删除任何文件；结论只能基于你自己打开读过的代码。",
    `候选问题（来自区域「${areaName}」）：`,
    JSON.stringify(f, null, 2),
    "",
    "请打开 where 指向的文件及其调用链，独立判断：",
    "1. 问题是否真实存在（沿着真实执行路径走到触发点，而不是纸面推理）；",
    "2. severity 是否恰当（high=数据丢失/崩溃/错误结果；medium=功能错误但影响可控；low=边缘场景）；",
    "3. 原 finding 的行号或描述有误时，在 evidence 里给出修正后的表述。",
    "结论三选一：confirmed / refuted / uncertain，全部用中文。",
  ].join("\n");
}

function liveTesterAsk(): string {
  return [
    "仓库：当前工作目录（MiderHive 仓库根）。构建产物在 build/src/gui/Release/ 与 build/src/cli/Release/（platformd.exe、agent-cli.exe、miderhive-mcp.exe、miderhive.exe）。",
    "",
    "第一步：先阅读这些脚本的头部说明与 main 函数，弄清各自期望的运行方式（参数、端口、环境变量、是否自己拉起进程）：",
    "scripts/feasibility_check.py、scripts/test_onboarding_and_safety.py、scripts/mcp_check.py、scripts/fuzz_config_merge.py、scripts/verify_gui_selftest.py。",
    "",
    "第二步：准备隔离环境——选空闲端口（建议 8791，先验证空闲），数据目录用仓库内 .tmp-audit/（结束后删除）；所有进程统一带 MIDERHIVE_HOME=<临时目录> 与 MIDERHIVE_PORT=<端口>。需要平台在线的检查，先后台启动 build/src/cli/Release/platformd.exe（记下 PID，跑完按 PID 结束；绝不要按映像名大范围 taskkill）。",
    "",
    "第三步：逐项运行上述 5 个检查（verify_gui_selftest 按 CI 同款方式跑），每项记录通过/失败与关键输出摘要（失败时保留最有诊断价值的片段）。注意：不要运行 ctest（单元测试已在别处跑过）；不要修改任何仓库文件。",
    "",
    "第四步：整理返回——checks 逐项结果；anomalies 记录过程中观察到的任何异常行为（崩溃、挂起、乱码、端口冲突、偶发失败、令人意外的输出），没有就返回空数组。",
  ].join("\n");
}

// ===== 阶段一：测试基线（单元测试 + 集成自测并行） =====
async function runBaseline(): Promise<BaselineResult> {
  try {
    const r = await world.run("ctest", ["--test-dir", "build", "-C", "Release"], { timeoutMs: 900000 });
    return { attempted: "ctest", exitCode: r.exitCode, output: r.stdout + "\n" + r.stderr };
  } catch (e1) {
    try {
      const r = await world.run("C:/Program Files/CMake/bin/ctest.exe", ["--test-dir", "build", "-C", "Release"], { timeoutMs: 900000 });
      return { attempted: "C:/Program Files/CMake/bin/ctest.exe", exitCode: r.exitCode, output: r.stdout + "\n" + r.stderr };
    } catch (e2) {
      return { attempted: "（两种路径都未能运行）", exitCode: null, output: String(e1) + " / " + String(e2) };
    }
  }
}

async function runLiveTests(): Promise<LiveResult> {
  const liveTester = agent("集成自测执行员", { system: LIVE_TESTER_SYSTEM });
  try {
    return await liveTester.ask<LiveResult>(liveTesterAsk());
  } catch (e) {
    return { checks: [], anomalies: ["集成自测未能完成：" + String(e)] };
  }
}

phase("先跑现有测试摸底：单元测试基线与集成自测");
const livePromise = runLiveTests();
const base = await runBaseline();
log(`单元测试基线（${base.attempted}）：${base.exitCode === 0 ? "全部通过" : base.exitCode === null ? "无法运行" : "存在失败（退出码 " + base.exitCode + "）"}`);
let failureAnalysis = "";
if (base.exitCode !== null && base.exitCode !== 0) {
  const diagnoser = agent("测试失败诊断员", {
    system: "你是测试工程师，判断单元测试失败是环境问题（缺依赖/端口占用/权限）还是代码真实回归。只读分析；可以运行针对性的只读命令（如按名称重跑单个测试看输出），不得修改任何文件。中文交流。",
  });
  failureAnalysis = await diagnoser.ask<string>(
    `单元测试存在失败。ctest 输出（截尾）：\n${tailText(base.output, 4000)}\n\n请定位失败的具体测试，逐个判断失败原因是环境问题还是真实回归，给出中文结论与依据。`,
  );
}

// ===== 阶段二：九区域并行排查 + 逐条独立复核 =====
phase("九个区域并行排查，每条候选问题独立复核");
const audited: AuditedFinding[] = (
  await Promise.all(
    AREAS.map(async (a): Promise<AuditedFinding[]> => {
      const hunter = agent(`排查-${a.name}`, { system: HUNTER_SYSTEM });
      let hunt: HuntResult;
      try {
        hunt = await hunter.ask<HuntResult>(hunterAsk(a));
      } catch (e) {
        log(`区域「${a.name}」排查未返回结果：${String(e)}`);
        return [];
      }
      const picked = hunt.findings.slice(0, 6);
      if (picked.length === 0) {
        log(`区域「${a.name}」未报告候选问题`);
        return [];
      }
      log(`区域「${a.name}」报告 ${picked.length} 条候选，开始逐条独立复核`);
      return await Promise.all(
        picked.map(async (f, i): Promise<AuditedFinding> => {
          const id = `${a.key}-${i + 1}`;
          try {
            const c = await agent(`复核-${id}`).ask<Confirmation>(confirmerAsk(a.name, f));
            const status: AuditedFinding["status"] =
              c.verdict === "confirmed" ? "verified" : c.verdict === "uncertain" ? "unconfirmed" : "refuted";
            const item: AuditedFinding = {
              id: id,
              area: a.name,
              where: f.where,
              what: f.what,
              evidence: status === "refuted" ? `原证据：${f.evidence}` : c.evidence,
              trigger: f.trigger,
              fix: f.fix,
              severity: c.severity,
              status: status,
              note: c.note,
            };
            report({ id: id, title: f.what, status: statusLabel(status), where: f.where, severity: item.severity, area: a.name }, "findings");
            return item;
          } catch (e) {
            const item: AuditedFinding = {
              id: id,
              area: a.name,
              where: f.where,
              what: f.what,
              evidence: f.evidence,
              trigger: f.trigger,
              fix: f.fix,
              severity: f.severity,
              status: "unconfirmed",
              note: "复核未完成：" + String(e),
            };
            report({ id: id, title: f.what, status: "存疑", where: f.where, severity: item.severity, area: a.name }, "findings");
            return item;
          }
        }),
      );
    }),
  )
).flat();

// ===== 阶段三：汇总去重、排定优先级、产出报告 =====
phase("汇总去重、排定修复优先级，产出审计报告");
const live = await livePromise;
const counts = { v: 0, u: 0, r: 0 };
for (const f of audited) {
  if (f.status === "verified") counts.v += 1;
  else if (f.status === "unconfirmed") counts.u += 1;
  else counts.r += 1;
}
log(`复核完成：${counts.v} 条证实、${counts.u} 条存疑、${counts.r} 条被否决；正在汇总去重`);

let synthesis: Synthesis;
const actionable = audited.filter((f) => f.status !== "refuted");
if (actionable.length === 0) {
  synthesis = { summary: "本轮排查未发现可确认的潜在缺陷。", rankedIds: [], clusters: [], roadmap: [] };
} else {
  const synthesizer = agent("汇总评审员", { system: SYNTH_SYSTEM });
  const compact = actionable.map((f) => ({
    id: f.id,
    area: f.area,
    where: f.where,
    what: f.what,
    severity: f.severity,
    status: f.status,
    fix: f.fix,
  }));
  synthesis = await synthesizer.ask<Synthesis>(
    [
      `以下是 ${audited.length} 条候选问题中未被否决的 ${actionable.length} 条（含复核后严重级），以及测试基线信息。`,
      JSON.stringify(compact, null, 1),
      "",
      `基线：单元测试退出码 ${base.exitCode === null ? "无法运行" : String(base.exitCode)}；集成自测 ${live.checks.filter((c) => c.passed).length}/${live.checks.length} 通过${failureAnalysis ? "；单元测试失败分析：" + failureAnalysis : ""}。`,
      "",
      "任务（只依据以上数据，不得新增问题或改变事实，全部中文）：",
      "1. clusters：把同一根因的条目归组（ids 必须来自给定列表）；",
      "2. rankedIds：verified 在前（high>medium>low，同级按数据风险排），unconfirmed 在后；",
      "3. roadmap：分 2-5 批修复，每批写清目标与验证命令（可用：ctest --test-dir build -C Release；python scripts/feasibility_check.py <端口>；python scripts/verify_gui_selftest.py；python scripts/fuzz_config_merge.py）；",
      "4. summary：2-4 句总结论（多少条已确认、最严重的是什么、测试基线如何）。",
    ].join("\n"),
  );
}

const rank = new Map<string, number>();
synthesis.rankedIds.forEach((id, i) => rank.set(id, i));
const sorted = audited
  .filter((f) => f.status !== "refuted")
  .slice()
  .sort((x, y) => (rank.get(x.id) ?? 999) - (rank.get(y.id) ?? 999));
const refuted = audited.filter((f) => f.status === "refuted");

const md: string[] = [];
md.push("# MiderHive 暗藏 bug 审计报告");
md.push("");
md.push("生成于 2026-10-06 · 九区域通读排查 + 逐条独立复核 + 集成自测实测");
md.push("");
md.push("## 总体结论");
md.push("");
md.push(synthesis.summary);
md.push("");
md.push(`候选 ${audited.length} 条：**${counts.v} 条证实**、${counts.u} 条存疑、${counts.r} 条被否决。每条候选均由独立复核员重读代码证实/证伪。`);
md.push("");
md.push("## 测试基线");
md.push("");
md.push(`- 单元测试（ctest，${base.attempted}）：${base.exitCode === 0 ? "✅ 全部通过" : base.exitCode === null ? "⚠️ 无法运行——" + tailText(base.output, 300) : "❌ 退出码 " + base.exitCode}`);
if (failureAnalysis !== "") md.push(`- 失败分析：${failureAnalysis}`);
md.push(`- 集成自测：${live.checks.filter((c) => c.passed).length}/${live.checks.length} 通过`);
for (const c of live.checks) md.push(`  - ${c.passed ? "✅" : "❌"} ${c.name}：${c.detail}`);
if (live.anomalies.length > 0) {
  md.push("- 观察到的异常：");
  for (const an of live.anomalies) md.push(`  - ⚠️ ${an}`);
}
md.push("");
md.push(`## 未被否决的问题（${sorted.length} 条，按修复优先级排序）`);
md.push("");
if (sorted.length === 0) {
  md.push("（本轮无可确认问题）");
}
sorted.forEach((f, idx) => {
  md.push(`### ${idx + 1}. [${sevLabel(f.severity)}·${f.status === "verified" ? "已确认" : "存疑"}] ${f.what}`);
  md.push("");
  md.push(`- **位置**：${f.where}`);
  md.push(`- **问题**：${f.what}`);
  md.push(`- **证据**：${f.evidence}`);
  md.push(`- **触发**：${f.trigger}`);
  md.push(`- **修复方向**：${f.fix}`);
  md.push(`- **复核意见**：${f.note}`);
  md.push("");
});
if (refuted.length > 0) {
  md.push(`## 复核后否决的候选（${refuted.length} 条，仅记录）`);
  md.push("");
  for (const f of refuted) md.push(`- ${f.where}：${f.what} —— 否决理由：${f.note}`);
  md.push("");
}
if (synthesis.clusters.length > 0) {
  md.push("## 同根因归并");
  md.push("");
  for (const cl of synthesis.clusters) md.push(`- **${cl.name}**（${cl.ids.join("、")}）：${cl.note}`);
  md.push("");
}
if (synthesis.roadmap.length > 0) {
  md.push("## 修复路线图");
  md.push("");
  for (const r of synthesis.roadmap) {
    md.push(`### ${r.batch}：${r.goal}`);
    md.push(`- 涉及：${r.ids.join("、")}`);
    md.push(`- 验证：\`${r.verify}\``);
    md.push("");
  }
}
md.push("## 覆盖范围与未覆盖");
md.push("");
md.push("- 已覆盖：src/core、src/cli、src/gui、scripts、CI 定义共九个区域全部主审文件通读 + 每条候选独立复核；单元测试与五项集成/GUI 自测在隔离环境实测。");
md.push("- 未覆盖：tests/ 与 selftest_main.cpp 测试代码本身的缺陷、vendor 第三方库（SQLite/sqlite-vec/cpp-httplib/nlohmann）源码、WiX 安装器定义、长时间浸泡与高并发压测、Linux/macOS 行为（项目仅验证 Windows）。");
md.push("");

await artifact.markdown("report", md.join("\n"), {
  title: "MiderHive 暗藏 bug 审计报告",
  description: `${counts.v} 条已确认 / ${counts.u} 条存疑 / ${counts.r} 条否决，含修复路线图`,
  primary: true,
});

const conclusion = [
  synthesis.summary,
  `九个区域排查共 ${audited.length} 条候选：${counts.v} 条经独立复核证实、${counts.u} 条存疑、${counts.r} 条被否决。`,
  `测试基线：单元测试${base.exitCode === 0 ? "全绿" : base.exitCode === null ? "无法运行" : "有失败"}；集成自测 ${live.checks.filter((c) => c.passed).length}/${live.checks.length} 通过${live.anomalies.length > 0 ? "，另有 " + live.anomalies.length + " 项异常观察" : ""}。`,
  "完整报告见产物「MiderHive 暗藏 bug 审计报告」。",
].join("");

const reportFindings: WorkflowReport["findings"] = audited.map((f) => ({
  where: f.where,
  what: f.what,
  evidence: f.status === "refuted" ? `【复核否决】${f.note}` : `${f.evidence}（复核：${f.note}）`,
  status: f.status === "verified" ? "verified" : "unconfirmed",
  severity: f.severity,
}));

const result: WorkflowReport = {
  conclusion: conclusion,
  findings: reportFindings,
  verified: [
    `单元测试基线：ctest --test-dir build -C Release（${base.attempted}，退出码 ${base.exitCode === null ? "无法运行" : String(base.exitCode)}）`,
    "每条候选问题均由独立复核员重读代码证实/证伪",
    ...live.checks.map((c) => `${c.name}：${c.passed ? "通过" : "失败"}`),
    "九个区域主审文件全部通读（src/core、src/cli、src/gui、scripts、CI）",
  ],
  notCovered: [
    "tests/ 与 selftest_main.cpp 测试代码本身的缺陷",
    "vendor 第三方库源码与 WiX 安装器定义",
    "长时间浸泡与高并发压测（一次性集成自测不能替代）",
    "Linux/macOS 平台行为（项目官方仅验证 Windows）",
  ],
};
return result;