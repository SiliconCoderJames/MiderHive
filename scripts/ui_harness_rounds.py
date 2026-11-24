# -*- coding: utf-8 -*-
"""Repeat the UI harness in fresh environments and check that every round is identical.

Why this exists: determinism is the precondition for the UI harness to work as a
regression gate. One run matching the committed baselines does not show that the
next run will - timing, the calendar, the time zone, random numbers, fonts and
whatever is installed on the machine all leak in. This script runs the same
build N times, each round on a brand-new data directory, and checks:

  1. every round exits 0 (identical to the committed baselines, no rule
     violations, no missing baseline, no non-matching filter);
  2. the structural snapshots of all rounds are byte-identical, including the
     geometry that the baseline comparison itself does not look at;
  3. the golden images of all rounds are byte-identical.

Any failure is reported with the round, the scene and the first differing line.

Usage (from the repository root):
  python scripts/ui_harness_rounds.py                    # 20 rounds
  python scripts/ui_harness_rounds.py --rounds 5
  python scripts/ui_harness_rounds.py --filter dashboard --rounds 10

The Qt bin directory must hold Qt6Core.dll (default C:/Qt/6.8.3/msvc2022_64/bin,
override with --qt-bin). The harness needs it on PATH for its DLLs and plugins.
It creates its own scratch directories under the system temp directory and
removes them afterwards (unless --keep); nothing else is written outside
build/ui_harness/, which the harness owns.
"""
import argparse
import difflib
import glob
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENE_JSON = re.compile(r"^(.+)\.json$")
SUMMARY = re.compile(r"==== ui_harness .*?场景 (\d+)，PASS (\d+)，新建基线 (\d+)，结构差异 (\d+)，"
                     r"规则违例 (\d+)，弹窗 (\d+)，金样差异 (\d+)")


def parse_args():
    ap = argparse.ArgumentParser(description="Repeat the UI harness and require identical results.")
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--filter", default="", help="passed to the harness as --filter (substring)")
    ap.add_argument("--build", default="build", help="build tree that holds ui_harness.exe")
    ap.add_argument("--qt-bin", default="C:/Qt/6.8.3/msvc2022_64/bin")
    ap.add_argument("--keep", action="store_true", help="keep the per-round scratch directories")
    return ap.parse_args()


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def collect(out_dir):
    """Everything a run produced that must be reproducible: scene JSON + golden images."""
    scenes = {}
    for path in glob.glob(os.path.join(out_dir, "*.json")):
        name = os.path.basename(path)
        with io.open(path, "rb") as f:
            scenes[name] = f.read()
    goldens = {}
    for path in glob.glob(os.path.join(out_dir, "golden", "*.seeded.png")):
        with open(path, "rb") as f:
            goldens[os.path.basename(path)] = f.read()
    return scenes, goldens


def first_diff(a_bytes, b_bytes):
    a = a_bytes.decode("utf-8", "replace").splitlines()
    b = b_bytes.decode("utf-8", "replace").splitlines()
    for line in difflib.unified_diff(a, b, "round 1", "later round", n=0, lineterm=""):
        if line.startswith(("@@", "+", "-")) and not line.startswith(("+++", "---")):
            return line[:160]
    return "files differ but no line difference was found"


def main():
    args = parse_args()
    if args.rounds < 1:
        print("--rounds must be at least 1")
        return 2
    exe = os.path.join(ROOT, args.build, "src", "gui", "Release", "ui_harness.exe")
    out_dir = os.path.join(ROOT, args.build, "ui_harness")
    qt_bin = os.path.join(ROOT, args.qt_bin) if not os.path.isabs(args.qt_bin) else args.qt_bin
    if not os.path.isfile(exe):
        print("ui_harness.exe not found: %s (build it first)" % exe)
        return 2
    if not os.path.isfile(os.path.join(qt_bin, "Qt6Core.dll")):
        print("Qt6Core.dll not found in %s (use --qt-bin)" % qt_bin)
        return 2

    print("exe    : %s" % exe)
    print("rounds : %d%s" % (args.rounds, ("  filter: " + args.filter) if args.filter else ""))
    base_env = os.environ.copy()
    base_env["PATH"] = qt_bin + os.pathsep + base_env.get("PATH", "")
    base_env["QT_QPA_PLATFORM"] = "offscreen"
    base_env.pop("MIDERHIVE_UI_NOW", None)  # the harness freezes its own clock

    reference = None
    failures = []
    rows = []
    for r in range(1, args.rounds + 1):
        home = tempfile.mkdtemp(prefix="miderhive-uiharness-round-")
        env = dict(base_env, MIDERHIVE_HOME=home)
        cmd = [exe] + (["--filter", args.filter] if args.filter else [])
        try:
            proc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=600)
            text = proc.stdout.decode("utf-8", "replace")
            m = SUMMARY.search(text)
            summary = m.groups() if m else None
            scenes, goldens = collect(out_dir)
        finally:
            if not args.keep:
                shutil.rmtree(home, ignore_errors=True)
            else:
                print("  (kept scratch directory %s)" % home)

        problems = []
        if proc.returncode != 0:
            fatal = [ln for ln in text.splitlines() if ln.startswith(("FATAL", "RULE", "DIFF"))]
            problems.append("exit code %d; first findings: %s" % (proc.returncode, fatal[:3]))
        if summary is None:
            problems.append("no summary line in the output")
        elif summary[0] == "0":
            problems.append("no scene matched --filter; nothing was checked")

        snapshot = (summary, scenes, goldens)
        if reference is None:
            reference = snapshot
        else:
            ref_summary, ref_scenes, ref_goldens = reference
            if summary and ref_summary and summary != ref_summary:
                problems.append("summary differs from round 1: %s vs %s" % (summary, ref_summary))
            if set(scenes) != set(ref_scenes):
                problems.append("scene set differs from round 1: %s" % sorted(
                    set(scenes) ^ set(ref_scenes))[:6])
            for name in sorted(set(scenes) & set(ref_scenes)):
                if scenes[name] != ref_scenes[name]:
                    problems.append("%s: %s" % (name, first_diff(ref_scenes[name], scenes[name])))
                    if len(problems) >= 6:
                        break
            for name in sorted(set(goldens) & set(ref_goldens)):
                if goldens[name] != ref_goldens[name]:
                    problems.append("golden image %s differs from round 1" % name)

        rows.append((r, proc.returncode, summary, problems))
        failures.extend((r, p) for p in problems)
        if not problems:
            print("round %2d: identical to round 1  (exit %d)" % (r, proc.returncode))
        else:
            print("round %2d: FAILED" % r)
            for p in problems:
                print("         %s" % p[:200])

    print()
    print("%-6s %-6s %-6s %-7s %-6s %-6s %-6s %-6s %-6s" % (
        "round", "exit", "scenes", "PASS", "NEW", "DIFF", "rules", "dialogs", "golden"))
    for r, code, summary, problems in rows:
        cells = summary if summary else ("?",) * 7
        print("%-6d %-6d %-6s %-7s %-6s %-6s %-6s %-6s %-6s%s" % (
            (r, code) + cells + ("   <-- FAILED" if problems else "",)))

    print()
    if failures:
        print("RESULT: FAILED - %d problem(s) across %d round(s)" % (len(failures), args.rounds))
        return 1
    print("RESULT: PASS - %d round(s), every snapshot and golden image byte-identical" % args.rounds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
