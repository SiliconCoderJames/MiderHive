#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MiderHive 端到端协作测试（独立运行，仅用 Python 标准库）。

测试链路（严格按序执行，任一步失败立即停止并打印原始报错）：
  0  平台健康检查 GET /api/health
  1  A 注册            POST /api/agents/register   （需 X-Master-Key）
  2  A 写入共享记忆     POST /api/memory
  3  A 注册技能         POST /api/skills
  4  B 注册            POST /api/agents/register
  5  B 查询技能列表     GET  /api/skills            —— 必须看到 A 的技能
  6  B 调用该技能       POST /api/skills/<name>/invoke
  7  调用记录入库核验   GET  /api/skills/<name>/invocations
  8  B 写入错误日志     POST /api/errors
  9  A 查询错误日志     GET  /api/errors?status=open —— 必须看到 B 的记录
 10  协作页事件流核验   GET  /api/audit             —— 必须含 error.report 事件
 11  附加：B 读取共享记忆 GET /api/memory?section=project（验证共享可见性）

环境变量：
  MH_BASE        平台地址（默认 http://127.0.0.1:8790）
  MH_MASTER_KEY  主密钥（默认 e2e-master-key，须与服务端一致）
  MH_AGENT_A / MH_AGENT_B / MH_SKILL   名称前缀（默认 alpha-e2e / beta-e2e / e2e-hello）

用法：
  python scripts/e2e_collaboration_test.py
