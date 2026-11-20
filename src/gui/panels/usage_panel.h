#pragma once
// 用量分析面板：Token 消耗与预算的专属视图（参考 cc-switch 用量看板的交互形态）——
// 时间范围 / Agent / 模型三维筛选，逐日趋势 + Agent 明细表 + 模型分布，
// 「本周预算」的界面修改入口（Token / 请求数双口径）+ 自动摘要沉淀控制。
#include <QWidget>

#include <atomic>
#include <thread>

#include "panel_base.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

class UsagePanel : public PanelBase {
public:
    explicit UsagePanel(ah::Platform& platform, QWidget* parent = nullptr);
    ~UsagePanel() override;
    void refresh() override;
    void retranslate() override;
    // 作为下钻目标：接收 "agent" / "model" / "range" 三类筛选
    void applyFilter(const QString& key, const QString& value) override;

private:
    void onEditBudget();
    void onRunDigest();
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
    QLabel* budgetModeBadge_ = nullptr;  // 口径徽标（tokens/requests 一眼可辨）
    ui::RingProgress* ring_ = nullptr;
    QPushButton* editBudgetBtn_ = nullptr;

    // ---- 自动摘要沉淀（digest）----
    QCheckBox* digestCheck_ = nullptr;
    QSpinBox* digestHours_ = nullptr;
    QPushButton* digestRunBtn_ = nullptr;
    QPushButton* digestViewBtn_ = nullptr;
    QLabel* digestStatus_ = nullptr;
    // 在途沉淀：成员线程（析构 cancel+join）。无取消句柄面时 join 最多等一个
    // LLM 超时上限——maxTokens 已限 1024，实际远短于此
    std::thread digestThread_;
    std::atomic<bool> digestBusy_{false};

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
