#include "usage_panel.h"

#include <algorithm>

#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

UsagePanel::UsagePanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);
    buildHeader(root, "usage", "用量分析", "Usage",
                "Token 消耗与预算 · 按时间 / Agent / 模型筛选",
                "Token spend & budget · filter by time / agent / model");

    // ---- 筛选行：时间范围 + Agent + 模型（cc-switch 式全局筛选，改选即刷新）----
    auto* filterBar = new QHBoxLayout;
    filterBar->setSpacing(8);
    rangeLabel_ = new QLabel(i18n::trs("时间范围:", "Range:"), this);
    rangeCombo_ = new QComboBox(this);
    rangeCombo_->addItems({i18n::trs("最近 7 天", "Last 7 days"),
                           i18n::trs("最近 14 天", "Last 14 days"),
                           i18n::trs("最近 30 天", "Last 30 days")});
    rangeCombo_->setCurrentIndex(1);
    rangeCombo_->setStyleSheet(ui::th("QComboBox { padding:4px 10px; }"));
    agentLabel_ = new QLabel("Agent:", this);
    agentCombo_ = new QComboBox(this);
    agentCombo_->setStyleSheet(ui::th("QComboBox { padding:4px 10px; }"));
    modelLabel_ = new QLabel(i18n::trs("模型:", "Model:"), this);
    modelCombo_ = new QComboBox(this);
    modelCombo_->setStyleSheet(ui::th("QComboBox { padding:4px 10px; }"));
    filterBar->addWidget(rangeLabel_);
    filterBar->addWidget(rangeCombo_);
    filterBar->addSpacing(6);
    filterBar->addWidget(agentLabel_);
    filterBar->addWidget(agentCombo_);
    filterBar->addSpacing(6);
    filterBar->addWidget(modelLabel_);
    filterBar->addWidget(modelCombo_);
    filterBar->addStretch(1);
    root->addLayout(filterBar);

    connect(rangeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!populate_) refresh(); });
    connect(agentCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!populate_) refresh(); });
    connect(modelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!populate_) refresh(); });

    // ---- 第一行：预算环形（含调整入口）+ 窗口 KPI ----
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(14);
    budgetCard_ = new QGroupBox(i18n::trs("本周 Token 预算", "Weekly Token Budget"), this);
    budgetCard_->setObjectName("card");
    auto* bl = new QVBoxLayout(budgetCard_);
    bl->setContentsMargins(8, 16, 8, 8);
    bl->setSpacing(6);
    ring_ = new ui::RingProgress(budgetCard_);
    bl->addWidget(ring_, 1);
    editBudgetBtn_ = new QPushButton(i18n::trs("调整预算", "Adjust budget"), budgetCard_);
    editBudgetBtn_->setObjectName("editBudget");  // UI 自动化/测试按稳定 id 定位（文案随语言变）
    editBudgetBtn_->setCursor(Qt::PointingHandCursor);
    editBudgetBtn_->setStyleSheet(ui::th("padding:5px 14px;"));
    connect(editBudgetBtn_, &QPushButton::clicked, this, &UsagePanel::onEditBudget);
    bl->addWidget(editBudgetBtn_, 0, Qt::AlignCenter);
    topRow->addWidget(budgetCard_, 2);

    // KPI：窗口内消耗（输入/输出拆分）· 调用次数 · 日均
    auto* kpis = new QWidget(this);
    kpis->setStyleSheet("background:transparent;");
    auto* kl = new QVBoxLayout(kpis);
    kl->setContentsMargins(0, 0, 0, 0);
    kl->setSpacing(10);
    kpiWindow_ = new ui::MetricTile("usage", kpis);
    kpiCalls_ = new ui::MetricTile("audit", kpis);
    kpiAvg_ = new ui::MetricTile("overview", kpis);
    kl->addWidget(kpiWindow_, 1);
    kl->addWidget(kpiCalls_, 1);
    kl->addWidget(kpiAvg_, 1);
    topRow->addWidget(kpis, 3);
    root->addLayout(topRow, 3);

    // ---- 第二行：逐日趋势（随筛选）+ 模型分布（随筛选）----
    auto* midRow = new QHBoxLayout;
    midRow->setSpacing(14);
    trendCard_ = new QGroupBox(i18n::trs("逐日消耗（随筛选）", "Daily Tokens (filtered)"), this);
    trendCard_->setObjectName("card");
    auto* trl = new QVBoxLayout(trendCard_);
    trl->setContentsMargins(8, 16, 8, 6);
    trendChart_ = new ui::VBarChart(trendCard_);
    trl->addWidget(trendChart_, 1);
    midRow->addWidget(trendCard_, 3);
    modelCard_ = new QGroupBox(i18n::trs("模型分布（随筛选）", "Tokens by Model (filtered)"), this);
    modelCard_->setObjectName("card");
    auto* ml = new QVBoxLayout(modelCard_);
    ml->setContentsMargins(8, 16, 8, 6);
    modelChart_ = new ui::HBarChart(modelCard_);
    // 页内下钻：点模型条 → 直接把"模型"筛选设为该模型（同一页上下联动，不用跳走）
    modelChart_->setOnEntryClick([this](int idx) {
        if (idx < 0 || idx >= modelChart_->entries().size()) return;
        populate_ = true;
        const int at = modelCombo_->findText(modelChart_->entries()[idx].first);
        if (at > 0) modelCombo_->setCurrentIndex(at);
        populate_ = false;
        refresh();
    });
    ml->addWidget(modelChart_, 1);
    midRow->addWidget(modelCard_, 2);
    root->addLayout(midRow, 3);

    // ---- 第三行：Agent 明细表（输入/输出/合计/调用/占比，可点列头排序）----
    tableCard_ = new QGroupBox(i18n::trs("Agent 用量明细", "Per-Agent Breakdown"), this);
    tableCard_->setObjectName("card");
    auto* tl = new QVBoxLayout(tableCard_);
    tl->setContentsMargins(8, 16, 8, 8);
    agentTable_ = new QTableWidget(0, 6, tableCard_);
    polishTable(agentTable_);
    agentTable_->setHorizontalHeaderLabels(
        {i18n::trs("Agent", "Agent"), i18n::trs("输入", "Input"), i18n::trs("输出", "Output"),
         i18n::trs("合计", "Total"), i18n::trs("调用", "Calls"), i18n::trs("占比", "Share")});
    agentTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    agentTable_->setColumnWidth(1, 110);
    agentTable_->setColumnWidth(2, 110);
    agentTable_->setColumnWidth(3, 120);
    agentTable_->setColumnWidth(4, 90);
    agentTable_->setColumnWidth(5, 110);
    // Agent 明细是这张页面的"结论区"：给它可用高度下限，行数多时靠自身滚动条，
    // 不把整页撑到必须滚动才能看全（面板外层已有滚动容器）
    agentTable_->setMinimumHeight(200);
    // 双击某行 → 直接把「Agent」筛选设为它（页内下钻，和点模型条是一对）
    connect(agentTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto* it = agentTable_->item(row, 0)) {
            populate_ = true;
            const int at = agentCombo_->findText(it->text());
            if (at > 0) agentCombo_->setCurrentIndex(at);
            populate_ = false;
            refresh();
        }
    });
    tableEmpty_ = new ui::InlineEmpty(
        "usage", i18n::trs("该筛选下暂无用量", "no usage under this filter"),
        i18n::trs("放宽时间范围，或把 Agent / 模型切回「全部」",
                  "widen the range, or set agent / model back to all"), tableCard_);
    tableStack_ = new QStackedWidget(tableCard_);
    tableStack_->addWidget(agentTable_);
    tableStack_->addWidget(tableEmpty_);
    tl->addWidget(tableStack_, 1);
    root->addWidget(tableCard_, 5);

    rebuildAgentCombo();
    rebuildModelCombo();
}

