#pragma once
// 运行时可切换主题（配色参考主流 Agent harness）：
//   hive    MiderHive 蜂巢 · 夜蓝 + 蜜金（默认）
//   claude  Claude 暖沙 · 暖灰 + 陶橙
//   codex   Codex 石墨 · 纯黑灰 + 翡翠
//   copilot GitHub Copilot 暗淡 · 蓝灰 + 钴蓝
//   zcode   ZCode 经典 · VS Dark+
// 所有 GUI 取色经 theme() 与取色函数在运行时解析；样式串用 ui::th() 做
// @token@ 颜色替换与 font-size 字号缩放。切换主题 = 重生成 QSS + 重建面板。
#include <QColor>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <functional>
#include <utility>
#include <vector>

namespace ui {

struct Theme {
    const char* id;
    const char* zh;
    const char* en;
    QColor bg, deep, card, field, fieldHover, line;   // 层级底色与描边
    QColor accent, accentHi, accentDeep, brand;       // 强调色与品牌色
    QColor ok, danger, warn, note;                    // 语义色
    QColor text, muted, selText;                      // 文本
    const char* mono;                                 // 等宽字体族
    int baseSize;                                     // 基准字号 px
};

inline const std::vector<Theme>& themes() {
    static const std::vector<Theme> k = {
        {"hive", "蜂巢 · 夜蓝蜜金", "Hive · Navy & Honey",
            {"#10141c"}, {"#0c101a"}, {"#1a2130"}, {"#151b28"}, {"#223047"}, {"#2c3854"},
            {"#0ea5e9"}, {"#38bdf8"}, {"#0284c7"}, {"#f59e0b"},
            {"#22c55e"}, {"#ef4444"}, {"#f59e0b"}, {"#eab308"},
            {"#e7ecf5"}, {"#94a3b8"}, {"#7dd3fc"},
            "Consolas", 12},
        {"claude", "暖沙 · Claude 风", "Claude · Warm Sand",
            {"#262624"}, {"#1f1f1d"}, {"#30302e"}, {"#2b2b28"}, {"#3a3a35"}, {"#4a4944"},
            {"#d97757"}, {"#e8956f"}, {"#b85c3f"}, {"#d97757"},
            {"#8fbc72"}, {"#e05d5d"}, {"#e0a458"}, {"#d4c05e"},
            {"#f0ece1"}, {"#a8a29a"}, {"#f0c4b2"},
            "Consolas", 13},
        {"codex", "石墨 · Codex 风", "Codex · Graphite",
            {"#0d0d0d"}, {"#080808"}, {"#1b1b1b"}, {"#151515"}, {"#262626"}, {"#2f2f2f"},
            {"#10a37f"}, {"#35c99e"}, {"#0b7a5f"}, {"#10a37f"},
            {"#43a047"}, {"#e2574c"}, {"#e8a33d"}, {"#d9c34d"},
            {"#ececec"}, {"#969696"}, {"#8fe0c4"},
            "Consolas", 12},
        {"copilot", "暗淡 · Copilot 风", "Copilot · Dimmed",
            {"#22272e"}, {"#1c2127"}, {"#2d333b"}, {"#262c33"}, {"#37414c"}, {"#444c56"},
            {"#539bf5"}, {"#6cb2ff"}, {"#34739b"}, {"#8957e5"},
            {"#57ab5a"}, {"#e5534b"}, {"#c69026"}, {"#caa53d"},
            {"#adbac7"}, {"#768390"}, {"#a2c1f0"},
            "Consolas", 12},
        {"zcode", "经典 · ZCode 风", "ZCode · Classic",
            {"#1e1e1e"}, {"#181818"}, {"#252526"}, {"#262626"}, {"#2a2d2e"}, {"#3c3c3c"},
            {"#0098ff"}, {"#29b6f6"}, {"#007acc"}, {"#cca700"},
            {"#4ec9b0"}, {"#f14c4c"}, {"#cca700"}, {"#dcdcaa"},
            {"#d4d4d4"}, {"#8a8a8a"}, {"#9fd8ff"},
            "Consolas", 12},
    };
    return k;
}

// ---- 当前主题 / 字号（懒加载自 QSettings，可运行时切换）----
inline int& themeIdx() {
    static int i = [] {
        QSettings s;
        QString id = s.value("ui/theme").toString();
        const auto& ts = themes();
        for (size_t k = 0; k < ts.size(); ++k)
            if (id == ts[k].id) return static_cast<int>(k);
        return 0;  // 默认 hive
    }();
    return i;
}
inline const Theme& theme() { return themes()[static_cast<size_t>(themeIdx())]; }

inline std::vector<std::function<void()>>& themeListeners() {
    static std::vector<std::function<void()>> l;
    return l;
}
// 返回监听器槽位 id（供 removeThemeListener 精确移除；1 起始）
inline std::size_t addThemeListener(std::function<void()> f) {
    auto& l = themeListeners();
    l.push_back(std::move(f));
    return l.size();
}
inline void removeThemeListener(std::size_t id) {
    auto& l = themeListeners();
    if (id > 0 && id <= l.size()) l[id - 1] = nullptr;  // 置空而非删除，保持其余槽位稳定
}
inline void notifyThemeListeners() {
    for (auto& f : themeListeners())
        if (f) f();
}
inline void setThemeIndex(int i) {
    i = qBound(0, i, static_cast<int>(themes().size()) - 1);
    themeIdx() = i;
    QSettings s;
    s.setValue("ui/theme", theme().id);
    notifyThemeListeners();
}

inline int& fontBaseRef() {
    static int b = [] {
        QSettings s;
        int v = s.value("ui/fontBase", 12).toInt();
        return (v >= 12 && v <= 14) ? v : 12;
    }();
    return b;
}
inline void setFontBase(int px) {
    fontBaseRef() = qBound(12, px, 14);
    QSettings s;
    s.setValue("ui/fontBase", fontBaseRef());
    notifyThemeListeners();
}

// ---- 取色函数（替代原编译期常量，切换主题后即时生效）----
inline QColor bg() { return theme().bg; }
inline QColor deep() { return theme().deep; }
inline QColor card() { return theme().card; }
inline QColor field() { return theme().field; }
inline QColor fieldHover() { return theme().fieldHover; }
inline QColor line() { return theme().line; }
inline QColor accent() { return theme().accent; }
inline QColor accentHi() { return theme().accentHi; }
inline QColor accentDeep() { return theme().accentDeep; }
inline QColor brand() { return theme().brand; }
inline QColor ok() { return theme().ok; }
inline QColor danger() { return theme().danger; }
inline QColor warn() { return theme().warn; }
inline QColor note() { return theme().note; }
inline QColor text() { return theme().text; }
inline QColor muted() { return theme().muted; }
inline QColor selText() { return theme().selText; }

inline QString mono() { return QString(theme().mono); }
inline QString sans() {
    return QStringLiteral("Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI");
}

// 用量占比 → 颜色（预算临近渐变：<80% 强调色，<95% 警告，其余危险）
inline QColor usageColor(double ratio) {
    if (ratio < 0.80) return accent();
    if (ratio < 0.95) return warn();
    return danger();
}

// ---- 样式串主题化 ----
// 1) @token@ → 当前主题色（selbg/okbg/errbg/warnbg 等派生色自动计算，@mono@ → 等宽字体族）。
//    这里没有的标记会原样留在样式表里，Qt 随即整条解析失败（控件失去样式）——新增
//    派生色必须加进这张表；UI harness 有规则检查残留的 @token@
// 2) font-size:Npx → 按当前基准字号等比缩放（字体大小可调）
inline QString th(QString s) {
    const Theme& t = theme();
    const std::pair<const char*, QString> map[] = {
        {"accent", t.accent.name()},
        {"accenthi", t.accentHi.name()},
        {"accentdeep", t.accentDeep.name()},
        {"brand", t.brand.name()},
        {"bg", t.bg.name()},
        {"deep", t.deep.name()},
        {"card", t.card.name()},
        {"field", t.field.name()},
        {"fieldhover", t.fieldHover.name()},
        {"line", t.line.name()},
        {"ok", t.ok.name()},
        {"danger", t.danger.name()},
        {"warn", t.warn.name()},
        {"note", t.note.name()},
        {"text", t.text.name()},
        {"muted", t.muted.name()},
        {"seltext", t.selText.name()},
        {"mono", t.mono},
        {"selbg", QString("rgba(%1,%2,%3,40)")
                      .arg(t.accent.red())
                      .arg(t.accent.green())
                      .arg(t.accent.blue())},
        {"okbg", QString("rgba(%1,%2,%3,50)")
                      .arg(t.ok.red())
                      .arg(t.ok.green())
                      .arg(t.ok.blue())},
        {"errbg", QString("rgba(%1,%2,%3,50)")
                       .arg(t.danger.red())
                       .arg(t.danger.green())
                       .arg(t.danger.blue())},
        // 总览页的接入提示条用的是 @warnbg@，这张表里原先没有它：占位符原样留在样式
        // 表里，整条样式表解析失败——提示条没有底色，里面的「一键接入」按钮成了一块
        // 看不见文字的黑块（UI harness 的 qss-unsubstituted-token 规则抓到的）
        {"warnbg", QString("rgba(%1,%2,%3,50)")
                        .arg(t.warn.red())
                        .arg(t.warn.green())
                        .arg(t.warn.blue())},
    };
    for (const auto& kv : map) s.replace(QString("@%1@").arg(kv.first), kv.second);

    const double scale = double(fontBaseRef()) / 12.0;
    static const QRegularExpression re("font-size:\\s*(\\d+)px");
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(s); it.hasNext();) {
        auto m = it.next();
        out += s.mid(last, m.capturedStart(1) - last);
        out += QString::number(qMax(9, qRound(m.captured(1).toDouble() * scale)));
        last = m.capturedEnd(1);
    }
    out += s.mid(last);
    return out;
}

// 全局 QSS：模板资源经 th() 主题化后应用
inline QString themeQss() {
    QFile f(":/theme/qss/dark.qss");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("theme qss open FAILED: %s", f.errorString().toUtf8().constData());
        return {};
    }
    return th(QString::fromUtf8(f.readAll()));
}

}  // namespace ui
