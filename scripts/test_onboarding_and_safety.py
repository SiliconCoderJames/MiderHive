#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MiderHive 首次接入引导 + 防呆设计 自动化测试（独立运行，仅用 Python 标准库）。

自动拉起一个隔离的 platformd（独立数据目录 + 独立端口），验证四条主线：

  A. 引导的后端链路
     A1  GET  /api/diagnostics           —— 健康基线（http/db/目录可写/密钥缓存完整）
     A2  POST /api/agents/provision      —— 给"claude-code"预配接入身份（与界面同一能力）
     A3  用预配身份心跳                    —— 证明生成的配置(名字+密钥)语义正确
     A4  GET  /api/agents                —— 该身份状态变为 online（界面据此弹「接入成功」）
     A5  POST /api/memory                —— 写入欢迎记忆（与界面观察器同一调用）
     A6  GET  /api/memory?section=project —— 欢迎记忆可见

  B. 防呆：密钥丢失 → 诊断可见 → 轮换修复
     B1  停服 → 从 config/agents.json 删除该身份条目（模拟密钥缓存丢失）→ 重启
     B2  GET  /api/diagnostics           —— keyfile_missing 必须列出该身份
     B3  旧密钥心跳仍 200                    —— 认证走库内哈希，明文缓存只影响查看/补配
     B4  POST /api/agents/rotate         —— 轮换修复，拿到新密钥
     B5  旧钥立即失效 + 新钥在线 + 诊断不再报缺失  —— 修复闭环

  C. 防呆：报错可读
     C1  重复注册同名 → 400 "already registered"（界面上由 humanError 翻译成人话）
     C2  无效主密钥 → 403 "invalid master key"

  D. GUI 层（弹窗/横幅/配置 JSON 文本）无法走 HTTP，脚本仅验证其依赖的后端口径；
     手动验证清单见 scripts 输出末尾。

环境变量：
  MH_PLATFORMD   platformd.exe 路径（默认 build/e2e/src/cli/Release/platformd.exe）
  MH_PORT        测试端口（默认 8791，避免与开发实例冲突）

退出码：0=全部通过；1=任一步失败。
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parent.parent
PLATFORMD = Path(os.environ.get("MH_PLATFORMD", ROOT / "build/e2e/src/cli/Release/platformd.exe"))
PORT = int(os.environ.get("MH_PORT", "8791"))
BASE = f"http://127.0.0.1:{PORT}"
MASTER_KEY = "e2e-onboard-master-key"
AGENT = "claude-code"
TIMEOUT = 15

_passed = 0
_proc = None
_home = None


def call(method, path, body=None, name=None, key=None, master=False):
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
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode("utf-8"))
        except Exception:
            return e.code, {}
    except Exception as e:
        return None, {"message": f"{type(e).__name__}: {e}"}


def step(no, title, ok, detail=""):
    global _passed
    mark = "PASS" if ok else "FAIL"
    print(f"[{mark}] 步骤 {no:>2}  {title}" + (f"   —— {detail}" if detail and ok else ""))
    if not ok:
        print(f"\n========== 在步骤 {no} 失败：{title} ==========")
        print(f"detail: {detail}")
        cleanup()
        sys.exit(1)
    _passed += 1


def expect_env(resp, what):
    status, payload = resp
    ok = status is not None and 200 <= status < 300 and payload.get("code") == 0
    if not ok:
        step("?", what, False, f"HTTP={status} 原始响应={payload!r}")
    return payload.get("data")


