#include "graph_panel.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <thread>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

namespace {

using ah::KnowledgeGraph;
using ah::KnowledgeGraphEdge;
using ah::KnowledgeGraphNode;

// 力导向画布：斥力（O(n²) 两两）+ 弹簧（沿边）+ 向心，阻尼收敛后停帧省 CPU。
// 滚轮缩放（以鼠标为中心）/ 空白处拖拽平移 / 节点拖拽（物理跳过被拖节点）/
// 点击选中回调。位置按 uuid 记忆，setGraph 时新节点就近初始化、旧节点保留。
class GraphCanvas : public QWidget {
public:
    struct Node {
        QString uuid;
        QString title;
        QString category;
        double x = 0, y = 0, vx = 0, vy = 0;
        int degree = 0;
        bool fixed = false;  // 用户拖拽中：物理跳过，位置跟手
    };
    struct Edge {
        int a = -1, b = -1;
        double similarity = 0;
    };

    explicit GraphCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(320);
        setMouseTracking(true);
        setCursor(Qt::ArrowCursor);
        // 物理模拟：收敛（总动能低于阈值连续若干帧）后停帧，setGraph/拖拽时重启
        simTimer_ = new QTimer(this);
        connect(simTimer_, &QTimer::timeout, this, [this] { tick(); });
        simTimer_->start(16);
    }

    void setGraph(const KnowledgeGraph& g) {
        // 保留旧位置：uuid 相同的节点位置/被拖状态不动，新节点从中心附近随机展开。
        // 拖拽/选中状态按 uuid 重映射（审计 M2）：索引在重建后语义改变，裸下标
        // 会让"另一个节点被钉到光标上"、高亮环落错节点
        const QString dragUuid = dragIdx_ >= 0 && dragIdx_ < nodes_.size()
                                     ? nodes_[dragIdx_].uuid
                                     : QString();
        const QString selUuid = selected_ >= 0 && selected_ < nodes_.size()
                                    ? nodes_[selected_].uuid
                                    : QString();
        QHash<QString, QPointF> keep;
        for (const auto& n : nodes_) keep.insert(n.uuid, QPointF(n.x, n.y));
        nodes_.clear();
        edges_.clear();
        byUuid_.clear();
        for (const auto& n : g.nodes) {
            Node node;
            node.uuid = QString::fromStdString(n.uuid);
            node.title = QString::fromStdString(n.title);
            node.category = QString::fromStdString(n.category);
            node.degree = static_cast<int>(n.degree);
            const auto it = keep.constFind(node.uuid);
            if (it != keep.constEnd()) {
                node.x = it->x();
                node.y = it->y();
            } else {
                const double ang = (nodes_.size() % 12) * 0.5236;  // 錯开初始角
                node.x = 300 + 120 * std::cos(ang) + (rand01() - 0.5) * 60;
                node.y = 260 + 120 * std::sin(ang) + (rand01() - 0.5) * 60;
            }
            byUuid_.insert(node.uuid, static_cast<int>(nodes_.size()));
            nodes_.push_back(node);
        }
        for (const auto& e : g.edges) {
            const auto a = byUuid_.value(QString::fromStdString(e.a_uuid), -1);
            const auto b = byUuid_.value(QString::fromStdString(e.b_uuid), -1);
            if (a >= 0 && b >= 0) edges_.push_back({a, b, e.similarity});
        }
        // 重映射拖拽/选中（uuid 失效即取消对应状态）
        dragIdx_ = byUuid_.value(dragUuid, -1);
        if (dragIdx_ >= 0) nodes_[dragIdx_].fixed = true;  // 拖拽仍在途：恢复固定态
        selected_ = byUuid_.value(selUuid, -1);
        running_ = true;  // 新拓扑重启物理
        update();
    }

    void relayout() {
        // 重新布局：位置全部重掷 + 重启物理 + 视口复位（审计 L4：用户平移很远
        // 后点重布局，节点落在视口外会看似按钮失效）
        rng_.seed(std::random_device{}());  // 每次点都是一份新布局
        for (auto& n : nodes_) {
            const double ang = rand01() * 6.2832;
            n.x = 300 + 200 * std::cos(ang);
            n.y = 260 + 200 * std::sin(ang);
            n.vx = n.vy = 0;
        }
        panX_ = panY_ = 0;
        scale_ = 1.0;
        running_ = true;
    }

