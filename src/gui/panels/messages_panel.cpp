#include "messages_panel.h"

#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

namespace {
QString kindChip(const QString& kind) {
    const auto chip = [](const QColor& c, const QString& label) {
        return QString("<span style='background:rgba(%1,%2,%3,70);color:%4;"
                       "border-radius:4px;padding:1px 6px;'>%5</span>")
            .arg(c.red())
            .arg(c.green())
            .arg(c.blue())
            .arg(c.lighter(140).name())
            .arg(label);
    };
    if (kind == "task") return chip(ui::danger(), i18n::trs("任务", "Task"));
    if (kind == "question") return chip(ui::warn(), i18n::trs("提问", "Q"));
    return chip(ui::accent(), i18n::trs("留言", "Note"));
}
QString statusChip(const QString& s) {
    QString color = s == "done" ? ui::ok().name()
                                 : (s == "declined" ? ui::danger().name()
                                                    : (s == "accepted" ? ui::accent().name()
                                                                       : ui::note().name()));
    return QString("<span style='color:%1;'>● %2</span>").arg(color).arg(s.toHtmlEscaped());
}
// 发送者头像圈：取首字符，颜色由名字哈希决定（固定 6 色板）
QString avatar(const QString& sender) {
    static const char* kPalettes[] = {
        "#0ea5e9", "#22c55e", "#f59e0b", "#a78bfa", "#f472b6", "#34d399"};
    quint32 h = 0;
    for (QChar c : sender) h = h * 31 + c.unicode();
    QString initial = sender.left(1).toUpper().toHtmlEscaped();
    return QString("<span style='display:inline-block; min-width:18px; text-align:center;"
                   " background:%1; color:#101010; font-weight:700; border-radius:9px;"
                   " padding:1px 0;'>%2</span>")
        .arg(kPalettes[h % 6], initial);
}
}  // namespace

MessagesPanel::MessagesPanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    buildHeader(layout, "messages", "Agent 交流", "Messaging",
                "留言 / 提问 / 指派任务，异步流转，不要求同时在线",
                "Notes, questions and tasks with async state machine");

    auto* toolbar = new QHBoxLayout;
    kindCombo_ = new QComboBox(this);
    // 首项是"不过滤"的展示项（下标 0 → 后端空筛选），可安全翻译；
    // 其余项是后端枚举值，按下标 > 0 原样回传，不译
    kindCombo_->addItem(i18n::trs("全部", "All"));
    kindCombo_->addItems({"note", "question", "task"});
    statusCombo_ = new QComboBox(this);
    statusCombo_->addItem(i18n::trs("全部", "All"));
    statusCombo_->addItems({"unread", "read", "pending", "accepted", "done", "declined"});
    composeBtn_ = new QPushButton(i18n::trs("＋ 发消息 / 指派任务", "＋ Message / Assign Task"), this);
    composeBtn_->setObjectName("primary");
    kindLabel_ = new QLabel(i18n::trs("类型:", "Kind:"), this);
    statusLabel_ = new QLabel(i18n::trs("状态:", "Status:"), this);
    toolbar->addWidget(kindLabel_);
    toolbar->addWidget(kindCombo_);
    toolbar->addWidget(statusLabel_);
    toolbar->addWidget(statusCombo_);
    toolbar->addStretch(1);
    toolbar->addWidget(composeBtn_);
    layout->addLayout(toolbar);
    connect(kindCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(statusCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(composeBtn_, &QPushButton::clicked, this, &MessagesPanel::onCompose);

    // 对话流（点击气泡选中，锚点携带消息 uuid）
    chat_ = new QTextBrowser(this);
    chat_->setOpenLinks(false);
    layout->addWidget(chat_, 1);
    connect(chat_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        selectedUuid_ = url.toString().toStdString();
        updateActions();
    });
    // 双击气泡 = 直接进入回复（锚点即消息 uuid）
    chat_->viewport()->installEventFilter(this);

    // 操作条
    infoLabel_ = new QLabel(this);
    infoLabel_->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    auto* brow = new QHBoxLayout;
    replyBtn_ = new QPushButton(i18n::trs("回复", "Reply"), this);
    readBtn_ = new QPushButton(i18n::trs("标记已读", "Mark Read"), this);
    acceptBtn_ = new QPushButton(i18n::trs("接受任务", "Accept"), this);
    doneBtn_ = new QPushButton(i18n::trs("任务完成", "Done"), this);
    declineBtn_ = new QPushButton(i18n::trs("拒绝任务", "Decline"), this);
    // 动作层级：回复是主操作；"拒绝任务"用危险色，避免和一排同色按钮混在一起误点
    replyBtn_->setObjectName("primary");
    declineBtn_->setObjectName("danger");
    brow->addWidget(infoLabel_, 1);
    brow->addWidget(replyBtn_);
    brow->addWidget(readBtn_);
    brow->addWidget(acceptBtn_);
    brow->addWidget(doneBtn_);
    brow->addWidget(declineBtn_);
    layout->addLayout(brow);

    connect(replyBtn_, &QPushButton::clicked, this, &MessagesPanel::onReply);
    connect(readBtn_, &QPushButton::clicked, this, [this] { onStatus("read"); });
    connect(acceptBtn_, &QPushButton::clicked, this, [this] { onStatus("accepted"); });
    connect(doneBtn_, &QPushButton::clicked, this, [this] { onStatus("done"); });
    connect(declineBtn_, &QPushButton::clicked, this, [this] { onStatus("declined"); });
    updateActions();
}