void UsagePanel::rebuildAgentCombo() {
    populate_ = true;
    const QString keep = agentCombo_->currentIndex() > 0
                             ? agentCombo_->currentText()
                             : QString();
    agentCombo_->clear();
    agentCombo_->addItem(i18n::trs("全部 Agent", "All agents"));
    std::vector<ah::AgentInfo> agents;
    std::string err;
    if (platform_.listAgents(agents, err))
        for (const auto& a : agents) agentCombo_->addItem(QString::fromStdString(a.name));
    if (!keep.isEmpty()) {
        const int at = agentCombo_->findText(keep);
        if (at > 0) agentCombo_->setCurrentIndex(at);
    }
    populate_ = false;
}

void UsagePanel::rebuildModelCombo() {
    populate_ = true;
    const QString keep = modelCombo_->currentIndex() > 0 ? modelCombo_->currentText() : QString();
    modelCombo_->clear();
    modelCombo_->addItem(i18n::trs("全部模型", "All models"));
    std::vector<ah::UsageModelRow> rows;
    std::string err;
    if (platform_.usageByModel(rows, err))
        for (const auto& r : rows) modelCombo_->addItem(QString::fromStdString(r.model));
    if (!keep.isEmpty()) {
        const int at = modelCombo_->findText(keep);
        if (at > 0) modelCombo_->setCurrentIndex(at);
    }
    populate_ = false;
}