    void setOnSelect(std::function<void(const QString& uuid)> fn) { onSelect_ = std::move(fn); }

    int nodeCount() const { return static_cast<int>(nodes_.size()); }
    int edgeCount() const { return static_cast<int>(edges_.size()); }
    bool running() const { return running_; }

    // 同步把物理跑到它自己的停止条件。给测试用：UI harness 要的是可复现的最终布局，
    // 不能取决于拍摄前定时器恰好跑了多少帧（阻尼较弱，5 个节点也要跑几秒才停——
    // 两次运行因此差 1% 的像素）。有上限，万一不振荡收敛也不至于卡死。
    void settle(int maxTicks = 50000) {
        int n = 0;
        while (running_ && n < maxTicks) {
            tick();
            ++n;
        }
    }

protected:
    // 单帧物理：斥力 O(n²)（n≤300 安全）+ 边弹簧 + 轻向心力，阻尼 0.85
    void tick() {
        if (!running_ || nodes_.isEmpty()) return;
        constexpr double kRepulse = 9000.0, kSpring = 0.02, kRest = 130.0, kCenter = 0.003,
                         kDamp = 0.85, kMinMove = 0.5;
        double energy = 0;
        for (int i = 0; i < nodes_.size(); ++i) {
            for (int j = i + 1; j < nodes_.size(); ++j) {
                double dx = nodes_[j].x - nodes_[i].x, dy = nodes_[j].y - nodes_[i].y;
                double d2 = dx * dx + dy * dy;
                if (d2 < 1.0) { dx = rand01() - 0.5; dy = rand01() - 0.5; d2 = 1.0; }
                const double d = std::sqrt(d2);
                const double f = kRepulse / d2;
                const double fx = f * dx / d, fy = f * dy / d;
                nodes_[i].vx -= fx; nodes_[i].vy -= fy;
                nodes_[j].vx += fx; nodes_[j].vy += fy;
            }
        }
        for (const auto& e : edges_) {
            auto& a = nodes_[e.a];
            auto& b = nodes_[e.b];
            double dx = b.x - a.x, dy = b.y - a.y;
            const double d = std::sqrt(std::max(dx * dx + dy * dy, 1.0));
            const double f = kSpring * (d - kRest) / d;
            a.vx += f * dx; a.vy += f * dy;
            b.vx -= f * dx; b.vy -= f * dy;
        }
        for (auto& n : nodes_) {
            if (n.fixed) { n.vx = n.vy = 0; continue; }
            n.vx -= (n.x - 300) * kCenter;
            n.vy -= (n.y - 260) * kCenter;
            n.x += std::clamp(n.vx * kDamp, -20.0, 20.0);
            n.y += std::clamp(n.vy * kDamp, -20.0, 20.0);
            energy += std::fabs(n.vx) + std::fabs(n.vy);
            n.vx *= kDamp; n.vy *= kDamp;
        }
        // 拖拽中的节点跟手（世界坐标由 widget 坐标反算，见 nodeAt/drag 逻辑）
        if (dragIdx_ >= 0 && dragIdx_ < nodes_.size()) {
            auto& n = nodes_[dragIdx_];
            n.x = dragX_; n.y = dragY_;
        }
        update();
        if (energy < kMinMove * nodes_.size()) {
            ++settled_;
            if (settled_ > 30) running_ = false;  // 收敛后停帧省 CPU
        } else {
            settled_ = 0;
        }
    }

