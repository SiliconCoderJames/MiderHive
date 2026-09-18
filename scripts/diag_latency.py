#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""诊断检索延迟的 ~40ms 地板来自哪里。

区分三种假设：
  H1 传输层（每次新建 TCP 连接 + Nagle/延迟 ACK）
  H2 服务端每请求固定成本（JSON 解析/鉴权/全局锁）
  H3 检索本身（数据量相关）

方法：同一 platformd 上测三组：
  A. GET /api/health，每次新建连接（最小服务端工作 + 无 keep-alive）
  B. GET /api/health，单条 keep-alive 连接连发（排除连接建立）
  C. POST /api/knowledge/search（含检索工作）
"""
import http.client
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19321
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLATFORMD = os.path.join(REPO, "build", "full", "src", "cli", "Release", "platformd.exe")


def stats(samples):
    return "avg=%6.1f  p50=%6.1f  p95=%6.1f  min=%6.1f" % (
        statistics.mean(samples), statistics.median(samples),
        sorted(samples)[int(len(samples) * 0.95)], min(samples))


def main():
    home = tempfile.mkdtemp(prefix="mh-lat-")
    env = dict(os.environ)
    env["MIDERHIVE_HOME"] = home
    env["MIDERHIVE_PORT"] = str(PORT)
    env["MIDERHIVE_MASTER_KEY"] = "lat"
    proc = subprocess.Popen([PLATFORMD], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # 等就绪
        for _ in range(60):
            try:
                c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
                c.request("GET", "/api/health")
                c.getresponse().read()
                c.close()
                break
            except Exception:
                time.sleep(0.5)

        # A: 每次新建连接
        a = []
        for _ in range(60):
            t = time.perf_counter()
            c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
            c.request("GET", "/api/health")
            c.getresponse().read()
            c.close()
            a.append((time.perf_counter() - t) * 1000)

        # B: keep-alive 连发（同一连接）
        c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
        b = []
        for _ in range(60):
            t = time.perf_counter()
            c.request("GET", "/api/health")
            c.getresponse().read()
            b.append((time.perf_counter() - t) * 1000)

        # C: keep-alive + 检索（空库，量测服务端检索路径的地板）
        c.request("POST", "/api/agents/register",
                  json.dumps({"name": "bench", "role": "member"}),
                  {"X-Master-Key": "lat", "Content-Type": "application/json"})
        key = json.loads(c.getresponse().read())["data"]["api_key"]
        hdr = {"X-Agent-Name": "bench", "X-Api-Key": key,
               "Content-Type": "application/json"}
        cc = []
        for _ in range(60):
            t = time.perf_counter()
            c.request("POST", "/api/knowledge/search",
                      json.dumps({"query": "anything", "mode": "semantic", "limit": 20}),
                      hdr)
            c.getresponse().read()
            cc.append((time.perf_counter() - t) * 1000)
        c.close()

        print("A. health 新建连接     :", stats(a))
        print("B. health keep-alive   :", stats(b))
        print("C. semantic(空库) KA   :", stats(cc))
        print()
        print("解读：A≈B≫min → 服务端每请求成本；A≫B → 连接建立/传输层；B≈C → 检索非主导")
    finally:
        if proc.poll() is None:
            proc.kill()
        import shutil
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
