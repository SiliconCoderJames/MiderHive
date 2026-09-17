#include "knowledge_panel.h"

#include <algorithm>
#include <map>

#include <QClipboard>
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
#include "core/util.h"

KnowledgePanel::KnowledgePanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    buildHeader(layout, "knowledge", "知识库", "Knowledge Base",
                "经验 / 方案 / 踩坑统一沉淀，关键词与语义双模式检索，版本只追加不覆盖",
                "Shared know-how with keyword & semantic search; append-only versions");

    // 工具栏：拆成两行。原先 6 个控件挤在一行，搜索框和过滤框都被压到占位符截断。
    // 第一行 = 主操作（搜索 / 新建），第二行 = 统计与次级筛选。
    auto* toolbar = new QHBoxLayout;
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(i18n::trs("搜索知识库（关键词或自然语言）…", "Search knowledge (keyword or natural language)..."));
    searchEdit_->setClearButtonEnabled(true);
    semanticCheck_ = new QCheckBox(i18n::trs("语义搜索", "Semantic"), this);
    semanticCheck_->setToolTip(i18n::trs("按向量距离排序（内置 n-gram 模糊匹配，偏召回）",
                                         "Rank by vector distance (built-in n-gram, recall-oriented)"));
    tagEdit_ = new QLineEdit(this);
    tagEdit_->setPlaceholderText(i18n::trs("按标签过滤", "Filter by tag"));
    tagEdit_->setClearButtonEnabled(true);
    searchBtn_ = new QPushButton(i18n::trs("搜索", "Search"), this);
    searchBtn_->setObjectName("primary");
    newBtn_ = new QPushButton(i18n::trs("＋ 新建条目", "＋ New Entry"), this);
    toolbar->addWidget(searchEdit_, 1);
    toolbar->addWidget(semanticCheck_);
    toolbar->addWidget(searchBtn_);
    toolbar->addWidget(newBtn_);
    layout->addLayout(toolbar);
    connect(searchBtn_, &QPushButton::clicked, this, &KnowledgePanel::onSearch);
    connect(searchEdit_, &QLineEdit::returnPressed, this, &KnowledgePanel::onSearch);
    connect(newBtn_, &QPushButton::clicked, this, &KnowledgePanel::onNewEntry);

    // 第二行：统计摘要（左）+ 标签过滤 + 结果即筛（右）
    auto* subBar = new QHBoxLayout;
    statsLabel_ = new QLabel(this);
    statsLabel_->setObjectName("muted");
    subBar->addWidget(statsLabel_);
    subBar->addStretch(1);
    tagLabel_ = new QLabel(i18n::trs("标签", "Tag"), this);
    subBar->addWidget(tagLabel_);
    tagEdit_->setMaximumWidth(160);
    subBar->addWidget(tagEdit_);
    layout->addLayout(subBar);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    table_ = new QTableWidget(0, 5, splitter);
    table_->setHorizontalHeaderLabels({i18n::trs("标题", "Title"), i18n::trs("作者", "Author"),
                                      i18n::trs("标签", "Tags"), i18n::trs("版本", "Ver."), i18n::trs("时间", "Time")});
    table_->horizontalHeader()->setStretchLastSection(true);
    polishTable(table_);
    splitter->addWidget(table_);
    // 结果即筛：对当前搜索结果做本地即时过滤（与上方服务端搜索互补）
    viewFilter_ = makeTableFilter(table_, this,
                                  i18n::trs("结果即筛…", "Filter results…"));
    viewFilter_->setMaximumWidth(170);
    subBar->addWidget(viewFilter_);
    // 双击行 = 大窗阅读全文（右侧详情栏偏窄时不挤）
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            &KnowledgePanel::onEntryViewer);
    // 分栏宽度持久化：跨会话记住左右比例
    QSettings s;
    splitter->restoreState(s.value("ui/splitter/knowledge").toByteArray());
    connect(splitter, &QSplitter::splitterMoved, this, [splitter](int, int) {
        QSettings s;
        s.setValue("ui/splitter/knowledge", splitter->saveState());
    });

    auto* right = new QWidget(splitter);
    auto* rl = new QVBoxLayout(right);
    metaLabel_ = new QLabel(right);
    rl->addWidget(metaLabel_);
    detail_ = new QTextBrowser(right);
    // 详情/空状态分页：全库为空时右侧直接给"模块是什么 + 怎么产生内容 + 显眼的新建按钮"
    detailStack_ = new QStackedWidget(right);
    detailStack_->addWidget(detail_);
    emptyState_ = new ui::InlineEmpty(
        "knowledge",
        i18n::trs("还没有任何知识条目", "No knowledge entries yet"),
        i18n::trs("知识库存放团队沉淀的经验与方案，供所有 Agent 检索引用；"
                  "Agent 也可通过 POST /api/knowledge 直接写入。",
                  "The knowledge base holds shared experience and solutions for every agent to "
                  "retrieve; agents can also write via POST /api/knowledge."),
        detailStack_);
    emptyState_->setAction(i18n::trs("＋ 新建条目", "＋ New Entry"),
                           [this] { onNewEntry(); });
    detailStack_->addWidget(emptyState_);
    rl->addWidget(detailStack_, 1);
    auto* vrow = new QHBoxLayout;
    versionCombo_ = new QComboBox(right);
    addVersionBtn_ = new QPushButton(i18n::trs("追加新版本", "Append Version"), right);
    vrow->addWidget(versionCombo_, 1);
    vrow->addWidget(addVersionBtn_);
    rl->addLayout(vrow);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) { onSelectEntry(row); });
    connect(versionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &KnowledgePanel::onVersionChanged);
    connect(addVersionBtn_, &QPushButton::clicked, this, &KnowledgePanel::onAddVersion);
    attachTableContextMenu(table_);
}

