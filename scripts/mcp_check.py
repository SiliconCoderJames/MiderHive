#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiderHive MCP 服务器集成验证脚本

以一个真实 MCP 客户端的方式驱动 miderhive-mcp.exe：spawn 子进程，在 stdin/stdout
上按行交换 JSON-RPC 2.0，逐条断言 MCP 会话（initialize/tools/list/tools/call）
与平台语义（乐观并发、广播、错误上报等）走通。

用法: python scripts/mcp_check.py [port] [miderhive-mcp.exe 路径] [agent名] [agent密钥]
依赖: 仅 Python 3.8+ 标准库。平台（platformd/工作台）须已在 127.0.0.1:port 运行。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

# 与 feasibility_check.py 同理：Windows/CI 控制台默认 ANSI 代码页，中文会崩
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

PORT = sys.argv[1] if len(sys.argv) > 1 else "19090"
EXE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "build", "src", "cli", "Release", "miderhive-mcp.exe")
AGENT = sys.argv[3] if len(sys.argv) > 3 else "zcode"
KEY = sys.argv[4] if len(sys.argv) > 4 else ""

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail and not ok else ""))


class McpClient:
    """最小 MCP stdio 客户端：每行一条 JSON-RPC。"""

    def __init__(self, exe, port, agent, key):
        env = dict(os.environ)
        env["MIDERHIVE_PORT"] = port
        env["MIDERHIVE_AGENT_NAME"] = agent
        env["MIDERHIVE_AGENT_KEY"] = key
        # 身份已显式给出,但仍把 HOME 指到临时目录,确保绝不触碰真实用户数据
        self.home = tempfile.mkdtemp(prefix="miderhive-mcp-check-")
        env["MIDERHIVE_HOME"] = self.home
        self.proc = subprocess.Popen(
            [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, env=env, text=True, encoding="utf-8")
        self.next_id = 0

    def request(self, method, params=None):
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout (crashed?)")
        return rid, json.loads(line)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def call_tool(self, name, args=None):
        rid, resp = self.request("tools/call", {"name": name, "arguments": args or {}})
        result = resp.get("result", {})
        is_error = result.get("isError", False)
        text = "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        data = None
        try:
            data = json.loads(text)
        except Exception:
            pass
        return rid, resp, is_error, text, data

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        # 隔离数据目录用完即删:否则每次运行都会在 %TEMP% 里留一份(CI 上日积月累)
        shutil.rmtree(self.home, ignore_errors=True)


def main():
    if not os.path.exists(EXE):
        print("miderhive-mcp.exe 不存在: %s" % EXE)
        return 2
    if not KEY:
        print("缺少 agent 密钥参数（用法: mcp_check.py [port] [exe] [agent] [key]）")
        return 2

    cli = McpClient(EXE, PORT, AGENT, KEY)
    try:
        # ---- 会话建立 ----
        rid, resp = cli.request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp_check", "version": "0"}})
        result = resp.get("result", {})
        check("initialize 返回协议版本", result.get("protocolVersion") == "2025-06-18",
              str(result.get("protocolVersion")))
        check("initialize 声明 miderhive", result.get("serverInfo", {}).get("name") == "miderhive")
        check("initialize 声明 tools 能力", "tools" in result.get("capabilities", {}))

        # 通知不得产生响应行:紧随的 ping 应是下一行输出
        cli.notify("notifications/initialized")
        rid, resp = cli.request("ping", {})
        check("通知无回包且 ping 应答", resp.get("id") == rid and resp.get("result") == {})

        # ---- 工具清单 ----
        rid, resp = cli.request("tools/list", {})
        tools = resp.get("result", {}).get("tools", [])
        names = {t.get("name") for t in tools}
        check("工具数量 >= 27", len(tools) >= 27, "实际 %d" % len(tools))
        check("工具描述与 schema 齐全",
              all(t.get("name") and t.get("description") and "properties" in t.get("inputSchema", {})
                  for t in tools))
        expected_tools = (
            # 自检/在场
            "hive_status", "agents_list", "heartbeat",
            # 记忆
            "memory_list", "memory_write", "memory_history", "memory_remove",
            # 知识（含版本迭代）
            "knowledge_add", "knowledge_search", "knowledge_list", "knowledge_get",
            "knowledge_versions", "knowledge_add_version",
            # 消息（含回复）
            "message_send", "message_list", "message_set_status", "message_reply",
            # 错误
            "error_report", "error_list", "error_resolve",
            # 技能（含注册）
            "skill_list", "skill_register", "skill_invoke",
            # 用量（含上报与多维）
            "usage_summary", "usage_report", "usage_daily", "usage_breakdown",
        )
        missing = [n for n in expected_tools if n not in names]
        check("27 个工具全部在列", not missing, "缺失 %s" % missing)
        check("无多余/未预期工具", names == set(expected_tools),
              "多出 %s" % sorted(names - set(expected_tools)))

        # ---- hive_status：一次调用回答"我连上了吗" ----
        _, _, is_err, _, data = cli.call_tool("hive_status")
        check("hive_status 成功", not is_err and isinstance(data, dict))
        check("hive_status 报告已连接与身份",
              isinstance(data, dict) and data.get("connected") is True and data.get("you") == AGENT,
              str(data)[:160] if isinstance(data, dict) else "")
        check("hive_status 带上平台版本与在线数",
              isinstance(data, dict) and data.get("platform", {}).get("version")
              and isinstance(data.get("online_count"), int))

        # ---- agents / 心跳 ----
        _, _, is_err, _, data = cli.call_tool("agents_list")
        check("agents_list 成功", not is_err and isinstance(data, list) and
              any(a.get("name") == AGENT for a in data))
        _, _, is_err, _, _ = cli.call_tool("heartbeat", {"current_task": "mcp 集成验证"})
        check("heartbeat 成功", not is_err)

        # ---- memory：写入/追加/乐观并发/历史 ----
        _, _, is_err, _, data = cli.call_tool(
            "memory_write", {"section": "project", "key": "mcp-smoke", "value": "v1"})
        check("memory_write 首写 v1", not is_err and data.get("version") == 1, str(data)[:120])
        _, _, is_err, _, data = cli.call_tool(
            "memory_write", {"section": "project", "key": "mcp-smoke", "value": "v2"})
        check("memory_write 追加 v2", not is_err and data.get("version") == 2, str(data)[:120])
        _, resp, is_err, text, _ = cli.call_tool(
            "memory_write", {"section": "project", "key": "mcp-smoke", "value": "v3",
                             "base_version": 1})
        check("base_version 过期被拒(工具级错误)", is_err and "version conflict" in text, text[:120])
        _, _, is_err, _, data = cli.call_tool(
            "memory_history", {"section": "project", "key": "mcp-smoke"})
        check("memory_history 两个版本", not is_err and isinstance(data, list) and len(data) == 2,
              str(len(data) if isinstance(data, list) else data)[:80])

        # ---- knowledge：新增 + 关键词/语义双检索命中 ----
        # 两版用互不包含的标记词，才能验证 is_latest 收敛（旧版不再被检索命中）
        content_v1 = "MCP 冒烟验证条目 mcpold111：MiderHive vector pipeline smoke entry。"
        content_v2 = "MCP 冒烟验证条目第二版 mcpnew222：MiderHive vector pipeline smoke entry。"
        _, _, is_err, ktext, data = cli.call_tool(
            "knowledge_add", {"title": "MCP 冒烟条目", "content": content_v1, "tags": ["mcp", "smoke"],
                              "category": "验证"})
        check("knowledge_add 成功", not is_err and data.get("uuid"), ktext[:120])
        kn_uuid = data.get("uuid") if isinstance(data, dict) else None
        _, _, is_err, _, hits = cli.call_tool(
            "knowledge_search", {"query": "mcpold111", "mode": "keyword"})
        check("knowledge_search keyword 命中", not is_err and
              any(h.get("uuid") == kn_uuid and "mcpold111" in (h.get("content") or "")
                  for h in hits or []))
        _, _, is_err, _, hits = cli.call_tool(
            "knowledge_search", {"query": "vector pipeline smoke", "mode": "semantic"})
        check("knowledge_search semantic 命中", not is_err and bool(hits))

        # ---- knowledge：按 uuid 取回 / 版本迭代（追加不覆盖）----
        _, _, is_err, _, data = cli.call_tool("knowledge_get", {"uuid": kn_uuid})
        check("knowledge_get 取回最新版本",
              not is_err and data.get("uuid") == kn_uuid and data.get("version") == 1,
              str(data)[:120])
        _, _, is_err, vtext, data = cli.call_tool(
            "knowledge_add_version", {"uuid": kn_uuid, "content": content_v2})
        check("knowledge_add_version 追加为 v2",
              not is_err and data.get("version") == 2, vtext[:120])
        _, _, is_err, _, versions = cli.call_tool("knowledge_versions", {"uuid": kn_uuid})
        check("knowledge_versions 有两个版本（旧版保留）",
              not is_err and isinstance(versions, list) and len(versions) == 2,
              str(len(versions) if isinstance(versions, list) else versions)[:80])
        check("历史版本仍保留 v1 原文",
              isinstance(versions, list) and
              any(v.get("version") == 1 and "mcpold111" in (v.get("content") or "")
                  for v in versions))
        _, _, is_err, _, hits = cli.call_tool(
            "knowledge_search", {"query": "mcpnew222", "mode": "keyword"})
        check("新版内容可检索", not is_err and
              any(h.get("uuid") == kn_uuid for h in hits or []))
        _, _, is_err, _, hits = cli.call_tool(
            "knowledge_search", {"query": "mcpold111", "mode": "keyword"})
        check("被迭代的旧版本不再是最新（is_latest 收敛）",
              not is_err and not any(h.get("uuid") == kn_uuid for h in hits or []))

        # ---- messages：广播默认、列表、状态 ----
        _, _, is_err, _, data = cli.call_tool(
            "message_send", {"kind": "note", "subject": "MCP 冒烟广播",
                             "body": "来自 miderhive-mcp 的广播"})
        check("message_send 广播成功(缺省 recipient)", not is_err and data.get("uuid"), text[:120])
        _, _, is_err, _, msgs = cli.call_tool(
            "message_list", {"kind": "note", "limit": 50})
        check("message_list 可见广播", not is_err and
              any(m.get("subject") == "MCP 冒烟广播" for m in msgs or []))
        if isinstance(msgs, list) and msgs:
            target = msgs[0]["uuid"]
            _, _, is_err, _, data = cli.call_tool(
                "message_set_status", {"uuid": target, "status": "read"})
            check("message_set_status read", not is_err and data.get("status") == "read")
            _, _, is_err, rtext, data = cli.call_tool(
                "message_reply", {"uuid": target, "body": "MCP 冒烟回复"})
            check("message_reply 回复成功", not is_err and data.get("uuid"), rtext[:120])

        # ---- errors：上报/列表/闭环（严重度词表:info|warning|error|critical）----
        _, _, is_err, _, data = cli.call_tool(
            "error_report", {"title": "MCP 冒烟错误", "detail": "集成验证自动上报",
                             "severity": "warning", "source": "mcp_check"})
        check("error_report 成功", not is_err and data.get("uuid"))
        smoke_uuid = data.get("uuid") if isinstance(data, dict) else None
        _, resp, is_err, text, _ = cli.call_tool(
            "error_report", {"title": "坏词表", "detail": "d", "severity": "warn"})
        check("拼错严重度被显式拒绝", is_err and "severity must be" in text, text[:120])
        _, _, is_err, _, errs = cli.call_tool("error_list", {"status": "open", "severity": "warning"})
        check("error_list 可见新上报", not is_err and
              any(e.get("uuid") == smoke_uuid for e in errs or []))
        _, _, is_err, _, data = cli.call_tool(
            "error_resolve", {"uuid": smoke_uuid, "notes": "已验证,闭环"})
        check("error_resolve 闭环", not is_err and data.get("status") == "resolved")

        # ---- skills：注册 → 参数校验 → 调用留痕 ----
        _, _, is_err, stext, _ = cli.call_tool(
            "skill_register", {"name": "mcp-smoke-skill", "description": "MCP 验证用技能",
                               "display_name": "MCP 冒烟技能", "category": "验证",
                               "param_schema": {"type": "object",
                                                "properties": {"target": {"type": "string"}},
                                                "required": ["target"]}})
        check("skill_register 注册成功", not is_err, stext[:160])
        _, _, is_err, _, data = cli.call_tool(
            "skill_invoke", {"name": "mcp-smoke-skill", "params": {"target": "x"},
                             "result_summary": "ok", "status": "success",
                             "duration_ms": 5, "tokens_in": 10, "tokens_out": 20})
        check("skill_invoke 调用留痕成功", not is_err and data.get("skill") == "mcp-smoke-skill",
              str(data)[:160])
        _, _, is_err, itext, _ = cli.call_tool(
            "skill_invoke", {"name": "mcp-smoke-skill", "params": {}, "status": "success"})
        check("缺必填参数被 param_schema 拦下", is_err, itext[:160])
        _, _, is_err, _, _ = cli.call_tool(
            "skill_invoke", {"name": "ghost-skill-not-registered", "status": "success"})
        check("未注册技能调用被拒(工具级错误)", is_err)

        # ---- usage：汇总 / 上报（幂等）/ 逐日 / 多维切片 ----
        _, _, is_err, _, data = cli.call_tool("usage_summary", {})
        check("usage_summary 含预算", not is_err and "budget" in (data or {}))
        _, _, is_err, utext, data = cli.call_tool(
            "usage_report", {"tokens_in": 111, "tokens_out": 222, "model": "mcp-smoke-model",
                             "call_type": "smoke", "idempotency_key": "mcp-smoke-key-1"})
        check("usage_report 成功并回传余额",
              not is_err and "remaining" in (data or {}) and data.get("duplicate") is False,
              utext[:160])
        _, _, is_err, _, data = cli.call_tool(
            "usage_report", {"tokens_in": 111, "tokens_out": 222, "model": "mcp-smoke-model",
                             "idempotency_key": "mcp-smoke-key-1"})
        check("同 idempotency_key 重复上报不重复计数",
              not is_err and data.get("duplicate") is True, str(data)[:160])
        _, _, is_err, _, data = cli.call_tool("usage_daily", {"days": 7})
        check("usage_daily 返回 7 天", not is_err and len((data or {}).get("days", [])) == 7,
              str(data)[:120])
        _, _, is_err, _, data = cli.call_tool(
            "usage_breakdown", {"days": 7, "model": "mcp-smoke-model"})
        check("usage_breakdown 按模型切片可见",
              not is_err and any(m.get("model") == "mcp-smoke-model"
                                 for m in (data or {}).get("per_model", [])), str(data)[:160])

        # ---- 协议错误面 ----
        rid, resp = cli.request("tools/call", {"name": "no_such_tool", "arguments": {}})
        check("未知工具 -> -32602 且 id 回传",
              resp.get("error", {}).get("code") == -32602 and resp.get("id") == rid)
        rid, resp = cli.request("bogus/method", {})
        check("未知方法 -> -32601", resp.get("error", {}).get("code") == -32601)

        # ---- 畸形输入不得杀死服务器（此前 name 非字符串会 std::terminate）----
        rid, resp = cli.request("tools/call", {"name": 123, "arguments": {}})
        check("name 非字符串 -> -32602 不崩溃",
              resp.get("error", {}).get("code") == -32602 and resp.get("id") == rid)
        rid, resp = cli.request("tools/call", "params-not-object")
        check("params 非对象 -> -32602", resp.get("error", {}).get("code") == -32602)
        rid, resp = cli.request("ping", {})
        check("畸形输入后服务器存活", resp.get("id") == rid and resp.get("result") == {})
        # 通知后再来一个请求：通知处理不得吞掉后续回包
        cli.notify("notifications/other")
        rid, resp = cli.request("ping", {})
        check("任意通知后请求仍应答", resp.get("id") == rid and resp.get("result") == {})

        # ---- memory_remove（zcode 权限）----
        _, _, is_err, _, _ = cli.call_tool(
            "memory_remove", {"section": "project", "key": "mcp-smoke"})
        check("memory_remove 清理成功", not is_err)
        _, _, is_err, _, data = cli.call_tool(
            "memory_history", {"section": "project", "key": "mcp-smoke"})
        check("remove 后历史为空", not is_err and data == [])
    finally:
        cli.close()

    failed = [r for r in results if not r[1]]
    print("MCP 验证: %d 项, 失败 %d 项" % (len(results), len(failed)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
