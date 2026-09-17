#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""知识库检索性能基准（一次性测量工具，不进 CI 硬断言）。

流程：起一个隔离的 platformd（临时数据目录）→ 预配身份 → 批量播种知识条目 →
对三条检索路径各计时若干轮，输出 P50/P95/最大值：

  keyword-fts     >=3 码点，走 FTS5 trigram 全文索引（主路径）
  keyword-like    2 码点，回退 LIKE 子串扫描（慢路径基线）
  semantic        384 维语义检索（vec0 kNN）

注意：
  * 数字是**一次性的本机测量**，随机器/数据分布变化；不要拿它当 SLA。
  * --assert-p95-ms 可选开关：给出阈值则超限退出非 0（供手动回归对比），默认不断言。

用法:
  python scripts/bench_search.py [--rows 10000] [--runs 30] [--port 19291]
                                 [--platformd EXE] [--assert-p95-ms MS]
"""
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PLATFORMD = os.path.join(REPO, "build", "full", "src", "cli", "Release", "platformd.exe")


def http_json(method, url, body=None, headers=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def wait_healthy(base, seconds=30):
    for _ in range(seconds * 2):
        try:
            http_json("GET", base + "/api/health")
            return True
        except Exception:
            time.sleep(0.5)
    return False


def percentile(samples, p):
    if not samples:
        return float("nan")
    ordered = sorted(samples)
    idx = min(len(ordered) - 1, max(0, int(round(p / 100.0 * (len(ordered) - 1)))))
    return ordered[idx]


def main():
    ap = argparse.ArgumentParser(description="知识库检索性能基准")
    ap.add_argument("--rows", type=int, default=10000, help="播种条数（默认 10000）")
    ap.add_argument("--runs", type=int, default=30, help="每条路径计时轮数（默认 30）")
    ap.add_argument("--port", default="19291")
    ap.add_argument("--platformd", default=DEFAULT_PLATFORMD)
    ap.add_argument("--assert-p95-ms", type=float, default=None,
                    help="可选：keyword-fts 的 P95 超过该毫秒数则退出非 0")
    args = ap.parse_args()

    if not os.path.exists(args.platformd):
        print("platformd 不存在: %s（先构建，或用 --platformd 指定）" % args.platformd)
        return 2

    home = tempfile.mkdtemp(prefix="mh-bench-")
    env = dict(os.environ)
    env["MIDERHIVE_HOME"] = home
    env["MIDERHIVE_PORT"] = args.port
    env["MIDERHIVE_MASTER_KEY"] = "bench-not-a-secret"
    base = "http://127.0.0.1:" + args.port

    proc = subprocess.Popen([args.platformd], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_healthy(base):
            print("platformd 未在 %ss 内就绪" % 30)
            return 2

        reg = http_json("POST", base + "/api/agents/register",
                        {"name": "bench", "role": "member"},
                        {"X-Master-Key": "bench-not-a-secret"})
        auth = {"X-Agent-Name": "bench", "X-Api-Key": reg["data"]["api_key"]}

        # ---- 播种：条目共享一个召回词（保证语义/关键词都能命中），编号区分 ----
        print("播种 %d 条 ..." % args.rows)
        filler = ("Benchmark filler paragraph about build systems, vector search and CI "
                  "pipelines. ")
        t0 = time.perf_counter()
        for i in range(args.rows):
            body = {
                "title": "bench entry %d" % i,
                # 内容里放一个共享 token（检索目标）+ 大段填充（让 trigram/kNN 有真实工作量）
                "content": "benchfeed shared token entry %d. %s" % (i, filler * 3),
                "tags": ["bench"],
                "category": "bench",
            }
            http_json("POST", base + "/api/knowledge", body, auth)
        seed_secs = time.perf_counter() - t0
        print("播种完成：%.1fs（%.0f 条/秒）" % (seed_secs, args.rows / seed_secs))

        # ---- 计时 ----
        queries = [
            ("keyword-fts", {"query": "benchfeed shared token", "mode": "keyword",
                             "limit": 20}),
            ("keyword-like", {"query": "ar", "mode": "keyword", "limit": 20}),  # 2 字符 → LIKE
            ("semantic", {"query": "benchfeed shared token", "mode": "semantic",
                          "limit": 20}),
        ]
        results = {}
        warmup = 5
        for name, body in queries:
            for _ in range(warmup):
                http_json("POST", base + "/api/knowledge/search", body, auth)
            samples = []
            for _ in range(args.runs):
                t = time.perf_counter()
                http_json("POST", base + "/api/knowledge/search", body, auth)
                samples.append((time.perf_counter() - t) * 1000.0)
            results[name] = samples
            print("%-13s P50=%7.1f ms  P95=%7.1f ms  max=%7.1f ms"
                  % (name, percentile(samples, 50), percentile(samples, 95), max(samples)))

        fts_p95 = percentile(results["keyword-fts"], 95)
        like_p95 = percentile(results["keyword-like"], 95)
        print("\n可粘贴结论（%d rows，%d runs，本机一次性测量）：" % (args.rows, args.runs))
        print("  keyword(FTS) P95=%.1fms | keyword(LIKE 回退) P95=%.1fms | semantic P95=%.1fms"
              % (fts_p95, like_p95, percentile(results["semantic"], 95)))

        if args.assert_p95_ms is not None:
            if fts_p95 > args.assert_p95_ms:
                print("FAIL  keyword-fts P95 %.1fms 超过阈值 %.1fms" % (fts_p95, args.assert_p95_ms))
                return 1
            print("PASS  keyword-fts P95 %.1fms <= 阈值 %.1fms" % (fts_p95, args.assert_p95_ms))
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
