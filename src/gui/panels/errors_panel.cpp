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

#include <iterator>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

namespace {
// 枚举的展示名（中文, 英文, 存储值）。筛选项、表格、详情栏共用这一份：此前筛选项
// 直接显示存储值（open / warning），表格里却是「未解决」「警告」——同一个值两种写法
struct EnumName {
    const char* zh;
    const char* en;
    const char* value;
};
constexpr EnumName kSeverity[] = {{"信息", "info", "info"},
                                  {"警告", "warning", "warning"},
                                  {"错误", "error", "error"},
                                  {"严重", "critical", "critical"}};
constexpr EnumName kStatus[] = {{"未解决", "open", "open"},
                                {"排查中", "investigating", "investigating"},
                                {"已解决", "resolved", "resolved"}};

template <size_t N>
QString enumText(const EnumName (&table)[N], const std::string& value) {
    for (const auto& e : table)
        if (value == e.value) return i18n::trs(e.zh, e.en);
    return QString::fromStdString(value);  // 认不出的值原样显示，不吞信息
}

// 筛选下拉：首项「全部」不过滤，其余项显示本地化名称、存储值放进 item data 回传后端
template <size_t N>
void fillFilterCombo(QComboBox* combo, const EnumName (&table)[N]) {
    combo->addItem(i18n::trs("全部", "All"));
    for (const auto& e : table)
        combo->addItem(i18n::trs(e.zh, e.en), QString::fromLatin1(e.value));
}
}  // namespace

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
    fillFilterCombo(statusCombo_, kStatus);
    severityCombo_ = new QComboBox(this);
    fillFilterCombo(severityCombo_, kSeverity);
    resolveBtn_ = new QPushButton(i18n::trs("登记解决", "Mark Resolved"), this);
    statusLabel_ = new QLabel(i18n::trs("状态:", "Status:"), this);
    severityLabel_ = new QLabel(i18n::trs("严重度:", "Severity:"), this);
    toolbar->addWidget(statusLabel_);
    toolbar->addWidget(statusCombo_);
    toolbar->addWidget(severityLabel_);
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
    // 标题、上报者、来源都是用户输入，长了要换行：不换行会顶出详情栏被裁掉
    infoLabel_->setWordWrap(true);
    rl->addWidget(infoLabel_);
    detail_ = new QTextBrowser(right);
    rl->addWidget(detail_, 1);
    splitter->addWidget(right);
    // 左侧表格占大头：标题是最该读全的一列。此前反过来是 2:3，1280 宽的窗口里标题列
    // 被挤成"agents.json 写…"，右边详情栏却空着一大片
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
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

    connect(table_, &QTableWidget::cellClicked, this,
            [this](int row, int) { showDetail(row); });
}

void ErrorsPanel::showDetail(int row) {
    if (row < 0 || row >= static_cast<int>(errors_.size())) return;
    const auto& e = errors_[static_cast<size_t>(row)];
    // 富文本：用户输入逐项转义；状态用展示名，与表格、筛选项同一种写法
    infoLabel_->setText(i18n::trs("<b>%1</b> · 上报者: %2 · 来源: %3 · 状态: %4",
                                  "<b>%1</b> · reporter: %2 · source: %3 · status: %4")
                            .arg(QString::fromStdString(e.title).toHtmlEscaped(),
                                 QString::fromStdString(e.reporter).toHtmlEscaped(),
                                 QString::fromStdString(e.source).toHtmlEscaped(),
                                 enumText(kStatus, e.status)));
    QString text = QString::fromStdString(e.detail);
    if (!e.stack_trace.empty())
        text += i18n::trs("\n\n---- 堆栈 ----\n", "\n\n---- Stack trace ----\n") +
                QString::fromStdString(e.stack_trace);
    if (!e.resolution_notes.empty())
        text += i18n::trs("\n\n---- 解决记录（追加） ----\n",
                          "\n\n---- Resolution notes (appended) ----\n") +
                QString::fromStdString(e.resolution_notes);
    detail_->setPlainText(text);
}

