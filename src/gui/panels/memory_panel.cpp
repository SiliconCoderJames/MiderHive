#include "memory_panel.h"

#include <algorithm>
#include <map>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSet>

#include "../gui_util.h"
#include "../i18n.h"
#include "../widgets.h"

namespace {
// 五大区块（中文显示名, 英文显示名, 存储名）。显示名不能在这里取词：
// 静态初始化早于 i18n::load()，会把启动时的语言永久固化，故存双语、用到时再取。
struct MemorySection {
    const char* zh;
    const char* en;
    const char* key;
};
const std::vector<MemorySection> kSections{
    {"项目档案", "Project", "project"},        {"决策日志", "Decisions", "decision"},
    {"偏好记录", "Preferences", "preference"}, {"设备环境", "Environment", "environment"},
    {"工作习惯", "Work habits", "work_style"},
};

// 一条记忆的富文本行：键（强调色）、版本 · 作者 · 相对时间（小字，悬停看精确时刻）、值
QString rowHtml(const ah::MemoryEntry& m) {
    const QString raw = QString::fromStdString(m.created_at);
    return ui::th(
        QString("<b style='color:@accent@;'>%1</b>"
                " <span style='color:@muted@; font-size:10px;' title='%6'>v%2 · %3 · %4</span><br>%5")
            .arg(QString::fromStdString(m.key).toHtmlEscaped())
            .arg(m.version)
            .arg(QString::fromStdString(m.author))
            .arg(relTime(raw))
            .arg(QString::fromStdString(m.value).toHtmlEscaped().left(200))
            .arg(localStamp(raw)));
}
}  // namespace

MemoryPanel::MemoryPanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    buildHeader(layout, "memory", "用户记忆", "User Memory",
                "项目档案 · 决策日志 · 偏好记录 · 设备环境 · 工作习惯，所有 Agent 共享",
                "Project, decisions, preferences, environment and habits - shared by all agents");

    auto* toolbar = new QHBoxLayout;
    headerLabel_ = new QLabel(this);
    headerLabel_->setStyleSheet(ui::th("font-size:13px; color:@muted@;"));
    editBtn_ = new QPushButton(i18n::trs("编辑 / 新增（生成新版本）", "Edit / Add (new version)"), this);
    editBtn_->setObjectName("primary");
    historyBtn_ = new QPushButton(i18n::trs("查看历史版本", "History"), this);
    toolbar->addWidget(headerLabel_);
    toolbar->addStretch(1);
    toolbar->addWidget(historyBtn_);
    toolbar->addWidget(editBtn_);
    layout->addLayout(toolbar);
    connect(editBtn_, &QPushButton::clicked, this, &MemoryPanel::onEdit);
    connect(historyBtn_, &QPushButton::clicked, this, &MemoryPanel::onShowHistory);

    // 折叠卡片滚动区
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* holder = new QWidget(scroll);
    sectionsLay_ = new QVBoxLayout(holder);
    sectionsLay_->setContentsMargins(0, 0, 12, 0);
    sectionsLay_->setSpacing(10);
    sectionsLay_->addStretch(1);
    scroll->setWidget(holder);
    layout->addWidget(scroll, 1);
}

void MemoryPanel::refresh() {
    std::string err;
    if (!platform_.memoryList("", entries_, err)) {
        // 读失败如实提示：服务层失败前已清空 entries_，不提示会把读失败呈现
        // 成"记忆条目 0"。保留上一轮内容不重建。
        ui::Toast::show(this, ui::humanError(QString::fromStdString(err)), false);
        return;
    }

    // 头部：条目总数 + 最后更新时间（相对时间；文案接入双语）
    QString lastRaw;
    for (const auto& m : entries_) {
        const QString t = QString::fromStdString(m.created_at);
        if (lastRaw.isEmpty() || t > lastRaw) lastRaw = t;  // UTC ISO 字典序即时序
    }
    headerLabel_->setText(i18n::trs("记忆条目 %1 · 最后更新 %2", "Entries %1 · last updated %2")
                              .arg(formatNum(static_cast<qint64>(entries_.size())))
                              .arg(lastRaw.isEmpty() ? QString("—") : relTime(lastRaw)));

    // 区块卡片只在条目集合变化时重建。此前每次轮询（3 秒）都把五张卡片全部销毁重建：
    // 用户折起来的区块会自己弹开，控件也一遍遍地白建。相对时间这类随钟变的文案
    // 在位更新（见下），不需要重建。签名带上语言：切换语言后文案要重来一遍
    QString sig;
    for (const auto& m : entries_)
        sig += QString::fromStdString(m.section) + '\x1f' + QString::fromStdString(m.key) + '\x1f' +
               QString::number(m.version) + '\x1f' + QString::fromStdString(m.author) + '\x1e';
    sig += '\x1d' + QString::number(static_cast<int>(i18n::g_lang));
    if (sig != sectionsSig_) {
        sectionsSig_ = sig;
        rebuildSections();
    }
    for (size_t i = 0; i < entries_.size() && i < rowLabels_.size(); ++i)
        rowLabels_[i]->setText(rowHtml(entries_[i]));
}

