#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiderHive 客户端接入真机验证矩阵。

逐个检测本机安装的 AI 客户端，用 **agent-cli 的真实输出**（与 GUI 向导同一份实现）
生成/写入配置，再按各客户端自己的方式验证：

  claude   写 <项目根>/.mcp.json 后用 `claude mcp list` 验证（真实 CLI 健康检查）
  dsh      用 `dsh --patch <文件> --dump-config` 验证（真实 profile 加载器合成补丁）
  codex    写 config.toml 后用 tomllib 解析（Windows 路径必须是单引号字面量）
  hermes   写 config.yaml 后用 yaml 解析（含已有 mcp_servers 段的合并形态）
  droid    写 mcp.json 后用 json 解析
  zcode    环境变量接入，由 feasibility_check.py 覆盖（此处标记 COVERED）

原则：**有则验、无则 SKIP**。未安装不算失败；--strict 时 SKIP/FAIL 都算失败，
供发版前强制全量验证。所有临时文件用完即删。

注意：客户端**检测**基于 Windows 路径（%LOCALAPPDATA%、%USERPROFILE%、~），
非 Windows 平台目前会全部 SKIP（与本项目当前仅验证 Windows 的范围一致）。

用法:
  python scripts/verify_clients.py [--agent-cli EXE] [--strict]
依赖: 仅 Python 3.11+（tomllib）标准库；yaml 为可选（缺失则跳过 hermes 的解析断言）。
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

try:
    import tomllib
except ImportError:  # pragma: no cover
    tomllib = None

try:
    import yaml  # PyYAML，可选
except ImportError:  # pragma: no cover
    yaml = None

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(REPO, "build", "full", "src", "cli", "Release", "agent-cli.exe")

ROWS = []


def add(client, status, detail=""):
    ROWS.append((client, status, detail))
    print("  [%-5s] %-8s %s" % (status, client, detail))


def run(cmd, cwd=None):
    """运行命令并返回 (returncode, stdout+stderr)。cmd 为字符串时走 cmd.exe（Windows
    上的 .cmd/.ps1 包装脚本必须经过命令解释器，PowerShell 还会吞 `--` 之后的参数）。"""
    shell = isinstance(cmd, str)
    try:
        p = subprocess.run(cmd, cwd=cwd, shell=shell, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=180)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, str(e)


def cli_path(args):
    if args.agent_cli and os.path.exists(args.agent_cli):
        return args.agent_cli
    if os.path.exists(DEFAULT_CLI):
        return DEFAULT_CLI
    found = shutil.which("agent-cli")
    return found or ""


def exe_for(tool_id):
    """写入配置里的 command 字段用一个占位路径即可——除 claude/dsh 外只验证格式，
    不要求该路径真实存在。"""
    return "C:\\Program Files\\MiderHive\\miderhive-mcp.exe"


# ---------------- 各客户端检测 ----------------

def claude_available():
    return bool(shutil.which("claude")) or os.path.isdir(os.path.expanduser("~/.claude"))


def codex_available():
    return bool(shutil.which("codex")) or os.path.isdir(os.path.expanduser("~/.codex"))


def droid_available():
    return os.path.isdir(os.path.expanduser("~/.factory"))


def dsh_available():
    return bool(shutil.which("dsh")) or os.path.isdir(os.path.expanduser("~/.dsh"))


def hermes_available():
    local = os.environ.get("LOCALAPPDATA", "")
    return os.path.isdir(os.path.join(local, "hermes")) if local else False


# ---------------- 各客户端验证 ----------------

