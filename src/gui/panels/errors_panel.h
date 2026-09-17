#pragma once
// 错误报告面板：浏览 / 筛选错误，查看堆栈与解决记录，登记解决说明。
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QLabel>

#include <vector>

#include "../widgets.h"
#include "panel_base.h"

class ErrorsPanel : public PanelBase {
    Q_OBJECT
public:
    explicit ErrorsPanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;
    void focusFilter() override;

private slots:
    void onResolve();
    void onManualReport();  // 手动上报一条错误（用户侧补录，与 Agent 上报同一条管道）

private:
    QComboBox* statusCombo_ = nullptr;
    QComboBox* severityCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;     // 「状态:」工具条标签（语言切换需重译）
    QLabel* severityLabel_ = nullptr;   // 「严重度:」工具条标签
    QLineEdit* filterEdit_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    ui::InlineEmpty* emptyState_ = nullptr;  // 无错误时的引导 + 手动上报入口
    QPushButton* resolveBtn_ = nullptr;
    std::vector<ah::ErrorReport> errors_;
};
