#include "errors_panel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

ErrorsPanel::ErrorsPanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    buildHeader(layout, "errors", "错误报告", "Error Reports",
                "报错必须记录、可认领解决；解决说明只追加不覆盖",
                "Errors must be reported and resolved; notes are append-only");

    auto* toolbar = new QHBoxLayout;
    statusCombo_ = new QComboBox(this);
    statusCombo_->addItems({"全部", "open", "investigating", "resolved"});
    severityCombo_ = new QComboBox(this);
    severityCombo_->addItems({"全部", "info", "warning", "error", "critical"});
    resolveBtn_ = new QPushButton(i18n::trs("登记解决", "Mark Resolved"), this);
    toolbar->addWidget(new QLabel("状态:", this));
    toolbar->addWidget(statusCombo_);
    toolbar->addWidget(new QLabel("严重度:", this));
    toolbar->addWidget(severityCombo_);
    toolbar->addStretch(1);
    toolbar->addWidget(resolveBtn_);
    layout->addLayout(toolbar);
    connect(statusCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(severityCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(resolveBtn_, &QPushButton::clicked, this, &ErrorsPanel::onResolve);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter_ = splitter;
    table_ = new QTableWidget(0, 5, splitter);
    table_->setHorizontalHeaderLabels({i18n::trs("严重度", "Severity"), i18n::trs("标题", "Title"),
                                      i18n::trs("上报者", "Reporter"), i18n::trs("状态", "Status"), i18n::trs("时间", "Time")});
    table_->horizontalHeader()->setStretchLastSection(true);
    polishTable(table_);
    attachTableContextMenu(table_);
    // 即时过滤框：置于「登记解决」左侧，需在 table_ 就绪后创建
    filterEdit_ = makeTableFilter(table_, this);
    toolbar->insertWidget(toolbar->indexOf(resolveBtn_), filterEdit_);
    // 双击错误行 = 直接进入「登记解决」流程
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { if (row >= 0) onResolve(); });
    // 分栏宽度持久化：跨会话记住左右比例
    QSettings s;
    splitter->restoreState(s.value("ui/splitter/errors").toByteArray());
    connect(splitter, &QSplitter::splitterMoved, this, [splitter](int, int) {
        QSettings s;
        s.setValue("ui/splitter/errors", splitter->saveState());
    });
    splitter->addWidget(table_);

    auto* right = new QWidget(splitter);
    auto* rl = new QVBoxLayout(right);
    infoLabel_ = new QLabel(right);
    rl->addWidget(infoLabel_);
    detail_ = new QTextBrowser(right);
    rl->addWidget(detail_, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    // 空状态：说明"这里会出现什么 + 怎么补充内容"，并给手动上报的显眼入口
    emptyState_ = new ui::InlineEmpty(
        "errors",
        i18n::trs("暂无错误，一切正常 ✓", "No errors — all clear ✓"),
        i18n::trs("Agent 运行中上报的错误会自动汇总到这里，按严重度分级，处理完登记解决说明。",
                  "Errors reported by agents are collected here, ranked by severity; record a "
                  "resolution note once handled."),
        this);
    emptyState_->setAction(i18n::trs("＋ 手动上报错误", "＋ Report an issue"),
                           [this] { onManualReport(); });
    layout->addWidget(emptyState_);

    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row < 0 || row >= static_cast<int>(errors_.size())) return;
        const auto& e = errors_[static_cast<size_t>(row)];
        infoLabel_->setText(QString("<b>%1</b> · 上报者: %2 · 来源: %3 · 状态: %4")
                                .arg(QString::fromStdString(e.title).toHtmlEscaped())
                                .arg(QString::fromStdString(e.reporter))
                                .arg(QString::fromStdString(e.source))
                                .arg(QString::fromStdString(e.status)));
        QString text = QString::fromStdString(e.detail);
        if (!e.stack_trace.empty())
            text += "\n\n---- 堆栈 ----\n" + QString::fromStdString(e.stack_trace);
        if (!e.resolution_notes.empty())
            text += "\n\n---- 解决记录（追加） ----\n" + QString::fromStdString(e.resolution_notes);
        detail_->setPlainText(text);
    });
}

