#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""并发吞吐基准（批次 10）：判断"单把全局互斥锁"到底是不是瓶颈。

背景：Platform 里所有写路径都走一把全局互斥锁（`std::mutex`），怀疑多 Agent
并发时互相排队。这个脚本不去猜，而是量：

  固定时长内跑 1 / 2 / 4 / 8 个并发 worker，每个 worker 一条 keep-alive 连接，
  按 80% 读（关键词检索）/ 20% 写（新建知识条目）混合发请求，分别统计

    · 吞吐（成功请求数 / 墙钟秒）
    · 延迟分位（p50 / p95 / p99，仅成功请求）
    · 错误数（HTTP 非 2xx 或异常）

  然后给出决策门：若 4 线程吞吐 ≤ 1.3 × 单线程吞吐，说明并发几乎无扩展性，
  锁细化/连接池值得立项；否则该缺陷判定为"非瓶颈"，结案并在报告里记数字。

用法:
  python scripts/bench_concurrent.py                    # 默认每档 8 秒
  python scripts/bench_concurrent.py --duration 20
  python scripts/bench_concurrent.py --threads 1,2,4,8 --rows 2000

注意：这是**测量**不是测试，不设通过阈值（只有已知的 0 错误才算失败）。
"""
import argparse
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PLATFORMD = os.path.join(REPO, "build", "full", "src", "cli", "Release", "platformd.exe")
MASTER = "bench-master-key"
WORDS = ["concurrency", "embedding", "rollback", "trigram", "migration", "heartbeat",
         "snapshot", "backpressure", "idempotent", "checkpoint"]


def pct(samples, p):
    if not samples:
        return 0.0
    ordered = sorted(samples)
    idx = min(len(ordered) - 1, int(len(ordered) * p))
    return ordered[idx]


def request(conn, method, path, agent, key, body=None):
    """发一个请求并读掉响应；返回 (status, 耗时毫秒, 解析后的 JSON 或 None)。"""
    head = {"Content-Type": "application/json"}
    if agent:
        head["X-Agent-Name"] = agent
    if key:
        head["X-Api-Key"] = key
    payload = json.dumps(body) if body is not None else None
    t = time.perf_counter()
    conn.request(method, path, payload, head)
    resp = conn.getresponse()
    raw = resp.read()
    ms = (time.perf_counter() - t) * 1000.0
    try:
        return resp.status, ms, json.loads(raw.decode("utf-8"))
    except Exception:
        return resp.status, ms, None


def wait_healthy(port, timeout=30):
    for _ in range(timeout * 2):
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/health" % port, timeout=3).read()
            return True
        except Exception:
            time.sleep(0.5)
    return False


def register(port, name, role="member"):
    body = json.dumps({"name": name, "role": role}).encode()
    req = urllib.request.Request("http://127.0.0.1:%d/api/agents/register" % port, data=body,
                                 headers={"X-Master-Key": MASTER,
                                          "Content-Type": "application/json"},
                                 method="POST")
    return json.loads(urllib.request.urlopen(req, timeout=20).read())["data"]["api_key"]


def seed(port, key, rows):
    """播种数据：worker 的检索要有真实命中，否则测的是空表。"""
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    for i in range(rows):
        w = WORDS[i % len(WORDS)]
        request(c, "POST", "/api/knowledge", "seeder", key,
                {"title": "bench entry %d about %s" % (i, w),
                 "content": ("seeded body %d; topic %s; " % (i, w)) * 8,
                 "tags": [w], "category": "bench"})
    c.close()


class Worker(threading.Thread):
    def __init__(self, port, agent, key, deadline, mix):
        super().__init__(daemon=True)
        self.port, self.agent, self.key = port, agent, key
        self.deadline = deadline
        self.mix = mix
        self.lat = []
        self.errors = 0
        self.reads = self.writes = 0
        self.seq = 0
        self.first_err = ""

    def run(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            while time.perf_counter() < self.deadline:
                self.seq += 1
                # mixed: 4/5 读、1/5 写；read/write: 全读或全写（用于把"全局锁"假设单独隔离）
                if self.mix == "read":
                    is_read = True
                elif self.mix == "write":
                    is_read = False
                else:
                    is_read = (self.seq % 5) != 0
                if is_read:
                    w = WORDS[self.seq % len(WORDS)]
                    status, ms, data = request(
                        conn, "POST", "/api/knowledge/search", self.agent, self.key,
                        {"query": w, "mode": "keyword", "limit": 20})
                    good = status == 200 and data is not None and data.get("code") == 0
                    if good:
                        self.reads += 1
                else:
                    w = WORDS[self.seq % len(WORDS)]
                    status, ms, data = request(
                        conn, "POST", "/api/knowledge", self.agent, self.key,
                        {"title": "live %d-%d %s" % (id(self) % 997, self.seq, w),
                         "content": ("live body %d about %s; " % (self.seq, w)) * 6,
                         "tags": [w], "category": "bench-live"})
                    good = status == 200 and data is not None and data.get("code") == 0
                    if good:
                        self.writes += 1
                if good:
                    self.lat.append(ms)
                else:
                    self.errors += 1
                    if not self.first_err:
                        self.first_err = "HTTP %s %s" % (status, str(data)[:120])
        except Exception as exc:                      # 连接层异常也算错误，不能吞
            self.errors += 1
            if not self.first_err:
                self.first_err = "%s: %s" % (type(exc).__name__, exc)
        finally:
            try:
                conn.close()
            except Exception:
                pass


def run_level(port, agents, seconds, mix):
    deadline = time.perf_counter() + seconds
    workers = [Worker(port, name, key, deadline, mix) for name, key in agents]
    t0 = time.perf_counter()
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    wall = time.perf_counter() - t0
    lat = [x for w in workers for x in w.lat]
    total = len(lat)
    return {
        "threads": len(workers),
        "wall": wall,
        "ops": total,
        "throughput": total / wall if wall > 0 else 0.0,
        "reads": sum(w.reads for w in workers),
        "writes": sum(w.writes for w in workers),
        "errors": sum(w.errors for w in workers),
        "first_err": next((w.first_err for w in workers if w.first_err), ""),
        "p50": pct(lat, 0.50), "p95": pct(lat, 0.95), "p99": pct(lat, 0.99),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platformd", default="")
    ap.add_argument("--port", type=int, default=19402)
    ap.add_argument("--duration", type=float, default=8.0, help="每档持续秒数")
    ap.add_argument("--threads", default="1,2,4,8")
    ap.add_argument("--rows", type=int, default=500, help="播种条目数")
    ap.add_argument("--mix", default="mixed", choices=["mixed", "read", "write"],
                    help="mixed=80/20 读写（默认）；read/write 用于隔离全局锁假设")
    args = ap.parse_args()
    levels = [int(x) for x in args.threads.split(",") if x.strip()]
    max_workers = max(levels)

    exe = args.platformd if args.platformd and os.path.exists(args.platformd) else DEFAULT_PLATFORMD
    if not os.path.exists(exe):
        print("platformd 不存在: %s" % exe)
        return 2

    home = tempfile.mkdtemp(prefix="mh-bench-")
    env = dict(os.environ)
    env["MIDERHIVE_HOME"] = home
    env["MIDERHIVE_PORT"] = str(args.port)
    env["MIDERHIVE_MASTER_KEY"] = MASTER
    proc = subprocess.Popen([exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_healthy(args.port):
            print("platformd 未在 30s 内就绪")
            return 1
        agents = [("bench%d" % i, register(args.port, "bench%d" % i)) for i in range(max_workers)]
        print("播种 %d 条（每档前不复用，读混合里有真实命中）..." % args.rows)
        t = time.perf_counter()
        seed(args.port, agents[0][1], args.rows)
        print("播种完成 %.1fs\n" % (time.perf_counter() - t))

        label = {"mixed": "80% keyword 检索 / 20% 新建条目",
                 "read": "100% keyword 检索",
                 "write": "100% 新建条目"}[args.mix]
        print("==== 并发吞吐基准（每档 %.0fs，%s）====" % (args.duration, label))
        print("%7s %12s %10s %9s %9s %9s %8s %7s %7s" %
              ("线程", "吞吐(req/s)", "墙钟(s)", "p50(ms)", "p95(ms)", "p99(ms)", "成功", "错误", "写占比"))
        results = []
        for n in levels:
            r = run_level(args.port, agents[:n], args.duration, args.mix)
            results.append(r)
            print("%7d %12.1f %10.2f %9.2f %9.2f %9.2f %8d %7d %6.0f%%" %
                  (r["threads"], r["throughput"], r["wall"], r["p50"], r["p95"], r["p99"],
                   r["ops"], r["errors"],
                   100.0 * r["writes"] / max(1, r["ops"])))
            if r["errors"]:
                print("         首个错误: %s" % r["first_err"])

        base = next((r for r in results if r["threads"] == 1), None)
        quad = next((r for r in results if r["threads"] == 4), None)
        print()
        total_errors = sum(r["errors"] for r in results)
        if base and quad:
            ratio = quad["throughput"] / base["throughput"] if base["throughput"] else 0.0
            kind = {"mixed": "混合负载", "read": "纯读（不经全局锁）", "write": "纯写（全经全局锁）"}[args.mix]
            print("决策门（%s）：4 线程 / 1 线程 吞吐比 = %.2fx（阈值 1.30x）" % (kind, ratio))
            if ratio <= 1.30:
                print("结论：并发扩展性不足 —— 全局锁细化/连接池应立项（见 docs/hardening-report.md）")
            else:
                print("结论：该负载下并发可扩展 —— 数字记入 docs/hardening-report.md 后再判是否立项")
        else:
            print("决策门：需要同时测 1 与 4 线程两档（--threads 1,4）才能判定")
        print("\n错误总数: %d" % total_errors)
        return 1 if total_errors else 0
    finally:
        if proc.poll() is None:
            proc.kill()
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