def start_platformd():
    global _proc
    env = dict(os.environ)
    env["MIDERHIVE_HOME"] = str(_home)
    env["MIDERHIVE_PORT"] = str(PORT)
    env["MIDERHIVE_MASTER_KEY"] = MASTER_KEY
    _proc = subprocess.Popen([str(PLATFORMD)], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            status, payload = call("GET", "/api/health")
            if status == 200 and payload.get("code") == 0:
                return
        except Exception:
            pass
        time.sleep(0.15)
    step("start", "platformd 启动并监听", False, f"platformd={PLATFORMD} port={PORT}")


def stop_platformd():
    global _proc
    if _proc is not None:
        _proc.terminate()
        try:
            _proc.wait(timeout=10)
        except Exception:
            _proc.kill()
        _proc = None
    # 端口让位：轮询直到连不上
    for _ in range(50):
        status, _ = call("GET", "/api/health")
        if status is None:
            return
        time.sleep(0.1)


def cleanup():
    stop_platformd()
    if _home is not None:
        try:
            import shutil
            shutil.rmtree(_home, ignore_errors=True)
        except Exception:
            pass


def diagnostics():
    return expect_env(call("GET", "/api/diagnostics", master=True), "读取 /api/diagnostics")


def main():
    global _home
    if not PLATFORMD.exists():
        print(f"找不到 platformd：{PLATFORMD}\n请先构建：cmake --build build/e2e --config Release --target platformd")
        sys.exit(1)
    _home = Path(tempfile.mkdtemp(prefix="miderhive-onboard-"))
    print(f"隔离数据目录: {_home}")
    print(f"测试端口: {PORT}")
    print("=" * 72)

    start_platformd()

    # ---- A1 健康基线 ----
    d = diagnostics()
    step(1, "诊断基线：HTTP 在跑 / 数据库可用 / 目录可写 / 密钥缓存完整",
         d.get("http_running") is True and d.get("db_ok") is True
         and d.get("home_writable") is True and d.get("agents_json_readable") is True
         and d.get("keyfile_missing") == [],
         f"port={d.get('port')} keyfile_missing={d.get('keyfile_missing')}")

    # ---- A2 预配接入身份（界面"一键接入"走的同一后端能力）----
    data = expect_env(call("POST", "/api/agents/provision", {"name": AGENT}, master=True),
                      "预配接入身份 claude-code")
    key_v1 = data.get("api_key", "")
    step(2, "预配身份成功并返回密钥（不打印明文）", bool(key_v1))

    # ---- A3 预配身份可用于认证（等价于粘贴 MCP 配置后 Agent 连通）----
    data = expect_env(call("POST", "/api/agents/heartbeat", {}, name=AGENT, key=key_v1),
                      "用预配身份心跳")
    step(3, "预配身份认证并心跳成功（信封 ok 即认证通过）", True)

    # ---- A4 身份在线（界面上「接入成功」弹窗的触发条件）----
    agents = expect_env(call("GET", "/api/agents", name=AGENT, key=key_v1), "查询 Agent 列表")
    hit = next((a for a in agents if a.get("name") == AGENT), None)
    step(4, "该身份在列表中且状态为 online", hit is not None and hit.get("status") == "online",
         f"agents={len(agents)}")

    # ---- A5+A6 欢迎记忆（界面观察器写入的同一调用）----
    welcome = "Claude Code 于（测试时间）首次接入 MiderHive / joined MiderHive"
    data = expect_env(call("POST", "/api/memory",
                           {"author": AGENT, "section": "project", "key": f"welcome/{AGENT}",
                            "value": welcome, "base_version": 0},
                           name=AGENT, key=key_v1),
                      "写入欢迎记忆")
    step(5, "欢迎记忆写入成功", data.get("version") == 1, f"version={data.get('version')}")
    mems = expect_env(call("GET", "/api/memory?section=project", name=AGENT, key=key_v1),
                      "读取项目档案")
    hit = next((m for m in mems if m.get("key") == f"welcome/{AGENT}"), None)
    step(6, "欢迎记忆可读（共享给所有 Agent）", hit is not None)

    # ---- B1 模拟密钥缓存丢失 ----
    stop_platformd()
    agents_json = _home / "config" / "agents.json"
    with open(agents_json, "r", encoding="utf-8") as f:
        cached = json.load(f)
    cached.pop(AGENT, None)
    with open(agents_json, "w", encoding="utf-8") as f:
        json.dump(cached, f, ensure_ascii=False, indent=2)
    start_platformd()

    # ---- B2 诊断必须报出密钥丢失（总览页横幅的数据源）----
    d = diagnostics()
    step(7, "密钥丢失后诊断报出 keyfile_missing",
         AGENT in d.get("keyfile_missing", []), f"keyfile_missing={d.get('keyfile_missing')}")

    # ---- B3 认证走库内哈希：旧密钥仍可用；丢失的是明文缓存（无法再查看/补配）----
    status, _ = call("POST", "/api/agents/heartbeat", {}, name=AGENT, key=key_v1)
    step(8, "明文缓存丢失不影响库内哈希认证（旧密钥仍 200）", status == 200, f"HTTP={status}")

    # ---- B4 轮换修复（界面「轮换密钥修复」按钮的同一后端调用）----
    data = expect_env(call("POST", "/api/agents/rotate", {"name": AGENT}, master=True),
                      "轮换密钥修复")
    key_v2 = data.get("api_key", "")
    step(9, "轮换成功并返回新密钥（不打印明文）", bool(key_v2) and key_v2 != key_v1)

    # ---- B5 修复闭环：旧钥立即失效 + 新钥在线 + 诊断不再报缺失 ----
    status, _ = call("POST", "/api/agents/heartbeat", {}, name=AGENT, key=key_v1)
    step(10, "轮换后旧密钥立即失效（401）", status == 401, f"HTTP={status}")
    expect_env(call("POST", "/api/agents/heartbeat", {}, name=AGENT, key=key_v2),
               "新密钥心跳")
    agents = expect_env(call("GET", "/api/agents", name=AGENT, key=key_v2), "新密钥查列表")
    hit = next((a for a in agents if a.get("name") == AGENT), None)
    step(11, "新密钥认证成功且状态为 online", hit is not None and hit.get("status") == "online",
         f"status={hit.get('status') if hit else '未找到'}")
    d = diagnostics()
    step(12, "诊断不再报密钥缺失", AGENT not in d.get("keyfile_missing", []),
         f"keyfile_missing={d.get('keyfile_missing')}")

    # ---- C1 重复注册：后端报错文案稳定，界面上由 humanError 翻译 ----
    status, payload = call("POST", "/api/agents/register", {"name": AGENT}, master=True)
    msg = str(payload.get("message", ""))
    step(13, "重复注册被拒并返回可翻译的报错", status == 400 and "already registered" in msg,
         f"HTTP={status} message={msg!r}")

    # ---- C2 无效主密钥 ----
    status, payload = call("GET", "/api/diagnostics",
                           body=None, master=False)
    # 上一步没带主密钥 → 403 invalid master key
    step(14, "无主密钥访问诊断被拒（403）", status == 403,
         f"HTTP={status} message={payload.get('message')!r}")

    print("=" * 72)
    print(f"全部 {_passed} 步通过 ✓")
    print("-" * 72)
    print("GUI 层（需手动/界面验证，脚本无法覆盖 HTTP 之外的部分）：")
    print("  1) 首次启动（删除 QSettings 的 ui/welcomeSeen）→ 引导弹窗出现，")
    print("     点 Claude Code / Cursor / Codex CLI → 检测安装并生成 MCP 配置 JSON/TOML；")
    print("  2) 「复制配置」→ 粘贴进对应工具 → 重启该工具 → 工作台弹「接入成功」")
    print("     且「用户记忆→项目档案」出现 welcome/<tool> 条目（每个身份只提示一次）；")
    print("  3) 总览页顶部健康横幅：制造密钥丢失（删除 agents.json 条目并重启）→")
    print("     黄色横幅出现 → 「轮换密钥修复」→ 新钥弹窗可复制；")
    print("  4) 设置 →「Agent 管理」→「重新打开接入引导」可随时重进引导。")

    cleanup()


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as e:  # 兜底：不留孤儿进程
        print(f"测试框架异常: {type(e).__name__}: {e}")
        cleanup()
        sys.exit(1)
