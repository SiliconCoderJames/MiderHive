#pragma once
// 临时问答对话框：直接向已配置的大模型提问，Codex 式会话流（多轮 + 流式）。
// 用量照常记账（token_usage，agent="user"），但走豁免通道（source=llm-ask），
// 不占周预算；状态行与结尾行如实展示本会话累计。
// 流式：llmAsk 的 delta 在 worker 线程到达，经 QMetaObject::invokeMethod 排队
// 回 UI 线程逐字插入；abort_ 置位沿 ChunkSink 返回 false 一路传导到传输层中止。
// 关闭对话框即中止：析构前 join worker，不悬挂线程。
#include <QDialog>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "core/llm/llm_client.h"
#include "core/platform.h"

class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTextBrowser;

class AskDialog : public QDialog {
    Q_OBJECT
public:
    // openSettings：未配置时的"去设置"引导（MainWindow 注入，跳设置页）
    explicit AskDialog(ah::Platform& platform, std::function<void()> openSettings,
                       QWidget* parent = nullptr);
    ~AskDialog() override;

protected:
    void showEvent(QShowEvent*) override;  // 每次打开：配置检查 + 焦点
    void closeEvent(QCloseEvent*) override;

private:
    void send();
    void refreshStateLine();
    void appendHtml(const QString& html);
    void appendDelta(const QString& delta);
    void finishAssistant(bool ok, bool cancelled, const QString& errText,
                         const QString& text, bool truncated, qint64 in, qint64 out,
                         const QString& recordWarn);
    void retheme();

    ah::Platform& platform_;
    std::function<void()> openSettings_;
    QTextBrowser* chat_ = nullptr;
    QPlainTextEdit* input_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* goSettings_ = nullptr;
    QLabel* stateLine_ = nullptr;
    std::vector<ah::llm::LlmMessage> history_;
    // 在途请求的客户端：GUI 与 worker 各持一份 shared_ptr——关窗时 GUI 侧
    // cancel()（transport 硬中止阻塞读），client 存活期由 worker 的副本兜底，
    // 不存在"回调指向已析构客户端"的竞态
    std::shared_ptr<ah::llm::LlmClient> activeClient_;
    bool streamLineOpen_ = false;  // 流式正文首 delta 前需要另起一行
    qint64 sessIn_ = 0, sessOut_ = 0;
    bool inputConfigured_ = false;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_{false};
    std::size_t themeListenerId_ = 0;

    bool eventFilter(QObject* obj, QEvent* ev) override;
};