void MemoryPanel::rebuildSections() {
    // 记下用户折起来了哪些区块，重建后原样折回去
    QSet<QString> collapsed;
    for (auto it = cards_.cbegin(); it != cards_.cend(); ++it)
        if (!it.value()->expanded()) collapsed.insert(it.key());
    while (sectionsLay_->count() > 1) {  // 末尾是 stretch
        QLayoutItem* it = sectionsLay_->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    cards_.clear();
    rowLabels_.assign(entries_.size(), nullptr);
    // 全库为空时：先给一条"模块是什么 + 显眼的写入入口"，再进各区块的空状态。
    // 插入位置每次重算（stretch 之前）——只算一次的话后面每张卡都插到同一个下标，
    // 区块顺序就整个倒过来
    if (entries_.empty()) {
        auto* guide = new ui::InlineEmpty(
            "memory",
            i18n::trs("用户记忆还是空的", "User memory is empty"),
            i18n::trs("记忆分项目档案 / 决策日志 / 偏好记录 / 设备环境 / 工作习惯五大区块，"
                      "所有 Agent 共享同一份；写下第一条，协作就有了共同的背景。",
                      "Memory has five sections (project, decisions, preferences, environment, "
                      "work habits) shared by all agents; write the first entry to give every "
                      "agent shared context."),
            this);
        guide->setAction(i18n::trs("＋ 写入记忆", "＋ Add memory"), [this] { onEdit(); });
        sectionsLay_->insertWidget(sectionsLay_->count() - 1, guide);
    }
    for (const auto& sec : kSections) {
        auto* content = new QWidget(this);
        auto* cl = new QVBoxLayout(content);
        cl->setContentsMargins(14, 10, 14, 12);
        cl->setSpacing(6);
        bool any = false;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto& m = entries_[i];
            if (m.section != sec.key) continue;
            any = true;
            auto* row = new QLabel(rowHtml(m), content);
            row->setTextFormat(Qt::RichText);
            row->setWordWrap(true);
            cl->addWidget(row);
            rowLabels_[i] = row;
        }
        if (!any) {
            // 空区块只给一行朴素提示（带出路）。此前每个空区块各放一幅大号六边形插画：
            // 一屏里同一个插画重复四次，真正有内容的那一条反而被淹没
            auto* hint = new QLabel(
                i18n::trs("该区块暂无记忆", "No memories in this section yet") + " · " +
                    i18n::trs("通过 memory set 或工具栏写入第一条，所有 Agent 共享",
                              "Add the first entry via memory set or the toolbar — shared by "
                              "all agents"),
                content);
            hint->setWordWrap(true);
            hint->setStyleSheet(ui::th("color:@muted@; font-size:12px;"));
            cl->addWidget(hint);
        }
        cl->addStretch(1);
        auto* card = new ui::SectionCard(i18n::trs(sec.zh, sec.en), content, this);
        cards_.insert(sec.key, card);
        sectionsLay_->insertWidget(sectionsLay_->count() - 1, card);
    }
    for (auto it = cards_.cbegin(); it != cards_.cend(); ++it)
        if (collapsed.contains(it.key())) it.value()->setExpanded(false);
}

void MemoryPanel::retranslate() {
    PanelBase::retranslate();
    editBtn_->setText(i18n::trs("编辑 / 新增（生成新版本）", "Edit / Add (new version)"));
    historyBtn_->setText(i18n::trs("查看历史版本", "History"));
    // 五个区块卡片与顶部摘要都由 refresh() 生成（区块标题在 SectionCard 构造期取词），
    // 重跑一次 refresh() 即可整体换语言；数据量小，且与 3s 轮询同一条路径
    refresh();
}

