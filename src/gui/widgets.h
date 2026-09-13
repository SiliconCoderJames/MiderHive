#pragma once
// 态势感知工作台自定义控件：环形进度、横向/纵向柱状图、主题色卡、Toast、
// Agent 卡片、告警卡片、可折叠区块卡片。全部 QPainter / 原生 widget 实现，无 QML。
#include <QBrush>
#include <QConicalGradient>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QIcon>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSpinBox>
#include <QSvgRenderer>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWidget>

#include <cmath>
#include <functional>

#include "theme.h"
#include "i18n.h"

namespace ui {

// 卡片悬停高亮：进入边框变强调色（AgentCard / 通用 QFrame 卡片）
inline void hoverGlow(QFrame* frame, const char* objectName) {
    frame->setAttribute(Qt::WA_Hover);
    frame->setStyleSheet(th(QString("QFrame#%1 { background:@card@; border:1px solid @line@;"
                           " border-radius:8px; }"
                           "QFrame#%1:hover { background:@fieldhover@; border:1px solid @accent@;"
                           " border-radius:8px; }").arg(objectName)));
}

// 状态胶囊（在线/离线徽标）：主题语义色淡染底 + 亮色文字
inline QString pillStyle(const QColor& c, int alpha = 50) {
    return QString("font-size:10px; padding:1px 8px; border-radius:8px;"
                   " color:%1; background:rgba(%2,%3,%4,%5);")
        .arg(c.lighter(140).name())
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(alpha);
}

// ---- 图表空状态：弱化蜂巢描边 + 主文案 + 出路提示 ----
// 比一行裸文字更明确「这里将会出现什么、需要做什么」，空数据时不再是一块死区。
inline void paintChartEmpty(QPainter& p, const QRect& r, const QString& title, const QString& hint) {
    p.setRenderHint(QPainter::Antialiasing);
    const int cy = r.center().y();
    // 高度够时才画蜂巢母题，避免小卡片里被裁切
    if (r.height() >= 130) {
        const qreal r0 = 13.0;
        const QPointF c(r.center().x(), cy - 30.0);
        QPen pen(QColor(brand().red(), brand().green(), brand().blue(), 90), 3.0, Qt::SolidLine,
                 Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        auto hex = [&](const QPointF& ctr) {
            QPolygonF h;
            for (int i = 0; i < 6; ++i) {
                qreal a = M_PI / 180.0 * (60.0 * i - 30.0);
                h << ctr + QPointF(r0 * std::cos(a), r0 * std::sin(a));
            }
            p.drawPolygon(h);
        };
        hex(c + QPointF(0, -r0 * 1.02));
        hex(c + QPointF(-r0 * 0.9, r0 * 0.55));
        hex(c + QPointF(r0 * 0.9, r0 * 0.55));
    }
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QPen(muted()));
    p.drawText(QRect(r.left(), cy + 6, r.width(), 18), Qt::AlignCenter, title);
    f.setPixelSize(11);
    f.setBold(false);
    p.setFont(f);
    QColor dim = muted();
    dim.setAlpha(165);
    p.setPen(QPen(dim));
    p.drawText(QRect(r.left() + 8, cy + 26, r.width() - 16, 18), Qt::AlignCenter, hint);
}

// 紧凑数字：<1万原样，1万~100万 "12.3k"，≥100万 "1.23M"（柱顶标注与环内文字自适应用）
inline QString fmtCompact(qint64 v) {
    if (v < 10000) return QString::number(v);
    const bool mega = v >= 1000000;
    double x = mega ? double(v) / 1000000.0 : double(v) / 1000.0;
    QString s = QString::number(x, 'f', x < 100 ? 1 : 0);
    while (s.contains('.') && s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    return s + (mega ? "M" : "k");
}

// 前置声明：文件前部的控件（空状态等）就要用图标工厂；真正的定义在文件末尾。
// 与末尾那处声明同签名，重复声明合法。
inline QIcon makeIcon(const QString& kind, const QColor& color, int px = 18,
                      const QColor& selectedColor = {});

// ---- 换行标签：wordWrap 的 QLabel 会把 minimumSizeHint 按"最长单词宽度"折算高度，
// 实测把告警卡的最小高度从单行 ~20px 撑到 ~79px（4 张卡 = 384px），整页因此被顶出视口、
// 事件流与告警掉到折页之下。这里给最小高度封顶：宽度够时按一行算，窄窗口下仍会换行
// （真实高度由 sizeHint 决定），既不失窄窗口的换行保护，也不再虚高。
class WrappedLabel : public QLabel {
public:
    explicit WrappedLabel(int maxMinLines, const QString& text, QWidget* parent = nullptr)
        : QLabel(text, parent), maxMinLines_(maxMinLines) {
        setWordWrap(true);
    }
    QSize minimumSizeHint() const override {
        QSize s = QLabel::minimumSizeHint();
        const int lineH = fontMetrics().lineSpacing();
        s.setHeight(qMin(s.height(), lineH * maxMinLines_ + 4));
        return s;
    }

private:
    int maxMinLines_ = 2;
};

// ---- 趋势小图：指标卡里的迷你面积折线（无坐标轴，只表达"走向"）----
// 现状数字只能说明"现在多少"，看不出"在涨还是在跌"；小图补上趋势维度，
// 让指标卡从"一个数"变成"一个数 + 一条走势"。
class Sparkline : public QWidget {
public:
    explicit Sparkline(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(30);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setStyleSheet("background:transparent;");
    }
    void setSeries(const QVector<qint64>& series, const QColor& c) {
        series_ = series;
        color_ = c.isValid() ? c : accent();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (series_.size() < 2) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal pad = 2.0;
        const qreal w = width() - pad * 2;
        const qreal h = height() - pad * 2;
        qint64 mx = 1, mn = series_.front();
        for (qint64 v : series_) { mx = qMax(mx, v); mn = qMin(mn, v); }
        if (mx == mn) mn = 0;  // 全平：贴着上沿画不出走势，退化为从 0 起
        QPolygonF line;
        for (int i = 0; i < series_.size(); ++i) {
            const qreal x = pad + w * i / qreal(series_.size() - 1);
            const qreal y = pad + h * (1.0 - double(series_[i] - mn) / double(mx - mn));
            line << QPointF(x, y);
        }
        // 面积填充：本色低透明度，视觉上轻，不抢主数值
        QPolygonF area = line;
        area << QPointF(line.back().x(), pad + h) << QPointF(line.front().x(), pad + h);
        QLinearGradient g(0, 0, 0, height());
        QColor c0 = color_;
        c0.setAlpha(86);
        QColor c1 = color_;
        c1.setAlpha(6);
        g.setColorAt(0.0, c0);
        g.setColorAt(1.0, c1);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPolygon(area);
        p.setPen(QPen(color_, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);
        // 末点强调：当前值的位置一眼可见
        p.setPen(Qt::NoPen);
        p.setBrush(color_);
        p.drawEllipse(line.back(), 2.4, 2.4);
    }

private:
    QVector<qint64> series_;
    QColor color_ = accent();
};

// ---- 微条：指标卡里的一行分段条（在线/离线、严重度构成等）----
// 比例构成用"条"表达比再写一行文字更快读懂，也把数字与图形放在同一个视觉单元里。
class MicroStrip : public QWidget {
public:
    explicit MicroStrip(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(8);
        setMaximumHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setStyleSheet("background:transparent;");
    }
    void setSegments(const QVector<QPair<QColor, int>>& segs) {
        segs_ = segs;
        setToolTip(tooltip_);
        update();
    }
    void setTooltipText(const QString& t) {
        tooltip_ = t;
        setToolTip(t);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        int total = 0;
        for (const auto& s : segs_) total += qMax(0, s.second);
        if (total <= 0) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal h = 6.0;
        const qreal y = (height() - h) / 2.0;
        qreal x = 0;
        for (const auto& s : segs_) {
            if (s.second <= 0) continue;
            const qreal w = width() * double(s.second) / double(total);
            p.setPen(Qt::NoPen);
            p.setBrush(s.first);
            p.drawRoundedRect(QRectF(x, y, qMax(w - 1.5, 1.5), h), 3, 3);
            x += w;
        }
    }

private:
    QVector<QPair<QColor, int>> segs_;
    QString tooltip_;
};

// ---- 紧凑空状态：图标 + 主文案 + 出路提示（可选动作按钮）----
// 比一行裸文字多两件事：说明"这里会有什么"，以及"下一步该做什么"。
class InlineEmpty : public QFrame {
public:
    InlineEmpty(const QString& iconKind, const QString& title, const QString& hint,
                QWidget* parent = nullptr)
        : QFrame(parent) {
        setStyleSheet("background:transparent; border:none;");
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(10, 14, 10, 14);
        lay->setSpacing(6);
        auto* icon = new QLabel(this);
        icon->setAlignment(Qt::AlignCenter);
        icon->setPixmap(makeIcon(iconKind, muted(), 22).pixmap(22, 22));
        lay->addWidget(icon, 0, Qt::AlignHCenter);
        title_ = new WrappedLabel(2, title, this);
        title_->setAlignment(Qt::AlignCenter);
        title_->setStyleSheet(th("color:@muted@; font-size:12px; font-weight:600;"));
        lay->addWidget(title_);
        hint_ = new WrappedLabel(2, hint, this);
        hint_->setAlignment(Qt::AlignCenter);
        QColor dim = muted();
        dim.setAlpha(170);
        hint_->setStyleSheet(QString("color:rgba(%1,%2,%3,%4); font-size:11px;")
                                 .arg(dim.red()).arg(dim.green()).arg(dim.blue())
                                 .arg(dim.alpha()));
        lay->addWidget(hint_);
        lay_ = lay;  // 供 setAction 追加动作按钮
    }
    QLabel* titleLabel() const { return title_; }
    QLabel* hintLabel() const { return hint_; }
    // 可选动作按钮：空状态的"显眼下一步"（如「＋ 新建条目」）。不调用则不占布局，
    // 既有用法零变化；调用后按钮出现在提示文字下方居中。
    void setAction(const QString& text, std::function<void()> fn) {
        if (!actionBtn_) {
            actionBtn_ = new QPushButton(this);
            actionBtn_->setObjectName("primary");
            actionBtn_->setCursor(Qt::PointingHandCursor);
            actionBtn_->setFixedHeight(30);
            lay_->addWidget(actionBtn_, 0, Qt::AlignHCenter);
        }
        actionBtn_->setText(text);
        lay_->update();
        if (fn) connect(actionBtn_, &QPushButton::clicked, this, [fn = std::move(fn)] { fn(); });
    }

private:
    QLabel* title_ = nullptr;
    QLabel* hint_ = nullptr;
    QPushButton* actionBtn_ = nullptr;
    QVBoxLayout* lay_ = nullptr;
};

// ---- 骨架屏：首屏数据回来之前的占位块（微光扫过，表明"正在取数"而非"没有数据"）----
class Skeleton : public QWidget {
public:
    explicit Skeleton(QWidget* parent = nullptr) : QWidget(parent) {
        setStyleSheet("background:transparent;");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] {
            phase_ += 0.02;
            if (phase_ > 1.4) phase_ = -0.4;
            update();
        });
    }
    void setRows(int rows) { rows_ = qMax(1, rows); update(); }
    void start() { timer_.start(); show(); }
    void stop() { timer_.stop(); hide(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor base = line();
        base.setAlpha(120);
        const int h = qMax(10, height() / rows_ - 10);
        for (int i = 0; i < rows_; ++i) {
            const QRectF r(0, i * (h + 10), width(), h);
            p.setPen(Qt::NoPen);
            p.setBrush(base);
            p.drawRoundedRect(r, 8, 8);
            // 微光带：从左到右扫过，暗示加载中
            QLinearGradient g(r.left() + r.width() * (phase_ - 0.25), 0,
                              r.left() + r.width() * (phase_ + 0.25), 0);
            QColor hi = fieldHover();
            hi.setAlpha(150);
            QColor none = base;
            none.setAlpha(0);
            g.setColorAt(0.0, none);
            g.setColorAt(0.5, hi);
            g.setColorAt(1.0, none);
            p.setBrush(g);
            p.drawRoundedRect(r, 8, 8);
        }
    }

private:
    int rows_ = 4;
    double phase_ = -0.4;
    QTimer timer_;
};

// ---- 环形进度：中间显示 已用/总额/百分比，临近预算橙→红，进度弧平滑动画 ----
class RingProgress : public QWidget {
public:
    RingProgress(QWidget* parent = nullptr) : QWidget(parent) {
        // 最小尺寸刻意压小：窗口在 1080x680 逻辑尺寸 + 12/13/14 三档字号下都要放得下
        setMinimumSize(106, 106);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // 透明底色：否则全局 QSS 的 QWidget 背景会在卡片内再涂一层页面底色，
        // 卡片里会出现一圈"卡中卡"暗框
        setStyleSheet("background:transparent;");
    }
    void setValues(qint64 used, qint64 total, const QString& caption) {
        used_ = used;
        total_ = total > 0 ? total : 1;
        caption_ = caption;
        // 进度弧从当前角度平滑扫掠到目标，避免 3s 刷新时生硬跳变
        double target = qMin(1.0, double(used_) / double(total_));
        if (anim_) anim_->stop();  // QPointer：动画自删后自动置空，安全
        auto* anim = new QVariantAnimation(this);
        anim_ = anim;
        anim->setDuration(450);
        anim->setStartValue(animRatio_);
        anim->setEndValue(target);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            animRatio_ = v.toDouble();
            update();
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        int side = qMin(width(), height());
        QRectF rect((width() - side) / 2 + 10, (height() - side) / 2 + 10,
                    side - 20, side - 20);
        double ratio = animRatio_;
        // 背景环
        QPen pen(line(), 12, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.drawArc(rect, 45 * 16, -270 * 16);
        // 进度环：外圈柔光营造发光质感，主弧沿环锥形渐变（亮端→本色）；
        // 占比驱动颜色（蓝→蜜金→红）
        QColor c = usageColor(ratio);
        if (ratio > 0.001) {
            QPen glow(QColor(c.red(), c.green(), c.blue(), 46), 22, Qt::SolidLine, Qt::RoundCap);
            p.setPen(glow);
            p.drawArc(rect, 45 * 16, int(-270 * 16 * ratio));
            QConicalGradient sheen(rect.center(), 90);
            sheen.setColorAt(0.0, c.lighter(150));
            sheen.setColorAt(1.0, c);
            QPen prog(QBrush(sheen), 12, Qt::SolidLine, Qt::RoundCap);
            p.setPen(prog);
            p.drawArc(rect, 45 * 16, int(-270 * 16 * ratio));
        }
        // 中心文字（百分比随用量着色，与环体呼应）
        p.setPen(QPen(usageColor(ratio).lighter(115)));
        QFont f = p.font();
        f.setPixelSize(side / 6);
        f.setBold(true);
        p.setFont(f);
        QRectF center = rect.adjusted(14, 14, -14, -14);
        p.drawText(center.adjusted(0, -14, 0, -14), Qt::AlignCenter,
                   QString("%1%").arg(ratio * 100, 0, 'f', 1));
        f.setPixelSize(side / 14);
        p.setFont(f);
        p.setPen(QPen(muted()));
        auto fmt = [](qint64 v) {
            QString s = QString::number(v);
            for (int i = s.size() - 3; i > 0; i -= 3) s.insert(i, ',');
            return s;
        };
        // drawText 传入矩形会按矩形裁剪：环变小或数字变长时，首个数字会被切掉
        // （曾出现 "4,200 / 10,000,00"）。这里按可用宽度决定是否退化为紧凑记法。
        QFontMetrics fm(p.font());
        const int avail = int(center.width());
        auto fit = [&](QString s, qint64 a, qint64 b) {
            if (fm.horizontalAdvance(s) <= avail) return s;
            return QString("%1 / %2").arg(fmtCompact(a), fmtCompact(b));
        };
        p.drawText(center.adjusted(0, 16, 0, 16), Qt::AlignCenter,
                   fit(QString("%1 / %2").arg(fmt(used_), fmt(total_)), used_, total_));
        if (!caption_.isEmpty()) {
            QString cap = caption_;
            if (fm.horizontalAdvance(cap) > avail) {
                cap = QString("%1 %2").arg(i18n::trs("剩余", "left"),
                                           fmtCompact(used_ > total_ ? 0 : total_ - used_));
            }
            p.drawText(center.adjusted(0, 40, 0, 40), Qt::AlignCenter, cap);
        }
    }

private:
    qint64 used_ = 0, total_ = 1;
    QString caption_;
    double animRatio_ = 0.0;      // 动画当前扫掠比例（与目标值的差由 QVariantAnimation 收敛）
    QPointer<QVariantAnimation> anim_;  // DeleteWhenStopped 会自删，用 QPointer 防悬空
};

// ---- 本周 Token 预算调整对话框：唯一的界面修改入口（总览环形图与「用量分析」共用）。
// 此前预算只能改库或靠脚本，界面上无入口——用户找不到在哪里调。----
class BudgetEditDialog : public QDialog {
public:
    BudgetEditDialog(qint64 current, QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(i18n::trs("调整本周 Token 预算", "Adjust Weekly Token Budget"));
        setModal(true);
        setMinimumWidth(380);
        auto* lay = new QVBoxLayout(this);
        lay->setSpacing(10);
        auto* tip = new QLabel(i18n::trs(
            "预算用于用量告警：80% 提醒、95% 严重、超出标红；只做提示，不拦截上报。",
            "The budget drives usage alerts: 80% warn, 95% critical, over shows red. "
            "Advisory only — reporting is never blocked."), this);
        tip->setWordWrap(true);
        tip->setStyleSheet(ui::th("color:@muted@; font-size:12px;"));
        lay->addWidget(tip);
        spin_ = new QSpinBox(this);
        spin_->setRange(1'000, 2'000'000'000);
        spin_->setSingleStep(100'000);
        spin_->setGroupSeparatorShown(true);  // 千分位：大数字一眼读出量级
        spin_->setAccelerated(true);
        spin_->setValue(int(qBound<qint64>(qint64(1000), current, qint64(2000000000))));
        spin_->setStyleSheet(ui::th("QSpinBox { font-size:15px; padding:6px 8px; }"));
        lay->addWidget(spin_);
        auto* presets = new QHBoxLayout;
        auto* presetsLabel = new QLabel(i18n::trs("常用:", "Presets:"), this);
        presetsLabel->setStyleSheet(ui::th("color:@muted@;"));
        presets->addWidget(presetsLabel);
        for (qint64 p : {qint64(1'000'000), qint64(5'000'000), qint64(10'000'000),
                         qint64(50'000'000), qint64(100'000'000)}) {
            auto* b = new QPushButton(ui::fmtCompact(p), this);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(ui::th("padding:3px 12px;"));
            connect(b, &QPushButton::clicked, this, [this, p] { spin_->setValue(int(p)); });
            presets->addWidget(b);
        }
        presets->addStretch(1);
        lay->addLayout(presets);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        lay->addWidget(bb);
    }
    qint64 value() const { return spin_->value(); }

private:
    QSpinBox* spin_;
};

// ---- 横向柱状图：各 Agent 用量对比，柱体平滑扫掠入场 ----
class HBarChart : public QWidget {
public:
    explicit HBarChart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(106);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setStyleSheet("background:transparent;");  // 见 RingProgress：避免卡片内出现暗框
        setMouseTracking(true);  // 悬停行高亮 + tooltip：光看条长读不出确切数字
    }
    // 点击下钻：把"看到某一行"直接接到"看这一行的细节"（回调按序号交给面板处理）
    void setOnEntryClick(std::function<void(int)> fn) {
        onClick_ = std::move(fn);
        setCursor(onClick_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    const QVector<QPair<QString, qint64>>& entries() const { return entries_; }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !onClick_) return;
        const int n = entries_.size();
        const int idx = (n > 0) ? int(e->position().y()) / qMax(1, height() / n) : -1;
        if (idx >= 0 && idx < n) onClick_(idx);
    }
    // 悬停行：给出精确数值（千分位），并在绘制时高亮该行
    void mouseMoveEvent(QMouseEvent* e) override {
        const int n = entries_.size();
        const int idx = (n > 0) ? int(e->position().y()) / qMax(1, height() / n) : -1;
        const int next = (idx >= 0 && idx < n) ? idx : -1;
        if (next == hovered_) return;
        hovered_ = next;
        if (hovered_ >= 0)
            setToolTip(QString("%1: %2").arg(entries_[hovered_].first)
                           .arg(QLocale().toString(entries_[hovered_].second)));
        else
            setToolTip(QString());
        update();
    }
    void leaveEvent(QEvent*) override {
        if (hovered_ < 0) return;
        hovered_ = -1;
        setToolTip(QString());
        update();
    }
    void setEntries(const QVector<QPair<QString, qint64>>& entries) {
        entries_ = entries;
        if (anim_) anim_->stop();
        auto* anim = new QVariantAnimation(this);
        anim_ = anim;
        anim->setDuration(450);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            sweep_ = v.toDouble();
            update();
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        bool allZero = true;
        for (const auto& e : entries_)
            if (e.second != 0) { allZero = false; break; }
        // 空表或全 0：走空状态，避免画出一排没有意义的 "0" 标签
        if (entries_.isEmpty() || allZero) {
            paintChartEmpty(p, rect(), i18n::trs("暂无用量数据", "no usage data yet"),
                            i18n::trs("Agent 上报 Token 后在此对比",
                                      "shows up as agents report tokens"));
            return;
        }
        qint64 maxV = 1;
        qint64 sumV = 0;
        for (const auto& e : entries_) {
            maxV = qMax(maxV, e.second);
            sumV += qMax<qint64>(0, e.second);
        }
        int rowH = height() / entries_.size();
        int labelW = qMin(110, width() / 4);
        int valueW = 110;
        int barX = labelW + 8;
        int barW = width() - barX - valueW - 8;
        QFont f = p.font();
        f.setPixelSize(11);
        p.setFont(f);
        auto fmt = [](qint64 v) {
            QString s = QString::number(v);
            for (int i = s.size() - 3; i > 0; i -= 3) s.insert(i, ',');
            return s;
        };
        for (int i = 0; i < entries_.size(); ++i) {
            int y = i * rowH;
            if (i == hovered_) {  // 悬停行底色，扫读时不易看错行
                QColor hl = fieldHover();
                hl.setAlpha(120);
                p.setPen(Qt::NoPen);
                p.setBrush(hl);
                p.drawRoundedRect(QRect(0, y + 1, width() - 1, rowH - 2), 6, 6);
            }
            // 名称过长省略号收尾，避免与柱体重叠
            QFontMetrics fm(p.font());
            QString label = fm.elidedText(entries_[i].first, Qt::ElideRight, labelW - 8);
            p.setPen(QPen(text()));
            p.drawText(QRect(0, y, labelW - 4, rowH), Qt::AlignVCenter | Qt::AlignRight, label);
            // 柱体：最大值高亮蓝，其余随占比变暗；横向渐变（本色→亮）更有质感；
            // 宽度乘以入场扫掠进度
            double ratio = double(entries_[i].second) / double(maxV) * sweep_;
            int w = int(double(barW) * ratio);
            // 轨道：柱体不再是悬空色条，"完成度"一眼可读（商业图表惯例）
            QColor track = line();
            track.setAlpha(95);
            p.setPen(Qt::NoPen);
            p.setBrush(track);
            p.drawRoundedRect(QRect(barX, y + rowH / 2 - 5, barW, 10), 5, 5);
            QColor bar = accent();
            if (entries_[i].second != maxV) {
                bar = accent().darker(100 + int((1.0 - ratio) * 90));
                bar.setAlpha(210);
            }
            QLinearGradient sheen(barX, 0, barX + qMax(w, 4), 0);
            sheen.setColorAt(0.0, bar);
            sheen.setColorAt(1.0, bar.lighter(140));
            p.setBrush(sheen);
            p.drawRoundedRect(QRect(barX, y + rowH / 2 - 5, qMax(w, 4), 10), 5, 5);
            // 数值 + 占比（等宽）。占比 = 该项 / 全部之和——不能用 bar 长度那个比例：
            // 它带着入场动画的瞬时进度，动画途中会显示出"18%"这种与事实不符的占比。
            QFont mf = p.font();
            mf.setFamily(mono());
            p.setFont(mf);
            p.setPen(QPen(muted()));
            const double share = sumV > 0 ? 100.0 * double(entries_[i].second) / double(sumV) : 0.0;
            p.drawText(QRect(width() - valueW, y, valueW, rowH), Qt::AlignVCenter,
                       QString("%1 · %2%").arg(fmt(entries_[i].second))
                           .arg(share, 0, 'f', 0));
            p.setFont(f);
        }
    }

private:
    QVector<QPair<QString, qint64>> entries_;
    double sweep_ = 1.0;
    int hovered_ = -1;
    QPointer<QVariantAnimation> anim_;
    std::function<void(int)> onClick_;
};

// ---- 主题色卡：迷你界面预览（底色/侧栏/文本线/强调块/品牌点），点击选择主题 ----
class ThemeSwatch : public QFrame {
public:
    ThemeSwatch(const QString& name, QColor bg, QColor deep, QColor accent, QColor brand,
                QColor text, std::function<void()> onClick, QWidget* parent = nullptr)
        : QFrame(parent), name_(std::move(name)), bg_(std::move(bg)), deep_(std::move(deep)),
          accent_(std::move(accent)), brand_(std::move(brand)), text_(std::move(text)),
          onClick_(std::move(onClick)) {
        setFixedSize(108, 78);
        setCursor(Qt::PointingHandCursor);
    }

    void setSelected(bool on) {
        if (selected_ == on) return;
        selected_ = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        // 卡片底 = 主题窗口底色
        p.setPen(Qt::NoPen);
        p.setBrush(bg_);
        p.drawRoundedRect(r, 9, 9);
        // 左侧侧栏条 + 右侧内容示意线
        p.setBrush(deep_);
        p.drawRoundedRect(QRectF(7, 7, 20, height() - 26), 4, 4);
        p.setPen(QPen(QColor(255, 255, 255, 36), 3, Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 3; ++i)
            p.drawLine(QPointF(34, 13 + i * 9), QPointF(width() - 10.0, 13 + i * 9));
        p.setPen(Qt::NoPen);
        // 强调色块 + 品牌色圆点
        p.setBrush(accent_);
        p.drawRoundedRect(QRectF(34, height() - 26, 30, 8), 3, 3);
        p.setBrush(brand_);
        p.drawEllipse(QPointF(width() - 16, height() - 22), 5, 5);
        // 名称
        p.setPen(QPen(text_));
        QFont f = p.font();
        f.setPixelSize(10);
        p.setFont(f);
        p.drawText(QRectF(0, height() - 17, width(), 15), Qt::AlignCenter, name_);
        // 边框：选中 = 强调色 + 对勾；悬停 = 微亮
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(selected_ ? accent_ : QColor(255, 255, 255, hovered_ ? 70 : 26),
                      selected_ ? 2 : 1));
        p.drawRoundedRect(r, 9, 9);
        if (selected_) {
            p.setPen(QPen(accent_));
            QFont bf = p.font();
            bf.setPixelSize(11);
            bf.setBold(true);
            p.setFont(bf);
            p.drawText(QRectF(0, 4, width() - 6, 14), Qt::AlignRight, "✓");
        }
    }
    void mousePressEvent(QMouseEvent*) override {
        if (onClick_) onClick_();
    }
    void enterEvent(QEnterEvent*) override {
        hovered_ = true;
        update();
    }
    void leaveEvent(QEvent*) override {
        hovered_ = false;
        update();
    }

private:
    QString name_;
    QColor bg_, deep_, accent_, brand_, text_;
    std::function<void()> onClick_;
    bool selected_ = false;
    bool hovered_ = false;
};

// ---- 纵向柱状图：每日 Token 趋势，柱体自底部扫掠升起 ----
// 峰值柱用品牌色蜜金 + 亮色数值，其余强调色纵向渐变；底部日期 MM-DD。
class VBarChart : public QWidget {
public:
    explicit VBarChart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(106);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setStyleSheet("background:transparent;");  // 见 RingProgress：避免卡片内出现暗框
        setMouseTracking(true);  // 柱顶只有紧凑标注（34.2k），悬停给出确切数值
    }
    // 点击下钻：按横坐标定位柱子（与悬停同一套坐标换算）
    void setOnBarClick(std::function<void(int)> fn) {
        onClick_ = std::move(fn);
        setCursor(onClick_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !onClick_) return;
        const int n = entries_.size();
        if (n <= 0 || width() <= 4) return;
        const double slot = double(width() - 4) / n;
        const int idx = int((e->position().x() - 2) / qMax(1.0, slot));
        if (idx >= 0 && idx < n) onClick_(idx);
    }
    // 悬停柱：tooltip 给出「日期: 数值」，并把该柱提亮
    void mouseMoveEvent(QMouseEvent* e) override {
        const int n = entries_.size();
        int next = -1;
        if (n > 0 && width() > 4) {
            const double slot = double(width() - 4) / n;
            const int idx = int((e->position().x() - 2) / qMax(1.0, slot));
            if (idx >= 0 && idx < n) next = idx;
        }
        if (next == hovered_) return;
        hovered_ = next;
        if (hovered_ >= 0)
            setToolTip(QString("%1: %2").arg(entries_[hovered_].first)
                           .arg(QLocale().toString(entries_[hovered_].second)));
        else
            setToolTip(QString());
        update();
    }
    void leaveEvent(QEvent*) override {
        if (hovered_ < 0) return;
        hovered_ = -1;
        setToolTip(QString());
        update();
    }
    void setEntries(const QVector<QPair<QString, qint64>>& entries) {
        entries_ = entries;
        if (anim_) anim_->stop();
        auto* anim = new QVariantAnimation(this);
        anim_ = anim;
        anim->setDuration(450);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            sweep_ = v.toDouble();
            update();
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        bool allZero = true;
        for (const auto& e : entries_)
            if (e.second != 0) { allZero = false; break; }
        // 空表或全 0：走空状态（连续日期补 0 时不会画成一条贴着轴的零线）
        if (entries_.isEmpty() || allZero) {
            paintChartEmpty(p, rect(), i18n::trs("暂无用量数据", "no usage data yet"),
                            i18n::trs("Agent 上报 Token 后在此累积",
                                      "accumulates as agents report tokens"));
            return;
        }
        const int valueH = 18;  // 顶部数值标签区
        const int labelH = 20;  // 底部日期标签区
        const int axisW = 34;   // 左侧刻度区：没有刻度的柱状图读不出量级
        QRectF plot(axisW, valueH, width() - axisW - 2, height() - valueH - labelH);
        qint64 maxV = 1;
        for (const auto& e : entries_) maxV = qMax(maxV, e.second);
        // 网格：顶/中/底三条淡虚线 + 左侧量级刻度（顶=峰值，中=一半，底=0）
        p.setPen(QPen(line(), 1, Qt::DashLine));
        for (double r : {0.0, 0.5, 1.0}) {
            double y = plot.bottom() - plot.height() * r;
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        {
            QFont af = p.font();
            af.setPixelSize(9);
            p.setFont(af);
            QColor dim = muted();
            dim.setAlpha(190);
            p.setPen(QPen(dim));
            for (double r : {0.0, 0.5, 1.0}) {
                double y = plot.bottom() - plot.height() * r;
                // 刻度单位统一：同一根轴上不允许出现 "1.1M" 与 "571k" 混排——
                // 按轴最大值选定单位，全部刻度共用（读数时不必来回换算量级）
                const qint64 v = qint64(maxV * r);
                QString lbl;
                if (v == 0)
                    lbl = "0";  // 零点写 "0M" 别扭：它不属于任何量级
                else if (maxV >= 1000000)
                    lbl = QString::number(v / 1000000.0, 'f', 1) + "M";
                else if (maxV >= 1000)
                    lbl = QString::number(v / 1000.0, 'f', 0) + "k";
                else
                    lbl = QString::number(v);
                p.drawText(QRectF(0, y - 8, axisW - 5, 16), Qt::AlignRight | Qt::AlignVCenter, lbl);
            }
        }
        const int n = entries_.size();
        const double slot = plot.width() / n;
        const double barW = qMin(26.0, slot * 0.62);
        QFont f = p.font();
        f.setPixelSize(9);
        p.setFont(f);
        for (int i = 0; i < n; ++i) {
            double cx = plot.left() + slot * (i + 0.5);
            double ratio = double(entries_[i].second) / double(maxV) * sweep_;
            double h = plot.height() * ratio;
            QRectF bar(cx - barW / 2, plot.bottom() - h, barW, h);
            bool isMax = entries_[i].second == maxV && maxV > 1;
            QColor base = isMax ? brand() : accent();
            if (i == hovered_) base = base.lighter(125);  // 悬停柱提亮
            QLinearGradient sheen(bar.topLeft(), bar.bottomLeft());
            sheen.setColorAt(0.0, base.lighter(135));
            sheen.setColorAt(1.0, base.darker(108));
            p.setPen(Qt::NoPen);
            p.setBrush(sheen);
            if (h > 0.5) p.drawRoundedRect(bar, 3, 3);
            // 顶部数值（峰值亮色，其余弱化）；0 值柱不标注——否则轴线上会多出一个 "0"，
            // 和 Y 轴刻度撞在一起，像是两个不同的零点
            if (entries_[i].second > 0) {
                p.setPen(QPen(isMax ? brand() : muted()));
                p.drawText(QRectF(cx - slot / 2, bar.top() - valueH + 2, slot, valueH),
                           Qt::AlignCenter, fmtCompact(entries_[i].second));
            }
            // 底部日期 MM-DD
            QString day = entries_[i].first;
            if (day.size() >= 10) day = day.mid(5);
            p.setPen(QPen(muted()));
            p.drawText(QRectF(cx - slot / 2, plot.bottom() + 3, slot, labelH - 3),
                       Qt::AlignCenter, day);
        }
    }

private:
    QVector<QPair<QString, qint64>> entries_;
    double sweep_ = 1.0;
    int hovered_ = -1;
    QPointer<QVariantAnimation> anim_;
    std::function<void(int)> onClick_;
};

// ---- Toast：右下角气泡提示（成功绿 / 失败红），1.8s 自动消失 ----
class Toast : public QLabel {
public:
    static void show(QWidget* parent, const QString& msg, bool success = true) {
        auto* t = new Toast(parent, msg, success);
        t->popup();
    }

private:
    Toast(QWidget* parent, const QString& msg, bool success) : QLabel(msg, parent) {
        setObjectName(success ? "toastOk" : "toastErr");
        setAttribute(Qt::WA_DeleteOnClose);
        setStyleSheet(th(success
            ? "QLabel#toastOk { background:@okbg@; color:@ok@; border:1px solid @ok@;"
              " border-radius:8px; padding:10px 18px; font-size:12px; }"
            : "QLabel#toastErr { background:@errbg@; color:@danger@; border:1px solid @danger@;"
              " border-radius:8px; padding:10px 18px; font-size:12px; }"));
        adjustSize();
    }
    void popup() {
        QWidget* top = parentWidget();
        while (top && !top->isWindow()) top = top->parentWidget();
        if (!top) { deleteLater(); return; }
        // 提升为顶层独立气泡，置于右下角，淡出消失
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QPoint pos = top->pos() + QPoint(top->width() - width() - 24,
                                         top->height() - height() - 46);
        move(pos);
        QWidget::show();  // 显式调用基类，避免被静态 show(QString,bool) 遮蔽
        auto* fx = new QGraphicsOpacityEffect(this);
        fx->setOpacity(1.0);
        setGraphicsEffect(fx);
        auto* anim = new QPropertyAnimation(fx, "opacity", this);
        anim->setDuration(500);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        anim->setEasingCurve(QEasingCurve::InQuad);
        QTimer::singleShot(1500, this, [anim] { anim->start(QAbstractAnimation::DeleteWhenStopped); });
        QTimer::singleShot(2050, this, &QObject::deleteLater);
    }
};

// ---- Agent 状态卡片：名称 + 指示灯 + 任务摘要 + 最后活跃 ----
class AgentCard : public QFrame {
public:
    explicit AgentCard(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("agentCard");
        hoverGlow(this, "agentCard");
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(13, 9, 13, 10);
        lay->setSpacing(4);
        auto* head = new QHBoxLayout();
        name_ = new QLabel(this);
        name_->setStyleSheet("font-size:13px; font-weight:700; background:transparent;");
        dot_ = new QLabel(this);
        dot_->setFixedWidth(14);
        dot_->setAlignment(Qt::AlignCenter);
        status_ = new QLabel(this);
        status_->setStyleSheet(pillStyle(ok()));
        role_ = new QLabel(this);
        role_->setStyleSheet(th("color:@muted@; font-size:11px; background:transparent;"));
        head->addWidget(dot_);
        head->addWidget(name_);
        head->addWidget(status_);
        head->addStretch(1);
        head->addWidget(role_);
        lay->addLayout(head);
        task_ = new QLabel(this);
        task_->setStyleSheet(th("color:@text@; font-size:11px; background:transparent;"));
        task_->setWordWrap(true);
        lay->addWidget(task_);
        seen_ = new QLabel(this);
        seen_->setStyleSheet(th("color:@muted@; font-size:10px; background:transparent;"));
        lay->addWidget(seen_);
        // 在线呼吸灯：点亮的绿点每 900ms 明暗交替，离线则恒灰
        pulse_ = new QTimer(this);
        pulse_->setInterval(900);
        connect(pulse_, &QTimer::timeout, this, [this] {
            pulseOn_ = !pulseOn_;
            updateDot();
        });
        pulse_->start();
    }
    // 点击卡片 = 打开该 Agent 的操作入口（离线卡用来复制接入命令等，见 dashboard）。
    // 鼠标变成手型是"可点击"的通用暗示，避免用户以为卡片只是展示。
    void setOnActivate(std::function<void()> fn) {
        onClick_ = std::move(fn);
        setCursor(onClick_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setToolTip(onClick_ ? i18n::trs("点击查看可执行操作", "click for available actions")
                            : QString());
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onClick_) onClick_();
        QFrame::mouseReleaseEvent(e);
    }
    // lastSeenRel/lastSeenAbs：由调用方用 ui::relTime 生成的相对文案 + 本地绝对时刻
    // （tooltip），避免控件层依赖时间格式化，也避免在卡片里出现裸 ISO 串。
    void setAgent(const QString& name, const QString& status, const QString& role,
                  const QString& task, const QString& lastSeenRel, const QString& lastSeenAbs) {
        name_->setText(name);
        name_->setToolTip(name);
        online_ = status == "online";
        updateDot();
        status_->setText(online_ ? i18n::trs("在线", "online") : i18n::trs("离线", "offline"));
        status_->setStyleSheet(online_ ? pillStyle(ok()) : pillStyle(muted(), 40));
        role_->setText(role);
        role_->setToolTip(i18n::trs("角色：%1", "role: %1").arg(role));
        task_->setText(task.isEmpty() ? i18n::trs("（无当前任务）", "(no current task)") : task);
        task_->setToolTip(task);
        if (lastSeenRel.isEmpty()) {
            seen_->setText(i18n::trs("从未活跃", "never seen"));
            seen_->setToolTip(QString());
        } else {
            seen_->setText(online_ ? i18n::trs("活跃于 %1", "active %1").arg(lastSeenRel)
                                   : i18n::trs("最后活跃 %1", "last seen %1").arg(lastSeenRel));
            seen_->setToolTip(lastSeenAbs);
        }
    }

private:
    QLabel* name_ = nullptr;
    QLabel* dot_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* role_ = nullptr;
    QLabel* task_ = nullptr;
    QLabel* seen_ = nullptr;
    QTimer* pulse_ = nullptr;
    bool online_ = false;
    bool pulseOn_ = true;
    std::function<void()> onClick_;
    void updateDot() {
        dot_->setText(online_ ? QString("<span style='color:%1;'>●</span>")
                                    .arg(pulseOn_ ? ok().name() : ok().darker(150).name())
                              : QString("<span style='color:%1;'>●</span>")
                                    .arg(muted().name()));
    }
};

// ---- 换行标签：wordWrap 的 QLabel 会把 minimumSizeHint 按"最长单词宽度"折算高度，
// WrappedLabel 的完整定义在文件前部（InlineEmpty 等前部控件依赖它）：
// 给 wordWrap 标签的最小高度封顶，宽度够时按一行算，窄窗口下仍会换行。

// ---- 告警卡片：红=阻断 / 橙=警告 / 黄=注意 ----
// 前置声明：卡片需要一个与导航同源的矢量图标；定义在文件末尾（同为 inline）。
// 默认参数留在这里（文件前部的空状态控件就要按 3 参调用），末尾那处不再重复给默认值。
inline QIcon makeIcon(const QString& kind, const QColor& color, int px,
                      const QColor& selectedColor);

class AlertCard : public QFrame {
public:
    AlertCard(const QString& severity, const QString& text, QWidget* parent = nullptr)
        : QFrame(parent) {
        QColor c = severity == "critical" ? danger() : (severity == "warn" ? warn() : note());
        // 卡底按严重度淡染（12% 透明度），左侧色条 + 同色文字
        setStyleSheet(QString("QFrame { background:rgba(%1,%2,%3,28); border-left:4px solid %4;"
                              " border-radius:6px; padding:2px; }")
                          .arg(c.red())
                          .arg(c.green())
                          .arg(c.blue())
                          .arg(c.name()));
        // 高度随内容增长：长文案换行后卡片必须跟着长高，否则文字会被卡片裁掉
        // （审计脚本实测：超预算告警换行后被裁 33px）
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(12, 8, 12, 8);
        lay->setSpacing(8);
        // 矢量警示图标（替代 ⚠/⛔/🚫 emoji：颜色随严重度、形状跨平台一致）
        auto* ic = new QLabel(this);
        ic->setPixmap(makeIcon("errors", c, 15).pixmap(15, 15));
        ic->setStyleSheet("background:transparent;");
        ic->setFixedWidth(15);
        lay->addWidget(ic);
        auto* label = new WrappedLabel(2, text, this);
        label->setText(text);
        label->setToolTip(text);   // 换行/省略时全文仍可读
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        label->setStyleSheet(QString("color:%1; font-size:12px; background:transparent;").arg(c.name()));
        lay->addWidget(label, 1);
    }
    // 点击告警 → 跳到能处理它的面板（下钻）：告警卡是入口，不是终点
    void setOnClick(std::function<void()> fn) {
        onClick_ = std::move(fn);
        if (onClick_) {
            setCursor(Qt::PointingHandCursor);
            setToolTip(i18n::trs("点击前往处理", "click to handle"));
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onClick_) onClick_();
        QFrame::mouseReleaseEvent(e);
    }

private:
    std::function<void()> onClick_;
};

// ---- KPI 磁贴：一屏顶部的关键数字（标题 / 大号数值 / 说明 + 图标徽章）----
// 商业仪表盘惯例：数值用大号粗体 + 等宽数字（app 级 tnum），标题与说明弱化；
// 悬停时描边点亮，颜色随语义（预算黄→红、错误红、正常绿）而变。
class MetricTile : public QFrame {
public:
    MetricTile(const QString& iconKind, QWidget* parent = nullptr)
        : QFrame(parent), iconKind_(iconKind) {
        setObjectName("metricTile");
        setAttribute(Qt::WA_Hover);
        setMinimumHeight(68);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(14, 10, 14, 10);
        lay->setSpacing(10);
        auto* col = new QVBoxLayout;
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(1);
        title_ = new QLabel(this);
        title_->setStyleSheet(th("color:@muted@; font-size:11px; background:transparent;"
                                " letter-spacing:0.3px;"));
        value_ = new QLabel(this);
        // 数值用等宽字体：多张卡并排时数字位对齐，扫读时不会因字宽跳动（仪表盘惯例）
        value_->setStyleSheet(
            th("color:@text@; font-family:'@mono@'; font-size:23px; font-weight:700;"
               " background:transparent;"));
        value_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        // 数值 + 环比胶囊同一行：变化量挨着数字，扫一眼就知道"涨了还是跌了"
        delta_ = new QLabel(this);
        delta_->setVisible(false);
        auto* valueRow = new QHBoxLayout;
        valueRow->setContentsMargins(0, 0, 0, 0);
        valueRow->setSpacing(7);
        valueRow->addWidget(value_);
        valueRow->addWidget(delta_, 0, Qt::AlignVCenter);
        valueRow->addStretch(1);
        // 迷你可视化槽位：趋势小图（连续性指标）或微条（构成型指标）二选一
        miniHolder_ = new QWidget(this);
        miniHolder_->setStyleSheet("background:transparent;");
        miniLay_ = new QVBoxLayout(miniHolder_);
        miniLay_->setContentsMargins(0, 2, 0, 0);  // 小图铺满可用宽度（图标在右侧列外）
        miniLay_->setSpacing(2);
        miniHolder_->setVisible(false);
        spark_ = new Sparkline(miniHolder_);
        strip_ = new MicroStrip(miniHolder_);
        miniLay_->addWidget(spark_);
        miniLay_->addWidget(strip_);
        spark_->setVisible(false);
        strip_->setVisible(false);
        cap_ = new QLabel(this);
        cap_->setStyleSheet(th("color:@muted@; font-size:11px; background:transparent;"));
        col->addWidget(title_);
        col->addLayout(valueRow);
        col->addWidget(miniHolder_);
        col->addWidget(cap_);
        lay->addLayout(col, 1);
        icon_ = new QLabel(this);
        icon_->setFixedSize(34, 34);
        icon_->setAlignment(Qt::AlignCenter);
        lay->addWidget(icon_, 0, Qt::AlignTop);
        setStyleSheet(th("QFrame#metricTile { background:@card@; border:1px solid @line@;"
                         " border-radius:12px; }"
                         "QFrame#metricTile:hover { background:@fieldhover@;"
                         " border:1px solid @accent@; }"));
        paintIcon(accent());
    }
    // valueColor / iconColor 传无效色则回落为主题常规色
    void set(const QString& title, const QString& value, const QString& caption,
             const QColor& valueColor = QColor(), const QColor& iconColor = QColor()) {
        title_->setText(title);
        value_->setText(value);
        cap_->setText(caption);
        value_->setStyleSheet(
            th(QString("color:%1; font-family:'@mono@'; font-size:23px; font-weight:700;"
                       " background:transparent;")
                   .arg((valueColor.isValid() ? valueColor : text()).name())));
        paintIcon(iconColor.isValid() ? iconColor : accent());
    }
    // 趋势小图：给连续性指标（Token 消耗等）补上"走势"这一维
    void setSparkline(const QVector<qint64>& series, const QColor& c) {
        spark_->setSeries(series, c.isValid() ? c : accent());
        spark_->setVisible(series.size() >= 2);
        strip_->setVisible(false);
        miniHolder_->setVisible(series.size() >= 2);
        setMinimumHeight(series.size() >= 2 ? 100 : 68);
    }
    // 微条：给构成型指标（在线/离线、严重度构成）表达比例
    void setMicroStrip(const QVector<QPair<QColor, int>>& segs, const QString& tip) {
        strip_->setSegments(segs);
        strip_->setTooltipText(tip);
        const bool use = !segs.isEmpty();
        strip_->setVisible(use);
        spark_->setVisible(false);
        miniHolder_->setVisible(use);
        setMinimumHeight(use ? 84 : 68);
    }
    void setToolTipAll(const QString& tip) {
        setToolTip(tip);
        value_->setToolTip(tip);
        title_->setToolTip(tip);
        cap_->setToolTip(tip);
    }
    // 环比/占比胶囊：小号语义色徽标贴在数值右侧；传空文本即隐藏
    void setDelta(const QString& text, const QColor& c) {
        if (!deltaTip_.isEmpty()) delta_->setToolTip(deltaTip_);
        if (text.isEmpty()) {
            delta_->setVisible(false);
            return;
        }
        delta_->setText(text);
        delta_->setStyleSheet(th(QString("color:%1; background:rgba(%2,%3,%4,38);"
                                         " border-radius:7px; padding:1px 7px;"
                                         " font-size:11px; font-weight:600;")
                                     .arg(c.lighter(125).name())
                                     .arg(c.red())
                                     .arg(c.green())
                                     .arg(c.blue())));
        delta_->setVisible(true);
    }
    // 胶囊的含义（"跟什么比"）单独说明：只有百分比时用户不知道参照系
    void setDeltaTooltip(const QString& tip) {
        deltaTip_ = tip;
        delta_->setToolTip(tip);
    }

private:
    QString iconKind_;
    QLabel* title_ = nullptr;
    QLabel* value_ = nullptr;
    QLabel* cap_ = nullptr;
    QLabel* delta_ = nullptr;
    QLabel* icon_ = nullptr;
    QWidget* miniHolder_ = nullptr;
    QVBoxLayout* miniLay_ = nullptr;
    Sparkline* spark_ = nullptr;
    MicroStrip* strip_ = nullptr;
    QString deltaTip_;
    QColor iconPainted_;
    void paintIcon(const QColor& c) {
        if (iconPainted_.isValid() && iconPainted_ == c) return;  // 颜色未变则跳过重绘
        iconPainted_ = c;
        icon_->setPixmap(makeIcon(iconKind_, c, 18).pixmap(18, 18));
        icon_->setStyleSheet(th(QString("background:rgba(%1,%2,%3,34); border-radius:10px;")
                                    .arg(c.red())
                                    .arg(c.green())
                                    .arg(c.blue())));
    }
};

// ---- 可折叠区块卡片（用户记忆分组等）----
class SectionCard : public QWidget {
public:
    SectionCard(const QString& title, QWidget* content, QWidget* parent = nullptr)
        : QWidget(parent), content_(content) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        auto* toggle = new QToolButton(this);
        toggle->setText("▾  " + title);
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setStyleSheet(th(
            "QToolButton { background:@card@; border:1px solid @line@;"
            " border-radius:8px; padding:10px 14px; font-size:13px; font-weight:600;"
            " text-align:left; }"
            "QToolButton:hover { border-color:@accent@; }"));
        content_->setStyleSheet(th(
            "QWidget { background:@field@; border:1px solid @line@;"
            " border-top:none; border-radius:0 0 8px 8px; }"));
        lay->addWidget(toggle);
        lay->addWidget(content_);
        connect(toggle, &QToolButton::toggled, this, [this, toggle](bool on) {
            content_->setVisible(on);
            toggle->setText(on ? "▾  " + toggle->text().mid(3)
                               : "▸  " + toggle->text().mid(3));
        });
    }

private:
    QWidget* content_;
};

// ---- 空状态母题：蜜金蜂巢三六边形 + 标题 + 出路提示（品牌触点，见 docs/brand.md §3）----
// 品牌规范指定空状态插画用六边形母题；原先在其上再叠一个 emoji，
// 与新的矢量图标体系不一致，已移除（母题本身即插画）。
class HexEmptyState : public QWidget {
public:
    HexEmptyState(const QString& title, const QString& hint, QWidget* parent = nullptr)
        : QWidget(parent), title_(title), hint_(hint) {
        setMinimumHeight(170);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(width() / 2.0, 64.0);
        const qreal r = 22.0;
        // 蜂巢三六边形：共享边拼接，描边圆角连接（与 logo 同构，浅描边弱化）
        QPen pen(brand(), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        pen.setColor(QColor(brand().red(), brand().green(), brand().blue(), 130));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        auto hex = [&](const QPointF& ctr) {
            QPolygonF h;
            for (int i = 0; i < 6; ++i) {
                qreal a = M_PI / 180.0 * (60.0 * i - 30.0);
                h << ctr + QPointF(r * std::cos(a), r * std::sin(a));
            }
            p.drawPolygon(h);
        };
        hex(c + QPointF(0, -r * 1.02));
        hex(c + QPointF(-r * 0.9, r * 0.55));
        hex(c + QPointF(r * 0.9, r * 0.55));
        p.setPen(Qt::NoPen);
        p.setBrush(accent());
        p.drawEllipse(c + QPointF(0, -r * 1.02), r * 0.3, r * 0.3);
        // 标题 + 出路提示
        p.setPen(QPen(text()));
        QFont f = p.font();
        f.setPixelSize(13);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(0, 116, width(), 20), Qt::AlignCenter, title_);
        f.setPixelSize(11);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QPen(muted()));
        p.drawText(QRect(24, 138, width() - 48, 40), Qt::AlignHCenter | Qt::TextWordWrap, hint_);
    }

private:
    QString title_, hint_;
};

// 蜂巢母题注册为文档图片资源：HTML 空状态里 <img src="hexmotif"> 引用，
// 颜色随当前主题（蜜金描边 + 强调色入口点，见 docs/brand.md §3）
inline void attachHexMotif(QTextDocument* doc) {
    const int w = 96, h = 70, r = 15;
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(brand().red(), brand().green(), brand().blue(), 128), 3.5, Qt::SolidLine,
             Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto hex = [&](const QPointF& c) {
        QPolygonF poly;
        for (int i = 0; i < 6; ++i) {
            qreal a = M_PI / 180.0 * (60.0 * i - 30.0);
            poly << c + QPointF(r * std::cos(a), r * std::sin(a));
        }
        p.drawPolygon(poly);
    };
    const QPointF top(w / 2.0, 30.0);
    hex(top);
    hex(top + QPointF(-r * 0.9 * 1.68, r * 1.34));
    hex(top + QPointF(r * 0.9 * 1.68, r * 1.34));
    p.setPen(Qt::NoPen);
    p.setBrush(accent());
    p.drawEllipse(top, r * 0.3, r * 0.3);
    p.end();
    doc->addResource(QTextDocument::ImageResource, QUrl("hexmotif"), pm);
}

// ---- 线性图标：Lucide 几何（MIT，24 网格 stroke-2 圆角端点）+ 主题色注入，
//      QSvgRenderer 渲染为高清位图。替代 emoji 与手绘形状。
// kind: overview / knowledge / skills / memory / messages / errors / audit / gear
// （默认参数在文件前部的 AlertCard 前置声明处给出）
inline QIcon makeIcon(const QString& kind, const QColor& color, int px,
                      const QColor& selectedColor) {
    QString inner;
    if (kind == "overview")  // 品牌母题：六边形 + 蜂巢入口点
        inner = "<path d='M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 "
                "1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z'/>"
                "<circle cx='12' cy='12' r='2.3' fill='%1' stroke='none'/>";
    else if (kind == "knowledge")
        inner = "<path d='M2 3h6a4 4 0 0 1 4 4v14a3 3 0 0 0-3-3H2z'/>"
                "<path d='M22 3h-6a4 4 0 0 0-4 4v14a3 3 0 0 1 3 3h7z'/>";
    else if (kind == "skills")
        inner = "<polygon points='13 2 3 14 12 14 11 22 21 10 12 10 13 2'/>";
    else if (kind == "memory")
        inner = "<polygon points='12 2 2 7 12 12 22 7 12 2'/>"
                "<polyline points='2 17 12 22 22 17'/>"
                "<polyline points='2 12 12 17 22 12'/>";
    else if (kind == "messages")
        inner = "<path d='M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z'/>";
    else if (kind == "errors")
        inner = "<path d='m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3z'/>"
                "<line x1='12' x2='12' y1='9' y2='13'/>"
                "<line x1='12' x2='12.01' y1='17' y2='17'/>";
    else if (kind == "audit")
        inner = "<circle cx='12' cy='12' r='10'/>"
                "<polyline points='12 6 12 12 16 14'/>";
    else if (kind == "usage")  // 用量分析：错落柱状图
        inner = "<line x1='4' x2='4' y1='15' y2='20'/><line x1='10' x2='10' y1='8' y2='20'/>"
                "<line x1='16' x2='16' y1='12' y2='20'/><line x1='22' x2='22' y1='5' y2='20'/>";
    else if (kind == "palette")  // 外观
        inner = "<circle cx='13.5' cy='6.5' r='.5'/><circle cx='17.5' cy='10.5' r='.5'/>"
                "<circle cx='8.5' cy='7.5' r='.5'/><circle cx='6.5' cy='12.5' r='.5'/>"
                "<path d='M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10c.9 0 1.6-.7 1.6-1.7 0-.4-.2-.8-.4-1.1"
                "-.3-.3-.4-.7-.4-1.1a1.6 1.6 0 0 1 1.7-1.7h2c3 0 5.5-2.5 5.5-5.5C22 6 17.5 2 12 2z'/>";
    else if (kind == "database")  // 数据与备份
        inner = "<ellipse cx='12' cy='5' rx='9' ry='3'/>"
                "<path d='M3 5v14a9 3 0 0 0 18 0V5'/><path d='M3 12a9 3 0 0 0 18 0'/>";
    else if (kind == "bell")  // 通知
        inner = "<path d='M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9'/>"
                "<path d='M10.3 21a1.94 1.94 0 0 0 3.4 0'/>";
    else if (kind == "plug")  // 接入 / API
        inner = "<path d='M12 22v-5'/><path d='M9 8V2'/><path d='M15 8V2'/>"
                "<path d='M18 8v5a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V8Z'/>";
    else if (kind == "refresh")  // 更新
        inner = "<path d='M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8'/>"
                "<path d='M21 3v5h-5'/>"
                "<path d='M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16'/>"
                "<path d='M8 16H3v5'/>";
    else if (kind == "info")  // 关于
        inner = "<circle cx='12' cy='12' r='10'/><path d='M12 16v-4'/><path d='M12 8h.01'/>";
    else if (kind == "key")  // 密钥 / 接入命令
        inner = "<path d='m15.5 7.5 2.3 2.3a1 1 0 0 0 1.4 0l2.1-2.1a1 1 0 0 0 0-1.4L19 4'/>"
                "<path d='m21 2-9.6 9.6'/><circle cx='7.5' cy='15.5' r='5.5'/>";
    else if (kind == "gear")
        inner = "<path d='M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 "
                "2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 "
                "2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 "
                "2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 "
                "2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 "
                "0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 "
                "2 0 0 0-2-2z'/>"
                "<circle cx='12' cy='12' r='3'/>";

    auto render = [&](const QColor& c) {
        const QString colorName = c.name();
        const QString body = inner.contains("%1") ? inner.arg(colorName) : inner;
        const QString svg = QString("<svg xmlns='http://www.w3.org/2000/svg' width='%1' height='%1' "
                                    "viewBox='0 0 24 24' fill='none' stroke='%2' stroke-width='2' "
                                    "stroke-linecap='round' stroke-linejoin='round'>%3</svg>")
                                .arg(px * 2)
                                .arg(colorName)
                                .arg(body);
        QPixmap pm(px * 2, px * 2);
        pm.fill(Qt::transparent);
        QSvgRenderer r(svg.toUtf8());
        QPainter p(&pm);
        r.render(&p);
        p.end();
        return pm;
    };
    QIcon icon;
    icon.addPixmap(render(color));
    if (selectedColor.isValid()) icon.addPixmap(render(selectedColor), QIcon::Selected);
    return icon;
}

}  // namespace ui
