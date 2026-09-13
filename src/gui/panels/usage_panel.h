#pragma once
// 用量分析面板：Token 消耗与预算的专属视图（参考 cc-switch 用量看板的交互形态）——
// 时间范围 / Agent / 模型三维筛选，逐日趋势 + Agent 明细表 + 模型分布，
// 以及「本周 Token 预算」的界面修改入口（此前预算只能改库，界面无入口）。
#include <QWidget>

#include "panel_base.h"

class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableWidget;

class UsagePanel : public PanelBase {
public:
    explicit UsagePanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;
    // 作为下钻目标：接收 "agent" / "model" / "range" 三类筛选
    void applyFilter(const QString& key, const QString& value) override;

private:
    void onEditBudget();
    void rebuildAgentCombo();
    void rebuildModelCombo();

    // ---- 筛选行 ----
    QComboBox* rangeCombo_ = nullptr;
    QComboBox* agentCombo_ = nullptr;
    QComboBox* modelCombo_ = nullptr;
    QLabel* rangeLabel_ = nullptr;
    QLabel* agentLabel_ = nullptr;
    QLabel* modelLabel_ = nullptr;
    bool populate_ = false;  // 重建下拉框选项时抑制 currentIndexChanged 连锁刷新

    // ---- 预算卡 ----
    QGroupBox* budgetCard_ = nullptr;
    ui::RingProgress* ring_ = nullptr;
    QPushButton* editBudgetBtn_ = nullptr;

    // ---- KPI 与图表 ----
    ui::MetricTile* kpiWindow_ = nullptr;
    ui::MetricTile* kpiCalls_ = nullptr;
    ui::MetricTile* kpiAvg_ = nullptr;
    QGroupBox* trendCard_ = nullptr;
    ui::VBarChart* trendChart_ = nullptr;
    QGroupBox* modelCard_ = nullptr;
    ui::HBarChart* modelChart_ = nullptr;

    // ---- Agent 明细表 ----
    QGroupBox* tableCard_ = nullptr;
    QTableWidget* agentTable_ = nullptr;
    QStackedWidget* tableStack_ = nullptr;
    ui::InlineEmpty* tableEmpty_ = nullptr;
};