    // 画布自己的随机源，固定种子：初始摆放只取决于数据与操作顺序，同一份数据每次
    // 打开图谱的布局都一样（位置有记忆可循）。原先用全局 std::rand()——第三方库内部
    // 也会消耗同一个全局状态，初始抖动随运行而变
    double rand01() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), ui::th("color:@deep@").isEmpty()
                                    ? palette().window().color()
                                    : QColor(0, 0, 0, 0));
        painter.fillRect(rect(), QColor(0, 0, 0, 24));
        if (nodes_.isEmpty()) return;
        painter.save();
        painter.translate(panX_, panY_);
        painter.scale(scale_, scale_);
        // 边：透明度随相似度，选中节点相邻边加亮
        for (const auto& e : edges_) {
            const auto& a = nodes_[e.a];
            const auto& b = nodes_[e.b];
            const bool hot = selected_ >= 0 && (e.a == selected_ || e.b == selected_);
            QPen pen(hot ? ui::accent() : QColor(255, 255, 255, 28 + int(e.similarity * 60)));
            pen.setWidthF(hot ? 2.0 : 1.0);
            painter.setPen(pen);
            painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
        }
        // 节点：半径随度数，填充按 category 哈希取 6 色板（同消息头像惯例）
        static const char* kPalettes[] = {"#0ea5e9", "#22c55e", "#f59e0b", "#a78bfa",
                                          "#f472b6", "#34d399"};
        for (int i = 0; i < nodes_.size(); ++i) {
            const auto& n = nodes_[i];
            quint32 h = 0;
            for (QChar c : n.category) h = h * 31 + c.unicode();
            const QColor fill(kPalettes[h % 6]);
            const double r = 6 + std::min(n.degree, 12);
            painter.setPen(QPen(whiteOr(fill), 1.2));
            painter.setBrush(fill);
            painter.drawEllipse(QPointF(n.x, n.y), r, r);
            if (i == selected_)
                painter.setPen(QPen(ui::accent(), 2));
            else
                painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::NoBrush);
            if (i == selected_) painter.drawEllipse(QPointF(n.x, n.y), r + 3, r + 3);
            // 标题：放大或选中时显示（n≤120 时小倍率也显示，防止密图文字糊成一片）
            if (scale_ > 0.85 || nodes_.size() <= 120 || i == selected_) {
                painter.setPen(QColor(255, 255, 255, 210));
                painter.drawText(QPointF(n.x + r + 3, n.y + 4), elide(n.title, 24));
            }
        }
        painter.restore();
    }

    static QColor whiteOr(const QColor& fill) {
        return fill.lightness() > 128 ? QColor(20, 20, 20) : QColor(245, 245, 245);
    }
    static QString elide(const QString& s, int n) {
        return s.size() <= n ? s : s.left(n) + "…";
    }

    // 世界 → 屏幕：screen = world * scale + pan
    QPointF toScreen(const QPointF& world) const {
        return QPointF(world.x() * scale_ + panX_, world.y() * scale_ + panY_);
    }
    QPointF toWorld(const QPointF& screen) const {
        return QPointF((screen.x() - panX_) / scale_, (screen.y() - panY_) / scale_);
    }
    int nodeAt(const QPointF& screen) const {
        int best = -1;
        double bestD = 18.0;  // 屏幕像素半径
        for (int i = 0; i < nodes_.size(); ++i) {
            const QPointF s = toScreen(QPointF(nodes_[i].x, nodes_[i].y));
            const double d = std::hypot(s.x() - screen.x(), s.y() - screen.y());
            const double r = (6 + std::min(nodes_[i].degree, 12)) * scale_ + 4;
            if (d <= std::max(r, bestD)) { best = i; bestD = d; }
        }
        return best;
    }

    void wheelEvent(QWheelEvent* e) override {
        const double factor = e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15;
        const double newScale = std::clamp(scale_ * factor, 0.2, 4.0);
        // 以鼠标为中心缩放：光标下的世界点缩放后仍在光标下
        const QPointF before = toWorld(e->position());
        scale_ = newScale;
        const QPointF after = toWorld(e->position());
        panX_ += (after.x() - before.x()) * scale_;
        panY_ += (after.y() - before.y()) * scale_;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const int idx = nodeAt(e->position());
        if (idx >= 0) {
            dragIdx_ = idx;
            selected_ = idx;
            const QPointF w = toWorld(e->position());
            dragX_ = w.x();
            dragY_ = w.y();
            nodes_[idx].fixed = true;
            running_ = true;
            if (onSelect_) onSelect_(nodes_[idx].uuid);
        } else {
            panning_ = true;
            panStart_ = e->position();
            panStartX_ = panX_;
            panStartY_ = panY_;
            if (selected_ >= 0) { selected_ = -1; if (onSelect_) onSelect_(QString()); }
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragIdx_ >= 0) {
            const QPointF w = toWorld(e->position());
            dragX_ = w.x();
            dragY_ = w.y();
            // 拖拽中物理可能已收敛停帧（审计 H1）：不重启的话 mouseMove 只改
            // dragX_/dragY_ 而无人应用到节点——节点视觉上脱手冻结
            running_ = true;
            settled_ = 0;
        } else if (panning_) {
            panX_ = panStartX_ + (e->position().x() - panStart_.x());
            panY_ = panStartY_ + (e->position().y() - panStart_.y());
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            if (dragIdx_ >= 0 && dragIdx_ < nodes_.size()) nodes_[dragIdx_].fixed = false;
            dragIdx_ = -1;
            panning_ = false;
            running_ = true;  // 松手后让物理重新收敛一下
            settled_ = 0;
        }
    }

