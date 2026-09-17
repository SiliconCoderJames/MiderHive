#pragma once
// 技能库面板：按 Agent / 分类筛选浏览、注册新技能、查看调用记录。
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>

#include <vector>

#include "../widgets.h"
#include "panel_base.h"

class SkillsPanel : public PanelBase {
    Q_OBJECT
public:
    explicit SkillsPanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;
    void focusFilter() override;

private slots:
    void onRegister();
    void onSelectSkill(int row);

private:
    QComboBox* categoryCombo_ = nullptr;
    QComboBox* ownerCombo_ = nullptr;
    QLabel* categoryLabel_ = nullptr;  // 工具条「分类:」（语言切换需重译）
    QLabel* ownerLabel_ = nullptr;     // 工具条「提供者:」
    QPushButton* refreshBtn_ = nullptr;    // 工具条「刷新」
    QPushButton* registerBtn_ = nullptr;   // 工具条「＋ 注册技能」
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QStackedWidget* detailStack_ = nullptr;  // 详情 / 空状态 分页
    ui::InlineEmpty* emptyState_ = nullptr;  // 全库为空时的引导 + 显眼动作按钮
    std::vector<ah::SkillInfo> skills_;
};
