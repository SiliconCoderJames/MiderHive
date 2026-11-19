#include "ask_dialog.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

#include "i18n.h"
#include "theme.h"

AskDialog::AskDialog(ah::Platform& platform, std::function<void()> openSettings,
                     QWidget* parent)
    : QDialog(parent), platform_(platform), openSettings_(std::move(openSettings)) {
    setWindowTitle(i18n::trs("临时问答", "Ask Console"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(760, 640);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    // 状态行：模型名 + 本会话用量累计（豁免口径的如实展示）
    stateLine_ = new QLabel(this);
    stateLine_->setTextFormat(Qt::RichText);
    stateLine_->setWordWrap(true);
    root->addWidget(stateLine_);

    chat_ = new QTextBrowser(this);
    chat_->setReadOnly(true);
    chat_->setOpenExternalLinks(false);
    chat_->setPlaceholderText(i18n::trs(
        "向下方的模型提问。回答逐字流式返回，用法见设置 → LLM 接入。",
        "Ask the configured model below. Replies stream in; set it up in Settings > LLM."));
    root->addWidget(chat_, 1);

    auto* bottom = new QHBoxLayout;
    input_ = new QPlainTextEdit(this);
    input_->setPlaceholderText(i18n::trs("输入问题，Enter 发送，Shift+Enter 换行…",
                                         "Type a question; Enter sends, Shift+Enter for newline…"));
    input_->setFixedHeight(72);
    input_->installEventFilter(this);
    bottom->addWidget(input_, 1);
    send_ = new QPushButton(i18n::trs("发送", "Send"), this);
    send_->setDefault(true);
    bottom->addWidget(send_, 0, Qt::AlignBottom);
    root->addLayout(bottom);

    goSettings_ = new QPushButton(i18n::trs("去设置 LLM 接入", "Open LLM settings"), this);
    goSettings_->hide();
    root->addWidget(goSettings_, 0, Qt::AlignLeft);

    connect(send_, &QPushButton::clicked, this, &AskDialog::send);
    connect(goSettings_, &QPushButton::clicked, this, [this] {
        if (openSettings_) openSettings_();
    });

    // 长生命周期窗口：注册主题监听（换主题即时重涂）；析构时移除
    themeListenerId_ = ui::addThemeListener([this] { retheme(); });
    retheme();
}

AskDialog::~AskDialog() {
    ui::removeThemeListener(themeListenerId_);
    // 传导取消：delta 回调返回 false + transport 句柄硬中止，worker 很快收尾；
    // 析构 join 在 UI 线程，但有了硬中止后最多等一个退出码的往返，不再冻 120s
    abort_ = true;
    if (activeClient_) activeClient_->cancel();
    if (worker_.joinable()) worker_.join();
}

void AskDialog::retheme() {
    chat_->setStyleSheet(ui::th("QTextBrowser { background:@card@; border:1px solid @line@;"
                                " border-radius:8px; padding:8px; color:@text@;"
                                " font-size:12px; }"));
    input_->setStyleSheet(ui::th("QPlainTextEdit { background:@deep@; border:1px solid @line@;"
                                 " border-radius:8px; padding:6px 8px; color:@text@;"
                                 " font-size:12px; }"));
    send_->setStyleSheet(ui::th("QPushButton { background:@selbg@; color:@seltext@;"
                                " padding:6px 18px; border-radius:7px; font-weight:600; }"
                                "QPushButton:disabled { color:@muted@; }"));
    goSettings_->setStyleSheet(ui::th(
        "QPushButton { color:@brand@; border:1px solid @line@; padding:4px 12px;"
        " border-radius:7px; background:transparent; }"));
    refreshStateLine();
    // 已有会话内容的角色标记颜色会随主题漂移——重绘整段由 DOM 自适应色承担，
    // 这里只重涂铬层，正文保持用户可读的纯文本流
}

void AskDialog::showEvent(QShowEvent*) {
    // 配置检查：未配置时禁输入并给"去设置"出口（不静默失败）
    ah::llm::LlmConfig cfg;
    bool configured = false;
    std::string err;
    inputConfigured_ = platform_.llmConfigGet(cfg, configured, err) && configured;
    input_->setEnabled(inputConfigured_);
    send_->setEnabled(inputConfigured_ && !busy_.load());
    goSettings_->setVisible(!inputConfigured_);
    if (!inputConfigured_) {
        chat_->setPlaceholderText(
            i18n::trs("尚未配置大模型——点下方「去设置 LLM 接入」完成后即可提问。",
                      "No LLM configured yet — use \"Open LLM settings\" below to ask."));
    }
    refreshStateLine();
    if (inputConfigured_) input_->setFocus();
}

void AskDialog::refreshStateLine() {
    ah::llm::LlmConfig cfg;
    bool configured = false;
    std::string err;
    const bool ok = platform_.llmConfigGet(cfg, configured, err) && configured;
    const QString model =
        ok ? QString::fromStdString(cfg.model) : i18n::trs("未配置", "not configured");
    stateLine_->setText(
        QString("<span style='color:%1'>%2 · %3</span> <span style='color:%4'>· "
                "%5 %6 / %7 %8</span>")
            .arg(ui::selText().name(), i18n::trs("模型", "model"), model.toHtmlEscaped(),
                 ui::muted().name(), i18n::trs("会话入", "in"), QString::number(sessIn_),
                 i18n::trs("出", "out"), QString::number(sessOut_)));
}

bool AskDialog::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == input_ && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
            !(ke->modifiers() & Qt::ShiftModifier)) {
            send();
            return true;
        }
    }
    return QDialog::eventFilter(obj, ev);
}