void MessagesPanel::refresh() {
    std::string kind, status;
    if (kindCombo_->currentIndex() > 0) kind = kindCombo_->currentText().toStdString();
    if (statusCombo_->currentIndex() > 0) status = statusCombo_->currentText().toStdString();

    std::string err;
    platform_.messageList("", kind, status, "", 200, messages_, err);
    renderChat();
}

bool MessagesPanel::eventFilter(QObject* obj, QEvent* e) {
    if (obj == chat_->viewport() && e->type() == QEvent::MouseButtonDblClick) {
        auto* me = static_cast<QMouseEvent*>(e);
        const QUrl url = chat_->anchorAt(me->position().toPoint());
        if (!url.isEmpty()) {
            selectedUuid_ = url.toString().toStdString();
            updateActions();
            onReply();
            return true;  // 双击已消费，避免再触发选中
        }
    }
    return PanelBase::eventFilter(obj, e);
}

void MessagesPanel::renderChat() {
    // 3s 刷新重建 HTML 会把滚动条归零：先记住位置，渲染后恢复
    int scrollPos = chat_->verticalScrollBar()->value();
    bool atBottom = chat_->verticalScrollBar()->value() >=
                    chat_->verticalScrollBar()->maximum() - 4;
    QString html;
    for (const auto& m : messages_) {
        bool selected = m.uuid == selectedUuid_;
        QString recipient =
            m.recipient.empty() ? i18n::trs("全员", "everyone") : QString::fromStdString(m.recipient);
        // 气泡：选中描边高亮；task/question/note 用色区分；头部带头像圈
        html += QString(
                    "<a name='%1'></a>"
                    "<div style='margin:6px 4px;'>"
                    "<span style='color:@muted@; font-size:10px; font-family:@mono@;'>%2</span> %7 "
                    "<span style='color:@text@; font-size:11px;'>%3</span>"
                    " <span style='color:@muted@;'>→ %4</span> %5 %6"
                    "<div style='%9'>"
                    "<b>%8</b><br>%10"
                    "</div></div>")
                    .arg(QString::fromStdString(m.uuid))
                    .arg(relTime(QString::fromStdString(m.created_at)))
                    .arg(QString::fromStdString(m.sender).toHtmlEscaped())
                    .arg(recipient.toHtmlEscaped())
                    .arg(kindChip(QString::fromStdString(m.kind)))
                    .arg(statusChip(QString::fromStdString(m.status)))
                    .arg(avatar(QString::fromStdString(m.sender)))
                    .arg(QString::fromStdString(m.subject).toHtmlEscaped())
                    .arg(selected
                             ? "background:@fieldhover@; border:1px solid @accent@; border-radius:8px; padding:8px 12px;"
                             : "background:@card@; border:1px solid @line@; border-radius:8px; padding:8px 12px;")
                    .arg(QString::fromStdString(m.body).toHtmlEscaped()
                             .replace("\n", "<br>")
                             .left(500));
    }
    if (html.isEmpty()) {
        ui::attachHexMotif(chat_->document());
        chat_->setHtml(ui::th(
            QString("<div style='text-align:center; margin-top:24px;'>"
                    "<img src='hexmotif' width='96' height='70'>"
                    "<div style='font-size:13px; font-weight:600; color:@text@; margin-top:6px;'>%1</div>"
                    "<div style='color:@muted@; margin-top:6px;'>%2</div></div>")
                .arg(i18n::trs("暂无消息", "No messages yet"))
                .arg(i18n::trs("点击右上角发起交流，或让任意 Agent 向你留言 / 指派任务",
                               "Start a conversation, or let any agent message / task you"))));
    } else {
        chat_->setHtml(ui::th(html));
    }
    // 恢复滚动位置；原本就在底部（阅读最新消息）则保持贴底
    QScrollBar* bar = chat_->verticalScrollBar();
    if (atBottom)
        bar->setValue(bar->maximum());
    else
        bar->setValue(qMin(scrollPos, bar->maximum()));
    // 选中气泡被重建后滚回可视区
    if (!selectedUuid_.empty()) chat_->scrollToAnchor(QString::fromStdString(selectedUuid_));
    updateActions();
}

