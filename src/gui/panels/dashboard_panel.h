#pragma once
// 总览面板：KPI 磁贴（趋势小图 + 环比）· 紧凑预算卡 · 各 Agent 用量 · 逐日趋势 ·
// 模型分布 · Agent 状态网格（离线可点开操作）· 事件流（类型过滤 + 点击下钻）· 分级告警。
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <vector>

#include "../widgets.h"
#include "panel_base.h"

class DashboardPanel : public PanelBase {
    Q_OBJECT
public:
    explicit DashboardPanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;

protected:
    // 首屏骨架屏铺满内容区，随面板尺寸变化（面板在 QScrollArea 里，尺寸由外层决定）
    void resizeEvent(QResizeEvent* e) override;

private:
    // ---- 顶部 KPI ----
    ui::MetricTile* kpiTokens_ = nullptr;
    ui::MetricTile* kpiAgents_ = nullptr;
    ui::MetricTile* kpiToday_ = nullptr;
    ui::MetricTile* kpiErrors_ = nullptr;

    // ---- 预算卡（紧凑：小环 + 右侧数字列 + 迷你走势）----
    ui::RingProgress* ring_ = nullptr;
    QPushButton* editBudgetBtn_ = nullptr;
    QLabel* budgetStats_ = nullptr;   // 已用 / 剩余 / 占比 三行紧凑数字
    ui::Sparkline* budgetSpark_ = nullptr;

    // ---- 图表 ----
    ui::HBarChart* usageChart_ = nullptr;
    ui::VBarChart* trendChart_ = nullptr;
    ui::HBarChart* modelChart_ = nullptr;
    QGridLayout* agentGrid_ = nullptr;

    // ---- 事件流（类型过滤 + 空状态分页）----
    QListWidget* timeline_ = nullptr;
    QComboBox* eventFilter_ = nullptr;
    QStackedWidget* eventStack_ = nullptr;
    ui::InlineEmpty* eventEmpty_ = nullptr;

    // ---- 告警 ----
    QVBoxLayout* alertsLay_ = nullptr;
    ui::InlineEmpty* emptyAlerts_ = nullptr;
    ui::InlineEmpty* agentsEmpty_ = nullptr;
    std::vector<ui::AlertCard*> alertCards_;

    // ---- 健康横幅（防呆）：数据目录/数据库/端口/密钥缓存问题只在出问题时出现，
    // 健康时零占位，不改既有版式。内容签名不变时不重建（3s 轮询不闪屏）。 ----
    QWidget* healthBox_ = nullptr;
    QVBoxLayout* healthLay_ = nullptr;
    QString lastHealthSig_;
    void refreshHealth();               // 轮询诊断 → 增量重建横幅
    void fixKeyfile(const QString& name);  // 密钥丢失的修复动作：轮换密钥并展示新钥

    std::vector<ui::AgentCard*> agentCards_;
    ui::Skeleton* skeleton_ = nullptr;   // 首屏加载占位

    // ---- 卡片 ----
    QGroupBox* budgetCard_ = nullptr;
    QGroupBox* usageCard_ = nullptr;
    QGroupBox* agentsCard_ = nullptr;
    QGroupBox* trendCard_ = nullptr;
    QGroupBox* modelCard_ = nullptr;
    QGroupBox* tlCard_ = nullptr;
    QGroupBox* alertCard_ = nullptr;

    QString budgetAlertLevel_;
    QString lastTimeline_;
    QVector<QPair<QString, qint64>> lastDaily_, lastModel_;
    // 图表下钻需要"序号 → 名称"的映射（回调只拿到序号）
    QVector<QPair<QString, qint64>> lastAgentBars_, lastModelBars_;
    int seenOpenErrors_ = -1;   // 会话内新增错误的基线（-1 = 尚未采样）
    bool firstLoadDone_ = false;

    // 事件动作 → 面板 id / 图标种类（过滤与下钻共用一张表，避免两处各写一套判断）
    static QString kindOfEvent(const QString& action, const QString& target);
    void rebuildEventStream(const std::vector<ah::AuditRecord>& records);
    void showAgentActions(const QString& name, bool online);
};