void AskDialog::appendHtml(const QString& html) {
    // 追加前判断是否贴底：贴底则追后保持贴底（阅读历史时不打扰）
    auto* bar = chat_->verticalScrollBar();
    const bool pinned = bar->value() >= bar->maximum() - 4;
    chat_->append(html);
    if (pinned) bar->setValue(bar->maximum());
}

void AskDialog::send() {
    if (busy_.load()) return;
    const QString text = input_->toPlainText().trimmed();
    if (text.isEmpty()) return;

    // 两段式：先在 GUI 线程拿客户端（只读配置，无网络），所有权 GUI/worker
    // 各持一份——关窗时 GUI 侧 cancel() 硬中止阻塞读，client 存活期由
    // worker 的 shared_ptr 副本兜底
    ah::llm::LlmConfig cfg;
    std::string cfgErr;
    auto client = platform_.llmAskOpen(cfg, cfgErr);
    if (!client) {
        goSettings_->setVisible(true);
        input_->setEnabled(false);
        send_->setEnabled(false);
        return;
    }

    history_.push_back({"user", text.toStdString()});
    appendHtml(QString("<div style='margin-top:10px'><span style='color:%1; font-weight:600;'>"
                       "%2 ›</span> <span style='color:%3;'>%4</span></div>")
                   .arg(ui::brand().name(), i18n::trs("你", "You"),
                        ui::selText().name(), text.toHtmlEscaped()));
    input_->clear();
    busy_ = true;
    abort_ = false;
    streamLineOpen_ = false;
    send_->setEnabled(false);
    activeClient_ = std::shared_ptr<ah::llm::LlmClient>(client.release());
    std::shared_ptr<ah::llm::LlmClient> shared = activeClient_;
    appendHtml(QString("<div style='margin-top:8px; color:%1;'>%2 · %3</div>")
                   .arg(ui::muted().name(), i18n::trs("助手", "assistant"),
                        QString::fromStdString(cfg.model).toHtmlEscaped()));

    // 值拷贝历史给 worker（3s 自动刷新机制不触碰本对话框，无消息面板那类
    // 悬垂引用；这里防的是调用方在流式中途关窗）
    const std::vector<ah::llm::LlmMessage> hist = history_;
    QPointer<AskDialog> guard(this);
    if (worker_.joinable()) worker_.join();  // 上一次已收尾（busy_ 为假才到这）
    worker_ = std::thread([guard, pl = &platform_, hist, shared, cfg] {
        std::string err;  // 新局部：stale-err 无从谈起（平台侧 complete 不清 err）
        ah::llm::LlmReply reply;
        auto onDelta = [guard](const char* d, std::size_t n) -> bool {
            if (!guard || guard->abort_.load()) return false;  // 取消：传导到传输层
            const QString piece = QString::fromUtf8(d, static_cast<qsizetype>(n));
            QMetaObject::invokeMethod(
                guard, [guard, piece] { if (guard) guard->appendDelta(piece); },
                Qt::QueuedConnection);
            return true;
        };
        bool ok = true;
        try {
            reply = shared->complete(hist, onDelta, err);
        } catch (const std::exception& ex) {
            ok = false;
            err = std::string("client error: ") + ex.what();
        }
        const bool cancelled = err == "cancelled";
        QString recordWarn;
        if (ok && err.empty() && !cancelled) {
            // 收尾记账+审计；失败如实带警告（回复本身有效，不掩蔽）
            std::string recErr;
            if (!pl->llmAskRecord(hist, cfg, reply, recErr))
                recordWarn = QString::fromStdString(recErr);
        } else {
            ok = false;
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, ok, cancelled, errText = QString::fromStdString(err),
             text = QString::fromStdString(reply.text), truncated = reply.truncated,
             in = reply.tokensIn, out = reply.tokensOut, recordWarn] {
                if (guard)
                    guard->finishAssistant(ok, cancelled, errText, text, truncated, in, out,
                                           recordWarn);
            },
            Qt::QueuedConnection);
    });
}

