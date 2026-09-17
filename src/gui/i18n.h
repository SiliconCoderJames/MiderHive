#pragma once
// 轻量 i18n：zh-CN 为源语言，en 为第二语言；运行时切换并持久化到 QSettings。
// 覆盖范围：全部界面文案经 trs()/tr() 取词；Qt 自带控件的文案（对话框的
// 「确定/取消」、QMessageBox 标准按钮、输入框右键菜单等）由 Qt 官方
// qtbase_<lang>.qm 提供，见 installQtTranslations()——不装的话这些字符串在
// 中文界面里会一直是英文。
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QSettings>
#include <QString>
#include <QTranslator>
#include <functional>
#include <vector>

namespace i18n {

enum class Lang { Zh, En };

inline Lang g_lang = Lang::Zh;
inline std::vector<std::function<void()>>& listeners() {
    static std::vector<std::function<void()>> v;
    return v;
}

// Qt 官方译文（qtbase_*.qm）随 Qt 安装在 TranslationsPath 下。中文界面下装上
// zh_CN，Qt 自己的按钮/菜单文案才会跟着中文；英文是 Qt 的源语言，直接卸载即可。
inline void installQtTranslations() {
    auto* app = QCoreApplication::instance();
    if (!app) return;
    static QTranslator* qtTr = new QTranslator();  // 首次调用发生在 QApplication 之后
    QCoreApplication::removeTranslator(qtTr);
    if (g_lang == Lang::En) return;
    const QString dir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTr->load(QStringLiteral("qtbase_zh_CN"), dir)) QCoreApplication::installTranslator(qtTr);
}

inline void load() {
    // 品牌更名后迁移到 ("miderhive","miderhive")；旧位置仍可读（只读兜底，不再写入）
    QSettings fresh("miderhive", "miderhive");
    QString lang = fresh.value("ui/lang").toString();
    if (lang.isEmpty()) lang = QSettings("agenthive", "agenthive").value("ui/lang").toString();
    g_lang = lang == "en" ? Lang::En : Lang::Zh;
    installQtTranslations();
}

inline void apply(Lang l) {
    g_lang = l;
    QSettings("miderhive", "miderhive").setValue("ui/lang", l == Lang::En ? "en" : "zh");
    installQtTranslations();
    for (auto& f : listeners()) f();
}

inline void toggle() { apply(g_lang == Lang::Zh ? Lang::En : Lang::Zh); }

// 双语取词：zh 为源语言文案，en 为英文对照
inline const char* tr(const char* zh, const char* en) {
    return g_lang == Lang::Zh ? zh : en;
}
inline QString trs(const QString& zh, const QString& en) {
    return g_lang == Lang::Zh ? zh : en;
}

}  // namespace i18n
