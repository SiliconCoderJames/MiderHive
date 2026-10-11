#!/usr/bin/env python3
"""i18n 键提取与覆盖度检查。

i18n 机制（src/gui/i18n.h）：全部界面文案写作 trs(简体源文, English)，简体源文
同时是额外语言（繁體/日本語…）覆盖表的**键**——键必须与运行时字符串逐字节一致。

本脚本从 src/gui 源码提取全部会作为 trs()/tr() 第一参或其等价物（工具注册表的
nameZh/installZh/noteZh、面板静态双语表、humanError 的中文模式）的字符串字面量：
  * 先做 C++ 状态机去注释（// 与 //**，感知字符串/字符字面量，避免把注释里的
    中文捞进键表）；
  * 再收集**含 CJK 字符**的字面量，处理 C++ 转义（\\n \\t \\" \\\\ \\'），
    键 = 运行时字节（转义解码后）；
  * trs( 调用点的相邻字符串拼接（"a" "b" 写法）按拼接结果成键。

用法:
  python scripts/i18n_extract.py                # 打印统计并写 .tmp-i18n-keys.json
  python scripts/i18n_extract.py --by-file      # 额外按源文件分组输出（翻译 fan-out 输入）
  python scripts/i18n_extract.py --coverage     # 对照两张 pack 表查未覆盖键（会回退英文的）
"""

import argparse
import glob
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "gui")
PACKS = {
    "zh-Hant": os.path.join(SRC, "i18n_pack_zh_hant.h"),
    "ja": os.path.join(SRC, "i18n_pack_ja.h"),
}


def strip_comments(src: str) -> str:
    """C++ 状态机去注释：感知 "..." 与 '...' 字面量（含转义），保留字符串原样。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i : i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            if j == -1:
                break
            i = j  # 保留换行
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j == -1 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def unescape(literal_body: str) -> str:
    """C++ 转义 → 运行时字节（键口径）。"""
    out = []
    i, n = 0, len(literal_body)
    while i < n:
        c = literal_body[i]
        if c == "\\" and i + 1 < n:
            nxt = literal_body[i + 1]
            simple = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "?": "?"}
            if nxt in simple:
                out.append(simple[nxt])
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def has_cjk(s: str) -> bool:
    return any(0x3400 <= ord(ch) <= 0x9FFF or 0xF900 <= ord(ch) <= 0xFAFF for ch in s)


def extract() -> "tuple[set, dict]":
    keys = set()
    by_file = {}
    trs_pat = re.compile(r'i18n::trs?\(\s*((?:"(?:[^"\\]|\\.)*"\s*\n?\s*)+)')
    lit_pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for path in sorted(glob.glob(os.path.join(SRC, "**", "*"), recursive=True)):
        if not path.endswith((".h", ".cpp")) or "i18n_pack" in path:
            continue
        src = strip_comments(io.open(path, encoding="utf-8").read())
        found = set()
        # 1) trs/tr 调用点（相邻字面量拼接成一个键）
        for m in trs_pat.finditer(src):
            parts = [unescape(x) for x in lit_pat.findall(m.group(1))]
            key = "".join(parts)
            if has_cjk(key):
                found.add(key)
        # 2) 其余含 CJK 的字面量（注册表字段 / 静态双语表 / humanError 中文模式）
        for m in lit_pat.finditer(src):
            key = unescape(m.group(1))
            if has_cjk(key):
                found.add(key)
        if found:
            keys |= found
            by_file[os.path.relpath(path, ROOT).replace("\\", "/")] = sorted(found)
    return keys, by_file


def load_pack(path: str) -> dict:
    """从 pack 头文件里解析 (键, 译文) 对（仅覆盖度检查用，够用的正则口径）。"""
    s = io.open(path, encoding="utf-8").read()
    pairs = {}
    for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', s):
        pairs[unescape(m.group(1))] = unescape(m.group(2))
    return pairs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--by-file", action="store_true", help="按源文件分组输出（翻译输入）")
    ap.add_argument("--coverage", action="store_true", help="对照 pack 表查未覆盖键")
    ap.add_argument("--out", default=".tmp-i18n-keys.json", help="输出文件")
    args = ap.parse_args()

    keys, by_file = extract()
    if args.coverage:
        bad = 0
        for lang, pack_path in PACKS.items():
            table = load_pack(pack_path)
            missing = sorted(k for k in keys if k not in table)
            print(f"{lang}: 覆盖 {len(keys) - len(missing)}/{len(keys)}，缺失 {len(missing)}")
            for k in missing[:40]:
                print("   MISSING:", k[:72])
            if len(missing) > 40:
                print(f"   …（其余 {len(missing) - 40} 条见 {args.out}）")
            bad += len(missing)
        json.dump({lang: sorted(k for k in keys if k not in load_pack(p))
                   for lang, p in PACKS.items()},
                  io.open(args.out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        return 1 if bad else 0

    payload = {"total": len(keys), "keys": sorted(keys)}
    if args.by_file:
        payload["by_file"] = by_file
    json.dump(payload, io.open(args.out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"unique keys: {len(keys)}（已写入 {args.out}）")
    if args.by_file:
        for p, ks in sorted(by_file.items(), key=lambda kv: -len(kv[1])):
            print(f"  {len(ks):4d}  {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