void MessagesPanel::retranslate() {
    PanelBase::retranslate();
    kindLabel_->setText(i18n::trs("类型:", "Kind:"));
    statusLabel_->setText(i18n::trs("状态:", "Status:"));
    // 只改首项文案：其余项是后端枚举值，改了会把筛选值一并改坏
    if (kindCombo_->count() > 0) kindCombo_->setItemText(0, i18n::trs("全部", "All"));
    if (statusCombo_->count() > 0) statusCombo_->setItemText(0, i18n::trs("全部", "All"));
    composeBtn_->setText(i18n::trs("＋ 发消息 / 指派任务", "＋ Message / Assign Task"));
    replyBtn_->setText(i18n::trs("回复", "Reply"));
    readBtn_->setText(i18n::trs("标记已读", "Mark Read"));
    acceptBtn_->setText(i18n::trs("接受任务", "Accept"));
    doneBtn_->setText(i18n::trs("任务完成", "Done"));
    declineBtn_->setText(i18n::trs("拒绝任务", "Decline"));
    // 气泡 HTML（类型/状态胶囊、收件人、空状态）与选中摘要在 renderChat/updateActions 里取词：
    // 就地重跑一次即可换语言，不再查库（用已有的 messages_；renderChat 末尾会带 updateActions）
    renderChat();
}

void MessagesPanel::updateActions() {
    bool has = !selectedUuid_.empty();
    const ah::Message* cur = nullptr;
    for (const auto& m : messages_)
        if (m.uuid == selectedUuid_) cur = &m;
    if (has && cur) {
        infoLabel_->setText(QString("%1: %2 (%3 · %4)")
                                .arg(i18n::trs("已选中", "Selected"))
                                .arg(QString::fromStdString(cur->subject).toHtmlEscaped())
                                .arg(QString::fromStdString(cur->kind))
                                .arg(QString::fromStdString(cur->status)));
    } else {
        infoLabel_->setText(i18n::trs("点击消息气泡以选中（可回复 / 流转状态）",
                                      "Click a message bubble to select (reply / change status)"));
    }
    replyBtn_->setEnabled(has);
    readBtn_->setEnabled(has && cur && cur->status == "unread" && cur->kind != "task");
    acceptBtn_->setEnabled(has && cur && cur->kind == "task" && cur->status == "pending");
    doneBtn_->setEnabled(has && cur && cur->kind == "task" && cur->status == "accepted");
    declineBtn_->setEnabled(has && cur && cur->kind == "task" && cur->status == "pending");
}

void MessagesPanel::onCompose() {
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("发消息 / 指派任务", "Send Message / Assign Task"));
    auto* form = new QFormLayout(&dlg);
    auto* kind = new QComboBox(&dlg);
    kind->addItems({"note", "question", "task"});
    auto* recipient = new QComboBox(&dlg);
    recipient->addItem(i18n::trs("（广播给所有 Agent）", "(broadcast to all agents)"), "");
    std::vector<ah::AgentInfo> agents;
    std::string err;
    if (platform_.listAgents(agents, err))
        for (const auto& a : agents)
            recipient->addItem(QString::fromStdString(a.name), QString::fromStdString(a.name));
    auto* subject = new QLineEdit(&dlg);
    auto* body = new QPlainTextEdit(&dlg);
    form->addRow(i18n::trs("类型", "Kind"), kind);
    form->addRow(i18n::trs("接收者", "Recipient"), recipient);
    form->addRow(i18n::trs("主题", "Subject"), subject);
    form->addRow(i18n::trs("内容", "Body"), body);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    ah::Message out;
    if (!platform_.messageSend(kind->currentText().toStdString(), "user",
                               recipient->currentData().toString().toStdString(),
                               subject->text().trimmed().toStdString(),
                               body->toPlainText().toStdString(), out, err)) {
        ui::Toast::show(this, i18n::trs("发送失败: %1", "Send failed: %1")
                                  .arg(QString::fromStdString(err)),
                        false);
        return;
    }
    ui::Toast::show(this, i18n::trs("消息已发送 ✓", "Message sent ✓"));
    refresh();
}

void MessagesPanel::onReply() {
    const ah::Message* cur = nullptr;
    for (const auto& m : messages_)
        if (m.uuid == selectedUuid_) cur = &m;
    if (!cur) return;
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("回复: %1", "Reply: %1").arg(QString::fromStdString(cur->subject)));
    auto* l = new QVBoxLayout(&dlg);
    auto* body = new QPlainTextEdit(&dlg);
    l->addWidget(body);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    l->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    ah::Message out;
    std::string err;
    if (!platform_.messageReply("user", cur->uuid, body->toPlainText().toStdString(), out, err)) {
        ui::Toast::show(this, i18n::trs("回复失败: %1", "Reply failed: %1")
                                  .arg(QString::fromStdString(err)),
                        false);
        return;
    }
    ui::Toast::show(this, i18n::trs("回复已发送 ✓", "Reply sent ✓"));
    refresh();
}

void MessagesPanel::onStatus(const QString& status) {
    const ah::Message* cur = nullptr;
    for (const auto& m : messages_)
        if (m.uuid == selectedUuid_) cur = &m;
    if (!cur) return;
    ah::Message out;
    std::string err;
    if (!platform_.messageSetStatus("user", cur->uuid, status.toStdString(), out, err)) {
        ui::Toast::show(this, i18n::trs("状态变更失败: %1", "Status change failed: %1")
                                  .arg(QString::fromStdString(err)),
                        false);
        return;
    }
    ui::Toast::show(this, i18n::trs("状态已更新为 %1 ✓", "Status updated to %1 ✓").arg(status));
    refresh();
}