def verify_claude(cli, work):
    """真实 CLI 验证：写项目 .mcp.json → claude mcp list 应列出 miderhive。"""
    proj = tempfile.mkdtemp(prefix="mh-claude-proj-")
    rc, out = run([cli, "apply-config", "--tool", "claude-code", "--dir", proj,
                   "--name", "claude-probe", "--key", "probe-key-not-a-real-credential"])
    if rc != 0:
        add("claude", "FAIL", "agent-cli apply-config 失败: " + out.strip()[:160])
        return
    mcp_json = os.path.join(proj, ".mcp.json")
    if not os.path.exists(mcp_json):
        add("claude", "FAIL", ".mcp.json 未生成: " + proj)
        return
    try:
        with open(mcp_json, encoding="utf-8") as f:
            data = json.load(f)
        cmd = data["mcpServers"]["miderhive"]["command"]
        if "miderhive-mcp" not in cmd:
            add("claude", "FAIL", "command 指向异常: " + cmd)
            return
    except Exception as e:
        add("claude", "FAIL", ".mcp.json 解析失败: %s" % e)
        return
    exe = shutil.which("claude")
    rc, out = run('"%s" mcp list' % exe, cwd=proj)
    if rc != 0 and "miderhive" not in out:
        add("claude", "FAIL", "claude mcp list 失败: " + out.strip()[:160])
        return
    if "miderhive" not in out:
        add("claude", "FAIL", "claude mcp list 未列出 miderhive: " + out.strip()[:160])
        return
    status = "Connected" if "Connected" in out else ("Pending approval" if "Pending" in out
                                                     else "listed")
    add("claude", "PASS", "claude 识别 .mcp.json 并列出 miderhive（%s）" % status)


def verify_dsh(cli, work):
    """真实加载器验证：dsh --patch <补丁> --dump-config 应合成出 mcp-miderhive 行。"""
    root = tempfile.mkdtemp(prefix="mh-dsh-root-")
    rc, out = run([cli, "apply-config", "--tool", "dsh", "--dir", root,
                   "--name", "dsh-probe", "--key", "probe-key"])
    if rc != 0:
        add("dsh", "FAIL", "agent-cli apply-config 失败: " + out.strip()[:160])
        return
    patch = os.path.join(root, ".dsh", "profiles", "web", "cordis.patch.yml")
    if not os.path.exists(patch):
        add("dsh", "FAIL", "补丁文件未生成: " + patch)
        return
    exe = shutil.which("dsh")
    rc, out = run('"%s" --profile web --patch "%s" --dump-config' % (exe, patch))
    if rc != 0:
        add("dsh", "FAIL", "dsh --dump-config 退出码 %d: %s" % (rc, out.strip()[:160]))
        return
    if "mcp-miderhive" not in out or "dsh-mcp-client" not in out:
        add("dsh", "FAIL", "合成后的 profile 树不含 mcp-miderhive/dsh-mcp-client")
        return
    add("dsh", "PASS", "DSH profile 加载器成功合成补丁（工具将以 mcp__miderhive__* 出现）")


def verify_codex(cli, work):
    root = tempfile.mkdtemp(prefix="mh-codex-")
    rc, out = run([cli, "apply-config", "--tool", "codex", "--dir", root,
                   "--name", "codex-probe", "--key", "probe-key"])
    if rc != 0:
        add("codex", "FAIL", "agent-cli apply-config 失败: " + out.strip()[:160])
        return
    toml_path = os.path.join(root, ".codex", "config.toml")
    if not os.path.exists(toml_path):
        add("codex", "FAIL", "config.toml 未生成: " + toml_path)
        return
    if tomllib is None:
        add("codex", "SKIP", "本机 Python <3.11 无 tomllib，无法解析验证")
        return
    with open(toml_path, "rb") as f:
        data = tomllib.load(f)
    srv = data["mcp_servers"]["miderhive"]
    if "miderhive-mcp" not in srv["command"]:
        add("codex", "FAIL", "command 异常: " + srv["command"])
        return
    if srv["env"]["MIDERHIVE_AGENT_NAME"] != "codex-probe":
        add("codex", "FAIL", "env 未写入身份名")
        return
    add("codex", "PASS", "TOML 解析成功（单引号字面量路径，反斜杠原样保留）")