退出码：0=全部通过；1=在首个失败步骤停止。
"""

import json
import os
import sys
import urllib.error
import urllib.request

BASE = os.environ.get("MH_BASE", "http://127.0.0.1:8790").rstrip("/")
MASTER_KEY = os.environ.get("MH_MASTER_KEY", "e2e-master-key")
AGENT_A = os.environ.get("MH_AGENT_A", "alpha-e2e")
AGENT_B = os.environ.get("MH_AGENT_B", "beta-e2e")
SKILL = os.environ.get("MH_SKILL", "e2e-hello")
TIMEOUT = 15

_passed = 0


def call(method, path, body=None, name=None, key=None, master=False):
    """发一次 HTTP 请求，返回 (http_status, 解析后的JSON或原始文本)。"""
    url = BASE + path
    data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if master:
        req.add_header("X-Master-Key", MASTER_KEY)
    if name and key:
        req.add_header("X-Agent-Name", name)
        req.add_header("X-Api-Key", key)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            return resp.status, _parse(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, _parse(e.read())
    except Exception as e:  # 连接失败等
        return None, f"{type(e).__name__}: {e}"


def _parse(raw):
    try:
        return json.loads(raw.decode("utf-8"))
    except Exception:
        return raw.decode("utf-8", "replace")


def step(no, title, ok, detail=""):
    """打印一步的结果；失败则打印原始响应并立即终止。"""
    global _passed
    mark = "PASS" if ok else "FAIL"
    print(f"[{mark}] 步骤 {no:>2}  {title}" + (f"   —— {detail}" if detail and ok else ""))
    if not ok:
        print(f"\n========== 在步骤 {no} 失败：{title} ==========")
        print(detail)
        print("========== 原始报错如上，测试终止 ==========")
        sys.exit(1)
    _passed += 1


def expect_env(resp, what):
    """校验平台响应包络：HTTP 2xx 且 code==0。返回 data 或终止。"""
    status, payload = resp
    ok = status is not None and 200 <= status < 300 and isinstance(payload, dict) and payload.get("code") == 0
    if not ok:
        step("?", what, False, f"HTTP={status} 原始响应={payload!r}")
    return payload.get("data")


def main():
    print(f"目标平台: {BASE}")
    print(f"测试身份: A={AGENT_A}  B={AGENT_B}  技能={SKILL}")
    print("=" * 72)

    # ---- 0 健康检查 ----
    status, payload = call("GET", "/api/health")
    ver = payload.get("data", {}).get("version") if isinstance(payload, dict) else "?"
    step(0, "平台健康检查 /api/health", status == 200 and isinstance(payload, dict) and payload.get("code") == 0,
         f"服务版本 v{ver}")

    # ---- 1 A 注册 ----
    data = expect_env(call("POST", "/api/agents/register", {"name": AGENT_A, "role": "member"}, master=True),
                      "A 注册")
    key_a = data.get("api_key", "")
    step(1, f"A（{AGENT_A}）注册", bool(key_a), f"已拿到 A 的密钥（内容不打印）")

    # ---- 2 A 写入共享记忆 ----
    mem_value = f"E2E-{AGENT_A}-协作测试记忆条目"
    data = expect_env(call("POST", "/api/memory",
                           {"section": "project", "key": "e2e-check", "value": mem_value},
                           name=AGENT_A, key=key_a),
                      "A 写入共享记忆")
    step(2, "A 写入共享记忆 project/e2e-check", data.get("version") == 1, f"版本 v{data.get('version')}")

    # ---- 3 A 注册技能 ----
    schema = {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}
    data = expect_env(call("POST", "/api/skills",
                           {"name": SKILL, "display_name": "E2E 测试技能",
                            "description": "端到端测试注册的示例技能",
                            "category": "e2e", "param_schema": schema},
                           name=AGENT_A, key=key_a),
                      "A 注册技能")
    step(3, f"A 注册技能 {SKILL}",
         data.get("owner_agent") == AGENT_A and data.get("status") == "active",
         f"owner={data.get('owner_agent')} status={data.get('status')}")

    # ---- 4 B 注册 ----
    data = expect_env(call("POST", "/api/agents/register", {"name": AGENT_B, "role": "member"}, master=True),
                      "B 注册")
    key_b = data.get("api_key", "")
    step(4, f"B（{AGENT_B}）注册", bool(key_b), "已拿到 B 的密钥（内容不打印）")

    # ---- 5 B 查询技能列表（必须看到 A 的技能）----
    data = expect_env(call("GET", "/api/skills", name=AGENT_B, key=key_b), "B 查询技能列表")
    hit = next((s for s in data if s.get("name") == SKILL), None)
    step(5, "B 查询技能列表必须看到 A 的技能",
         hit is not None and hit.get("owner_agent") == AGENT_A,
         f"共 {len(data)} 个技能，命中 owner={hit.get('owner_agent') if hit else '未找到'}")

    # ---- 6 B 调用该技能 ----
    data = expect_env(call("POST", f"/api/skills/{SKILL}/invoke",
                           {"params": {"text": "hello from B"}, "result_summary": "B 端调用成功",
                            "status": "success", "duration_ms": 5,
                            "tokens_in": 100, "tokens_out": 20},
                           name=AGENT_B, key=key_b),
                      "B 调用技能")
    step(6, "B 调用 A 的技能",
         "remaining_tokens" in data and "budget" in data,
         f"预算={data.get('budget')} 剩余={data.get('remaining_tokens')} 告警级={data.get('alert_level')}")

    # ---- 7 调用记录入库核验 ----
    data = expect_env(call("GET", f"/api/skills/{SKILL}/invocations", name=AGENT_A, key=key_a),
                      "查询调用记录")
    rec = next((r for r in data if r.get("caller_agent") == AGENT_B and r.get("status") == "success"), None)
    step(7, "技能调用记录已入库（含调用者/状态/耗时）",
         rec is not None and rec.get("duration_ms") == 5,
         f"共 {len(data)} 条记录，最新 caller={rec.get('caller_agent') if rec else '未找到'}")

    # ---- 8 B 写入错误日志 ----
    title = "E2E-B 的模拟错误"
    data = expect_env(call("POST", "/api/errors",
                           {"severity": "warning", "source": "e2e", "title": title,
                            "detail": f"由 {AGENT_B} 写入的端到端测试错误"},
                           name=AGENT_B, key=key_b),
                      "B 写入错误日志")
    err_uuid = data.get("uuid", "")
    step(8, "B 写入错误日志（severity=warning）", bool(err_uuid), f"uuid={err_uuid}")

    # ---- 9 A 查询错误日志（必须看到 B 的内容）----
    data = expect_env(call("GET", "/api/errors?status=open", name=AGENT_A, key=key_a), "A 查询错误日志")
    hit = next((e for e in data if e.get("uuid") == err_uuid), None)
    step(9, "A 查询错误日志必须看到 B 的记录",
         hit is not None and hit.get("title") == title and hit.get("severity") == "warning",
         f"open 错误共 {len(data)} 条，命中 reporter={hit.get('reporter') if hit else '未找到'}")

    # ---- 10 协作页事件流（/api/audit，总览页数据源）----
    data = expect_env(call("GET", "/api/audit?limit=200", name=AGENT_A, key=key_a), "协作页事件流")
    ev = next((r for r in data if r.get("action") == "error.report" and r.get("target") == err_uuid), None)
    kinds = sorted({r.get("action", "") for r in data})
    step(10, "协作页 API（/api/audit）返回 B 的错误上报事件",
         ev is not None and ev.get("actor") == AGENT_B,
         f"事件流共 {len(data)} 条，动作类型={kinds}")

    # ---- 11 附加：B 读取 A 写的共享记忆（共享可见性）----
    data = expect_env(call("GET", "/api/memory?section=project", name=AGENT_B, key=key_b), "B 读取共享记忆")
    hit = next((m for m in data if m.get("key") == "e2e-check" and m.get("value") == mem_value), None)
    step(11, "附加：B 能读到 A 写的共享记忆", hit is not None and hit.get("author") == AGENT_A,
         f"project 区共 {len(data)} 条，命中 author={hit.get('author') if hit else '未找到'}")

    # ---- 12 A 添加知识条目（为搜索回归做准备）----
    data = expect_env(call("POST", "/api/knowledge",
                           {"title": "E2E 部署流程知识", "content": "MiderHive 使用 CMake 与 Qt6 构建，"
                            "平台监听 127.0.0.1，Agent 经 HTTP API 协作。", "category": "e2e"},
                           name=AGENT_A, key=key_a),
                      "A 添加知识条目")
    step(12, "A 添加知识条目", bool(data.get("uuid")), f"uuid={data.get('uuid')}")

    # ---- 13 修复回归：合法 mode=semantic 可用，且 match_mode 为归一后的小写 ----
    status, payload = call("POST", "/api/knowledge/search",
                           {"query": "部署流程", "mode": "semantic", "limit": 5},
                           name=AGENT_B, key=key_b)
    ok13 = status == 200 and isinstance(payload, dict) and payload.get("code") == 0
    mm = payload.get("data", [{}])[0].get("match_mode") if ok13 and payload.get("data") else None
    step(13, "修复回归：mode=semantic 搜索正常，match_mode 归一为小写", ok13,
         f"HTTP={status} 命中={len(payload.get('data') or [])} match_mode={mm or '（空结果，无回显字段）'}")

    # ---- 14 修复回归：未知 mode 必须被明确拒绝（不得静默降级为 keyword）----
    status, payload = call("POST", "/api/knowledge/search",
                           {"query": "部署流程", "mode": "vector"},
                           name=AGENT_B, key=key_b)
    msg = payload.get("message") if isinstance(payload, dict) else repr(payload)
    step(14, "修复回归：未知 mode='vector' 被明确拒绝 HTTP 400",
         status == 400,
         f"HTTP={status} message={msg}")

    # ---- 15 修复回归：mode 大小写错误被归一为合法值，且 match_mode 诚实回显 ----
    status, payload = call("POST", "/api/knowledge/search",
                           {"query": "部署流程", "mode": "Semantic"},
                           name=AGENT_B, key=key_b)
    rows = payload.get("data") or [] if isinstance(payload, dict) else []
    mm = rows[0].get("match_mode") if rows else None
    step(15, "修复回归：mode='Semantic' 归一为 semantic（不再静默降级/原样回显）",
         status == 200 and mm == "semantic",
         f"HTTP={status} match_mode={mm}")

    # ---- 16 A 注册无 schema 技能（向后兼容：无约束不拦）----
    data = expect_env(call("POST", "/api/skills",
                           {"name": "e2e-free", "description": "无 param_schema 的自由技能",
                            "category": "e2e"},
                           name=AGENT_A, key=key_a),
                      "A 注册无 schema 技能")
    step(16, "A 注册无 param_schema 的自由技能", data.get("param_schema") in ({}, "{}"),
         f"param_schema={data.get('param_schema')!r}")

    # ---- 17 修复回归：无 schema 技能接受任意参数（宁放行勿误杀）----
    status, payload = call("POST", "/api/skills/e2e-free/invoke",
                           {"params": {"whatever": [1, 2, 3]}, "result_summary": "自由参数",
                            "status": "success"},
                           name=AGENT_B, key=key_b)
    step(17, "修复回归：无 schema 技能接受任意参数", status == 200, f"HTTP={status}")

    # ---- 18 修复回归：缺必填参数被拒绝 ----
    status, payload = call("POST", f"/api/skills/{SKILL}/invoke",
                           {"params": {}, "result_summary": "缺参调用", "status": "success"},
                           name=AGENT_B, key=key_b)
    msg = payload.get("message") if isinstance(payload, dict) else repr(payload)
    step(18, "修复回归：缺必填参数 text 被拒绝 HTTP 400",
         status == 400 and "missing required param" in str(msg),
         f"HTTP={status} message={msg}")

    # ---- 19 修复回归：参数类型错误被拒绝 ----
    status, payload = call("POST", f"/api/skills/{SKILL}/invoke",
                           {"params": {"text": 123}, "result_summary": "类型错调用",
                            "status": "success"},
                           name=AGENT_B, key=key_b)
    msg = payload.get("message") if isinstance(payload, dict) else repr(payload)
    step(19, "修复回归：text 传数字被拒绝（应为 string）HTTP 400",
         status == 400 and "must be string" in str(msg),
         f"HTTP={status} message={msg}")

    print("=" * 72)
    print(f"全部通过：{_passed} 步。A-B 协作链路（注册→记忆→技能→调用→错误互通→事件流）端到端可用。")


if __name__ == "__main__":
    main()
