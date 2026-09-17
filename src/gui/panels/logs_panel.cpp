#include "logs_panel.h"

#include <QBrush>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

LogsPanel::LogsPanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    buildHeader(layout, "audit", "操作日志", "Audit Log",
                "谁、什么时候、做了什么——全部写操作可追溯，按天分组浏览",
                "Every write, by whom and when - grouped by day");

    auto* toolbar = new QHBoxLayout;
    agentCombo_ = new QComboBox(this);
    agentCombo_->addItem(i18n::trs("全部身份", "All identities"));
    sinceEdit_ = new QDateEdit(this);
    sinceEdit_->setDisplayFormat("yyyy-MM-dd");
    sinceEdit_->setCalendarPopup(true);
    sinceEdit_->setDate(QDate::currentDate().addDays(-30));  // 默认看近 30 天，而非 2000 年
    refreshBtn_ = new QPushButton(i18n::trs("筛选", "Apply"), this);
    refreshBtn_->setObjectName("primary");
    countLabel_ = new QLabel(this);
    countLabel_->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    actorLabel_ = new QLabel(i18n::trs("身份:", "Actor:"), this);
    sinceLabel_ = new QLabel(i18n::trs("起始日期:", "Since:"), this);
    toolbar->addWidget(actorLabel_);
    toolbar->addWidget(agentCombo_);
    toolbar->addWidget(sinceLabel_);
    toolbar->addWidget(sinceEdit_);
    toolbar->addWidget(refreshBtn_);
    toolbar->addStretch(1);
    toolbar->addWidget(countLabel_);
    layout->addLayout(toolbar);
    connect(refreshBtn_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(agentCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });

    // 时间线：顶层 = 日期，子项 = 该日操作
    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({i18n::trs("时间", "Time"), i18n::trs("身份", "Actor"), i18n::trs("动作", "Action"),i18n::trs("对象", "Target"), i18n::trs("详情", "Detail")});
    // 固定列宽：时间列要同时容下"层级缩进 + 完整 HH:mm:ss"，否则被压成 "06:..."
    tree_->setIndentation(14);
    tree_->setColumnWidth(0, 116);
    tree_->setColumnWidth(1, 104);
    tree_->setColumnWidth(2, 200);
    tree_->setColumnWidth(3, 170);
    tree_->header()->setStretchLastSection(true);
    tree_->setAlternatingRowColors(true);
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_, 1);
    // 即时过滤框：树建好后创建，置于「共 N 条」左侧
    filterEdit_ = makeTreeFilter(tree_, this);
    toolbar->insertWidget(toolbar->indexOf(countLabel_), filterEdit_);
}

void LogsPanel::focusFilter() {
    filterEdit_->setFocus();
    filterEdit_->selectAll();
}

void LogsPanel::retranslate() {
    PanelBase::retranslate();
    actorLabel_->setText(i18n::trs("身份:", "Actor:"));
    sinceLabel_->setText(i18n::trs("起始日期:", "Since:"));
    refreshBtn_->setText(i18n::trs("筛选", "Apply"));
    // 即时过滤框由 gui_util 的 makeTreeFilter 建好（构造期已按当时语言取词），
    // 这里补上与之一致的重译，避免切语言后占位符停留在旧语言
    filterEdit_->setPlaceholderText(i18n::trs("输入即筛…", "Type to filter…"));
    filterEdit_->setToolTip(i18n::trs("即时过滤（Ctrl+F 聚焦 · Esc 清空）",
                                      "Live filter (Ctrl+F to focus · Esc to clear)"));
    // 只改首项文案：其余项是身份/Agent 名（数据），改了会把筛选值改坏
    if (agentCombo_->count() > 0)
        agentCombo_->setItemText(0, i18n::trs("全部身份", "All identities"));
    tree_->setHeaderLabels({i18n::trs("时间", "Time"), i18n::trs("身份", "Actor"),
                            i18n::trs("动作", "Action"), i18n::trs("对象", "Target"),
                            i18n::trs("详情", "Detail")});
    // 条数摘要是 refresh() 生成的：用最后一次加载的结果就地重译，不再查库
    countLabel_->setText(i18n::trs("共 %1 条", "%1 records")
                             .arg(formatNum(static_cast<qint64>(records_.size()))));
}

void LogsPanel::refresh() {
    // 身份下拉（含全部身份 + user + 各 Agent）
    std::vector<ah::AgentInfo> agents;
    std::string err;
    platform_.listAgents(agents, err);
    QString cur = agentCombo_->currentText();
    agentCombo_->blockSignals(true);
    agentCombo_->clear();
    agentCombo_->addItem(i18n::trs("全部身份", "All identities"));
    agentCombo_->addItem("user");
    for (const auto& a : agents) agentCombo_->addItem(QString::fromStdString(a.name));
    agentCombo_->setCurrentText(cur);
    agentCombo_->blockSignals(false);

    std::string actor;
    if (agentCombo_->currentIndex() > 0) actor = agentCombo_->currentText().toStdString();
    std::string since = sinceEdit_->date().toString("yyyy-MM-dd").toStdString() + "T00:00:00Z";
    platform_.auditList(actor, "", since, 2000, records_, err);

    // 时间线分组
    tree_->clear();
    QTreeWidgetItem* dayItem = nullptr;
    QString curDay;
    for (const auto& r : records_) {
        // 后端时间统一为 UTC；直接截字符串会把 UTC 时刻当本地时间显示。
        // 这里换算成本地时区后再分组与显示，避免"日志时间和手表对不上"。
        const QDateTime dt = parseUtcIso(QString::fromStdString(r.created_at));
        const QDateTime local = dt.isValid() ? dt.toLocalTime() : QDateTime();
        QString day = local.isValid() ? local.toString("yyyy-MM-dd")
                                      : QString::fromStdString(r.created_at).left(10);
        if (day != curDay) {
            curDay = day;
            dayItem = new QTreeWidgetItem(tree_, {day, "", "", "", ""});
            tree_->setFirstColumnSpanned(tree_->indexOfTopLevelItem(dayItem),
                                         QModelIndex(), true);
            QFont f = dayItem->font(0);
            f.setBold(true);
            dayItem->setFont(0, f);
            dayItem->setForeground(0, QBrush(ui::accent()));
        }
        auto* row = new QTreeWidgetItem(dayItem);
        row->setText(0, local.isValid() ? local.toString("HH:mm:ss")
                                       : QString::fromStdString(r.created_at).mid(11, 8));
        row->setText(1, QString::fromStdString(r.actor));
        row->setText(2, QString::fromStdString(r.action));
        row->setText(3, QString::fromStdString(r.target));
        // 详情是紧凑 JSON，直接显示既占宽又难读；清洗成 "字段=值 · 字段=值"
        row->setText(4, prettyDetail(QString::fromStdString(r.detail)));
        row->setToolTip(4, QString::fromStdString(r.detail));
        for (int c = 0; c < 5; ++c) row->setFlags(row->flags() & ~Qt::ItemIsEditable);
    }
    tree_->expandToDepth(0);
    applyTreeFilter(tree_, filterEdit_->text());  // 树已重建，重放即时过滤态
    countLabel_->setText(i18n::trs("共 %1 条", "%1 records").arg(formatNum(static_cast<qint64>(records_.size()))));
}
