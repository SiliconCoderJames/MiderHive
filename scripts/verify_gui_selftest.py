#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GUI 全链路自测运行器（离屏）。

拉起 build 产物里的 gui_selftest.exe（真实 MainWindow/WelcomeDialog/SettingsDialog），
以 QT_QPA_PLATFORM=offscreen 无头运行，覆盖：

  ① 首启接入引导自动弹出 → 选 Claude Code → 预配身份 + 生成 MCP 配置 + 登记
  ② 模拟 Agent 心跳上线 → 「接入成功」弹窗 + 欢迎记忆 + 登记清除（只提示一次）
  ③ 制造明文密钥丢失 → 总览页健康横幅 → 「轮换密钥修复」→ 旧密钥立即失效
  ④ 设置 → 「重新打开接入引导」重入
  另含纯逻辑断言：MCP 配置生成（JSON/TOML）、报错翻译、启动自检端口探测。

用法：
    python scripts/verify_gui_selftest.py [--build DIR]

退出码：0 = 全部通过；1 = 有断言失败 / 目标未构建 / 环境异常。
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

try:  # GBK 控制台下防止 print 中文炸编码
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

QT_BIN_CANDIDATES = [
    r"C:\Qt\6.8.3\msvc2022_64\bin",
    r"C:\Qt\6.8.2\msvc2022_64\bin",
    r"C:\Qt\6.7.3\msvc2022_64\bin",
]
BUILD_CANDIDATES = [
    os.path.join("build", "e2e"),
    os.path.join("build"),
]


def find_exe(build_dir: str) -> str:
    for sub in ("src/gui/Release", "src\\gui\\Release", "Release", ""):
        p = os.path.join(build_dir, sub, "gui_selftest.exe") if sub else os.path.join(build_dir, "gui_selftest.exe")
        if os.path.isfile(p):
            return p
    return ""


def free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main() -> int:
    if os.name != "nt":
        print("SKIP: GUI 自测当前仅在 Windows 构建（Qt 工作台平台限制）")
        return 0

    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="", help="CMake 构建目录（默认自动探测 build/e2e）")
    ap.add_argument("--keep", action="store_true", help="保留临时数据目录以便排查")
    ap.add_argument("--no-build", action="store_true",
                    help="即使 gui_selftest.exe 过期也不自动重建（CI 已先行构建）")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = args.build or ""
    if not build_dir:
        for cand in BUILD_CANDIDATES:
            if os.path.isdir(os.path.join(root, cand)):
                build_dir = os.path.join(root, cand)
                break
    exe = find_exe(build_dir) if build_dir else ""

    # 过期自检：gui_selftest.exe 比任一 GUI 源文件旧（或不存在）时自动重建。
    # 此前脚本只找 exe 不构建——改了 GUI 代码后直接跑会拿到旧产物，绿灯不可信
    #（CI 无此问题，ci.yml 先行构建；本开关主要护住本地快速循环）。
    # mtime 只覆盖 src/gui/**：core 层变更不触发重建，GUI 自测主要盯 GUI 层。
    if not args.no_build:
        def gui_sources_stale(exe_path):
            if not exe_path or not os.path.exists(exe_path):
                return True
            exe_mtime = os.path.getmtime(exe_path)
            gui_src = os.path.join(root, "src", "gui")
            for dirpath, _dirnames, filenames in os.walk(gui_src):
                for name in filenames:
                    if name.endswith((".cpp", ".h")):
                        if os.path.getmtime(os.path.join(dirpath, name)) > exe_mtime:
                            return True
            return False

        if gui_sources_stale(exe):
            if not build_dir:
                print("FAIL: gui_selftest 过期但未找到构建目录，请用 --build 指定")
                return 1
            print("[runner] gui_selftest.exe 过期（源码有改动），正在重新构建……")
            build_cmd = ["cmake", "--build", build_dir, "--config", "Release",
                         "--target", "gui_selftest"]
            try:
                subprocess.run(build_cmd, check=True)
            except (subprocess.CalledProcessError, FileNotFoundError) as e:
                print(f"FAIL: 自动构建失败（{e}）—— 请手动执行：")
                print(f"  cmake --build {build_dir} --config Release --target gui_selftest")
                return 1
            exe = find_exe(build_dir) if build_dir else ""

    if not exe:
        # 最后再全盘找一次（用户自定义构建树）
        for base in (root,):
            for dirpath, _dirnames, filenames in os.walk(os.path.join(base, "build")):
                if "gui_selftest.exe" in filenames:
                    exe = os.path.join(dirpath, "gui_selftest.exe")
                    break
            if exe:
                break
    if not exe:
        print("FAIL: 未找到 gui_selftest.exe —— 请先构建：")
        print('  cmake --build build\\e2e --config Release --target gui_selftest')
        return 1
    print(f"[runner] exe = {exe}")

    home = tempfile.mkdtemp(prefix="miderhive-guiself-")
    port = free_port()
    env = os.environ.copy()
    env["MIDERHIVE_HOME"] = home
    env["MIDERHIVE_PORT"] = str(port)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_LOGGING_TO_CONSOLE"] = "1"
    for qt in QT_BIN_CANDIDATES:
        if os.path.isdir(qt):
            env["PATH"] = qt + os.pathsep + env.get("PATH", "")
            print(f"[runner] Qt bin 前置: {qt}")
            break

    print(f"[runner] MIDERHIVE_HOME = {home}")
    print(f"[runner] MIDERHIVE_PORT = {port}")
    print("[runner] 启动 gui_selftest（offscreen，超时 180s）……")
    t0 = time.time()
    try:
        proc = subprocess.run(
            [exe],
            env=env,
            cwd=root,
            timeout=180,
            capture_output=True,
        )
        out = proc.stdout.decode("utf-8", errors="replace")
        errout = proc.stderr.decode("utf-8", errors="replace")
        code = proc.returncode
    except subprocess.TimeoutExpired:
        print("FAIL: gui_selftest 超时未退出（180s）")
        return 1
    elapsed = time.time() - t0

    print("---- gui_selftest 输出 ----")
    print(out.rstrip() or "(无 stdout)")
    if errout.strip():
        print("---- stderr（截尾） ----")
        print("\n".join(errout.strip().splitlines()[-15:]))

    ok = code == 0 and "failed=0" in out
    if ok:
        print(f"\nPASS: GUI 全链路自测通过（{elapsed:.1f}s）")
    else:
        print(f"\nFAIL: 退出码={code}（{elapsed:.1f}s）—— 见上方报告与失败明细")
    if args.keep:
        print(f"[runner] 保留数据目录：{home}")
    else:
        shutil.rmtree(home, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
