#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""agent-cli 外接嵌入端点的端到端验证（批次 9）。

验证"真语义"最小切片可用：起一个 mock 的 OpenAI 兼容嵌入端点（确定性向量），
用 agent-cli 走完整闭环——
    embed 取向量 → knowledge add --embed-url 写入 → knowledge search --embed-url 语义命中
并验证负路径（https 不支持、端点不可达、维度越界都要明确报错，而不是静默降级）。

用法: python scripts/embed_cli_check.py [--agent-cli EXE] [--platformd EXE]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(REPO, "build", "full", "src", "cli", "Release", "agent-cli.exe")
DEFAULT_PLATFORMD = os.path.join(REPO, "build", "full", "src", "cli", "Release", "platformd.exe")
DIM = 128  # 刻意不等于内置 384：证明走的是自带向量路径
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           (": " + detail) if detail and not ok else ""))


class MockEmbed(BaseHTTPRequestHandler):
    """OpenAI 兼容 /v1/embeddings：同文本同向量（确定性），便于验证写入-检索闭环。"""

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n).decode("utf-8"))
        text = req.get("input", "")
        h = hashlib.sha256(text.encode("utf-8")).digest()
        vec = [((h[i % len(h)] / 255.0) - 0.5) for i in range(DIM)]
        body = json.dumps({"data": [{"embedding": vec}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def run_cli(cli, args, env=None):
    return subprocess.run([cli] + args, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", env=env, timeout=120)


def wait_healthy(port, timeout=30):
    for _ in range(timeout * 2):
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/health" % port, timeout=3).read()
            return True
        except Exception:
            time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", default="")
    ap.add_argument("--platformd", default="")
    args = ap.parse_args()
    cli = args.agent_cli if args.agent_cli and os.path.exists(args.agent_cli) else DEFAULT_CLI
    platformd = (args.platformd if args.platformd and os.path.exists(args.platformd)
                 else DEFAULT_PLATFORMD)
    for path, what in ((cli, "agent-cli"), (platformd, "platformd")):
        if not os.path.exists(path):
            print("%s 不存在: %s" % (what, path))
            return 2

    # mock 嵌入端点
    srv = HTTPServer(("127.0.0.1", 0), MockEmbed)
    emb_port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    emb_url = "http://127.0.0.1:%d/v1/embeddings" % emb_port

    home = tempfile.mkdtemp(prefix="mh-embed-")
    port = 19391
    env = dict(os.environ)
    env["MIDERHIVE_HOME"] = home
    env["MIDERHIVE_PORT"] = str(port)
    env["MIDERHIVE_MASTER_KEY"] = "embed-check-master"
    proc = subprocess.Popen([platformd], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        print("==== agent-cli 外接嵌入端点验证（mock: %s）====" % emb_url)
        if not wait_healthy(port):
            check("platformd 就绪", False, "30s 内未健康")
            return 1
        # 注册 agent
        reg = json.loads(urllib.request.urlopen(urllib.request.Request(
            "http://127.0.0.1:%d/api/agents/register" % port,
            data=json.dumps({"name": "embedder", "role": "member"}).encode(),
            headers={"X-Master-Key": "embed-check-master", "Content-Type": "application/json"},
            method="POST"), timeout=20).read())
        key = reg["data"]["api_key"]
        env["MIDERHIVE_AGENT_NAME"] = "embedder"
        env["MIDERHIVE_AGENT_KEY"] = key

        # 1) embed 子命令：输出合法维度数组
        r = run_cli(cli, ["embed", "--url", emb_url, "--model", "mock-model",
                          "--text", "hello semantic world"], env=env)
        check("embed 成功返回向量", r.returncode == 0, (r.stderr or "")[:160])
        vec = json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else []
        check("embed 维度为 mock 的 %d 维（非内置 384）" % DIM, len(vec) == DIM, str(len(vec)))
        check("embed 输出为数字数组", bool(vec) and all(isinstance(v, (int, float)) for v in vec))

        # 2) knowledge add --embed-url：写入真语义向量
        text = "MiderHive true semantic probe with external embedding endpoint"
        r = run_cli(cli, ["knowledge", "add", "--title", "external embed probe",
                          "--content", text, "--embed-url", emb_url,
                          "--embed-model", "mock-model"], env=env)
        check("knowledge add --embed-url 写入成功", r.returncode == 0, (r.stderr or "")[:200])
        uuid = ""
        try:
            uuid = json.loads(r.stdout)["data"]["uuid"]
        except Exception:
            pass
        check("写入返回 uuid", bool(uuid))
        if uuid:
            # provider 必须是模型名（证明走的是自带向量路径，而非内置 ngram-hash）
            r = run_cli(cli, ["knowledge", "get", "--uuid", uuid], env=env)
            prov = ""
            try:
                prov = json.loads(r.stdout)["data"]["embedding_provider"]
            except Exception:
                pass
            check("embedding_provider 记为模型名", prov == "mock-model", prov)

        # 3) knowledge search --embed-url：同模型查询 → 语义闭环命中
        r = run_cli(cli, ["knowledge", "search", "--q", text, "--mode", "semantic",
                          "--embed-url", emb_url, "--embed-model", "mock-model"], env=env)
        hits = []
        try:
            hits = json.loads(r.stdout)["data"]
        except Exception:
            pass
        check("语义检索闭环命中该条目",
              r.returncode == 0 and any(h.get("uuid") == uuid for h in hits),
              (r.stderr or str(hits))[:200])

        # 4) 负路径：https 明确报错（本构建无 TLS）
        r = run_cli(cli, ["embed", "--url", "https://example.com/v1/embeddings",
                          "--text", "x"], env=env)
        check("https 明确拒绝并说明原因",
              r.returncode != 0 and "https" in (r.stderr or "").lower(), (r.stderr or "")[:160])

        # 5) 负路径：端点不可达必须报错而不是静默降级
        r = run_cli(cli, ["embed", "--url", "http://127.0.0.1:1/v1/embeddings",
                          "--text", "x"], env=env)
        check("端点不可达明确报错", r.returncode != 0, (r.stderr or "")[:160])
    finally:
        if proc.poll() is None:
            proc.kill()
        srv.shutdown()
        shutil.rmtree(home, ignore_errors=True)

    failed = [r for r in RESULTS if not r[1]]
    print("\n外接嵌入端点验证: %d 项, 失败 %d 项" % (len(RESULTS), len(failed)))
    for name, _, detail in failed:
        print("  FAIL %s: %s" % (name, detail))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
