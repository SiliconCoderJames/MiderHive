#!/usr/bin/env python3
"""把翻译工作流的产出（.tmp-i18n-out*/）合并进 pack 头文件。

输入：
  .tmp-i18n-out/<file>.json   : { "<简体键>": { "hant": "...", "ja": "..." } }        (工作流 1)
  .tmp-i18n-out2/<file>.json  : { "<简体键>": { "ko": "...", "de": "...", "fr": "...", "it": "..." } } (工作流 2)

输出：重写 src/gui/i18n_pack_zh_hant.h / i18n_pack_ja.h / i18n_pack_ko.h /
i18n_pack_de.h / i18n_pack_fr.h / i18n_pack_it.h（键按原序保留，每键一条）。
"""

import glob
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIRS = [".tmp-i18n-out", ".tmp-i18n-out2"]

# 语言 → (pack 头文件, 输出字段名, 表函数名, 头注释)
LANGS = {
    "hant": ("src/gui/i18n_pack_zh_hant.h", "zhHant",
             "繁體中文（zh-Hant）语言包：简体源文 → 繁体译文 的覆盖表。",
             "词汇采用台湾惯用语（檔案/記憶體/設定/網路…）。"),
    "ja": ("src/gui/i18n_pack_ja.h", "ja",
           "日本語（ja）语言包：简体源文 → 日文译文 的覆盖表。",
           "文体：按钮/菜单用体言止め，说明文用です・ます体；专有名词（MiderHive、Agent、Token 等）保留原文。"),
    "ko": ("src/gui/i18n_pack_ko.h", "ko",
           "한국어（ko）语言包：简体源文 → 韩文译文 的覆盖表。",
           "文体：버튼/메뉴는 명사형, 설명문은 합니다체；Agent/Token/MCP/MiderHive 등 고유명사는 원어 유지。"),
    "de": ("src/gui/i18n_pack_de.h", "de",
           "Deutsch（de）语言包：简体源文 → 德文译文 的覆盖表。",
           "文体：说明文用 Sie-Form（敬称）；按钮/菜单用名词短语；按德语 UI 惯例大写名词。"),
    "fr": ("src/gui/i18n_pack_fr.h", "fr",
           "Français（fr）语言包：简体源文 → 法文译文 的覆盖表。",
           "文体：按钮/菜单用不定式或名词短语，说明文用 vouvoiement。"),
    "it": ("src/gui/i18n_pack_it.h", "it",
           "Italiano（it）语言包：简体源文 → 意大利文译文 的覆盖表。",
           "文体：按钮/菜单用名词或不定式，说明文用 fornale。"),
}


def load_all() -> "tuple[dict, set]":
    tables = {lang: {} for lang in LANGS}
    all_keys = []
    seen = set()
    for d in OUT_DIRS:
        for f in sorted(glob.glob(d + "/*.json")):
            data = json.load(io.open(f, encoding="utf-8"))
            for zh, entry in data.items():
                if zh not in seen:
                    seen.add(zh)
                    all_keys.append(zh)
                for lang in LANGS:
                    v = entry.get(lang)
                    if isinstance(v, str) and v:
                        tables[lang][zh] = v
    return tables, all_keys


def esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")


def write_pack(lang: str, keys, table: dict) -> None:
    path, fn, title, style = LANGS[lang]
    NL = "\n"
    body = []
    untranslated = 0
    for k in keys:
        v = table.get(k)
        if v is None:
            untranslated += 1
            continue  # 未翻译：不写入 pack，让该语言界面回退英文（graceful fallback）
        body.append('        {"%s",\n         "%s"},' % (esc(k), esc(v)))
    header = (
        "#pragma once" + NL +
        "// " + title + NL +
        "// 由 scripts/i18n_extract.py（提取键）+ 翻译工作流产出合并生成" + NL +
        "// （scripts/merge_i18n_packs.py）。**手工改动请保持键与简体源文逐字节一致**，" + NL +
        "// 否则该条目孤儿化（界面回退英文，不炸）。" + style + NL +
        "#include <string_view>" + NL +
        "#include <unordered_map>" + NL + NL +
        "namespace i18n_pack {" + NL + NL +
        "inline const std::unordered_map<std::string_view, std::string_view>& " + fn + "() {" + NL +
        "    static const std::unordered_map<std::string_view, std::string_view> k = {" + NL +
        NL.join(body) + NL +
        "    };" + NL +
        "    return k;" + NL +
        "}" + NL + NL +
        "}  // namespace i18n_pack" + NL
    )
    io.open(os.path.join(ROOT, path), "w", encoding="utf-8", newline="").write(header)
    print(f"{lang}: {len(keys)} keys -> {path} (translated {len(table)}, untranslated {untranslated})")


def main() -> int:
    tables, all_keys = load_all()
    if not all_keys:
        print("no translation output found in", OUT_DIRS)
        return 2
    for lang in LANGS:
        write_pack(lang, all_keys, tables[lang])
    missing = {lang: sum(1 for k in all_keys if k not in tables[lang]) for lang in LANGS}
    print("coverage:", missing)
    return 0


if __name__ == "__main__":
    sys.exit(main())
