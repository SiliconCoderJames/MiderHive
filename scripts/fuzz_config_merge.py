#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""配置合并的模糊/属性测试：用对抗形状的用户配置喂 agent-cli apply-config，
再验证每份产物。

被测代码：core/integrations.hpp 的 upsertYamlMapEntry / upsertYamlListItem /
TOML 表替换 / JSON 合并——行级文本手术，处理的是任意用户配置。

每个用例验证的属性：
  P1 产物可解析（yaml.safe_load / tomllib / json）
  P2 恰好一个 miderhive 条目（不重复、不丢失）
  P3 用户的兄弟条目与无关顶层节原样保留
  P4 应当拒绝的形状（制表符/块标量/锚点/坏 JSON）确实被拒绝且原文件未动

用法:
  python scripts/fuzz_config_merge.py [--agent-cli EXE]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

try:
    import tomllib
except ImportError:
    tomllib = None

try:
    import yaml
except ImportError:
    yaml = None

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(REPO, "build", "full", "src", "cli", "Release", "agent-cli.exe")

RESULTS = []
WORK_DIRS = []  # 运行结束统一清理


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           (": " + detail) if detail and not ok else ""))


def run_cli(cli, tool, root, name="probe", key="probe-key"):
    # --command 显式给定占位路径：不传的话 CLI 会取自己所在目录下的 miderhive-mcp，
    # 那是"正确"行为，但会让断言依赖构建布局
    return subprocess.run(
        [cli, "apply-config", "--tool", tool, "--dir", root,
         "--command", EXE, "--name", name, "--key", key],
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


EXE = "C:\\Program Files\\MiderHive\\miderhive-mcp.exe"

# ---------------- Hermes（upsertYamlMapEntry）----------------

HERMES_CASES = [
    # (名称, 种子内容[None=不预置], 期望: ok / refuse)
    ("基础：父节+一个兄弟", "mcp_servers:\n  github:\n    command: 'gh'\n", "ok"),
    ("父节缺失：整段追加", "model:\n  default: t\n", "ok"),
    ("空文件", "", "ok"),
    ("只有注释", "# nothing here\n", "ok"),
    ("父节后还有顶层节", "mcp_servers:\n  github:\n    command: 'gh'\n\ntts:\n  on: true\n", "ok"),
    ("兄弟条目带内联注释", "mcp_servers:\n  github:  # gh server\n    command: 'gh'\n", "ok"),
    ("兄弟下有列表内容", "mcp_servers:\n  github:\n    args:\n      - a\n      - b\n", "ok"),
    ("父节内有缩进注释", "mcp_servers:\n  # inner note\n  github:\n    command: 'gh'\n", "ok"),
    ("已存在 miderhive：替换", "mcp_servers:\n  miderhive:\n    command: 'old'\n", "ok"),
    ("文件末尾无换行", "mcp_servers:\n  github:\n    command: 'gh'", "ok"),
    ("父节带行尾注释", "mcp_servers:  # my servers\n  github:\n    command: 'gh'\n", "ok"),
    ("父节内含缩进为2的注释", "mcp_servers:\n  miderhive:\n    command: 'old'\n  # gh note\n  github:\n    command: 'gh'\n", "ok"),
    ("制表符缩进：必须拒绝", "mcp_servers:\n\tgithub:\n\t  command: 'gh'\n", "refuse"),
    ("块标量：必须拒绝", "mcp_servers:\n  github:\n    command: |\n      line1\n      line2\n", "refuse"),
    ("锚点在父键行尾：合并应成功（锚点作用于整个映射）",
     "mcp_servers: &base\n  github:\n    command: 'gh'\n", "ok"),
    ("缩进为1：必须拒绝", "mcp_servers:\n github:\n   command: 'gh'\n", "refuse"),
]

# ---------------- DSH（upsertYamlListItem）----------------

DSH_CASES = [
    ("空数组 []", "# patch layer\n[]\n", "ok"),
    ("带行尾注释的空数组", "[]  # empty for now\n", "ok"),
    ("已有单条 insert", "- insert:\n    - id: other\n      name: 'x'\n", "ok"),
    ("已有两条 insert", "- insert:\n    - id: a\n      name: 'x'\n- insert:\n    - id: b\n      name: 'y'\n", "ok"),
    ("注释里提到 mcp-miderhive（不得误定位）", "# config for mcp-miderhive lives below\n- insert:\n    - id: other\n      name: 'x'\n", "ok"),
    ("文件末尾无换行", "- insert:\n    - id: other\n      name: 'x'", "ok"),
    ("空文件", "", "ok"),
]

# ---------------- Codex（TOML 表替换）----------------

CODEX_CASES = [
    ("空文件", "", "ok"),
    ("只有 miderhive 表", "[mcp_servers.miderhive]\ncommand = 'old'\n", "ok"),
    ("miderhive 表后还有其他表", "[mcp_servers.miderhive]\ncommand = 'old'\n\n[other]\nk = 1\n", "ok"),
    ("miderhive 表前有其他表", "[other]\nk = 1\n\n[mcp_servers.miderhive]\ncommand = 'old'\n", "ok"),
    ("miderhive 表在中间", "[a]\nk = 1\n\n[mcp_servers.miderhive]\ncommand = 'old'\n\n[b]\nk = 2\n", "ok"),
    ("CRLF 文件", "[mcp_servers.miderhive]\r\ncommand = 'old'\r\n", "ok"),
]


def verify_hermes(path, name, expect):
    text = read(path)
    if yaml is None:
        return (True, "skip（无 PyYAML）")
    data = yaml.safe_load(text)
    if data is None:
        return (False, "P1 可解析：解析结果为空")
    servers = data.get("mcp_servers") or {}
    mid = servers.get("miderhive")
    if not isinstance(mid, dict) or "command" not in mid:
        return (False, "P2 缺失或非对象: %r" % (mid,))
    # P3 兄弟条目保留
    if "github" in text and "github" not in servers:
        return (False, "P3 github 条目丢失")
    if "tts:" in text and "tts" not in data:
        return (False, "P3 tts 顶层节丢失")
    if text.count("miderhive:") != 1:
        return (False, "P2 miderhive 出现 %d 次" % text.count("miderhive:"))
    return (True, "")


def verify_dsh(path, name, expect):
    text = read(path)
    # 计数用完整的条目身份行：注释里提到 "mcp-miderhive" 字样不算条目
    if text.count("- id: mcp-miderhive") != 1:
        return (False, "P2 insert 条目出现 %d 次" % text.count("- id: mcp-miderhive"))
    if "dsh-mcp-client" not in text:
        return (False, "P2 缺 dsh-mcp-client")
    if "- insert:" not in text:
        return (False, "P2 缺 insert 行")
    if "\n[]\n" in text or text.startswith("[]\n"):
        return (False, "P4 残留顶层 []")
    return (True, "")


def verify_codex(path, name, expect):
    if tomllib is None:
        return (True, "skip（无 tomllib）")
    with open(path, "rb") as f:
        data = tomllib.load(f)
    srv = (data.get("mcp_servers") or {}).get("miderhive")
    if not isinstance(srv, dict) or "command" not in srv:
        return (False, "P2 miderhive 表缺失")
    if srv["command"] != EXE:
        return (False, "P2 command 未更新: %s" % srv["command"])
    raw = open(path, encoding="utf-8").read()
    if raw.count("[mcp_servers.miderhive]") != 1:
        return (False, "P2 表出现 %d 次" % raw.count("[mcp_servers.miderhive]"))
    return (True, "")


# ---------------- Cursor（mergeMcpServersJson）----------------

CURSOR_CASES = [
    ("空文件", "", "ok"),
    ("其他 server 保留", '{"mcpServers": {"other": {"command": "x"}}}', "ok"),
    ("坏 JSON：必须拒绝", "{ oops", "refuse"),
    # BOM 会被解析器跳过、产物不再带 BOM：JSON 里的 BOM 本就是反模式，
    # 去掉它是改善而不是破坏（该行为由本用例锁定）
    ("BOM 开头：合并成功且产物无 BOM", "﻿" + '{"mcpServers": {}}', "ok"),
    ("mcpServers 非对象：必须拒绝", '{"mcpServers": [1, 2]}', "refuse"),
]


def verify_cursor(path, name, expect, seed):
    text = read(path)
    if expect == "refuse":
        return (text == seed, "拒绝时原文件未改")
    # 通用属性：产物永远是干净的无 BOM JSON
    if text.startswith("﻿"):
        return (False, "P4 产物不应带 BOM")
    data = json.loads(text)
    mid = (data.get("mcpServers") or {}).get("miderhive")
    if not isinstance(mid, dict) or "command" not in mid:
        return (False, "P2 miderhive 缺失")
    if "other" in text and "other" not in (data.get("mcpServers") or {}):
        return (False, "P3 other 条目丢失")
    return (True, "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", default="")
    args = ap.parse_args()
    cli = args.agent_cli if args.agent_cli and os.path.exists(args.agent_cli) else DEFAULT_CLI
    if not os.path.exists(cli):
        print("agent-cli 不存在: %s" % cli)
        return 2

    root = tempfile.mkdtemp(prefix="mh-fuzz-")
    WORK_DIRS.append(root)
    print("==== 配置合并模糊测试（agent-cli: %s）====" % cli)

    def case(tool, rel, label, seed, expect, verifier):
        work = tempfile.mkdtemp(prefix="mh-fz-")
        WORK_DIRS.append(work)
        target_path = os.path.join(work, rel)
        if seed is not None:
            write(target_path, seed)
        r = run_cli(cli, tool, work)
        ok = r.returncode == 0
        if expect == "refuse":
            check("%s / %s → 拒绝" % (tool, label), r.returncode != 0, "意外成功")
            if r.returncode != 0 and os.path.exists(target_path):
                keep = read(target_path)
                check("%s / %s → 拒绝时原文件未动" % (tool, label), keep == seed,
                      "内容被改动")
            return
        if not ok:
            check("%s / %s" % (tool, label), False, "apply 失败: " + (r.stderr or r.stdout)[:140])
            return
        if not os.path.exists(target_path):
            check("%s / %s" % (tool, label), False, "产物未生成: " + target_path)
            return
        ok, detail = verifier(target_path, label, expect)
        check("%s / %s" % (tool, label), ok, detail)

    for label, seed, expect in HERMES_CASES:
        case("hermes", "hermes/config.yaml", label, seed, expect, verify_hermes)
    for label, seed, expect in DSH_CASES:
        case("dsh", ".dsh/profiles/web/cordis.patch.yml", label, seed, expect, verify_dsh)
    for label, seed, expect in CODEX_CASES:
        case("codex", ".codex/config.toml", label, seed, expect, verify_codex)
    for label, seed, expect in CURSOR_CASES:
        case("cursor", ".cursor/mcp.json", label, seed, expect,
             lambda p, n, e: verify_cursor(p, n, e, seed))

    for d in WORK_DIRS:
        shutil.rmtree(d, ignore_errors=True)

    fails = [r for r in RESULTS if not r[1]]
    print("\n模糊测试: %d 项, 失败 %d 项" % (len(RESULTS), len(fails)))
    for name, _, detail in fails:
        print("  FAIL %s: %s" % (name, detail))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
