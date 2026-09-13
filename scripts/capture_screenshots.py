#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""README 截图自动生成：演示数据 + 真实桌面启动 + 程序化截窗。

产出（默认写入 docs/assets/）：
  screenshot-dashboard.png   总览页：三个 Agent（2 在线）、知识/记忆/消息/错误/用量演示数据
  screenshot-health.png      防呆演示：明文密钥丢失 → 黄色健康横幅 + 「轮换密钥修复」按钮
  screenshot-onboarding.png  首次接入引导：工具选择/检测/配置生成弹窗

要点：
  - 全部使用隔离演示数据目录（~/.miderhive-demo-shots），不碰真实数据；
  - QSettings 仅写 ui/welcomeSeen 与 ui/autoCheck 两个键，退出前恢复原值；
  - 端口默认 8790（8787 常被真实工作台占用）；
  - 结束时终止演示进程、清理演示目录。

用法：python scripts/capture_screenshots.py [--stage DIR] [--port 8790] [--outdir docs/assets]
"""

import argparse
import ctypes
import ctypes.wintypes
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import winreg

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

from PIL import ImageGrab  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOW_TITLE_PREFIX = "MiderHive"
DEMO_HOME = os.path.join(os.path.expanduser("~"), ".miderhive-demo-shots")
SETTINGS_KEY = r"Software\miderhive\MiderHive 多 Agent 协作工作台"


def log(msg):
    print(f"[shots] {msg}", flush=True)


def dpi_aware():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        ctypes.windll.user32.SetProcessDPIAware()


def find_window_ctypes(prefix, timeout=20.0):
    """按标题前缀找可见、非最小化的顶层窗口（取面积最大者）。"""
    user32 = ctypes.windll.user32
    EnumWindows = user32.EnumWindows
    IsWindowVisible = user32.IsWindowVisible
    GetWindowTextLengthW = user32.GetWindowTextLengthW
    GetWindowTextW = user32.GetWindowTextW
    GetWindowRect = user32.GetWindowRect

    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    found = []

    def collect_titles():
        out = []
        hwnds = []
        @EnumWindowsProc
        def cb(hwnd, _l):
            hwnds.append(hwnd)
            return True
        EnumWindows(cb, None)
        for h in hwnds:
            if not IsWindowVisible(h):
                continue
            n = GetWindowTextLengthW(h)
            if not n:
                continue
            buf = ctypes.create_unicode_buffer(n + 1)
            GetWindowTextW(h, buf, n + 1)
            out.append((h, buf.value))
        return out

    deadline = time.time() + timeout
    while time.time() < deadline:
        cands = [(h, t) for h, t in collect_titles() if t.startswith(prefix)]
        # 最小化窗口坐标是 (-32000,-32000)，面积无意义；只接受正常尺寸的窗口
        good = [(h, t) for h, t in cands if _area(h) > 400 * 300]
        if good:
            if len(good) > 1:
                log(f"注意：多个 {prefix}* 窗口，取面积最大者 {[(t, _area(h)) for h, t in good]}")
            best = max(good, key=lambda ht: _area(ht[0]))
            return best
        time.sleep(0.3)
    raise RuntimeError(
        f"未找到标题以 {prefix!r} 开头的（非最小化）窗口；当前候选："
        f"{[(t, _area(h)) for h, t in cands]}")


def _area(hwnd):
    r = ctypes.wintypes.RECT()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(r))
    return max(0, r.right - r.left) * max(0, r.bottom - r.top)


def window_bounds(hwnd):
    """DWM 扩展边界（不含 Win11 阴影），失败回退 GetWindowRect。"""
    class RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]
    rect = RECT()
    r = ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(rect), ctypes.sizeof(rect))
    if r != 0:
        ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def raise_window(hwnd):
    SWP_NOSIZE, SWP_NOMOVE = 0x1, 0x2
    HWND_TOPMOST, HWND_NOTOPMOST = -1, -2
    u = ctypes.windll.user32
    u.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)
    time.sleep(0.6)
    u.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)
    time.sleep(0.3)


def capture(hwnd, path):
    """PrintWindow(PW_RENDERFULLCONTENT) 直接抓窗口自身像素：
    不依赖遮挡状态（用户桌面上盖了别的窗口也能截到），不打扰前台。"""
    l, t, r, b = window_bounds(hwnd)
    if r <= l or b <= t:
        raise RuntimeError(f"窗口边界异常 hwnd={hwnd}: {(l, t, r, b)}")
    w, h = r - l, b - t
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    hdc_win = user32.GetWindowDC(hwnd)
    if not hdc_win:
        raise RuntimeError("GetWindowDC 失败")
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    bmp = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    gdi32.SelectObject(hdc_mem, bmp)
    PW_RENDERFULLCONTENT = 2
    if not user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT):
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(hwnd, hdc_win)
        raise RuntimeError("PrintWindow 失败")

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]
    bmi = BMIH(ctypes.sizeof(BMIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)  # DIB_RGB_COLORS
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_win)

    from PIL import Image
    img = Image.frombuffer("RGBX", (w, h), buf.raw, "raw", "RGBX", 0, 1).convert("RGB")
    img.load()
    if w < 600 or h < 400:
        raise RuntimeError(f"裁剪尺寸过小 {(w, h)}: {path}")
    img.save(path)
    colors = img.getcolors(maxcolors=10 ** 7)
    if colors is None or len(colors) < 100:
        raise RuntimeError(f"截图异常（{img.size}, {len(colors) if colors else '>=100000'} 色，"
                           f"疑似黑屏/空窗口）: {path}")
    log(f"已保存 {path}  {img.size}")


def http(base, method, path, body=None, headers=None, timeout=8):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.loads(resp.read().decode())
    if payload.get("code") != 0:
        raise RuntimeError(f"{method} {path} -> {payload}")
    return resp.status, payload.get("data")


def wait_health(base, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            code, _ = http(base, "GET", "/api/health", timeout=3)
            if code == 200:
                return
        except Exception:
            pass
        time.sleep(0.4)
    raise RuntimeError("平台健康检查超时")


class SettingsGuard:
    """备份/恢复 QSettings(native) 里的 ui/welcomeSeen 与 ui/autoCheck。"""

    def __init__(self):
        self.subkey = SETTINGS_KEY + r"\ui"
        self.saved = {}
        self.written = False

    def _open(self, writable=False):
        return winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, self.subkey, 0, winreg.KEY_SET_VALUE | winreg.KEY_QUERY_VALUE if writable else winreg.KEY_QUERY_VALUE)

    def _read(self, name):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.subkey) as k:
                return winreg.QueryValueEx(k, name)[0]
        except OSError:
            return None

    def _write(self, name, value):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, self.subkey) as k:
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, 1 if value else 0)

    def _delete(self, name):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.subkey, 0, winreg.KEY_SET_VALUE) as k:
                winreg.DeleteValue(k, name)
        except OSError:
            pass

    def __enter__(self):
        for n in ("welcomeSeen", "autoCheck"):
            self.saved[n] = self._read(n)
        self.written = True
        self._write("welcomeSeen", 1)   # 总览页截图：不弹引导
        self._write("autoCheck", 0)     # 截图期间关闭自动更新检查
        return self

    def show_welcome_again(self):
        self._delete("welcomeSeen")     # 引导弹窗截图：恢复首启状态

    def __exit__(self, *exc):
        if not self.written:
            return
        for n, old in self.saved.items():
            if old is None:
                self._delete(n)
            else:
                self._write(n, old)


def seed(base, home):
    """造一套可信的演示数据（全中文、贴近真实研发场景）。"""
    mk = open(os.path.join(home, "config", "master.key"), encoding="utf-8").read().strip()
    mh = {"X-Master-Key": mk}
    _, res = http(base, "POST", "/api/agents/provision", {"name": "claude"}, mh)
    keys = {"claude": res["api_key"]}
    for name in ("codex", "cursor"):
        _, res = http(base, "POST", "/api/agents/provision", {"name": name}, mh)
        keys[name] = res["api_key"]

    def h(name):
        return {"X-Agent-Name": name, "X-Api-Key": keys[name]}

    def beat(name, task):
        http(base, "POST", "/api/agents/heartbeat", {"current_task": task}, h(name))

    beat("claude", "重构登录模块：抽取验证码服务")
    beat("codex", "为技能市场补充调用示例")
    beat("cursor", "修复 /api/knowledge 分页参数")

    kn = [
        ("MSVC /utf-8 编译选项必须显式开启",
         "中文注释在 MSVC 默认代码页下会被拆坏导致莫名编译错误；CMake 里 add_compile_options(/utf-8) 一次解决。",
         ["msvc", "cmake"], "踩坑"),
        ("SQLite WAL 模式的并发写要点",
         "两个进程同库写时 busy_timeout 要显式设置；写事务尽量小，长事务会放大 SQLITE_BUSY 概率。",
         ["sqlite"], "经验"),
        ("PowerShell 5.1 传参防截断",
         "给子进程传 -DKEY=1.0.0 这类带点号的参数必须整体加引号，否则会被截断成 KEY=1 且不报错。",
         ["powershell"], "踩坑"),
    ]
    for title, content, tags, cat in kn:
        http(base, "POST", "/api/knowledge",
             {"title": title, "content": content, "tags": tags, "category": cat}, h("claude"))

    http(base, "POST", "/api/memory",
         {"section": "project", "key": "当前迭代",
          "value": "首次接入引导与防呆设计已合入 master；下一轮评估本地嵌入模型（ONNX bge）。", "base_version": 0},
         h("claude"))

    http(base, "POST", "/api/skills",
         {"name": "release-package", "display_name": "出包流水线",
          "description": "package.ps1 一条命令出 MSI + ZIP + latest.json，并做可运行性自检",
          "category": "构建发布"}, h("codex"))
    http(base, "POST", "/api/skills",
         {"name": "db-backup", "display_name": "数据库快照",
          "description": "VACUUM INTO 一致性快照到备份目录，恢复需主密钥",
          "category": "运维"}, h("claude"))

    http(base, "POST", "/api/messages",
         {"kind": "note", "subject": "构建提示",
          "body": "发行包请走 scripts/package.ps1，不要手动 windeployqt——自检会拦住缺插件/缺运行库的包。"},
         h("claude"))
    http(base, "POST", "/api/messages",
         {"kind": "task", "recipient": "codex", "subject": "补齐 usage 面板空状态文案",
          "body": "预算未设置时给一句说明，避免新用户以为用量没在统计。"}, h("claude"))

    _, e1 = http(base, "POST", "/api/errors",
                 {"severity": "warning", "source": "miderforge",
                  "title": "技能调用超时：编译缓存重建",
                  "detail": "ccache 冷缓存重建超过 120s，已改为后台任务。"}, h("codex"))
    _, e2 = http(base, "POST", "/api/errors",
                 {"severity": "critical", "source": "mcp",
                  "title": "agents.json 半截写入导致注册失败",
                  "detail": "临时文件替换前崩溃；已改为写临时文件后原子替换。"}, h("cursor"))
    http(base, "POST", f"/api/errors/{e2['uuid']}/resolve",
         {"notes": "persistAgentKey 改为临时文件 + 原子替换，附带损坏文件拒绝解析的显式报错。"}, h("cursor"))

    calls = [("claude", 12400, 3450, "deepseek-chat"), ("claude", 8200, 2100, "deepseek-chat"),
             ("codex", 15200, 5600, "gpt-5.3-codex"), ("codex", 6400, 1900, "gpt-5.3-codex"),
             ("cursor", 4300, 1200, "composer-1")]
    for i, (name, ti, to, model) in enumerate(calls):
        http(base, "POST", "/api/usage/report",
             {"tokens_in": ti, "tokens_out": to, "model": model,
              "call_type": "chat", "idempotency_key": f"seed-{i}"}, h(name))
    log("演示数据就绪（3 Agent / 3 知识 / 2 技能 / 2 错误 / 用量 5 笔）")


def terminate(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default=os.path.join(ROOT, "_stage"),
                    help="暂存目录（含 miderhive.exe，Qt 同层）")
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--outdir", default=os.path.join(ROOT, "docs", "assets"))
    args = ap.parse_args()

    exe = os.path.join(args.stage, "miderhive.exe")
    if not os.path.isfile(exe):
        print(f"FAIL: 未找到 {exe} —— 先 cmake --install 出暂存目录，或用 --stage 指向含 Qt DLL 的目录")
        return 1

    dpi_aware()
    os.makedirs(args.outdir, exist_ok=True)
    out = lambda name: os.path.join(args.outdir, name)  # noqa: E731
    base = f"http://127.0.0.1:{args.port}"
    env = os.environ.copy()
    env.update({"MIDERHIVE_HOME": DEMO_HOME, "MIDERHIVE_PORT": str(args.port)})

    shutil.rmtree(DEMO_HOME, ignore_errors=True)
    os.makedirs(DEMO_HOME, exist_ok=True)

    gui = None
    try:
        with SettingsGuard() as guard:
            # ---- ① 总览页 ----
            log("启动工作台（总览页）……")
            gui = subprocess.Popen([exe], env=env)
            wait_health(base, timeout=30)
            seed(base, DEMO_HOME)
            hwnd, _title = find_window_ctypes(WINDOW_TITLE_PREFIX)
            raise_window(hwnd)
            time.sleep(4)  # 3s 轮询：Agent 卡片翻绿、事件流出现
            capture(hwnd, out("screenshot-dashboard.png"))

            # ---- ② 防呆：明文密钥丢失 → 健康横幅 ----
            log("制造密钥丢失现场（移除 cursor 明文条目）……")
            aj = os.path.join(DEMO_HOME, "config", "agents.json")
            cache = json.load(open(aj, encoding="utf-8"))
            cache.pop("cursor", None)
            json.dump(cache, open(aj, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
            time.sleep(5)  # 3s 轮询 + 重建
            # 服务端口径断言：诊断必须已报出 cursor 密钥缺失（渲染逻辑由 gui_selftest 覆盖）
            mk = open(os.path.join(DEMO_HOME, "config", "master.key"), encoding="utf-8").read().strip()
            _, diag = http(base, "GET", "/api/diagnostics", None, {"X-Master-Key": mk})
            if "cursor" not in diag["keyfile_missing"]:
                raise RuntimeError(f"诊断未报出 cursor 密钥缺失: {diag}")
            capture(hwnd, out("screenshot-health.png"))
            terminate(gui)

            # ---- ③ 首次接入引导：等弹窗自身的 HWND，直接截弹窗 ----
            log("重启并恢复首启状态（弹出接入引导）……")
            time.sleep(1)
            guard.show_welcome_again()
            gui = subprocess.Popen([exe], env=env)
            hwnd_dlg, dlg_title = find_window_ctypes("欢迎来到 MiderHive", timeout=20)
            log(f"引导弹窗已出现: {dlg_title}")
            raise_window(hwnd_dlg)
            time.sleep(0.8)
            capture(hwnd_dlg, out("screenshot-onboarding.png"))
            terminate(gui)
            gui = None
        print("DONE: 3 张截图已生成")
        return 0
    finally:
        terminate(gui)
        shutil.rmtree(DEMO_HOME, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
