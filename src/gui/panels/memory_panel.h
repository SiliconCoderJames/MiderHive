#pragma once
// 用户记忆面板：五大区块折叠卡片（项目档案/决策日志/偏好记录/设备环境/工作习惯），
// 顶部显示最后更新时间与条目总数；编辑生成新版本，历史可查。
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <vector>

#include "panel_base.h"

class MemoryPanel : public PanelBase {
    Q_OBJECT
public:
    explicit MemoryPanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;

private slots:
    void onEdit();
    void onShowHistory();

private:
    // 按当前的 entries_ 重建五个区块卡片；用户折起来的区块保持折起
    void rebuildSections();

    QLabel* headerLabel_ = nullptr;
    QVBoxLayout* sectionsLay_ = nullptr;
    QPushButton* editBtn_ = nullptr;      // 工具条「编辑 / 新增」（语言切换需重译）
    QPushButton* historyBtn_ = nullptr;   // 工具条「查看历史版本」
    std::vector<ah::MemoryEntry> entries_;
    QString sectionsSig_;  // 上次重建区块卡片时的条目集合签名；没变就不重建
    // 与 entries_ 下标对齐的行标签：相对时间这类文案在位更新，不必重建卡片
    std::vector<QLabel*> rowLabels_;
    QHash<QString, ui::SectionCard*> cards_;  // 区块存储名 → 卡片
};