private:
    QList<Node> nodes_;
    QList<Edge> edges_;
    QHash<QString, int> byUuid_;
    QTimer* simTimer_ = nullptr;
    bool running_ = false;
    int settled_ = 0;
    std::mt19937 rng_{0x9E3779B9u};  // 初始摆放的随机源（固定种子；relayout 时重新播种）
    double scale_ = 1.0;
    double panX_ = 0, panY_ = 0;
    bool panning_ = false;
    QPointF panStart_;
    double panStartX_ = 0, panStartY_ = 0;
    int dragIdx_ = -1;
    double dragX_ = 0, dragY_ = 0;
    int selected_ = -1;
    std::function<void(const QString& uuid)> onSelect_;
};

}  // namespace

GraphPanel::GraphPanel(ah::Platform& platform, QWidget* parent) : PanelBase(platform, parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);
    buildHeader(root, "overview", "图谱", "Graph",
                "语义关联知识图谱 · 向量 kNN 相似边",
                "Semantic knowledge graph · vector kNN similarity edges");

    // ---- 工具行：统计 + 标签筛选 + 重新布局 + 刷新 ----
    auto* bar = new QHBoxLayout;
    statsLabel_ = new QLabel(this);
    statsLabel_->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    bar->addWidget(statsLabel_, 1);
    tagCombo_ = new QComboBox(this);
    tagCombo_->setMinimumWidth(120);
    tagCombo_->setToolTip(i18n::trs("按标签过滤节点", "Filter nodes by tag"));
    bar->addWidget(tagCombo_);
    relayoutBtn_ = new QPushButton(i18n::trs("重新布局", "Re-layout"), this);
    relayoutBtn_->setToolTip(i18n::trs("清空缓存位置，重新计算力导向布局",
                                       "Discard cached positions and recompute the layout"));
    bar->addWidget(relayoutBtn_);
    refreshBtn_ = new QPushButton(i18n::trs("刷新快照", "Refresh"), this);
    bar->addWidget(refreshBtn_);
    root->addLayout(bar);

    // ---- 主体：画布 + 详情栏 ----
    auto* body = new QHBoxLayout;
    graphCard_ = new QGroupBox(i18n::trs("语义知识图谱", "Semantic Knowledge Graph"), this);
    graphCard_->setObjectName("card");
    auto* gl = new QVBoxLayout(graphCard_);
    gl->setContentsMargins(8, 8, 8, 8);
    auto* canvas = new GraphCanvas(graphCard_);
    graphView_ = canvas;
    gl->addWidget(canvas, 1);
    root->addWidget(graphCard_, 1);

    detailCard_ = new QGroupBox(i18n::trs("节点详情", "Node Details"), this);
    detailCard_->setObjectName("card");
    detailCard_->setFixedWidth(280);
    auto* dl = new QVBoxLayout(detailCard_);
    dl->setSpacing(6);
    detailTitle_ = new QLabel(this);
    detailTitle_->setWordWrap(true);
    detailTitle_->setStyleSheet(ui::th("font-size:13px; font-weight:700; color:@text@;"));
    detailMeta_ = new QLabel(this);
    detailMeta_->setWordWrap(true);
    detailMeta_->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    detailTags_ = new QLabel(this);
    detailTags_->setWordWrap(true);
    detailTags_->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    openKnowledgeBtn_ = new QPushButton(i18n::trs("在知识库中打开", "Open in Knowledge"), this);
    openKnowledgeBtn_->setEnabled(false);
    dl->addWidget(detailTitle_);
    dl->addWidget(detailMeta_);
    dl->addWidget(detailTags_);
    dl->addStretch(1);
    dl->addWidget(openKnowledgeBtn_, 0, Qt::AlignLeft);
    body->addWidget(graphCard_, 1);
    body->addWidget(detailCard_);
    root->addLayout(body, 1);

    auto* hint = new QLabel(this);
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    hint->setText(digestHintText());
    hint->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    root->addWidget(hint);

    connect(relayoutBtn_, &QPushButton::clicked, this,
            [this] { static_cast<GraphCanvas*>(graphView_)->relayout(); });
    connect(refreshBtn_, &QPushButton::clicked, this, [this] { lastSig_.clear(); refresh(); });
    // 标签筛选（审计 H2）：此前无任何信号连接，改选标签画面毫无反应
    connect(tagCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { applyTagFilter(); });
    connect(openKnowledgeBtn_, &QPushButton::clicked, this,
            [this] { drillTo("knowledge"); });
    // 节点选中：右侧详情联动
    canvas->setOnSelect([this](const QString& uuid) { showNodeDetail(uuid); });
    clearNodeDetail();
    refresh();
}