void UsagePanel::refresh() {
    static const int kDaysByIndex[] = {7, 14, 30};
    const int days = kDaysByIndex[qBound(0, rangeCombo_->currentIndex(), 2)];
    const QString agent = agentCombo_->currentIndex() > 0 ? agentCombo_->currentText() : QString();
    const QString model = modelCombo_->currentIndex() > 0 ? modelCombo_->currentText() : QString();

    std::string err;
    ah::UsageBreakdown bd;
    if (!platform_.usageBreakdown(days, agent.toStdString(), model.toStdString(), bd, err))
        return;

    // ---- KPI：窗口内消耗（输入/输出）· 调用次数 · 日均 ----
    kpiWindow_->set(i18n::trs("窗口内 Token", "Tokens in range"), ui::fmtCompact(bd.total_tokens),
                    i18n::trs("输入 %1 · 输出 %2", "in %1 · out %2")
                        .arg(ui::fmtCompact(bd.total_in), ui::fmtCompact(bd.total_out)),
                    ui::accent());
    kpiWindow_->setToolTipAll(i18n::trs("最近 %1 天：输入 %2 + 输出 %3 = %4 tokens",
                                        "Last %1 day(s): in %2 + out %3 = %4 tokens")
                                  .arg(days)
                                  .arg(formatNum(bd.total_in))
                                  .arg(formatNum(bd.total_out))
                                  .arg(formatNum(bd.total_tokens)));
    kpiCalls_->set(i18n::trs("调用次数", "Calls"), formatNum(bd.calls),
                   i18n::trs("窗口内上报次数", "reports within range"), ui::brand());
    const qint64 avg = bd.calls > 0 ? bd.total_tokens / qMax<int64_t>(bd.calls, 1) : 0;
    kpiAvg_->set(i18n::trs("次均消耗", "Avg per call"), ui::fmtCompact(avg),
                 i18n::trs("tokens / 调用", "tokens / call"), ui::selText());

    // ---- 预算环：全局本周口径（不随窗口/筛选变化，预算与告警以周为单位）----
    ah::UsageSummary sum;
    if (platform_.usageSummary(sum, err)) {
        ring_->setValues(sum.total_tokens, sum.budget,
                         i18n::trs("剩余 %1", "left %1")
                             .arg(ui::fmtCompact(qMax<qint64>(0, sum.budget - sum.total_tokens))));
        const double ratio = sum.budget > 0 ? double(sum.total_tokens) / double(sum.budget) : 0.0;
        editBudgetBtn_->setStyleSheet(ui::th(QString(
            "padding:5px 14px; color:%1;").arg(ui::usageColor(ratio).name())));
    }

    // ---- 图表：逐日 + 模型（内容不变不重绘，避免刷新闪烁）----
    QVector<QPair<QString, qint64>> daily;
    for (const auto& d : bd.daily) daily.push_back({QString::fromStdString(d.day).mid(5), d.tokens});
    trendChart_->setEntries(daily);
    QVector<QPair<QString, qint64>> models;
    for (size_t i = 0; i < bd.per_model.size() && i < 8; ++i)
        models.push_back({QString::fromStdString(bd.per_model[i].model), bd.per_model[i].tokens});
    modelChart_->setEntries(models);

    // ---- Agent 明细表：占比列按用量着色，精确值进 tooltip，数字列等宽 ----
    agentTable_->setSortingEnabled(false);  // 填充期间禁排序，行号与数据对齐
    const int rows = static_cast<int>(bd.per_agent.size());
    agentTable_->setRowCount(rows);
    // 数字列等宽：多行数字上下对齐，量级差异一眼可比（仪表盘惯例）
    QFont monoFont = agentTable_->font();
    monoFont.setFamily(ui::mono());
    for (int i = 0; i < rows; ++i) {
        const auto& r = bd.per_agent[size_t(i)];
        const double share =
            bd.total_tokens > 0 ? 100.0 * double(r.tokens) / double(bd.total_tokens) : 0.0;
        auto* name = new QTableWidgetItem(QString::fromStdString(r.agent));
        name->setToolTip(QString("%1 · in %2 / out %3")
                             .arg(QString::fromStdString(r.agent))
                             .arg(formatNum(r.in))
                             .arg(formatNum(r.out)));
        auto* inItem = new QTableWidgetItem(ui::fmtCompact(r.in));
        inItem->setToolTip(formatNum(r.in));
        inItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        inItem->setFont(monoFont);
        auto* outItem = new QTableWidgetItem(ui::fmtCompact(r.out));
        outItem->setToolTip(formatNum(r.out));
        outItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        outItem->setFont(monoFont);
        auto* totalItem = new QTableWidgetItem(ui::fmtCompact(r.tokens));
        totalItem->setToolTip(formatNum(r.tokens));
        totalItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        totalItem->setFont(monoFont);
        auto* callsItem = new QTableWidgetItem(QString::number(r.calls));
        callsItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        callsItem->setFont(monoFont);
        auto* shareItem = new QTableWidgetItem(QString("%1%").arg(share, 0, 'f', 1));
        shareItem->setForeground(ui::usageColor(share / 100.0));
        shareItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        shareItem->setFont(monoFont);
        // 排序按数值而非文本：把原始值藏进 data(Qt::UserRole)
        inItem->setData(Qt::UserRole, qlonglong(r.in));
        outItem->setData(Qt::UserRole, qlonglong(r.out));
        totalItem->setData(Qt::UserRole, qlonglong(r.tokens));
        callsItem->setData(Qt::UserRole, qlonglong(r.calls));
        agentTable_->setItem(i, 0, name);
        agentTable_->setItem(i, 1, inItem);
        agentTable_->setItem(i, 2, outItem);
        agentTable_->setItem(i, 3, totalItem);
        agentTable_->setItem(i, 4, callsItem);
        agentTable_->setItem(i, 5, shareItem);
    }
    agentTable_->setSortingEnabled(true);
    if (rows > 0) agentTable_->sortItems(3, Qt::DescendingOrder);  // 默认按合计降序
    // 空状态：该筛选下没有数据时给"怎么放宽"的出路，而不是一张空表
    tableStack_->setCurrentWidget(rows == 0 ? static_cast<QWidget*>(tableEmpty_)
                                           : static_cast<QWidget*>(agentTable_));
}