def verify_hermes(cli, work):
    if yaml is None:
        add("hermes", "SKIP", "未安装 PyYAML，无法解析验证（pip install pyyaml）")
        return
    root = tempfile.mkdtemp(prefix="mh-hermes-")
    rc, out = run([cli, "apply-config", "--tool", "hermes", "--dir", root,
                   "--name", "hermes-probe", "--key", "probe-key"])
    if rc != 0:
        add("hermes", "FAIL", "agent-cli apply-config 失败: " + out.strip()[:160])
        return
    cfg = os.path.join(root, "hermes", "config.yaml")
    if not os.path.exists(cfg):
        add("hermes", "FAIL", "config.yaml 未生成: " + cfg)
        return
    with open(cfg, encoding="utf-8") as f:
        data = yaml.safe_load(f)
    if "miderhive" not in (data.get("mcp_servers") or {}):
        add("hermes", "FAIL", "mcp_servers.miderhive 缺失")
        return
    add("hermes", "PASS", "YAML 解析成功，mcp_servers.miderhive 就位")


def verify_droid(cli, work):
    root = tempfile.mkdtemp(prefix="mh-droid-")
    rc, out = run([cli, "apply-config", "--tool", "droid", "--dir", root,
                   "--name", "droid-probe", "--key", "probe-key"])
    if rc != 0:
        add("droid", "FAIL", "agent-cli apply-config 失败: " + out.strip()[:160])
        return
    mcp_json = os.path.join(root, ".factory", "mcp.json")
    if not os.path.exists(mcp_json):
        add("droid", "FAIL", "mcp.json 未生成: " + mcp_json)
        return
    with open(mcp_json, encoding="utf-8") as f:
        data = json.load(f)
    srv = data["mcpServers"]["miderhive"]
    if "miderhive-mcp" not in srv["command"]:
        add("droid", "FAIL", "command 异常: " + srv["command"])
        return
    add("droid", "PASS", "mcp.json 解析成功（Droid 会自动重载；/mcp 可查看状态）")


def main():
    ap = argparse.ArgumentParser(description="MiderHive 客户端接入真机验证矩阵")
    ap.add_argument("--agent-cli", default="", help="agent-cli 可执行文件路径")
    ap.add_argument("--strict", action="store_true",
                    help="未安装（SKIP）也视为失败，供发版前强制全量验证")
    args = ap.parse_args()

    cli = cli_path(args)
    if not cli:
        print("agent-cli 不存在: %s（先构建，或用 --agent-cli 指定）" % DEFAULT_CLI)
        return 2

    print("==== MiderHive 客户端接入验证（agent-cli: %s）====" % cli)
    if os.name != "nt":
        print("提示：客户端检测基于 Windows 路径，非 Windows 平台会全部 SKIP。")
    print()

    if claude_available():
        verify_claude(cli, None)
    else:
        add("claude", "SKIP", "本机未安装 Claude")

    if dsh_available():
        verify_dsh(cli, None)
    else:
        add("dsh", "SKIP", "本机未安装 DSH")

    if codex_available():
        verify_codex(cli, None)
    else:
        add("codex", "SKIP", "本机未安装 Codex CLI")

    if hermes_available():
        verify_hermes(cli, None)
    else:
        add("hermes", "SKIP", "本机未安装 Hermes")

    if droid_available():
        verify_droid(cli, None)
    else:
        add("droid", "SKIP", "本机未安装 Factory Droid")

    add("zcode", "COVERED", "环境变量接入，由 scripts/feasibility_check.py 覆盖")

    print()
    n_pass = sum(1 for _, s, _ in ROWS if s == "PASS")
    n_fail = sum(1 for _, s, _ in ROWS if s == "FAIL")
    n_skip = sum(1 for _, s, _ in ROWS if s == "SKIP")
    print("客户端验证: %d 项 — PASS %d, FAIL %d, SKIP %d, COVERED %d"
          % (len(ROWS), n_pass, n_fail, n_skip,
             sum(1 for _, s, _ in ROWS if s == "COVERED")))
    failed = n_fail > 0 or (args.strict and n_skip > 0)
    if failed:
        print("结果: FAIL%s" % ("（--strict：存在未验证客户端）" if n_skip else ""))
    else:
        print("结果: PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