void GraphPanel::retranslate() {
    PanelBase::retranslate();
    graphCard_->setTitle(i18n::trs("语义知识图谱", "Semantic Knowledge Graph"));
    detailCard_->setTitle(i18n::trs("节点详情", "Node Details"));
    statsLabel_->setText(i18n::trs("%1 个节点 · %2 条相似边", "%1 nodes · %2 edges")
                             .arg(graph_.nodes.size())
                             .arg(graph_.edges.size()));
    relayoutBtn_->setText(i18n::trs("重新布局", "Re-layout"));
    relayoutBtn_->setToolTip(i18n::trs("清空缓存位置，重新计算力导向布局",
                                       "Discard cached positions and recompute the layout"));
    refreshBtn_->setText(i18n::trs("刷新快照", "Refresh"));
    tagCombo_->setItemText(0, i18n::trs("全部标签", "All tags"));
    tagCombo_->setToolTip(i18n::trs("按标签过滤节点", "Filter nodes by tag"));
    openKnowledgeBtn_->setText(i18n::trs("在知识库中打开", "Open in Knowledge"));
    // hint 行是构造期的局部 QLabel：语言切换时按 objectName 就地重译
    for (QLabel* w : findChildren<QLabel*>())
        if (w->objectName() == "muted" && w->wordWrap()) w->setText(digestHintText());
    clearNodeDetail();
    refresh();
}

QString GraphPanel::digestHintText() {
    return i18n::trs(
        "节点 = 最新知识条目（大小 = 关联边数，颜色 = 类别）；边 = 向量相似。"
        "滚轮缩放 · 拖拽平移 · 点击节点看详情 · 拖动节点可固定位置。",
        "Nodes = latest entries (size = degree, color = category); edges = vector similarity. "
        "Wheel to zoom · drag to pan · click a node for details · drag a node to pin it.");
}

void GraphPanel::settleLayout() {
    static_cast<GraphCanvas*>(graphView_)->settle();
}

