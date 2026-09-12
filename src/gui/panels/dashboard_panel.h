#pragma once
// 总览面板：环形 Token 预算图 + 各 Agent 用量柱状图 + Agent 状态卡片网格
// + 底部事件流时间线 + 分级告警卡片。
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <vector>

#include "../widgets.h"
#include "panel_base.h"

class DashboardPanel : public PanelBase {
    Q_OBJECT
public:
    explicit DashboardPanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;

private:
    ui::MetricTile* kpiTokens_ = nullptr;    // 顶部 KPI 磁贴行：先给结论，再给图表
    ui::MetricTile* kpiAgents_ = nullptr;
    ui::MetricTile* kpiToday_ = nullptr;
    ui::MetricTile* kpiErrors_ = nullptr;
    ui::RingProgress* ring_ = nullptr;
    class QPushButton* editBudgetBtn_ = nullptr;  // 预算调整入口（打开 BudgetEditDialog）
    ui::HBarChart* usageChart_ = nullptr;
    ui::VBarChart* trendChart_ = nullptr;    // 最近 14 天逐日消耗
    ui::HBarChart* modelChart_ = nullptr;    // 按模型累计
    QGridLayout* agentGrid_ = nullptr;
    QListWidget* timeline_ = nullptr;
    QVBoxLayout* alertsLay_ = nullptr;
    QLabel* emptyAlerts_ = nullptr;
    QGroupBox* budgetCard_ = nullptr;
    QGroupBox* usageCard_ = nullptr;
    QGroupBox* agentsCard_ = nullptr;
    QGroupBox* trendCard_ = nullptr;
    QGroupBox* modelCard_ = nullptr;
    QGroupBox* tlCard_ = nullptr;
    QGroupBox* alertCard_ = nullptr;
    std::vector<ui::AgentCard*> agentCards_;
    std::vector<QWidget*> alertCards_;
    QString budgetAlertLevel_;
    QString lastTimeline_;   // 事件流去重：内容未变化时跳过重建，避免闪烁
    QVector<QPair<QString, qint64>> lastDaily_, lastModel_;  // 用量图表去重

public:
    void retranslate() override;
};
