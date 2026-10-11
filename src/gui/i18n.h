#pragma once
// 轻量 i18n：zh-CN 为源语言；en / zh-Hant（繁體中文）/ ja（日本語）为可切换语言。
// 运行时切换并持久化到 QSettings。
//
// 机制（2026-10 升级：从"中英双内联"到"N 语言覆盖表"）：
//   * 全部界面文案仍写作 trs(简体源文, English) —— 668 个调用点不需要因新增
//     语言而改动；简体源文同时充当**翻译键**。
//   * 额外语言 = 一张"简体源文 → 译文"的覆盖表（i18n_pack_zh_hant.h /
//     i18n_pack_ja.h，由 scripts/i18n_extract.py 提取键 + 翻译流程生成）。
//     取词：切换到该语言时先查覆盖表，查不到**回退英文**（graceful，永不
//     出现半翻译的破界面）。
//   * 新增一门语言 = 新建一张 pack 表 + 在 languages() 里注册一行。键必须与
//     简体源文逐字节一致——源文改动会让对应译文"孤儿化"（回退英文，不炸），
//     用 i18n_extract.py --coverage 可查未覆盖键。
// 覆盖范围：全部界面文案经 trs()/tr() 取词；Qt 自带控件的文案（对话框的
// 「确定/取消」、QMessageBox 标准按钮等）由 Qt 官方 qtbase_<lang>.qm 提供，
// 见 installQtTranslations()。
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QSettings>
#include <QString>
#include <QTranslator>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "i18n_pack_de.h"
#include "i18n_pack_fr.h"
#include "i18n_pack_it.h"
#include "i18n_pack_ja.h"
#include "i18n_pack_ko.h"
#include "i18n_pack_zh_hant.h"

namespace i18n {

enum class Lang { Zh, En, ZhHant, Ja, Ko, De, Fr, It };

// 语言清单（语言菜单的展示顺序；nativeName 用该语言自己的文字）
struct LangInfo {
    Lang lang;
    const char* id;         // 持久化用的稳定标识
    const char* nativeName; // 菜单里显示的名字
    const char* qtBase;     // Qt 自带文案的 qm 名（空 = 用源语言，不装）
};
inline const std::vector<LangInfo>& languages() {
    static const std::vector<LangInfo> k = {
        {Lang::Zh,     "zh",      "简体中文", "qtbase_zh_CN"},
        {Lang::ZhHant, "zh-Hant", "繁體中文", "qtbase_zh_TW"},
        {Lang::Ja,     "ja",      "日本語",   "qtbase_ja"},
        {Lang::Ko,     "ko",      "한국어",    "qtbase_ko"},
        {Lang::De,     "de",      "Deutsch",  "qtbase_de"},
        {Lang::Fr,     "fr",      "Français", "qtbase_fr"},
        {Lang::It,     "it",      "Italiano", "qtbase_it"},
        {Lang::En,     "en",      "English",  ""},
    };
    return k;
}
inline const LangInfo* langInfo(Lang l) {
    for (const auto& li : languages())
        if (li.lang == l) return &li;
    return nullptr;
}

inline Lang g_lang = Lang::Zh;
inline std::vector<std::function<void()>>& listeners() {
    static std::vector<std::function<void()>> v;
    return v;
}

// 覆盖表查询：键 = 简体源文的 UTF-8 字节。查不到返回空 view（调用方回退英文）。
inline std::string_view lookupOverride(std::string_view zhKey) {
    switch (g_lang) {
        case Lang::ZhHant: {
            const auto& t = i18n_pack::zhHant();
            auto it = t.find(zhKey);
            return it == t.end() ? std::string_view{} : it->second;
        }
        case Lang::Ja: {
            const auto& t = i18n_pack::ja();
            auto it = t.find(zhKey);
            return it == t.end() ? std::string_view{} : it->second;
        }
        case Lang::Ko: {
            const auto& t = i18n_pack::ko();
            auto it = t.find(zhKey);
            return it == t.end() ? std::string_view{} : it->second;
        }
        case Lang::De: {
            const auto& t = i18n_pack::de();
            auto it = t.find(zhKey);
            return it == t.end() ? std::string_view{} : it->second;
        }
        case Lang::Fr: {
            const auto& t = i18n_pack::fr();
            auto it = t.find(zhKey);
            return it == t.end() ? std::string_view{} : it->second;
        }
        case Lang::It: {
            const auto& t = i18n_pack::it();
            auto it = t.find(zhKey);
            return it == t.end() ? std::string_view{} : it->second;
        }
        default:
            return {};
    }
}

// Qt 官方译文（qtbase_*.qm）随 Qt 安装在 TranslationsPath 下，Qt 自己的按钮
// 文案才跟着界面语言走；英文是 Qt 的源语言，直接卸载。
inline void installQtTranslations() {
    auto* app = QCoreApplication::instance();
    if (!app) return;
    static QTranslator* qtTr = new QTranslator();  // 首次调用发生在 QApplication 之后
    QCoreApplication::removeTranslator(qtTr);
    const LangInfo* li = langInfo(g_lang);
    if (!li || !*li->qtBase) return;
    const QString dir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTr->load(QString::fromLatin1(li->qtBase), dir)) QCoreApplication::installTranslator(qtTr);
}

inline Lang fromId(const QString& id) {
    for (const auto& li : languages())
        if (id == QLatin1String(li.id)) return li.lang;
    return Lang::Zh;
}

inline void load() {
    // 品牌更名后迁移到 ("miderhive","miderhive")；旧位置仍可读（只读兜底，不再写入）
    QSettings fresh("miderhive", "miderhive");
    QString lang = fresh.value("ui/lang").toString();
    if (lang.isEmpty()) lang = QSettings("agenthive", "agenthive").value("ui/lang").toString();
    g_lang = fromId(lang);  // 旧值 "zh"/"en" 天然兼容
    installQtTranslations();
}

inline void apply(Lang l) {
    g_lang = l;
    const LangInfo* li = langInfo(l);
    QSettings("miderhive", "miderhive").setValue("ui/lang", QString::fromLatin1(li->id));
    installQtTranslations();
    for (auto& f : listeners()) f();
}

// 双语取词：zh 为源语言文案（同时是覆盖表的键），en 为英文对照；
// 繁体/日语先查覆盖表，缺失回退英文。
inline const char* tr(const char* zh, const char* en) {
    if (g_lang == Lang::Zh) return zh;
    if (g_lang == Lang::En) return en;
    const std::string_view hit = lookupOverride(std::string_view(zh));
    return hit.empty() ? en : hit.data();
}
inline QString trs(const QString& zh, const QString& en) {
    if (g_lang == Lang::Zh) return zh;
    if (g_lang == Lang::En) return en;
    const std::string zhBytes = zh.toUtf8().toStdString();
    const std::string_view hit = lookupOverride(zhBytes);
    return hit.empty() ? en : QString::fromUtf8(hit.data(), static_cast<qsizetype>(hit.size()));
}

}  // namespace i18n
