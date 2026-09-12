#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiderHive 本地多 Agent 协作平台 · 可行性验证脚本

以三个外部 Agent（claude/codex/hermes）的真实协作流程走通全部核心场景，
仅通过 HTTP API 交互（模拟真实接入方式），逐条断言协作规则落地。

用法: python scripts/feasibility_check.py [port]
依赖: 仅 Python 3.8+ 标准库
"""
import json
import os
import sys
import time
import urllib.request
import urllib.error

# 输出含中文（场景名/断言说明）。Windows 控制台与 CI 的 stdout 默认是 ANSI 代码页
# （如 cp1252/GBK），print 中文会直接 UnicodeEncodeError 崩掉——检查还没跑就挂了。
# 强制 stdout/stderr 用 UTF-8，兼容 Python 3.7+；再老的解释器忽略之。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

BASE = "http://127.0.0.1:%s" % (sys.argv[1] if len(sys.argv) > 1 else "19090")
# 绕过系统代理，确保直连本机
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

MASTER = None          # 从 /api 初始化阶段读取（由调用方通过环境传入）
results = []           # (scenario, ok, detail)


# ---------------- HTTP 基础 ----------------

def http(method, path, agent=None, key=None, body=None, master=None, timeout=10):
    headers = {"Content-Type": "application/json"}
    if agent:
        headers["X-Agent-Name"] = agent
    if key:
        headers["X-Api-Key"] = key
    if master:
        headers["X-Master-Key"] = master
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    try:
        with OPENER.open(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode("utf-8"))
        except Exception:
            return e.code, {}


def data_of(resp):
    return resp[1].get("data")


def ok(resp):
    return resp[1].get("code") == 0


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print("[%s] %s%s" % ("PASS" if cond else "FAIL", name, (" -- " + detail) if (detail and not cond) else ""))


# ---------------- 场景 ----------------

def s01_health():
    st, body = http("GET", "/api/health")
    check("S01 平台健康检查", st == 200 and body.get("code") == 0,
          "status=%s" % st)


def s02_register(agents):
    # zcode 由平台 bootstrap 预置（密钥经环境变量注入），此处只注册普通成员
    for name in ("claude", "codex", "hermes"):
        st, body = http("POST", "/api/agents/register",
                        body={"name": name, "role": "member"}, master=MASTER)
        check("S02 注册 %s" % name, st == 200 and body.get("code") == 0 and
              len(body["data"].get("api_key", "")) == 64,
              "status=%s code=%s" % (st, body.get("code")))
        agents[name]["key"] = body["data"]["api_key"] if body.get("code") == 0 else ""
    # 重复注册必须被拒
    st, body = http("POST", "/api/agents/register",
                    body={"name": "claude"}, master=MASTER)
    check("S02b 重复注册被拒", body.get("code") == 400, "code=%s" % body.get("code"))


def s03_boot_protocol(agents):
    for name, a in agents.items():
        st, body = http("GET", "/api/agents", agent=name, key=a["key"])
        peers = [x["name"] for x in (body.get("data") or [])]
        st2, body2 = http("GET", "/api/memory", agent=name, key=a["key"])
        st3, _ = http("POST", "/api/agents/heartbeat", agent=name, key=a["key"],
                      body={"current_task": "可行性验证"})
        check("S03 %s 启动协议(协作者/记忆/心跳)" % name,
              st == 200 and "zcode" in peers and st2 == 200 and st3 == 200,
              "peers=%s" % peers)
    # 心跳后在状态表中应显示 online + 任务
    st, body = http("GET", "/api/agents", agent="claude", key=agents["claude"]["key"])
    hermes = [x for x in body["data"] if x["name"] == "hermes"][0]
    check("S03b 在线状态与任务可见", hermes["status"] == "online" and hermes["current_task"] == "可行性验证")


def s04_auth_rejected(agents):
    st, body = http("GET", "/api/agents", agent="claude", key="wrong-key")
    check("S04 错误密钥被拒(401)", st == 401 and body.get("code") == 401, "status=%s" % st)
    st, body = http("GET", "/api/agents")
    check("S04b 缺失认证头被拒", st == 401)


def s05_memory_shared(agents):
    # hermes 写入用户偏好，claude 读取 —— 跨 Agent 上下文连贯
    st, _ = http("POST", "/api/memory", agent="hermes", key=agents["hermes"]["key"],
                 body={"section": "preference", "key": "naming",
                       "value": "Python snake_case / C++ camelCase"})
    st, body = http("GET", "/api/memory?section=preference", agent="codex", key=agents["codex"]["key"])
    vals = {m["key"]: m["value"] for m in (body.get("data") or [])}
    check("S05 记忆跨 Agent 共享", st == 200 and vals.get("naming", "").startswith("Python"),
          "vals=%s" % vals)
    # 编辑生成新版本，历史保留
    http("POST", "/api/memory", agent="codex", key=agents["codex"]["key"],
         body={"section": "preference", "key": "naming", "value": "updated"})
    st, body = http("GET", "/api/memory/history?section=preference&key=naming",
                    agent="hermes", key=agents["hermes"]["key"])
    hist = body.get("data") or []
    check("S05b 记忆版本链(v2 最新+v1 可溯)", len(hist) == 2 and hist[0]["version"] == 2,
          "versions=%s" % [h["version"] for h in hist])


def s06_knowledge_semantic(agents):
    st, body = http("POST", "/api/knowledge", agent="hermes", key=agents["hermes"]["key"],
                    body={"title": "MSVC /utf-8 教训",
                          "content": "MSVC 默认按GBK解析UTF-8注释会吞掉换行导致include失效，必须加 /utf-8 编译选项",
                          "tags": ["msvc", "cmake"], "category": "踩坑"})
    check("S06 知识沉淀(中文)", ok((st, body)), "code=%s" % body.get("code"))
    t0 = time.time()
    st, body = http("POST", "/api/knowledge/search", agent="codex", key=agents["codex"]["key"],
                    body={"query": "中文注释导致编译错误怎么解决", "mode": "semantic", "limit": 5})
    dt = (time.time() - t0) * 1000
    hits = body.get("data") or []
    check("S06b 语义检索命中 top1", ok((st, body)) and hits and hits[0]["title"] == "MSVC /utf-8 教训",
          "hits=%s" % [h["title"] for h in hits])
    print("       semantic search latency: %.0f ms" % dt)
    return hits[0]["uuid"] if hits else None


def s07_append_only(agents, uuid):
    # codex 对 hermes 的条目追加新版本；v1 内容必须原样保留
    st, body = http("POST", "/api/knowledge/%s/versions" % uuid,
                    agent="codex", key=agents["codex"]["key"],
                    body={"content": "补充：MinGW 下无需该选项，但建议统一 /utf-8 减少 mojibake"})
    check("S07 追加版本生成 v2", ok((st, body)) and body["data"]["version"] == 2,
          "code=%s" % body.get("code"))
    st, body = http("GET", "/api/knowledge/%s/versions" % uuid, agent="claude", key=agents["claude"]["key"])
    versions = sorted(body.get("data") or [], key=lambda v: v["version"])
    v1_kept = len(versions) == 2 and versions[0]["version"] == 1 and "必须加 /utf-8" in versions[0]["content"]
    check("S07b v1 原样保留(禁止覆盖)", v1_kept, "versions=%s" % [v["version"] for v in versions])


def s08_skills(agents):
    # 规则：未注册技能调用必须被拒
    st, body = http("POST", "/api/skills/ghost/invoke", agent="claude", key=agents["claude"]["key"],
                    body={"params": {}})
    check("S08 未注册技能调用被拒", body.get("code") == 400, "code=%s" % body.get("code"))
    st, body = http("POST", "/api/skills", agent="hermes", key=agents["hermes"]["key"],
                    body={"name": "code-review", "display_name": "代码审查",
                          "description": "审查代码变更并给出可执行修改意见",
                          "category": "dev",
                          "param_schema": {"type": "object", "properties": {"file": {"type": "string"}}}})
    check("S08b 技能注册", ok((st, body)), "code=%s" % body.get("code"))
    # codex 发现并调用 hermes 注册的技能（Token 记账）
    st, body = http("POST", "/api/skills/code-review/invoke", agent="codex", key=agents["codex"]["key"],
                    body={"params": {"file": "src/core/platform.cpp"}, "status": "success",
                          "result_summary": "发现2处空指针风险", "duration_ms": 1200,
                          "tokens_in": 3000, "tokens_out": 2000})
    check("S08c 跨 Agent 技能调用+Token记账",
          ok((st, body)) and body["data"]["remaining_tokens"] < body["data"]["budget"],
          "code=%s" % body.get("code"))
    st, body = http("GET", "/api/skills/code-review/invocations", agent="claude", key=agents["claude"]["key"])
    check("S08d 调用记录可查", len(body.get("data") or []) >= 1)


def s09_async_task(agents):
    # claude 指派任务给 codex（异步：codex 此刻"不在线"也不影响投递）
    st, body = http("POST", "/api/messages", agent="claude", key=agents["claude"]["key"],
                    body={"kind": "task", "recipient": "codex",
                          "subject": "排查语义检索空结果", "body": "知识搜索偶发返回空，请排查 vec 表"})
    uuid = body["data"]["uuid"]
    check("S09 任务指派(pending)", ok((st, body)) and body["data"]["status"] == "pending")
    # 非法流转被拒：claude(发件人)不能替收件人 accepted
    st, body = http("POST", "/api/messages/%s/status" % uuid, agent="claude",
                    key=agents["claude"]["key"], body={"status": "accepted"})
    check("S09b 发件人越权流转被拒", body.get("code") != 0, "code=%s" % body.get("code"))
    # codex 上线，收件箱可见任务并流转 pending→accepted→done
    st, body = http("GET", "/api/messages?kind=task&status=pending", agent="codex", key=agents["codex"]["key"])
    inbox = body.get("data") or []
    check("S09c 收件箱异步可见", any(m["uuid"] == uuid for m in inbox))
    http("POST", "/api/messages/%s/status" % uuid, agent="codex", key=agents["codex"]["key"],
         body={"status": "accepted"})
    st, body = http("POST", "/api/messages/%s/status" % uuid, agent="codex",
                    key=agents["codex"]["key"], body={"status": "declined"})
    check("S09d 非法流转 accepted→declined 被拒", body.get("code") == 400, "code=%s" % body.get("code"))
    st, body = http("POST", "/api/messages/%s/status" % uuid, agent="codex",
                    key=agents["codex"]["key"], body={"status": "done"})
    check("S09e accepted→done 完成", ok((st, body)) and body["data"]["status"] == "done")
    return uuid


def s10_error_loop(agents):
    # codex 报错（规则：报错必须记录，不得静默）
    st, body = http("POST", "/api/errors", agent="codex", key=agents["codex"]["key"],
                    body={"severity": "error", "source": "knowledge/search",
                          "title": "vec 表查询返回空", "detail": "KNN 查询偶发空结果",
                          "stack_trace": "searchSemantic() at knowledge_service.cpp"})
    uuid = body["data"]["uuid"]
    check("S10 错误上报", ok((st, body)))
    # 非上报者不能解决
    st, body = http("POST", "/api/errors/%s/resolve" % uuid, agent="claude",
                    key=agents["claude"]["key"], body={"notes": "越权解决"})
    check("S10b 非上报者解决被拒", body.get("code") != 0, "code=%s" % body.get("code"))
    # 上报者登记解决；zcode 追加复核说明（追加不覆盖）
    http("POST", "/api/errors/%s/resolve" % uuid, agent="codex", key=agents["codex"]["key"],
         body={"notes": "已修复：k 与 LIMIT 不能同时绑定"})
    st, body = http("POST", "/api/errors/%s/resolve" % uuid, agent="zcode",
                    key=agents["zcode"]["key"], body={"notes": "zcode 复核通过"})
    check("S10c 管理者可追加复核", ok((st, body)), "status=%s body=%s" % (st, str(body)[:80]))
    st, body = http("GET", "/api/errors", agent="claude", key=agents["claude"]["key"])
    rec = [e for e in (body.get("data") or []) if e["uuid"] == uuid][0]
    check("S10d 解决闭环+追加不覆盖",
          rec["status"] == "resolved" and "已修复" in rec["resolution_notes"]
          and "复核通过" in rec["resolution_notes"],
          "notes=%s" % rec["resolution_notes"][:50])


def s11_token_budget(agents):
    http("PUT", "/api/usage/budget", master=MASTER, body={"budget": 100000})
    seq = [(0, "none"), (76000, "warn"), (16000, "critical"), (11000, "over")]
    total = 5000  # S08 已消耗 5000
    for add, expect in seq[1:]:
        total += add
        st, body = http("POST", "/api/usage/report", agent="claude", key=agents["claude"]["key"],
                        body={"tokens_in": add, "tokens_out": 0, "call_type": "llm"})
        level = body["data"]["alert_level"]
        check("S11 预算 %.0f%% → %s" % (total / 1000.0, expect), level == expect,
              "got=%s used=%s" % (level, body["data"]["used"]))
    st, body = http("GET", "/api/usage/summary", agent="hermes", key=agents["hermes"]["key"])
    per = {p["agent"]: p["tokens"] for p in (body.get("data") or {}).get("per_agent", [])}
    check("S11b 分 Agent 用量统计", per.get("claude", 0) > per.get("codex", 0), "per=%s" % per)
    # 多维切片（「用量分析」面板与筛选共用此接口）：合计 / 按 Agent / 按模型 / 逐日
    st, body = http("GET", "/api/usage/breakdown?days=7", agent="hermes", key=agents["hermes"]["key"])
    d = body.get("data") or {}
    check("S11c 切片默认 7 天且逐日补齐", st == 200 and len(d.get("daily") or []) == 7,
          "st=%s daily=%s" % (st, len(d.get("daily") or [])))
    check("S11d 切片合计等于按 Agent 汇总",
          d.get("total_tokens") == sum(r.get("tokens", 0) for r in (d.get("per_agent") or [])),
          "total=%s per_agent=%s" % (d.get("total_tokens"), d.get("per_agent")))
    st, body = http("GET", "/api/usage/breakdown?days=7&agent=claude", agent="hermes",
                    key=agents["hermes"]["key"])
    d2 = body.get("data") or {}
    names = {r.get("agent") for r in (d2.get("per_agent") or [])}
    check("S11e agent 筛选生效", st == 200 and names == {"claude"}, "names=%s" % names)
    check("S11f 筛选后合计与未筛选的该行一致",
          d2.get("total_tokens") == per.get("claude", 0),
          "filtered=%s row=%s" % (d2.get("total_tokens"), per.get("claude")))
    st, body = http("GET", "/api/usage/breakdown?days=0", agent="hermes", key=agents["hermes"]["key"])
    check("S11g 非法 days 被拒", st == 400, "st=%s" % st)
    # 预算改回默认，避免影响其它验证
    http("PUT", "/api/usage/budget", master=MASTER, body={"budget": 10000000})


def s12_audit(agents):
    st, body = http("GET", "/api/audit?limit=500", agent="claude", key=agents["claude"]["key"])
    records = body.get("data") or []
    actors = {r["actor"] for r in records}
    actions = {r["action"] for r in records}
    need = {"agent.register", "knowledge.create", "skill.register", "memory.set",
            "message.send", "error.report", "usage.report"}
    check("S12 审计全量留痕(身份+动作)", need.issubset(actions) and
          {"hermes", "codex", "claude"}.issubset(actors),
          "actors=%s missing=%s" % (actors, need - actions))
    # 按 actor 过滤
    st, body = http("GET", "/api/audit?actor=codex&limit=500", agent="codex", key=agents["codex"]["key"])
    check("S12b 按身份过滤", all(r["actor"] == "codex" for r in (body.get("data") or [])))


def s13_broadcast(agents):
    st, body = http("POST", "/api/messages", agent="hermes", key=agents["hermes"]["key"],
                    body={"kind": "note", "subject": "广播", "body": "全员注意：今晚升级构建"})
    check("S13 广播消息", ok((st, body)) and body["data"]["recipient"] is None)
    st, body = http("GET", "/api/messages?kind=note", agent="claude", key=agents["claude"]["key"])
    seen = any(m["subject"] == "广播" for m in (body.get("data") or []))
    check("S13b 无接收者=全员可见", seen)


def main():
    global MASTER
    MASTER = (os.environ.get("MIDERHIVE_MASTER_KEY")
              or os.environ.get("AGENTHIVE_MASTER_KEY")
              or os.environ.get("ZCODE_PLATFORM_MASTER_KEY", ""))
    if not MASTER:
        print("需要环境变量 MIDERHIVE_MASTER_KEY（兼容 AGENTHIVE_* / ZCODE_* 旧名）")
        return 2

    agents = {n: {} for n in ("claude", "codex", "hermes")}
    # zcode 管理者密钥：由运行方从数据目录 config/agents.json 提供
    agents["zcode"] = {"key": (os.environ.get("MIDERHIVE_ZCODE_KEY")
                               or os.environ.get("AGENTHIVE_ZCODE_KEY")
                               or os.environ.get("ZCODE_ZCODE_KEY", ""))}

    s01_health()
    s02_register(agents)
    s03_boot_protocol(agents)
    s04_auth_rejected(agents)
    s05_memory_shared(agents)
    uuid = s06_knowledge_semantic(agents)
    s07_append_only(agents, uuid)
    s08_skills(agents)
    s09_async_task(agents)
    s10_error_loop(agents)
    s11_token_budget(agents)
    s12_audit(agents)
    s13_broadcast(agents)

    fails = [r for r in results if not r[1]]
    print("\n===== 可行性验证: %d/%d 通过 =====" % (len(results) - len(fails), len(results)))
    for name, _, detail in fails:
        print("  FAIL: %s -- %s" % (name, detail))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
