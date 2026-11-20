#pragma once
// 图谱面板（Obsidian 式）：语义关联知识图谱。
// 左侧 = 力导向画布（滚轮缩放/拖拽平移/点击选中），右侧 = 选中节点详情。
// 节点 = 最新知识条目，边 = 向量 kNN 相似（数据面见 Platform::knowledgeGraphSnapshot）。
// 3s 轮询不重算图——只在数据签名变化（最新条目 created_at 变化）或手动刷新时
// 重取快照；布局位置由画布内部按 uuid 缓存，跨刷新保留（防抖动）。
#include <QWidget>

#include "panel_base.h"

class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QTextBrowser;

class GraphPanel : public PanelBase {
public:
    explicit GraphPanel(ah::Platform& platform, QWidget* parent = nullptr);
    void refresh() override;
    void retranslate() override;

private:
    void rebuildSnapshot();
    void showNodeDetail(const QString& uuid);
    void clearNodeDetail();
    static QString digestHintText();

    QGroupBox* graphCard_ = nullptr;
    QWidget* graphView_ = nullptr;  // 自绘力导向画布（graph_panel.cpp 内部实现）
    QPushButton* relayoutBtn_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;
    QComboBox* tagCombo_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QGroupBox* detailCard_ = nullptr;
    QLabel* detailTitle_ = nullptr;
    QLabel* detailMeta_ = nullptr;
    QLabel* detailTags_ = nullptr;
    QPushButton* openKnowledgeBtn_ = nullptr;
    QString selectedUuid_;
    ah::KnowledgeGraph graph_;
    std::string lastSig_;  // 数据签名（最新条目 created_at），变化才重取快照
};
