#pragma once
// 所有面板的公共基类：持有 Platform 引用，提供统一 refresh() 接口与页头构造。
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

#include "../i18n.h"
#include "../theme.h"
#include "../widgets.h"   // ui::makeIcon：页头图标徽章与侧栏图标同源
// 经 miderhive_core 的 PUBLIC include 路径（src/）解析
#include "core/platform.h"

class PanelBase : public QWidget {
public:
    explicit PanelBase(ah::Platform& platform, QWidget* parent = nullptr)
        : QWidget(parent), platform_(platform) {}
    virtual void refresh() = 0;
    // Ctrl+F：把焦点交给本面板的即时过滤框；无过滤框的面板不用覆写
    virtual void focusFilter() {}
    // 语言切换时重译铬层文案；面板有自有文案时覆写并先调用基类
    virtual void retranslate() {
        if (headerTitle_) headerTitle_->setText(i18n::trs(zhTitle_, enTitle_));
        if (headerSub_) headerSub_->setText(i18n::trs(zhSub_, enSub_));
    }
    // ---- 跨面板下钻 ----
    // 由主窗口注入：面板只管"跳到哪个面板、带什么筛选"，导航与筛选落地由 MainWindow 负责。
    // （面板之间不互相持有引用，加面板不会牵动其它面板。）
    using Navigator =
        std::function<void(const QString& panelId, const QString& key, const QString& value)>;
    void setNavigator(Navigator n) { navigate_ = std::move(n); }
    // 作为下钻目标时接收筛选条件（如用量页的 agent/model/range）；不关心的面板忽略即可
    virtual void applyFilter(const QString& /*key*/, const QString& /*value*/) {}

protected:
    // 面板内部触发下钻（图表点击、卡片点击等）
    void drillTo(const QString& panelId, const QString& key = QString(),
                 const QString& value = QString()) const {
        if (navigate_) navigate_(panelId, key, value);
    }

    // 页头：图标徽章 + 品牌饰条 + 面板标题/说明竖列，统一各面板的视觉节奏（双语，可重译）
    void buildHeader(QVBoxLayout* layout, const QString& iconKind, const QString& zhTitle,
                     const QString& enTitle, const QString& zhSub, const QString& enSub) {
        zhTitle_ = zhTitle; enTitle_ = enTitle;
        zhSub_ = zhSub; enSub_ = enSub;
        // 图标徽章：圆角色块 + 与侧栏同源的矢量图标，给每个面板一个统一的视觉锚点
        auto* chip = new QLabel(this);
        chip->setFixedSize(38, 38);
        chip->setAlignment(Qt::AlignCenter);
        chip->setPixmap(ui::makeIcon(iconKind, ui::accent(), 20).pixmap(20, 20));
        chip->setStyleSheet(ui::th("background:@selbg@; border:1px solid @line@;"
                                   " border-radius:11px;"));
        headerTitle_ = new QLabel(i18n::trs(zhTitle, enTitle), this);
        headerTitle_->setStyleSheet(
            ui::th("font-size:20px; font-weight:700; color:@text@;"));
        // 竖向固定：否则面板高于内容时，多余空间会被标题/副标题吸收，
        // 标题与正文之间被撑出一大片空白（内容区反而没拿到空间）
        headerTitle_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        headerSub_ = new QLabel(i18n::trs(zhSub, enSub), this);
        headerSub_->setStyleSheet(ui::th("font-size:12px; color:@muted@;"));
        headerSub_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        headerSub_->setWordWrap(true);
        // 标题与说明收成一个竖列：与徽章同高对齐，页头更紧凑
        auto* titleCol = new QVBoxLayout;
        titleCol->setSpacing(3);
        titleCol->setContentsMargins(0, 0, 0, 0);
        titleCol->addWidget(headerTitle_);
        titleCol->addWidget(headerSub_);
        // 标题蜜金→天蓝竖向渐变饰条：全面板统一的品牌签名
        auto* bar = new QFrame(this);
        bar->setFixedSize(4, 28);
        bar->setStyleSheet(
            ui::th("background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                   "stop:0 @brand@, stop:1 @accent@); border-radius:2px;"));
        auto* head = new QHBoxLayout;
        head->setSpacing(12);
        head->addWidget(chip);
        head->addWidget(bar);
        head->addLayout(titleCol, 1);
        layout->addLayout(head);
        layout->addSpacing(12);
    }

    ah::Platform& platform_;

private:
    Navigator navigate_;
    QLabel* headerTitle_ = nullptr;
    QLabel* headerSub_ = nullptr;
    QString zhTitle_, enTitle_, zhSub_, enSub_;
};