void MemoryPanel::onEdit() {
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("编辑用户记忆（保存后生成新版本，历史保留）", "Edit Memory (saved as a new version; history kept)"));
    auto* form = new QFormLayout(&dlg);
    auto* section = new QComboBox(&dlg);
    for (const auto& sec : kSections) section->addItem(i18n::trs(sec.zh, sec.en), sec.key);
    auto* key = new QLineEdit(&dlg);
    auto* value = new QPlainTextEdit(&dlg);
    form->addRow(i18n::trs("区块", "Section"), section);
    form->addRow(i18n::trs("键", "Key"), key);
    form->addRow(i18n::trs("值", "Value"), value);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    // 乐观并发：以当前最新版本为 base 保存，他人先写过则拒绝而非静默覆盖
    const std::string sectionName = section->currentData().toString().toStdString();
    const std::string keyName = key->text().trimmed().toStdString();
    int baseVersion = 0;
    std::vector<ah::MemoryEntry> existing;
    std::string probeErr;
    if (!keyName.empty()) {
        // 探测失败不能静默降级成 baseVersion=0：那会整体跳过乐观并发校验，
        // Agent 刚写的版本被静默顶替。如实失败让用户重试。
        if (!platform_.memoryList(sectionName, existing, probeErr)) {
            ui::Toast::show(this,
                            i18n::trs("读取当前版本失败，未保存：",
                                      "Couldn't read the current version; not saved: ") +
                                ui::humanError(QString::fromStdString(probeErr)),
                            false);
            return;
        }
        for (const auto& m : existing)
            if (m.key == keyName) baseVersion = m.version;
    }

    std::string err;
    ah::MemoryEntry out;
    if (!platform_.memorySet("user", sectionName, keyName, value->toPlainText().toStdString(),
                             baseVersion, out, err)) {
        ui::Toast::show(this, i18n::trs("保存失败: %1", "Save failed: %1")
                                  .arg(ui::humanError(QString::fromStdString(err))),
                        false);
        return;
    }
    ui::Toast::show(this, i18n::trs("记忆已保存（v%1）✓", "Memory saved (v%1) ✓").arg(out.version));
    refresh();
}

void MemoryPanel::onShowHistory() {
    QDialog dlg(this);
    dlg.setWindowTitle(i18n::trs("按键查询历史版本", "Query History by Key"));
    auto* form = new QFormLayout(&dlg);
    auto* section = new QComboBox(&dlg);
    for (const auto& sec : kSections) section->addItem(i18n::trs(sec.zh, sec.en), sec.key);
    auto* key = new QLineEdit(&dlg);
    form->addRow(i18n::trs("区块", "Section"), section);
    form->addRow(i18n::trs("键", "Key"), key);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    std::vector<ah::MemoryEntry> history;
    std::string err;
    if (!platform_.memoryHistory(section->currentData().toString().toStdString(),
                                 key->text().trimmed().toStdString(), history, err) ||
        history.empty()) {
        QMessageBox::information(this, i18n::trs("历史版本", "History"),
                            i18n::trs("该键无记忆记录", "No records for this key"));
        return;
    }
    QDialog view(this);
    view.setWindowTitle(i18n::trs("历史版本 (%1 条)", "History (%1 entries)").arg(history.size()));
    auto* l = new QVBoxLayout(&view);
    auto* table = new QTableWidget(static_cast<int>(history.size()), 4, &view);
    table->setHorizontalHeaderLabels({i18n::trs("版本", "Ver."), i18n::trs("作者", "Author"),
                                      i18n::trs("时间", "Time"), i18n::trs("值", "Value")});
    table->horizontalHeader()->setStretchLastSection(true);
    polishTable(table);
    for (size_t i = 0; i < history.size(); ++i) {
        // 列表按版本倒序：首行即最新版本，标注出来；值列截断显示，全文在悬停提示
        QString v = QString::fromStdString(history[i].value);
        QString shown = v.size() > 200 ? v.left(200) + "…" : v;
        QString ver = QString("v%1").arg(history[i].version);
        if (i == 0) ver += i18n::trs("（最新）", " (latest)");
        setRow(table, static_cast<int>(i),
               {ver, QString::fromStdString(history[i].author),
                relTime(QString::fromStdString(history[i].created_at)), shown});
    }
    table->resizeColumnToContents(0);
    table->resizeColumnToContents(1);
    table->resizeColumnToContents(2);
    l->addWidget(table);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &view);
    connect(close, &QDialogButtonBox::rejected, &view, &QDialog::reject);
    l->addWidget(close);
    view.resize(720, 400);
    view.exec();
}
