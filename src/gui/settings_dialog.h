#pragma once
// 设置对话框：左侧窄导航 + 右侧内容堆叠（对齐主流 AI 桌面应用范式）。
// 分区：外观（主题色卡/字号）· 数据与备份 · 通知偏好 · Agent 管理 · Agent API · 更新。
// 主题/字号切换即时生效；对话框注册 theme 监听器，随主题刷新自身样式。
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <cstddef>
#include <utility>
#include <vector>

#include "core/platform.h"
#include "update_checker.h"
#include "widgets.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QProgressBar;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ah::Platform& platform, QWidget* parent = nullptr);
    ~SettingsDialog() override;

protected:
    void showEvent(QShowEvent*) override;  // 每次打开刷新备份列表与 Agent 列表

private:
    QWidget* buildAppearancePage();
    QWidget* buildBackupPage();
    QWidget* buildNotifyPage();
    QWidget* buildAgentsPage();
    QWidget* buildApiPage();
    QWidget* buildLlmPage();
    QWidget* buildUpdatePage();
    QWidget* buildAboutPage();
    void refreshBackupList();
    void refreshAgents();
    void loadLlmConfig();  // 读 config/llm.json 回填表单（打开时）
    ah::llm::LlmConfig llmFormConfig() const;  // 表单 → 配置（不落盘）
    void applyChrome();  // 主题变化后重涂导航与 th() 取色控件
    QLabel* thLabel(const QString& tmpl, QWidget* parent);

    ah::Platform& platform_;
    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    std::vector<ui::ThemeSwatch*> swatches_;
    QComboBox* fontBox_ = nullptr;
    QTableWidget* backupTable_ = nullptr;
    QComboBox* refreshBox_ = nullptr;
    QCheckBox* errorToastBox_ = nullptr;
    QTableWidget* agentsTable_ = nullptr;
    QLabel* latest_ = nullptr;  // 更新页状态行
    QCheckBox* autoCheckBox_ = nullptr;
    QLineEdit* mirrorEdit_ = nullptr;        // 更新镜像前缀（GitHub 直连不稳时的兜底）
    QCheckBox* autoMirrorBox_ = nullptr;     // 直连失败后是否自动尝试公共镜像
    QPushButton* installBtn_ = nullptr;
    QPushButton* skipBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    ui::UpdateChecker* updater_ = nullptr;
    // ---- LLM 接入页 ----
    QComboBox* llmProviderBox_ = nullptr;
    QComboBox* llmProtoBox_ = nullptr;
    QLineEdit* llmUrlEdit_ = nullptr;
    QLineEdit* llmModelEdit_ = nullptr;
    QLineEdit* llmKeyEdit_ = nullptr;
    QPushButton* llmTestBtn_ = nullptr;
    QPushButton* llmSaveBtn_ = nullptr;
    QLabel* llmStatus_ = nullptr;
    ui::UpdateInfo pending_;    // 最近一次检查到的可用版本
    std::size_t themeListenerId_ = 0;
    // th() 取色控件登记表：切换主题时统一重涂
    std::vector<std::pair<QLabel*, QString>> styledLabels_;
};