void ErrorsPanel::refresh() {
    std::string status = statusCombo_->currentIndex() > 0
                             ? statusCombo_->currentText().toStdString()
                             : "";
    std::string severity = severityCombo_->currentIndex() > 0
                               ? severityCombo_->currentText().toStdString()
                               : "";
    std::string err;
    platform_.errorList(status, severity, 200, errors_, err);

    table_->setRowCount(static_cast<int>(errors_.size()));
    for (size_t i = 0; i < errors_.size(); ++i) {
        const auto& e = errors_[i];
        setRow(table_, static_cast<int>(i),
               {QString::fromStdString(e.severity), QString::fromStdString(e.title),
                QString::fromStdString(e.reporter), QString::fromStdString(e.status),
                relTime(QString::fromStdString(e.created_at))});
        // 严重度着色：critical 红 / error 橙 / warning 黄 / info 灰
        QString sev = QString::fromStdString(e.severity);
        QColor c = sev == "critical" ? ui::danger()
                                     : (sev == "error" ? ui::warn()
                                                       : (sev == "warning" ? ui::note() : ui::muted()));
        if (auto* item = table_->item(static_cast<int>(i), 0)) {
            item->setForeground(c);
            QFont f = item->font();
            f.setBold(sev == "critical" || sev == "error");
            item->setFont(f);
        }
    }
    fitTableColumns(table_, 1);  // 标题列吃剩余空间，避免窄列被截断 / 出现横向滚动条
    emptyState_->setVisible(errors_.empty());
    splitter_->setVisible(!errors_.empty());
    applyTableFilter(table_, filterEdit_->text());  // 行已重建，重放即时过滤态
}

void ErrorsPanel::focusFilter() {
    filterEdit_->setFocus();
    filterEdit_->selectAll();
}

void ErrorsPanel::onResolve() {
    int row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(errors_.size())) {
        QMessageBox::information(this, i18n::trs("解决", "Resolve"),
                             i18n::trs("请先选择一条错误", "Select an error first"));
        return;
    }
    const auto& e = errors_[static_cast<size_t>(row)];
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("登记解决: %1", "Resolve: %1").arg(QString::fromStdString(e.title)));
    auto* l = new QVBoxLayout(&dlg);
    auto* notes = new QPlainTextEdit(&dlg);
    notes->setPlaceholderText(i18n::trs("解决说明（追加到错误记录，不覆盖原内容）",
        "Resolution notes (appended; original content is never overwritten)"));
    l->addWidget(notes);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    l->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    ah::ErrorReport out;
    std::string err;
    if (!platform_.errorResolve("user", e.uuid, notes->toPlainText().toStdString(), out, err)) {
        ui::Toast::show(this, ui::humanError(QString::fromStdString(err)), false);
        return;
    }
    ui::Toast::show(this, i18n::trs("已登记解决 ✓", "Resolution recorded ✓"));
    refresh();
}

void ErrorsPanel::onManualReport() {
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("手动上报错误", "Report an issue"));
    auto* form = new QFormLayout(&dlg);
    auto* sev = new QComboBox(&dlg);
    sev->addItem(i18n::trs("提示 info", "info"), QString("info"));
    sev->addItem(i18n::trs("警告 warn", "warn"), QString("warn"));
    sev->addItem(i18n::trs("严重 critical", "critical"), QString("critical"));
    auto* title = new QLineEdit(&dlg);
    title->setPlaceholderText(i18n::trs("一句话概括问题", "One-line summary"));
    auto* detail = new QPlainTextEdit(&dlg);
    detail->setPlaceholderText(i18n::trs("发生了什么、怎么复现、影响范围…",
                                         "What happened, how to reproduce, impact…"));
    form->addRow(i18n::trs("严重度", "Severity"), sev);
    form->addRow(i18n::trs("标题", "Title"), title);
    form->addRow(i18n::trs("详情", "Details"), detail);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    if (title->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, i18n::trs("缺少标题", "Title required"),
                             i18n::trs("请先填写一句话概括，方便后续检索。",
                                       "Add a one-line summary so it can be found later."));
        return;
    }
    ah::ErrorReport out;
    std::string err;
    if (!platform_.errorReport("user", sev->currentData().toString().toStdString(), "manual",
                               title->text().trimmed().toStdString(),
                               detail->toPlainText().toStdString(), "", out, err)) {
        QMessageBox::warning(this, i18n::trs("上报失败", "Report failed"),
                             ui::humanError(QString::fromStdString(err)));
        return;
    }
    ui::Toast::show(this, i18n::trs("已上报，可在列表中查看 ✓", "Reported — see it in the list ✓"));
    refresh();
}
