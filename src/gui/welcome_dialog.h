#pragma once
// 首次运行引导：蜂巢是什么 → 三步接入（可复制注册命令）→ 一键接入工具（检测
// Claude Code / Cursor / Codex CLI 并生成对应 MCP 配置）→ 选主题色卡。
// 关闭时由调用方写入 ui/welcomeSeen，此后不再弹出；设置里保留「重新打开接入引导」。
// 接入身份（agentProvision 生成的名字+密钥）写入 ui/onboardingPending/<名字>=工具id，
// 由 MainWindow 轮询：该 Agent 上线即弹「接入成功」并写入欢迎记忆。
#include <QDialog>
#include <QVector>
#include <vector>

#include "core/platform.h"
#include "integrations.h"
#include "widgets.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

class WelcomeDialog : public QDialog {
    Q_OBJECT
public:
    explicit WelcomeDialog(ah::Platform& platform, QWidget* parent = nullptr);

private:
    // 选中某个接入工具：本地检测 → 预配 Agent → 生成配置片段并展示
    void pickTool(const ui::integrations::Tool& tool);

    std::vector<ui::ThemeSwatch*> swatches_;
    QVector<QPushButton*> toolBtns_;
    QLabel* detectLabel_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* configView_ = nullptr;
    QLabel* pathHint_ = nullptr;
    QLabel* stepsLabel_ = nullptr;
    QPushButton* copyBtn_ = nullptr;
    QWidget* resultBox_ = nullptr;
    ui::integrations::Tool current_;
    ah::Platform& platform_;
};
