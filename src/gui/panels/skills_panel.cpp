#include "skills_panel.h"

#include <map>
#include <set>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

SkillsPanel::SkillsPanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    buildHeader(layout, "skills", "技能库", "Skill Registry",
                "先注册后调用，每次调用留痕；使用热度反映真实依赖",
                "Register before invoke; every call recorded with usage heat");

    auto* toolbar = new QHBoxLayout;
    categoryCombo_ = new QComboBox(this);
    ownerCombo_ = new QComboBox(this);
    refreshBtn_ = new QPushButton(i18n::trs("刷新", "Refresh"), this);
    registerBtn_ = new QPushButton(i18n::trs("＋ 注册技能", "＋ Register Skill"), this);
    categoryLabel_ = new QLabel(i18n::trs("分类:", "Category:"), this);
    ownerLabel_ = new QLabel(i18n::trs("提供者:", "Owner:"), this);
    toolbar->addWidget(categoryLabel_);
    toolbar->addWidget(categoryCombo_);
    toolbar->addWidget(ownerLabel_);
    toolbar->addWidget(ownerCombo_);
    toolbar->addWidget(refreshBtn_);
    toolbar->addStretch(1);
    toolbar->addWidget(registerBtn_);
    layout->addLayout(toolbar);
    connect(refreshBtn_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(categoryCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(ownerCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(registerBtn_, &QPushButton::clicked, this, &SkillsPanel::onRegister);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    table_ = new QTableWidget(0, 7, splitter);
    table_->setHorizontalHeaderLabels({i18n::trs("名称", "Name"), i18n::trs("显示名", "Display"),
                                      i18n::trs("分类", "Category"), i18n::trs("提供者", "Owner"), i18n::trs("版本", "Ver."),
                                      i18n::trs("状态", "Status"), i18n::trs("使用热度", "Usage Heat")});
    fitTableColumns(table_, 6);  // 使用热度列吃剩余空间，其余按内容宽
    polishTable(table_);
    table_->resizeColumnsToContents();
    attachTableContextMenu(table_);
    splitter->addWidget(table_);
    // 即时过滤框：置于「注册技能」左侧，需在 table_ 就绪后创建
    filterEdit_ = makeTableFilter(table_, this);
    toolbar->insertWidget(toolbar->indexOf(registerBtn_), filterEdit_);
    detail_ = new QTextBrowser(splitter);
    // 详情/空状态分页：全库为空时右侧整栏给"技能库是什么 + 怎么产生内容 + 注册按钮"
    detailStack_ = new QStackedWidget(splitter);
    detailStack_->addWidget(detail_);
    emptyState_ = new ui::InlineEmpty(
        "skills",
        i18n::trs("还没有注册任何技能", "No skills registered yet"),
        i18n::trs("技能是可被所有 Agent 调用的能力单元：先注册、后调用，每次调用都留痕形成使用热度。",
                  "A skill is a capability any agent can invoke: register first, invoke later — "
                  "every call is recorded as usage heat."),
        detailStack_);
    emptyState_->setAction(i18n::trs("＋ 注册技能", "＋ Register Skill"), [this] { onRegister(); });
    detailStack_->addWidget(emptyState_);
    splitter->addWidget(detailStack_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    // 分栏宽度持久化：跨会话记住左右比例（主题/语言切换重建面板后同样恢复）
    QSettings s;
    splitter->restoreState(s.value("ui/splitter/skills").toByteArray());
    connect(splitter, &QSplitter::splitterMoved, this, [splitter](int, int) {
        QSettings s;
        s.setValue("ui/splitter/skills", splitter->saveState());
    });
    layout->addWidget(splitter, 1);

    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) { onSelectSkill(row); });
}

void SkillsPanel::focusFilter() {
    filterEdit_->setFocus();
    filterEdit_->selectAll();
}

void SkillsPanel::retranslate() {
    PanelBase::retranslate();
    categoryLabel_->setText(i18n::trs("分类:", "Category:"));
    ownerLabel_->setText(i18n::trs("提供者:", "Owner:"));
    refreshBtn_->setText(i18n::trs("刷新", "Refresh"));
    registerBtn_->setText(i18n::trs("＋ 注册技能", "＋ Register Skill"));
    // 首项是"不过滤"的展示项（refresh() 里与 currentText() 比较判定），只改文案不改语义
    const QString allItem = i18n::trs("全部", "All");
    if (categoryCombo_->count() > 0) categoryCombo_->setItemText(0, allItem);
    if (ownerCombo_->count() > 0) ownerCombo_->setItemText(0, allItem);
    table_->setHorizontalHeaderLabels({i18n::trs("名称", "Name"), i18n::trs("显示名", "Display"),
                                       i18n::trs("分类", "Category"), i18n::trs("提供者", "Owner"),
                                       i18n::trs("版本", "Ver."), i18n::trs("状态", "Status"),
                                       i18n::trs("使用热度", "Usage Heat")});
    filterEdit_->setPlaceholderText(i18n::trs("输入即筛…", "Type to filter…"));
    filterEdit_->setToolTip(i18n::trs("即时过滤（Ctrl+F 聚焦 · Esc 清空）",
                                      "Live filter (Ctrl+F to focus · Esc to clear)"));
    emptyState_->titleLabel()->setText(i18n::trs("还没有注册任何技能", "No skills registered yet"));
    emptyState_->hintLabel()->setText(
        i18n::trs("技能是可被所有 Agent 调用的能力单元：先注册、后调用，每次调用都留痕形成使用热度。",
                  "A skill is a capability any agent can invoke: register first, invoke later — "
                  "every call is recorded as usage heat."));
    // 空状态的动作按钮由 InlineEmpty::setAction 创建：只回取按钮改文案，不重复 connect
    if (auto* actionBtn = emptyState_->findChild<QPushButton*>())
        actionBtn->setText(i18n::trs("＋ 注册技能", "＋ Register Skill"));
    // 详情栏 HTML（提供者/分类/版本/状态/更新）由 onSelectSkill() 生成：
    // 用当前选中行就地重渲染即可换语言，不再查库
    if (table_->currentRow() >= 0) onSelectSkill(table_->currentRow());
}

void SkillsPanel::refresh() {
    // 填充筛选下拉（保留当前选择）
    std::string err;
    std::vector<ah::SkillInfo> all;
    platform_.skillList("", "", all, err);
    std::set<std::string> categories, owners;
    for (const auto& s : all) {
        if (!s.category.empty()) categories.insert(s.category);
        if (!s.owner_agent.empty()) owners.insert(s.owner_agent);
    }
    QString curCat = categoryCombo_->currentText();
    QString curOwner = ownerCombo_->currentText();
    // "不过滤"这一项既是显示文案又被用来判断是否过滤：展示名与比较必须取同一个值，
    // 否则翻译后 currentText() != "全部" 会把"全部"当成真实分类去过滤（空列表）
    const QString allItem = i18n::trs("全部", "All");
    categoryCombo_->blockSignals(true);
    ownerCombo_->blockSignals(true);
    categoryCombo_->clear();
    ownerCombo_->clear();
    categoryCombo_->addItem(allItem);
    ownerCombo_->addItem(allItem);
    for (const auto& c : categories) categoryCombo_->addItem(QString::fromStdString(c));
    for (const auto& o : owners) ownerCombo_->addItem(QString::fromStdString(o));
    categoryCombo_->setCurrentText(curCat);
    ownerCombo_->setCurrentText(curOwner);
    categoryCombo_->blockSignals(false);
    ownerCombo_->blockSignals(false);

    std::string cat = categoryCombo_->currentText() == allItem ? "" : categoryCombo_->currentText().toStdString();
    std::string owner = ownerCombo_->currentText() == allItem ? "" : ownerCombo_->currentText().toStdString();
    platform_.skillList(cat, owner, skills_, err);

    // 使用热度：统计每个技能的调用次数
    std::vector<ah::SkillInvocation> allInv;
    platform_.skillInvocations("", 10000, allInv, err);
    std::map<std::string, int> heat;
    for (const auto& i : allInv) ++heat[i.skill_name];

    // 刷新前记住选中技能，重建后恢复选中，避免 3s 自动刷新打断浏览
    QString prevSelected;
    if (auto* cur = table_->item(table_->currentRow(), 0)) prevSelected = cur->text();

    table_->setRowCount(static_cast<int>(skills_.size()));
    int restoreRow = -1;
    for (size_t i = 0; i < skills_.size(); ++i) {
        const auto& s = skills_[i];
        int h = heat.count(s.name) ? heat[s.name] : 0;
        QString heatStr = h == 0 ? "—"
                                 : (h >= 10 ? QString("🔥 %1").arg(h) : QString::number(h));
        setRow(table_, static_cast<int>(i),
               {QString::fromStdString(s.name), QString::fromStdString(s.display_name),
                QString::fromStdString(s.category), QString::fromStdString(s.owner_agent),
                QString::number(s.version), QString::fromStdString(s.status), heatStr});
        if (!prevSelected.isEmpty() && prevSelected == QString::fromStdString(s.name))
            restoreRow = static_cast<int>(i);
    }
    table_->resizeColumnsToContents();  // 按实际内容重算列宽，避免截断
    applyTableFilter(table_, filterEdit_->text());  // 行已重建，重放即时过滤态
    if (restoreRow >= 0) {
        detailStack_->setCurrentWidget(detail_);
        table_->selectRow(restoreRow);
        onSelectSkill(restoreRow);
    } else if (!skills_.empty()) {
        detailStack_->setCurrentWidget(detail_);
        table_->selectRow(0);
        onSelectSkill(0);
    } else {
        // 空状态：右侧整栏切到引导页（模块作用 + 如何产生内容 + 注册按钮）
        detailStack_->setCurrentWidget(emptyState_);
    }
}

void SkillsPanel::onSelectSkill(int row) {
    if (row < 0 || row >= static_cast<int>(skills_.size())) return;
    const auto& s = skills_[static_cast<size_t>(row)];
    detail_->setHtml(ui::th(
        i18n::trs(
            QString("<div style='margin:4px;'>"
                    "<span style='font-size:16px; font-weight:700; color:@accent@;'>%1</span>"
                    "&nbsp;<span style='color:@muted@;'>%2</span>"
                    "<div style='margin-top:6px; color:@text@;'>%3</div>"
                    "<div style='margin-top:10px; padding:7px 10px; background:@field@;"
                    " border-left:3px solid @accent@; color:@muted@;'>"
                    "<b style='color:@text@;'>提供者</b> %4 &nbsp;·&nbsp; "
                    "<b style='color:@text@;'>分类</b> %5"
                    " &nbsp;·&nbsp; <b style='color:@text@;'>版本</b> v%6"
                    " &nbsp;·&nbsp; <b style='color:@text@;'>状态</b> %7 &nbsp;·&nbsp; "
                    "<b style='color:@text@;'>更新</b> %8</div>"
                    "<div style='margin-top:12px; font-weight:600; color:@text@;'>参数 Schema</div>"
                    "<pre style='background:@bg@; border:1px solid @line@; border-radius:6px;"
                    " padding:8px; color:@accenthi@; font-family:@mono@,monospace;'>%9</pre>"
                    "</div>"),
            QString("<div style='margin:4px;'>"
                    "<span style='font-size:16px; font-weight:700; color:@accent@;'>%1</span>"
                    "&nbsp;<span style='color:@muted@;'>%2</span>"
                    "<div style='margin-top:6px; color:@text@;'>%3</div>"
                    "<div style='margin-top:10px; padding:7px 10px; background:@field@;"
                    " border-left:3px solid @accent@; color:@muted@;'>"
                    "<b style='color:@text@;'>Owner</b> %4 &nbsp;·&nbsp; "
                    "<b style='color:@text@;'>Category</b> %5"
                    " &nbsp;·&nbsp; <b style='color:@text@;'>Ver.</b> v%6"
                    " &nbsp;·&nbsp; <b style='color:@text@;'>Status</b> %7 &nbsp;·&nbsp; "
                    "<b style='color:@text@;'>Updated</b> %8</div>"
                    "<div style='margin-top:12px; font-weight:600; color:@text@;'>Param Schema</div>"
                    "<pre style='background:@bg@; border:1px solid @line@; border-radius:6px;"
                    " padding:8px; color:@accenthi@; font-family:@mono@,monospace;'>%9</pre>"
                    "</div>"))
            .arg(QString::fromStdString(s.name).toHtmlEscaped())
            .arg(QString::fromStdString(s.display_name).toHtmlEscaped())
            .arg(QString::fromStdString(s.description).toHtmlEscaped())
            .arg(QString::fromStdString(s.owner_agent).toHtmlEscaped())
            .arg(QString::fromStdString(s.category).toHtmlEscaped())
            .arg(s.version)
            .arg(QString::fromStdString(s.status).toHtmlEscaped())
            .arg(relTime(QString::fromStdString(s.updated_at)))
            .arg(QString::fromStdString(s.param_schema).toHtmlEscaped())));
}

void SkillsPanel::onRegister() {
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("注册新技能（先注册后调用）", "Register Skill (register before invoke)"));
    auto* form = new QFormLayout(&dlg);
    auto* name = new QLineEdit(&dlg);
    auto* display = new QLineEdit(&dlg);
    auto* desc = new QPlainTextEdit(&dlg);
    auto* category = new QLineEdit(&dlg);
    auto* schema = new QPlainTextEdit(&dlg);
    schema->setPlainText("{}");
    form->addRow(i18n::trs("技能名（唯一）", "Name (unique)"), name);
    form->addRow(i18n::trs("显示名", "Display name"), display);
    form->addRow(i18n::trs("描述", "Description"), desc);
    form->addRow(i18n::trs("分类", "Category"), category);
    form->addRow(i18n::trs("参数 Schema (JSON)", "Param Schema (JSON)"), schema);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    std::string err;
    ah::SkillInfo out;
    if (!platform_.skillRegister("user", name->text().trimmed().toStdString(),
                                 display->text().trimmed().toStdString(),
                                 desc->toPlainText().toStdString(),
                                 category->text().trimmed().toStdString(),
                                 schema->toPlainText().toStdString(), out, err)) {
        QMessageBox::warning(this, i18n::trs("注册失败", "Registration failed"),
                             QString::fromStdString(err));
        return;
    }
    refresh();
}