void ErrorsPanel::refresh() {
    // 存储值取自 item data（显示的是本地化名称，不能拿显示文字回传后端）
    const auto filterValue = [](QComboBox* c) {
        return c->currentIndex() > 0 ? c->currentData().toString().toStdString() : std::string();
    };
    const std::string status = filterValue(statusCombo_);
    const std::string severity = filterValue(severityCombo_);
    // 刷新前按 uuid 记住选中：行号在重建后不可靠——3 秒自动刷新 + 新错误插到
    // 顶端会让同一行号指向另一条记录，"登记解决"此前会写到错误的条目上
    QString prevUuid;
    if (int row = table_->currentRow(); row >= 0 && row < static_cast<int>(errors_.size()))
        prevUuid = QString::fromStdString(errors_[static_cast<size_t>(row)].uuid);

    std::string err;
    if (!platform_.errorList(status, severity, 200, errors_, err)) {
        // 读失败必须如实提示：服务层失败前已把 errors_ 清空，不提示的话界面
        // 会把读失败呈现成"暂无错误，一切正常 ✓"。保留上一轮表格不重建。
        ui::Toast::show(this, ui::humanError(QString::fromStdString(err)), false);
        return;
    }

    table_->setRowCount(static_cast<int>(errors_.size()));
    for (size_t i = 0; i < errors_.size(); ++i) {
        const auto& e = errors_[i];
        // 枚举本地化（展示层映射，存储仍英文枚举）
        const std::string sev = e.severity;
        setRow(table_, static_cast<int>(i),
               {enumText(kSeverity, sev), QString::fromStdString(e.title),
                QString::fromStdString(e.reporter), enumText(kStatus, e.status),
                relTime(QString::fromStdString(e.created_at))});
        // 严重度着色：critical 红 / error 橙 / warning 黄 / info 灰
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
    // 恢复选中到同一条记录（范式与 skills_panel 一致），详情栏同步——程序化
    // selectRow 不会触发 cellClicked。此前没有默认选中：列表里有错误，右侧详情栏
    // 却是一整块空白（知识库、技能库都是默认选中第一行）
    int restoreRow = -1;
    if (!prevUuid.isEmpty())
        for (size_t i = 0; i < errors_.size(); ++i)
            if (QString::fromStdString(errors_[i].uuid) == prevUuid) {
                restoreRow = static_cast<int>(i);
                break;
            }
    if (restoreRow < 0 && !errors_.empty()) restoreRow = 0;
    if (restoreRow >= 0) {
        table_->selectRow(restoreRow);
        showDetail(restoreRow);
    } else {
        // 筛选后一条不剩：清掉上一条的详情，别在空列表旁边留着旧内容
        infoLabel_->setText(QString());
        detail_->setPlainText(QString());
    }
    // 没有选中行时「登记解决」无事可做，直接禁用。此前它一直可点，点了再弹
    // 「请先选择一条错误」的消息框
    resolveBtn_->setEnabled(restoreRow >= 0);
    // 空列表分两种：真的一条错误都没有，或者只是当前筛选下没有。后者不能写
    // 「一切正常 ✓」——用户按「已解决」筛出空列表时，未解决的错误可能还有一堆
    const bool filtered = !status.empty() || !severity.empty();
    emptyState_->titleLabel()->setText(
        filtered ? i18n::trs("该筛选下没有错误", "No errors under this filter")
                 : i18n::trs("暂无错误，一切正常 ✓", "No errors — all clear ✓"));
    emptyState_->hintLabel()->setText(
        filtered ? i18n::trs("把状态 / 严重度切回「全部」查看其它错误",
                             "Set status / severity back to \"All\" to see other errors")
                 : i18n::trs("Agent 运行中上报的错误会自动汇总到这里，按严重度分级，处理完登记解决说明。",
                             "Errors reported by agents are collected here, ranked by severity; "
                             "record a resolution note once handled."));
    emptyState_->setVisible(errors_.empty());
    splitter_->setVisible(!errors_.empty());
    applyTableFilter(table_, filterEdit_->text());  // 行已重建，重放即时过滤态
}

void ErrorsPanel::focusFilter() {
    filterEdit_->setFocus();
    filterEdit_->selectAll();
}

void ErrorsPanel::retranslate() {
    PanelBase::retranslate();
    statusLabel_->setText(i18n::trs("状态:", "Status:"));
    severityLabel_->setText(i18n::trs("严重度:", "Severity:"));
    // 只改文字不改下标：枚举值在 item data 里，显示名随语言变
    if (statusCombo_->count() > 0) statusCombo_->setItemText(0, i18n::trs("全部", "All"));
    for (size_t i = 0; i < std::size(kStatus); ++i)
        if (int(i) + 1 < statusCombo_->count())
            statusCombo_->setItemText(static_cast<int>(i) + 1, i18n::trs(kStatus[i].zh, kStatus[i].en));
    if (severityCombo_->count() > 0) severityCombo_->setItemText(0, i18n::trs("全部", "All"));
    for (size_t i = 0; i < std::size(kSeverity); ++i)
        if (int(i) + 1 < severityCombo_->count())
            severityCombo_->setItemText(static_cast<int>(i) + 1,
                                        i18n::trs(kSeverity[i].zh, kSeverity[i].en));
    // 当前选中那条的详情（状态展示名）也跟着换语言
    if (int row = table_->currentRow(); row >= 0 && row < static_cast<int>(errors_.size()))
        showDetail(row);
    resolveBtn_->setText(i18n::trs("登记解决", "Mark Resolved"));
    // 即时过滤框由 gui_util 的 makeTableFilter 建好（构造期已按当时语言取词），这里补齐重译
    filterEdit_->setPlaceholderText(i18n::trs("输入即筛…", "Type to filter…"));
    filterEdit_->setToolTip(i18n::trs("即时过滤（Ctrl+F 聚焦 · Esc 清空）",
                                      "Live filter (Ctrl+F to focus · Esc to clear)"));
    table_->setHorizontalHeaderLabels({i18n::trs("严重度", "Severity"), i18n::trs("标题", "Title"),
                                       i18n::trs("上报者", "Reporter"), i18n::trs("状态", "Status"),
                                       i18n::trs("时间", "Time")});
    emptyState_->titleLabel()->setText(i18n::trs("暂无错误，一切正常 ✓", "No errors — all clear ✓"));
    emptyState_->hintLabel()->setText(
        i18n::trs("Agent 运行中上报的错误会自动汇总到这里，按严重度分级，处理完登记解决说明。",
                  "Errors reported by agents are collected here, ranked by severity; record a "
                  "resolution note once handled."));
    // 空状态的动作按钮由 InlineEmpty::setAction 创建（重复调用会重复接信号），
    // 这里只取回按钮改文案，保持原有 connect 不被破坏
    if (auto* actionBtn = emptyState_->findChild<QPushButton*>())
        actionBtn->setText(i18n::trs("＋ 手动上报错误", "＋ Report an issue"));
}

void ErrorsPanel::onResolve() {
    int row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(errors_.size())) {
        // 按钮在无选中行时已禁用，这里只是兜底（如快捷键）：用 toast，不弹模态框
        ui::Toast::show(this, i18n::trs("请先选择一条错误", "Select an error first"), false);
        return;
    }
    // 按值拷贝：对话框 exec 的模态事件循环里 3s 自动刷新会重建 errors_（新错误
    // 插到顶端），引用会悬空/错位——exec 返回后经旧引用读 uuid 是 use-after-free
    //（审计 workbench-1），可能把解决说明登记到另一条错误上。
    const ah::ErrorReport e = errors_[static_cast<size_t>(row)];
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("登记解决: %1", "Resolve: %1").arg(QString::fromStdString(e.title)));
    sizeFormDialog(&dlg, 560, 340);
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
    sizeFormDialog(&dlg, 580, 440);
    auto* form = new QFormLayout(&dlg);
    auto* sev = new QComboBox(&dlg);
    // 四个严重度全列出来，显示名与表格、筛选项同一种写法（存储值在 item data 里）。
    // 此前只列了三个、缺"错误"，而且显示的是"提示 info"这种名字加枚举的混合写法；
    // data 必须是后端契约枚举——曾写成 "warn"，归一化后入库为 error，在"警告"筛选下
    // 永远看不到自己刚上报的条目
    for (const auto& e : kSeverity) sev->addItem(enumText(kSeverity, e.value), QString::fromLatin1(e.value));
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
