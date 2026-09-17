#pragma once
// 知识库面板：关键词 / 语义搜索、条目浏览、新建条目、追加版本。
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QComboBox>

#include <vector>

#include "../widgets.h"
#include "panel_base.h"

class KnowledgePanel : public PanelBase {
    Q_OBJECT
public:
    explicit KnowledgePanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;
    void focusFilter() override;

private slots:
    void onSearch();
    void onNewEntry();
    void onAddVersion();
    void onSelectEntry(int row);
    void onVersionChanged(int idx);
    void onEntryViewer(int row);

private:
    QLineEdit* searchEdit_ = nullptr;
    QCheckBox* semanticCheck_ = nullptr;
    QLineEdit* tagEdit_ = nullptr;
    QLineEdit* viewFilter_ = nullptr;
    QPushButton* searchBtn_ = nullptr;   // 工具条「搜索」（语言切换需重译）
    QPushButton* newBtn_ = nullptr;      // 工具条「＋ 新建条目」
    QLabel* tagLabel_ = nullptr;         // 副工具条「标签」
    QLabel* statsLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QStackedWidget* detailStack_ = nullptr;  // 详情 / 空状态 分页
    ui::InlineEmpty* emptyState_ = nullptr;  // 全库为空时的引导 + 显眼动作按钮
    QLabel* metaLabel_ = nullptr;
    QComboBox* versionCombo_ = nullptr;
    QPushButton* addVersionBtn_ = nullptr;
    std::vector<ah::KnowledgeHit> hits_;
    std::vector<ah::KnowledgeEntry> versions_;
    std::string currentUuid_;
};