void AskDialog::appendDelta(const QString& delta) {
    QTextCursor cur = chat_->textCursor();
    cur.movePosition(QTextCursor::End);
    if (!streamLineOpen_) {
        // 标题行（"助手 · 模型"）之后另起一块：否则首个 delta 会续写在标题行尾
        // 并继承其灰色字符格式，整段回答灰贴在标题后
        cur.insertBlock();
        streamLineOpen_ = true;
    }
    cur.insertText(delta);
    auto* bar = chat_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void AskDialog::finishAssistant(bool ok, bool cancelled, const QString& errText,
                                const QString& text, bool truncated, qint64 in, qint64 out,
                                const QString& recordWarn) {
    busy_ = false;
    activeClient_.reset();
    send_->setEnabled(inputConfigured_);
    if (!ok) {
        // 失败的提问不留在历史里（没有对应回复的多轮上下文只会污染下一次请求）
        if (!history_.empty() && history_.back().role == "user") history_.pop_back();
        if (cancelled) {
            appendHtml(QString("<div style='color:%1; margin-top:4px;'>— %2 —</div>")
                           .arg(ui::muted().name(), i18n::trs("已中止", "cancelled")));
        } else {
            const QString shown = errText.isEmpty()
                                      ? i18n::trs("未知错误", "unknown error")
                                      : errText.toHtmlEscaped();
            appendHtml(QString("<div style='color:%1; margin-top:4px;'>%2：%3</div>")
                           .arg(ui::danger().name(), i18n::trs("请求失败", "request failed"),
                                shown));
        }
        return;
    }
    history_.push_back({"assistant", text.toStdString()});
    const QString truncNote =
        truncated ? i18n::trs("（回复因长度上限被截断）", " (truncated at length limit)")
                  : QString();
    if (!recordWarn.isEmpty()) {
        // 记账失败：回复有效但账面没记上——绝不能显示"已记账"糊弄用户
        appendHtml(QString("<div style='color:%1; margin-top:4px;'>%2：%3</div>")
                       .arg(ui::note().name(),
                            i18n::trs("已收到回复，但记账失败", "reply received, but accounting "
                                      "failed"),
                            recordWarn.toHtmlEscaped()));
        refreshStateLine();
        return;
    }
    sessIn_ += in;
    sessOut_ += out;
    const QString usage =
        in <= 0 && out <= 0
            ? i18n::trs("provider 未回 usage，记账记 0", "no usage returned, recorded as 0")
            : i18n::trs("本次 in %1 / out %2", "in %1 / out %2")
                  .arg(QString::number(in), QString::number(out));
    appendHtml(QString("<div style='color:%1; margin-top:4px;'>%2%3 · %4 %5 / %6 %7 · %8</div>")
                   .arg(ui::muted().name(),
                        i18n::trs("已记账（豁免预算）", "recorded (budget exempt)"), truncNote,
                        i18n::trs("会话", "session"), i18n::trs("入", "in"),
                        i18n::trs("出", "out"), usage));
    refreshStateLine();
}

void AskDialog::closeEvent(QCloseEvent* e) {
    // 流式中途关窗：abort 走 delta 回调取消，cancel() 直接关 transport 句柄
    // （provider 长时间不吐块时 join 不再冻结 UI）
    if (busy_.load()) {
        abort_ = true;
        if (activeClient_) activeClient_->cancel();
    }
    QDialog::closeEvent(e);
}
