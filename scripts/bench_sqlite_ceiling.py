#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""写入吞吐天花板诊断（批次 10 的配套证据）。

`bench_concurrent.py` 测出：纯写负载下单线程 167 req/s，4 线程只有 1.14x
（甚至比 2 线程更低），p50 从 1.6ms 涨到 18ms。这说明写入被串行化了。
但"串行化"有两个可能来源，必须分清，否则会把优化投错地方：

  H-lock : Platform 的全局互斥锁（所有写路径共用一把 std::mutex）
  H-fsync: SQLite WAL 每次提交都要 fsync —— 磁盘本身就是唯一写入者

本脚本用 Python 标准库 sqlite3 复刻**同样的写入形态**（每条一个事务），
在同一个数据目录、同一块盘上量三种配置：

  A. WAL + synchronous=FULL   每条一事务   ← 当前产品等价配置
  B. WAL + synchronous=NORMAL 每条一事务   ← 去掉每提交 fsync
  C. WAL + synchronous=FULL   每 20 条一事务 ← 组提交（group commit）

判读：
  · 若 A≈C 且都远低于 B → 瓶颈是 fsync，不是锁；优化方向是组提交，
    且"要不要放宽同步等级"是**用户可见的持久性取舍**，不能偷偷改。
  · 若 A 远低于 C（B 也不高）→ 每提交固定开销（含锁）主导，锁细化有意义。

用法: python scripts/bench_sqlite_ceiling.py [--rows 400]
"""
import argparse
import os
import shutil
import sqlite3
import statistics
import sys
import tempfile
import time

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

SCHEMA = """
CREATE TABLE knowledge_entries(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  uuid TEXT NOT NULL, title TEXT NOT NULL, content TEXT NOT NULL,
  category TEXT NOT NULL DEFAULT '', author TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL, is_latest INTEGER NOT NULL DEFAULT 1);
CREATE INDEX idx_knowledge_uuid ON knowledge_entries(uuid);
CREATE TABLE audit_log(id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT, actor TEXT,
  action TEXT, target TEXT, detail TEXT);
"""


def bench(path, sync, batch, rows):
    """返回 (ops_per_sec, p50_ms, p95_ms)"""
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.executescript("PRAGMA journal_mode=WAL; PRAGMA synchronous=%s;" % sync)
    db.executescript(SCHEMA)
    db.commit()
    body = "x" * 900                      # 贴近真实条目正文量级
    lat = []
    t0 = time.perf_counter()
    for i in range(rows):
        if i % batch == 0:
            t = time.perf_counter()
        db.execute("INSERT INTO knowledge_entries(uuid,title,content,created_at) "
                   "VALUES(?,?,?,'2026-01-01T00:00:00Z')", ("u%d" % i, "t%d" % i, body))
        db.execute("INSERT INTO audit_log(ts,actor,action) VALUES('now','a','knowledge.create')")
        if (i + 1) % batch == 0:
            db.commit()
            lat.append((time.perf_counter() - t) * 1000.0 / batch)
    if rows % batch:
        db.commit()
    wall = time.perf_counter() - t0
    db.close()
    ordered = sorted(lat) if lat else [0.0]
    return (rows / wall, statistics.median(ordered),
            ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=400)
    args = ap.parse_args()
    home = tempfile.mkdtemp(prefix="mh-sqlite-ceiling-")
    try:
        print("==== SQLite 写入天花板诊断（每条 2 个 INSERT，正文 ~900B，%d 条/档）====" % args.rows)
        print("%-38s %12s %10s %10s" % ("配置", "吞吐(txn/s)", "p50(ms)", "p95(ms)"))
        cases = [
            ("A. WAL + synchronous=FULL，每条一事务", "FULL", 1),
            ("B. WAL + synchronous=NORMAL，每条一事务", "NORMAL", 1),
            ("C. WAL + synchronous=FULL，20 条一事务", "FULL", 20),
            ("D. WAL + synchronous=NORMAL，20 条一事务", "NORMAL", 20),
        ]
        out = {}
        for label, sync, batch in cases:
            tps, p50, p95 = bench(os.path.join(home, "b_%s_%d.db" % (sync, batch)), sync, batch,
                                 args.rows)
            out[label[0]] = tps
            print("%-38s %12.1f %10.2f %10.2f" % (label, tps, p50, p95))
        a, b, c, d = out["A"], out["B"], out["C"], out["D"]
        print()
        print("A→B（去掉每提交 fsync）      : %5.2fx" % (b / a if a else 0))
        print("A→C（FULL 下组提交 20 条）   : %5.2fx" % (c / a if a else 0))
        print("A→D（同时放宽同步 + 组提交） : %5.2fx" % (d / a if a else 0))
        print()
        if c / a > 1.5 and b / a > 1.5:
            print("判读：fsync 是写入天花板的主因 → 优化方向是组提交；放宽同步等级属于")
            print("      用户可见的持久性取舍，只能做成显式选项，不能默认改。")
        elif c / a <= 1.5 and b / a > 1.5:
            print("判读：每提交固定开销（含锁）主导 → 锁细化 + 组提交都值得做。")
        else:
            print("判读：组提交收益有限，瓶颈不在提交路径 → 需另找原因。")
        return 0
    finally:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
