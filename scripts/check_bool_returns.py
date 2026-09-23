#!/usr/bin/env python3
"""bool 返回值纪律检查：关键调用返回的 bool 必须被使用。

背景（为什么需要这条规则）：
    "不静默失败"是本项目的明文承诺，但它**没有被任何检查钉住**。结果是：
    同一个 `audit_.log(...)`，在 skillInvoke 里是硬契约（失败即回滚），
    在 knowledgeCreate 等 20 多处却是装饰（返回值直接丢弃）。同类问题还有
    `maintenanceRun` 里三条 DELETE 的返回值被丢弃后拿 `sqlite3_changes64()`
    报统计——"删了 0 条"和"删除失败"因此完全同形。
    这些都不是能力问题，是缺一条机检规则。

规则（唯一一条）：
    清单内调用的返回值必须被使用；确实要忽略时，语句行必须带 `// IGNORE:` 和理由。

为什么用"行首模式"匹配而不是解析 C++：
    本仓库的风格是同一条语句写在一行（赋值、`if`、独立调用都好认），代价是
    跨行链式调用可能漏报。脚本刻意选择**宁可漏报也不要误报**——一条会天天误报的
    检查会在两天内被 continue-on-error 关掉，那就等于没有。

用法：
    python scripts/check_bool_returns.py            # 报告模式，恒返回 0
    python scripts/check_bool_returns.py --fail     # 有命中即返回 1（CI 用）
    python scripts/check_bool_returns.py --list     # 额外打印清单本身（自查用）
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# 需要检查的关键调用（写路径 + 有副作用且会失败的服务调用）。
# 只放"漏检会真的造成数据/留痕缺口"的调用，不放纯读取助手。
KEY_CALL = r"(?:db_|Database::)\.(?:query|execScript)\(" r"|(?:audit_|knowledge_|skills_|memory_|messages_|errors_|usage_|agents_)\.(?:log|create|addVersion|set|remove|send|reply|report|register|record[A-Za-z]*|rotate|update)\("

# 语句以关键词开头 = 返回值这一层已经不存在，无需检查
STMT_KEYWORDS = (
    "if", "while", "for", "switch", "else", "return", "assert",
    "case", "do", "catch", "throw", "namespace", "using",
)

SCAN_DIRS = ("src",)
SCAN_EXT = {".cpp", ".h", ".hpp", ".cc"}
SKIP_DIR_MARKERS = ("/vendor/", "/.mimic/", "/build/", "/_stage/", "/.git/")

# 允许的忽略标记：必须带理由，光写标记不算
IGNORE_RE = re.compile(r"//\s*IGNORE\s*:(?P<reason>.*?)\s*$")

# 三引号内的 SQL 片段不是可执行语句，误报来源之一
STRING_CONTENT_HINT = re.compile(r"^\s*(?:\"|R\")")


def looks_like_string_content(code: str) -> bool:
    """判断该行是否主要是一条字符串字面量的续行（SQL 正文等）。"""
    return bool(STRING_CONTENT_HINT.match(code))


def line_is_assigned(code: str, call_start: int) -> bool:
    """判断返回值是否被"赋值/包装"消费掉了。

    只在两种情况下算消费：
      * 行首是 `AUDIT_OR_FAIL(...)` 之类的策略宏——返回值进了宏的 if；
      * 调用点之前出现 `=`（排除字符串内的等号，例如 SQL 里的 `key='x'`）。
    注意 `==`/`!=`/`<=`/`>=` 不是赋值，先抹平再找。
    """
    if code.startswith("AUDIT_OR_FAIL("):
        return True
    head = code[:call_start]
    head = head.replace("!=", " ").replace("==", " ").replace("<=", " ").replace(">=", " ")
    if "=" not in head:
        return False
    # 等号出现在第一个字符串字面量之后 = SQL 文本里的等号，不是赋值
    quote = min((head.find(q) for q in ('"', "'") if head.find(q) != -1), default=-1)
    eq = head.find("=")
    return not (quote != -1 and quote < eq)


def matches_key_call(code: str) -> bool:
    return re.search(KEY_CALL, code) is not None


def strip_leading(code: str) -> str:
    return code.lstrip()


def starts_with_keyword(code: str) -> bool:
    stripped = strip_leading(code)
    for kw in STMT_KEYWORDS:
        if stripped.startswith(kw + " ") or stripped.startswith(kw + "("):
            return True
    return False


def scan_file(path: Path) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    """返回 (违规列表, 带理由的豁免列表)，元素为 (行号, 原文)。"""
    violations: list[tuple[int, str]] = []
    ignores: list[tuple[int, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return violations, ignores

    lines = text.splitlines()
    for lineno, raw in enumerate(lines, start=1):
        code = raw.strip()
        if not code or code.startswith("//") or code.startswith("*"):
            continue
        call = re.search(KEY_CALL, code)
        if not call:
            continue

        # 1) 带理由的显式豁免优先（豁免也必须能被审计）。
        #    支持两种写法：行尾 `// IGNORE: 理由`，或紧邻上方若干行注释里的
        #    `// IGNORE: 理由`（多行调用时行尾放不下，且理由通常需要一整段）。
        m = IGNORE_RE.search(raw)
        reason = m.group("reason").strip() if m else ""
        if not reason:
            # 向上找最近的 `// IGNORE: 理由`。允许跨过空行与代码行（调用常写在
            # `if (...) {` 块里，而理由需要独立一段放在块首），但有距离上限，
            # 避免"文件开头写一次 IGNORE 就豁免全文件"。
            j = lineno - 2
            for _ in range(12):
                if j < 0:
                    break
                prev = lines[j]
                pm = IGNORE_RE.search(prev)
                if pm and pm.group("reason").strip():
                    reason = pm.group("reason").strip()
                    break
                j -= 1
        if reason:
            ignores.append((lineno, code))
            continue
        if m:
            # 写了标记但没写理由：豁免本身也要能被审计
            violations.append((lineno, code + "   <-- // IGNORE: 缺少理由"))
            continue

        # 2) 显式 (void) 丢弃：明确表达"我知道返回值、我选择不用"
        if "(void)" in code:
            continue
        # 3) 返回值被赋值 / 进了策略宏
        if starts_with_keyword(code):
            continue
        if line_is_assigned(code, call.start()):
            continue
        # 4) 字符串字面量续行（SQL 正文）
        if looks_like_string_content(code):
            continue

        violations.append((lineno, code))

    return violations, ignores


def collect_targets() -> list[Path]:
    targets: list[Path] = []
    for d in SCAN_DIRS:
        base = REPO_ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix not in SCAN_EXT:
                continue
            posix = p.as_posix()
            if any(marker in posix for marker in SKIP_DIR_MARKERS):
                continue
            targets.append(p)
    return targets


def main() -> int:
    ap = argparse.ArgumentParser(description="bool 返回值纪律检查")
    ap.add_argument("--fail", action="store_true", help="有命中时返回退出码 1（CI 用）")
    ap.add_argument("--list", action="store_true", help="额外打印受检清单")
    args = ap.parse_args()

    if args.list:
        print("受检的关键调用：")
        print("  " + KEY_CALL)
        print()

    targets = collect_targets()
    total_violations = 0
    total_ignores = 0

    for path in targets:
        violations, ignores = scan_file(path)
        total_ignores += len(ignores)
        if not violations:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        for lineno, code in violations:
            print(f"{rel}:{lineno}: {code}")
            total_violations += 1

    print()
    print(f"scanned {len(targets)} files under {', '.join(SCAN_DIRS)}/")
    print(f"ignored with reason: {total_ignores}")
    print(f"violations: {total_violations}")

    if total_violations:
        print()
        print("修法：接住返回值，或写成 `(void)调用(...)`，或在该行加 `// IGNORE: <理由>`。")
        print("理由必填——豁免本身也要能被审计，否则这条规则会被逐条绕空。")

    return 1 if (args.fail and total_violations) else 0


if __name__ == "__main__":
    sys.exit(main())