void GraphPanel::refresh() {
    // 轻量签名检查：最新条目的 created_at 没变就不重取快照（3s 轮询不重算图）。
    // 首拍（lastSig_ 空）必取一次建立基线（审计 M4：尾码拼接会让每次手动刷新
    // 后必然多付一次 200 节点全量重取，已删）
    std::vector<ah::KnowledgeEntry> latest;
    std::string err;
    if (!platform_.knowledgeList(1, "", latest, err)) {
        ui::Toast::show(this, ui::humanError(QString::fromStdString(err)), false);
        return;
    }
    const std::string sig =
        latest.empty() ? std::string("empty") : latest.front().created_at;
    if (!lastSig_.empty() && sig == lastSig_) return;
    lastSig_ = sig;
    if (!platform_.knowledgeGraphSnapshot(200, 5, graph_, err)) {
        ui::Toast::show(this, ui::humanError(QString::fromStdString(err)), false);
        return;
    }
    static_cast<GraphCanvas*>(graphView_)->setGraph(graph_);

    // 标签筛选下拉：从快照聚合（重建后保持现选值）+ 应用当前筛选
    const QString keep = tagCombo_->currentText();
    std::map<QString, int> tagCount;
    for (const auto& n : graph_.nodes)
        for (const auto& t : n.tags) tagCount[QString::fromStdString(t)] += 1;
    tagCombo_->blockSignals(true);
    tagCombo_->clear();
    tagCombo_->addItem(i18n::trs("全部标签", "All tags"));
    for (const auto& [t, c] : tagCount) tagCombo_->addItem(t);
    if (!keep.isEmpty()) {
        const int at = tagCombo_->findText(keep);
        if (at >= 0) tagCombo_->setCurrentIndex(at);
    }
    tagCombo_->blockSignals(false);
    statsLabel_->setText(i18n::trs("%1 个节点 · %2 条相似边", "%1 nodes · %2 edges")
                             .arg(graph_.nodes.size())
                             .arg(graph_.edges.size()));
    applyTagFilter();
}

// 视图级标签过滤（审计 H2）：独立成方法——tagCombo_ 信号与 refresh 都调它；
// refresh 的签名早退只控制"是否重取快照"，不跳过筛选重应用
void GraphPanel::applyTagFilter() {
    if (tagCombo_->currentIndex() <= 0) {
        static_cast<GraphCanvas*>(graphView_)->setGraph(graph_);
        return;
    }
    const QString tag = tagCombo_->currentText();
    std::vector<ah::KnowledgeGraphNode> kept;
    for (const auto& n : graph_.nodes)
        if (std::find(n.tags.begin(), n.tags.end(), tag.toStdString()) != n.tags.end())
            kept.push_back(n);
    ah::KnowledgeGraph filtered;
    filtered.nodes = kept;
    std::map<std::string, bool> keepUuid;
    for (const auto& n : kept) keepUuid[n.uuid] = true;
    for (const auto& e : graph_.edges)
        if (keepUuid.count(e.a_uuid) && keepUuid.count(e.b_uuid)) filtered.edges.push_back(e);
    static_cast<GraphCanvas*>(graphView_)->setGraph(filtered);
}

void GraphPanel::showNodeDetail(const QString& uuid) {
    for (const auto& n : graph_.nodes)
        if (n.uuid == uuid.toStdString()) {
            detailTitle_->setText(QString::fromStdString(n.title).toHtmlEscaped());
            detailMeta_->setText(
                QString("%1 · %2 · %3")
                    .arg(QString::fromStdString(n.category).toHtmlEscaped(),
                         QString::fromStdString(n.author).toHtmlEscaped(),
                         QString::fromStdString(n.created_at).left(16)));
            QString tags;
            for (const auto& t : n.tags) tags += "#" + QString::fromStdString(t) + " ";
            detailTags_->setText(tags.toHtmlEscaped());
            openKnowledgeBtn_->setEnabled(true);
            selectedUuid_ = uuid;
            return;
        }
    clearNodeDetail();
}

void GraphPanel::clearNodeDetail() {
    detailTitle_->setText(i18n::trs("点击节点查看详情", "Click a node to see details"));
    detailMeta_->clear();
    detailTags_->clear();
    openKnowledgeBtn_->setEnabled(false);
    selectedUuid_.clear();
}