void KnowledgePanel::refresh() {
    if (table_->rowCount() == 0) onSearch();
}

void KnowledgePanel::onSearch() {
    std::string err;
    hits_.clear();
    std::string tag = tagEdit_->text().trimmed().toStdString();
    std::string query = searchEdit_->text().trimmed().toStdString();
    if (query.empty()) {
        std::vector<ah::KnowledgeEntry> entries;
        if (platform_.knowledgeList(200, tag, entries, err)) {
            for (auto& e : entries) hits_.push_back({std::move(e), 0.0});
        }
    } else {
        ah::SearchMode mode =
            semanticCheck_->isChecked() ? ah::SearchMode::Semantic : ah::SearchMode::Keyword;
        platform_.knowledgeSearch(query, mode, 50, tag, hits_, err);
    }

    table_->setRowCount(static_cast<int>(hits_.size()));
    for (size_t i = 0; i < hits_.size(); ++i) {
        const auto& e = hits_[i].entry;
        QString tags;
        for (const auto& t : e.tags) tags += QString::fromStdString(t) + " ";
        setRow(table_, static_cast<int>(i),
               {QString::fromStdString(e.title), QString::fromStdString(e.author), tags.trimmed(),
                QString::number(e.version), relTime(QString::fromStdString(e.created_at))});
    }
    fitTableColumns(table_, 0);  // 标题吃剩余空间，其余按内容宽（避免出现横向滚动条）
    applyTableFilter(table_, viewFilter_->text());  // 新结果套用当前过滤

    // 统计摘要：总数 / 今日新增 / 热门标签（"今日"按本地日期，避免与用户时区错位）
    const QDate todayLocal = QDate::currentDate();
    int todayCount = 0;
    std::map<QString, int> tagFreq;
    for (const auto& h : hits_) {
        const QDateTime dt = parseUtcIso(QString::fromStdString(h.entry.created_at));
        if (dt.isValid() && dt.toLocalTime().date() == todayLocal) ++todayCount;
        for (const auto& t : h.entry.tags) ++tagFreq[QString::fromStdString(t)];
    }
    QVector<QPair<QString, int>> top(tagFreq.begin(), tagFreq.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    QString tagStr;
    for (int i = 0; i < qMin(5, static_cast<int>(top.size())); ++i)
        tagStr += QString("%1(%2) ").arg(top[i].first).arg(top[i].second);
    statsLabel_->setText(QString(i18n::trs("总条目 %1 · 今日新增 %2 · 热门标签: %3", "Entries %1 · new today %2 · top tags: %3"))
                             .arg(formatNum(static_cast<qint64>(hits_.size())))
                             .arg(todayCount)
                             .arg(tagStr.isEmpty() ? "—" : tagStr));

    versionCombo_->clear();
    if (!hits_.empty()) {
        detailStack_->setCurrentWidget(detail_);
        table_->selectRow(0);
        onSelectEntry(0);
    } else {
        // 空状态：右侧整栏切到引导页（模块作用 + 如何产生内容 + 新建按钮）
        detailStack_->setCurrentWidget(emptyState_);
    }
}

void KnowledgePanel::onSelectEntry(int row) {
    if (row < 0 || row >= static_cast<int>(hits_.size())) return;
    const auto& hit = hits_[static_cast<size_t>(row)];
    const auto& e = hit.entry;
    currentUuid_ = e.uuid;
    detail_->setPlainText(QString::fromStdString(e.content));
    QString meta = QString("<b>%1</b> · %2: %3 · %4 v%5 · %6: %7 · %8: %9")
                       .arg(QString::fromStdString(e.title).toHtmlEscaped())
                       .arg(i18n::trs("作者", "Author"), QString::fromStdString(e.author))
                       .arg(i18n::trs("版本", "Version"))
                       .arg(e.version)
                       .arg(i18n::trs("分类", "Category"), QString::fromStdString(e.category))
                       .arg(i18n::trs("时间", "Time"), relTime(QString::fromStdString(e.created_at))
                                                            + " (" +
                                                            localStamp(QString::fromStdString(e.created_at)) + ")");
    if (semanticCheck_->isChecked())
        meta += QString(" · %1: %2").arg(i18n::trs("距离", "dist")).arg(hit.score, 0, 'f', 4);
    metaLabel_->setText(meta);

    // 历史版本
    versionCombo_->blockSignals(true);
    versionCombo_->clear();
    versions_.clear();
    std::string err;
    if (platform_.knowledgeVersions(e.uuid, versions_, err)) {
        for (const auto& v : versions_)
            versionCombo_->addItem(QString("v%1 — %2 (%3)")
                                       .arg(v.version)
                                       .arg(QString::fromStdString(v.author))
                                       .arg(relTime(QString::fromStdString(v.created_at))));
    }
    // 空下拉框会渲染成一个带小三角的空方块，看起来像渲染残留；给出占位并禁用
    if (versions_.empty()) {
        versionCombo_->addItem(i18n::trs("无历史版本", "no versions"));
        versionCombo_->setEnabled(false);
    } else {
        versionCombo_->setEnabled(true);
    }
    versionCombo_->setCurrentIndex(0);
    addVersionBtn_->setEnabled(!e.uuid.empty());
    versionCombo_->blockSignals(false);
}

void KnowledgePanel::focusFilter() {
    viewFilter_->setFocus();
    viewFilter_->selectAll();
}

void KnowledgePanel::retranslate() {
    PanelBase::retranslate();
    searchEdit_->setPlaceholderText(
        i18n::trs("搜索知识库（关键词或自然语言）…", "Search knowledge (keyword or natural language)..."));
    semanticCheck_->setText(i18n::trs("语义搜索", "Semantic"));
    semanticCheck_->setToolTip(i18n::trs("按向量距离排序（内置 n-gram 模糊匹配，偏召回）",
                                         "Rank by vector distance (built-in n-gram, recall-oriented)"));
    tagEdit_->setPlaceholderText(i18n::trs("按标签过滤", "Filter by tag"));
    tagLabel_->setText(i18n::trs("标签", "Tag"));
    searchBtn_->setText(i18n::trs("搜索", "Search"));
    newBtn_->setText(i18n::trs("＋ 新建条目", "＋ New Entry"));
    addVersionBtn_->setText(i18n::trs("追加新版本", "Append Version"));
    viewFilter_->setPlaceholderText(i18n::trs("结果即筛…", "Filter results…"));
    viewFilter_->setToolTip(i18n::trs("即时过滤（Ctrl+F 聚焦 · Esc 清空）",
                                      "Live filter (Ctrl+F to focus · Esc to clear)"));
    table_->setHorizontalHeaderLabels({i18n::trs("标题", "Title"), i18n::trs("作者", "Author"),
                                       i18n::trs("标签", "Tags"), i18n::trs("版本", "Ver."),
                                       i18n::trs("时间", "Time")});
    emptyState_->titleLabel()->setText(i18n::trs("还没有任何知识条目", "No knowledge entries yet"));
    emptyState_->hintLabel()->setText(
        i18n::trs("知识库存放团队沉淀的经验与方案，供所有 Agent 检索引用；"
                  "Agent 也可通过 POST /api/knowledge 直接写入。",
                  "The knowledge base holds shared experience and solutions for every agent to "
                  "retrieve; agents can also write via POST /api/knowledge."));
    // 空状态的动作按钮由 InlineEmpty::setAction 创建：只回取按钮改文案，不重复 connect
    if (auto* actionBtn = emptyState_->findChild<QPushButton*>())
        actionBtn->setText(i18n::trs("＋ 新建条目", "＋ New Entry"));
    // 统计摘要 / 详情元信息 / 版本下拉均由 onSearch() 生成：就地重跑一次即可换成新语言
    onSearch();
}

void KnowledgePanel::onEntryViewer(int row) {
    if (row < 0 || row >= static_cast<int>(hits_.size())) return;
    const auto& e = hits_[static_cast<size_t>(row)].entry;
    QString tags;
    for (const auto& t : e.tags) tags += QString::fromStdString(t) + " ";
    QDialog dlg(this);
    dlg.setWindowTitle(QString("%1 · v%2").arg(QString::fromStdString(e.title)).arg(e.version));
    dlg.resize(780, 560);
    auto* l = new QVBoxLayout(&dlg);
    auto* meta = new QLabel(
        QString("%1: %2 · %3: %4 · %5: %6 · %7: %8")
            .arg(i18n::trs("作者", "Author"), QString::fromStdString(e.author))
            .arg(i18n::trs("标签", "Tags"), tags.trimmed().toHtmlEscaped())
            .arg(i18n::trs("分类", "Category"), QString::fromStdString(e.category))
            .arg(i18n::trs("时间", "Time"), relTime(QString::fromStdString(e.created_at))));
    meta->setStyleSheet(ui::th("color:@muted@; font-size:12px;"));
    auto* view = new QPlainTextEdit(&dlg);
    view->setReadOnly(true);
    view->setPlainText(QString::fromStdString(e.content));
    l->addWidget(meta);
    l->addWidget(view, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(close, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    l->addWidget(close);
    dlg.exec();
}

void KnowledgePanel::onVersionChanged(int idx) {
    if (idx < 0 || idx >= static_cast<int>(versions_.size())) return;
    const auto& v = versions_[static_cast<size_t>(idx)];
    currentUuid_ = v.uuid;
    detail_->setPlainText(QString::fromStdString(v.content));
    metaLabel_->setText(QString("<b>%1</b> · %2: %3 · %4 v%5 · %6: %7")
                            .arg(QString::fromStdString(v.title).toHtmlEscaped())
                            .arg(i18n::trs("作者", "Author"), QString::fromStdString(v.author))
                            .arg(i18n::trs("版本", "Version"))
                            .arg(v.version)
                            .arg(i18n::trs("时间", "Time"), relTime(QString::fromStdString(v.created_at))));
}

void KnowledgePanel::onNewEntry() {
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("新建知识条目", "New Knowledge Entry"));
    auto* form = new QFormLayout(&dlg);
    auto* title = new QLineEdit(&dlg);
    auto* content = new QPlainTextEdit(&dlg);
    auto* tags = new QLineEdit(&dlg);
    tags->setPlaceholderText(i18n::trs("逗号分隔，如 qt,cmake", "Comma-separated, e.g. qt,cmake"));
    auto* category = new QLineEdit(&dlg);
    form->addRow(i18n::trs("标题", "Title"), title);
    form->addRow(i18n::trs("内容", "Content"), content);
    form->addRow(i18n::trs("标签", "Tags"), tags);
    form->addRow(i18n::trs("分类", "Category"), category);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    std::vector<std::string> tagList;
    for (const auto& t : tags->text().split(',', Qt::SkipEmptyParts))
        tagList.push_back(t.trimmed().toStdString());
    std::string err;
    ah::KnowledgeEntry out;
    if (!platform_.knowledgeCreate("user", title->text().trimmed().toStdString(),
                                   content->toPlainText().toStdString(), tagList,
                                   category->text().trimmed().toStdString(), {}, "", out, err)) {
        ui::Toast::show(this, i18n::trs("创建失败: %1", "Create failed: %1")
                                  .arg(QString::fromStdString(err)),
                        false);
        return;
    }
    ui::Toast::show(this, i18n::trs("知识条目已创建 ✓", "Entry created ✓"));
    onSearch();
}

void KnowledgePanel::onAddVersion() {
    if (currentUuid_.empty()) return;
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("追加新版本（旧版本保留，禁止覆盖）", "Append Version (old versions kept, overwrite forbidden)"));
    auto* form = new QFormLayout(&dlg);
    auto* title = new QLineEdit(&dlg);
    auto* content = new QPlainTextEdit(&dlg);
    form->addRow(i18n::trs("标题（留空沿用当前版本标题）", "Title (blank keeps current)"), title);
    form->addRow(i18n::trs("新内容", "New content"), content);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    std::string err;
    ah::KnowledgeEntry out;
    if (!platform_.knowledgeAddVersion("user", currentUuid_, title->text().trimmed().toStdString(),
                                       content->toPlainText().toStdString(), {}, "", out, err)) {
        ui::Toast::show(this, i18n::trs("追加失败: %1", "Append failed: %1")
                                  .arg(QString::fromStdString(err)),
                        false);
        return;
    }
    ui::Toast::show(this, i18n::trs("已追加 v%1（旧版本保留）✓", "Appended v%1 (old versions kept) ✓")
                              .arg(out.version));
    onSearch();
}