void UsagePanel::onEditBudget() {
    std::string err;
    const qint64 cur = platform_.usageBudget(err);
    ui::BudgetEditDialog dlg(cur > 0 ? cur : 10'000'000, this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (platform_.usageSetBudget(ah::kManagerName, dlg.value(), err)) {
        QMessageBox::information(
            this, i18n::trs("预算已更新", "Budget updated"),
            i18n::trs("本周 Token 预算已调整为 %1。", "Weekly token budget is now %1.")
                .arg(formatNum(dlg.value())));
        refresh();
    } else {
        QMessageBox::warning(this, i18n::trs("调整失败", "Adjustment failed"),
                             QString::fromStdString(err));
    }
}

void UsagePanel::applyFilter(const QString& key, const QString& value) {
    // 下钻落地：把来源面板的上下文变成这里的筛选条件（找不到就补一个选项，
    // 例如没有用量记录的 Agent 也应能作为筛选值出现）
    populate_ = true;
    if (key == "range") {
        const int days = value.toInt();
        rangeCombo_->setCurrentIndex(days <= 7 ? 0 : (days <= 14 ? 1 : 2));
    } else if (key == "agent" && !value.isEmpty()) {
        int at = agentCombo_->findText(value);
        if (at < 0) {
            agentCombo_->addItem(value);
            at = agentCombo_->count() - 1;
        }
        agentCombo_->setCurrentIndex(at);
    } else if (key == "model" && !value.isEmpty()) {
        int at = modelCombo_->findText(value);
        if (at < 0) {
            modelCombo_->addItem(value);
            at = modelCombo_->count() - 1;
        }
        modelCombo_->setCurrentIndex(at);
    }
    populate_ = false;
    refresh();
}

void UsagePanel::retranslate() {
    PanelBase::retranslate();
    rangeLabel_->setText(i18n::trs("时间范围:", "Range:"));
    agentLabel_->setText("Agent:");
    modelLabel_->setText(i18n::trs("模型:", "Model:"));
    populate_ = true;
    const int rangeAt = rangeCombo_->currentIndex();
    rangeCombo_->clear();
    rangeCombo_->addItems({i18n::trs("最近 7 天", "Last 7 days"),
                           i18n::trs("最近 14 天", "Last 14 days"),
                           i18n::trs("最近 30 天", "Last 30 days")});
    rangeCombo_->setCurrentIndex(rangeAt < 0 ? 1 : rangeAt);
    if (agentCombo_->count() > 0) agentCombo_->setItemText(0, i18n::trs("全部 Agent", "All agents"));
    if (modelCombo_->count() > 0) modelCombo_->setItemText(0, i18n::trs("全部模型", "All models"));
    populate_ = false;
    budgetCard_->setTitle(i18n::trs("本周 Token 预算", "Weekly Token Budget"));
    editBudgetBtn_->setText(i18n::trs("调整预算", "Adjust budget"));
    trendCard_->setTitle(i18n::trs("逐日消耗（随筛选）", "Daily Tokens (filtered)"));
    modelCard_->setTitle(i18n::trs("模型分布（随筛选）", "Tokens by Model (filtered)"));
    tableCard_->setTitle(i18n::trs("Agent 用量明细", "Per-Agent Breakdown"));
    const QStringList headers = {i18n::trs("Agent", "Agent"), i18n::trs("输入", "Input"),
                                 i18n::trs("输出", "Output"), i18n::trs("合计", "Total"),
                                 i18n::trs("调用", "Calls"), i18n::trs("占比", "Share")};
    for (int c = 0; c < agentTable_->columnCount() && c < headers.size(); ++c)
        agentTable_->horizontalHeaderItem(c)->setText(headers[c]);
    // 空状态标题/提示是构造期一次性写入的（不随 refresh 重建），需显式重译
    tableEmpty_->titleLabel()->setText(i18n::trs("该筛选下暂无用量", "no usage under this filter"));
    tableEmpty_->hintLabel()->setText(i18n::trs("放宽时间范围，或把 Agent / 模型切回「全部」",
                                                "widen the range, or set agent / model back to all"));
}
