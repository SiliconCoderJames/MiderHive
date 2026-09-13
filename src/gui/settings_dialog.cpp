#include "settings_dialog.h"

#include "connect_dialog.h"
#include "welcome_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QShowEvent>

#include <iterator>

#include "gui_util.h"
#include "i18n.h"
#include "theme.h"
#include "widgets.h"

namespace {
// 下载页链接（更新检查与下载由 ui::UpdateChecker 负责，那里用 release 资产的 latest.json）
constexpr const char* kRepoPage = "https://github.com/SiliconCoderJames/MiderHive/releases";

// 设置导航项：kind 取自 ui::makeIcon 的图标种类
constexpr const char* kNavKinds[] = {"palette",  "database", "bell", "overview",
                                     "plug",     "refresh",  "info"};
}  // namespace

SettingsDialog::SettingsDialog(ah::Platform& platform, QWidget* parent)
    : QDialog(parent), platform_(platform) {
    setWindowTitle(i18n::trs("设置", "Settings"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(760, 560);

    // 左侧窄导航 + 右侧内容堆叠（ChatGPT/Cursor 式骨架，分区扩展不改骨架）
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    nav_ = new QListWidget(this);
    nav_->setFixedWidth(150);
    nav_->setFocusPolicy(Qt::NoFocus);  // 去除虚线焦点框
    // 与主导航同一套矢量图标（原先这里是 emoji，和已经换成线性图标的主侧栏不一致）
    const std::vector<std::tuple<const char*, const char*, const char*>> navItems{
        {"palette", "外观", "Appearance"},
        {"database", "数据与备份", "Data & Backup"},
        {"bell", "通知偏好", "Notifications"},
        {"overview", "Agent 管理", "Agents"},
        {"plug", "Agent API", "Agent API"},
        {"refresh", "更新", "Update"},
        {"info", "关于", "About"},
    };
    for (const auto& [kind, zh, en] : navItems)
        nav_->addItem(new QListWidgetItem(ui::makeIcon(kind, ui::muted(), 16, ui::selText()),
                                          "  " + i18n::trs(zh, en)));
    root->addWidget(nav_);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(buildAppearancePage());
    stack_->addWidget(buildBackupPage());
    stack_->addWidget(buildNotifyPage());
    stack_->addWidget(buildAgentsPage());
    stack_->addWidget(buildApiPage());
    stack_->addWidget(buildUpdatePage());
    stack_->addWidget(buildAboutPage());
    root->addWidget(stack_, 1);

    connect(nav_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    nav_->setCurrentRow(0);

    // 主题/字号变化：即时重涂导航与对话框内取色控件
    themeListenerId_ = ui::addThemeListener([this] { applyChrome(); });
    applyChrome();
}

SettingsDialog::~SettingsDialog() {
    ui::removeThemeListener(themeListenerId_);
}

void SettingsDialog::showEvent(QShowEvent*) {
    refreshBackupList();
    refreshAgents();
}

QLabel* SettingsDialog::thLabel(const QString& tmpl, QWidget* parent) {
    auto* l = new QLabel(parent);
    styledLabels_.push_back({l, tmpl});
    l->setStyleSheet(ui::th(tmpl));
    return l;
}

void SettingsDialog::applyChrome() {
    nav_->setStyleSheet(ui::th(
        "QListWidget { background:@deep@; border:none; outline:0; font-size:13px; }"
        "QListWidget::item { color:@muted@; padding:11px 12px;"
        " border-left:3px solid transparent; }"
        "QListWidget::item:hover { color:@text@; background:@card@; }"
        "QListWidget::item:selected { color:@seltext@; background:@selbg@;"
        " border-left:3px solid @brand@; font-weight:600; }"));
    for (std::size_t i = 0; i < swatches_.size() && i < ui::themes().size(); ++i)
        swatches_[i]->setSelected(static_cast<int>(i) == ui::themeIdx());
    for (const auto& [w, tmpl] : styledLabels_) w->setStyleSheet(ui::th(tmpl));
    // 图标按当前主题重新着色（换主题时导航图标也要跟着变）
    for (int i = 0; i < nav_->count() && i < 7; ++i)
        if (auto* it = nav_->item(i))
            it->setIcon(ui::makeIcon(kNavKinds[i], ui::muted(), 16, ui::selText()));
}

// ---- 外观：主题色卡网格 + 字号 ----
QWidget* SettingsDialog::buildAppearancePage() {
    auto* page = new QWidget(stack_);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(14);

    auto* themeTitle = thLabel("font-size:14px; font-weight:700; color:@text@;", page);
    themeTitle->setText(i18n::trs("页面颜色", "Theme"));
    lay->addWidget(themeTitle);

    // 色卡网格：每套主题一张迷你预览，点击即换全窗配色
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    for (int i = 0; i < static_cast<int>(ui::themes().size()); ++i) {
        const auto& t = ui::themes()[static_cast<std::size_t>(i)];
        auto* sw = new ui::ThemeSwatch(i18n::trs(t.zh, t.en), t.bg, t.deep, t.accent, t.brand,
                                       t.text, [i] { ui::setThemeIndex(i); }, page);
        swatches_.push_back(sw);
        grid->addWidget(sw, i / 3, i % 3, Qt::AlignTop | Qt::AlignLeft);
    }
    lay->addLayout(grid);

    auto* themeHint = thLabel("font-size:11px; color:@muted@;", page);
    themeHint->setText(i18n::trs("点击色卡立即切换整套配色，选择会被记住。",
                                 "Click a swatch to switch instantly; your choice is saved."));
    lay->addWidget(themeHint);

    auto* fontTitle = thLabel("font-size:14px; font-weight:700; color:@text@;", page);
    fontTitle->setText(i18n::trs("界面字号", "Font size"));
    lay->addWidget(fontTitle);
    fontBox_ = new QComboBox(page);
    const std::vector<std::pair<int, const char*>> kSizes{
        {12, "紧凑 12px"}, {13, "标准 13px"}, {14, "大号 14px"}};
    for (const auto& [px, label] : kSizes) fontBox_->addItem(QString::fromUtf8(label), px);
    fontBox_->setCurrentIndex(fontBox_->findData(ui::fontBaseRef()));
    connect(fontBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        ui::setFontBase(fontBox_->currentData().toInt());
    });
    fontBox_->setFixedWidth(180);
    lay->addWidget(fontBox_);

    auto* fontHint = thLabel("font-size:11px; color:@muted@;", page);
    fontHint->setText(i18n::trs("所有界面文字按所选基准等比缩放。",
                                "All UI text scales relative to the selected base size."));
    lay->addWidget(fontHint);
    lay->addStretch(1);
    return page;
}

// ---- 数据与备份：VACUUM INTO 快照 / 恢复 / 手动维护 ----
QWidget* SettingsDialog::buildBackupPage() {
    auto* page = new QWidget(stack_);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(12);

    auto* intro = thLabel("font-size:11px; color:@muted@;", page);
    intro->setText(i18n::trs(
        "备份是数据库的一致性快照（VACUUM INTO），保存在数据目录的 backup/ 下；"
        "恢复会用快照整库替换当前数据。",
        "Backups are consistent SQLite snapshots (VACUUM INTO) stored in the data "
        "directory's backup/ folder; restoring replaces the current database."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    backupTable_ = new QTableWidget(0, 2, page);
    backupTable_->setHorizontalHeaderLabels(
        {i18n::trs("快照文件", "Snapshot"), i18n::trs("大小", "Size")});
    backupTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    polishTable(backupTable_);
    backupTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    lay->addWidget(backupTable_, 1);

    auto* row = new QHBoxLayout();
    auto* backupBtn = new QPushButton(i18n::trs("立即备份", "Back up now"), page);
    backupBtn->setObjectName("primary");
    connect(backupBtn, &QPushButton::clicked, this, [this] {
        std::string path, err;
        if (platform_.backupCreate(path, err)) {
            ui::Toast::show(this, i18n::trs("已备份：", "Backed up: ") +
                                      QString::fromStdString(path));
            refreshBackupList();
        } else {
            ui::Toast::show(this, i18n::trs("备份失败：", "Backup failed: ") +
                                      QString::fromStdString(err), false);
        }
    });
    auto* restoreBtn = new QPushButton(i18n::trs("恢复所选", "Restore selected"), page);
    connect(restoreBtn, &QPushButton::clicked, this, [this] {
        int row = backupTable_->currentRow();
        if (row < 0) {
            ui::Toast::show(this, i18n::trs("请先选择一个快照", "Select a snapshot first"), false);
            return;
        }
        QString name = backupTable_->item(row, 0)->text();
        if (QMessageBox::warning(
                this, i18n::trs("恢复快照", "Restore snapshot"),
                i18n::trs("将用快照「%1」整库替换当前数据，未备份的更改会丢失。继续？",
                          "Replace the current database with snapshot \"%1\"? Unsaved "
                          "changes will be lost. Continue?")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        std::string err;
        if (platform_.backupRestore(name.toStdString(), err)) {
            ui::Toast::show(this, i18n::trs("已恢复 ✓", "Restored ✓"));
            refreshBackupList();
        } else {
            ui::Toast::show(this, i18n::trs("恢复失败：", "Restore failed: ") +
                                      QString::fromStdString(err), false);
        }
    });
    auto* folderBtn = new QPushButton(i18n::trs("打开备份文件夹", "Open backup folder"), page);
    connect(folderBtn, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QString::fromStdString(platform_.homeDir()) + "/backup"));
    });
    row->addWidget(backupBtn);
    row->addWidget(restoreBtn);
    row->addWidget(folderBtn);
    row->addStretch(1);
    lay->addLayout(row);

    auto* maintBtn = new QPushButton(i18n::trs("手动维护（审计轮转 + 清理）",
                                               "Run maintenance (audit rotation + cleanup)"), page);
    connect(maintBtn, &QPushButton::clicked, this, [this] {
        std::string stats, err;
        if (platform_.maintenanceRun(ah::kManagerName, stats, err)) {
            ui::Toast::show(this, i18n::trs("维护完成", "Maintenance done"));
        } else {
            ui::Toast::show(this, i18n::trs("维护失败：", "Maintenance failed: ") +
                                      QString::fromStdString(err), false);
        }
    });
    lay->addWidget(maintBtn);
    return page;
}

// ---- 通知偏好：只影响工作台界面 ----
QWidget* SettingsDialog::buildNotifyPage() {
    auto* page = new QWidget(stack_);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(14);

    auto* form = new QFormLayout;
    form->setSpacing(12);
    refreshBox_ = new QComboBox(page);
    refreshBox_->addItem(i18n::trs("3 秒（实时）", "3s (live)"), 3000);
    refreshBox_->addItem(i18n::trs("5 秒", "5s"), 5000);
    refreshBox_->addItem(i18n::trs("10 秒（省电）", "10s (low power)"), 10000);
    {
        QSettings s;
        int ms = s.value("ui/refreshMs", 3000).toInt();
        int idx = refreshBox_->findData(ms);
        refreshBox_->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    connect(refreshBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        QSettings s;
        s.setValue("ui/refreshMs", refreshBox_->currentData().toInt());
        ui::notifyThemeListeners();  // 主窗口监听同一通知重读偏好
    });
    form->addRow(i18n::trs("数据刷新频率", "Data refresh rate"), refreshBox_);

    errorToastBox_ = new QCheckBox(i18n::trs("出现新的未解决错误时弹出提醒",
                                             "Toast when a new unresolved error appears"), page);
    {
        QSettings s;
        errorToastBox_->setChecked(s.value("ui/errorToast", false).toBool());
    }
    connect(errorToastBox_, &QCheckBox::toggled, this, [](bool on) {
        QSettings s;
        s.setValue("ui/errorToast", on);
        ui::notifyThemeListeners();
    });
    form->addRow(errorToastBox_);
    lay->addLayout(form);

    auto* hint = thLabel("font-size:11px; color:@muted@;", page);
    hint->setText(i18n::trs("这些偏好只影响本工作台界面，不影响 Agent 侧行为。",
                            "These only affect this workbench UI, not agent-side behavior."));
    lay->addWidget(hint);
    lay->addStretch(1);
    return page;
}

// ---- Agent 管理：注册列表 / 状态 / 移除 ----
QWidget* SettingsDialog::buildAgentsPage() {
    auto* page = new QWidget(stack_);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(12);

    agentsTable_ = new QTableWidget(0, 5, page);
    agentsTable_->setHorizontalHeaderLabels(
        {i18n::trs("名称", "Name"), i18n::trs("角色", "Role"), i18n::trs("状态", "Status"),
         i18n::trs("当前任务", "Current task"), i18n::trs("最后活跃", "Last active")});
    agentsTable_->horizontalHeader()->setStretchLastSection(true);
    polishTable(agentsTable_);
    lay->addWidget(agentsTable_, 1);

    auto* row = new QHBoxLayout();
    auto* refreshBtn = new QPushButton(i18n::trs("刷新", "Refresh"), page);
    connect(refreshBtn, &QPushButton::clicked, this, [this] { refreshAgents(); });
    // 防呆出路：首次接入引导不只出现在首启，随时可以从设置里重新打开
    auto* onboardBtn = new QPushButton(i18n::trs("重新打开接入引导", "Reopen onboarding"), page);
    onboardBtn->setToolTip(i18n::trs(
        "逐步引导你把 Claude Code / Cursor / Codex CLI 接入蜂巢（检测安装并生成配置）",
        "Step-by-step guide to connect Claude Code / Cursor / Codex CLI (detects installs and "
        "generates configs)"));
    connect(onboardBtn, &QPushButton::clicked, this, [this] {
        WelcomeDialog dlg(platform_, this);
        dlg.exec();
        refreshAgents();  // 引导中可能预配了新的接入身份
    });
    auto* connectBtn =
        new QPushButton(i18n::trs("一键接入常用 Agent…", "Connect common agents…"), page);
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        ConnectDialog dlg(platform_, this);
        dlg.exec();
        refreshAgents();  // 接入向导可能注册了新 Agent，回来立即刷新列表
    });
    auto* removeBtn = new QPushButton(i18n::trs("移除所选", "Remove selected"), page);
    removeBtn->setObjectName("danger");
    connect(removeBtn, &QPushButton::clicked, this, [this] {
        int row = agentsTable_->currentRow();
        if (row < 0) {
            ui::Toast::show(this, i18n::trs("请先选择一个 Agent", "Select an agent first"), false);
            return;
        }
        QString name = agentsTable_->item(row, 0)->text();
        if (name == QString::fromLatin1(ah::kManagerName)) {
            ui::Toast::show(this, i18n::trs("管理者不可移除", "The manager cannot be removed"),
                            false);
            return;
        }
        if (QMessageBox::question(
                this, i18n::trs("移除 Agent", "Remove agent"),
                i18n::trs("移除「%1」后其 API Key 立即失效，需要重新注册才能接入。继续？",
                          "Removing \"%1\" invalidates its API key immediately; it must "
                          "re-register to join again. Continue?")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        std::string err;
        if (platform_.agentRemove(ah::kManagerName, name.toStdString(), err)) {
            ui::Toast::show(this, i18n::trs("已移除 ✓", "Removed ✓"));
            refreshAgents();
        } else {
            ui::Toast::show(this, i18n::trs("移除失败：", "Remove failed: ") +
                                      QString::fromStdString(err), false);
        }
    });
    auto* rotateBtn = new QPushButton(i18n::trs("重新生成密钥", "Rotate API key"), page);
    connect(rotateBtn, &QPushButton::clicked, this, [this] {
        int row = agentsTable_->currentRow();
        if (row < 0) {
            ui::Toast::show(this, i18n::trs("请先选择一个 Agent", "Select an agent first"), false);
            return;
        }
        const QString name = agentsTable_->item(row, 0)->text();
        if (QMessageBox::question(
                this, i18n::trs("重新生成密钥", "Rotate API key"),
                i18n::trs("将为「%1」签发新密钥，旧密钥立即失效（该 Agent 必须改用新密钥，否则会掉线）。\n"
                          "数据库只保存加盐哈希，明文密钥丢失后无法找回，重新生成是唯一的恢复途径。继续？",
                          "A new key will be issued for \"%1\"; the old one stops working at once "
                          "(that agent must switch to the new key or it goes offline).\n"
                          "The database stores only a salted hash, so a lost key cannot be "
                          "recovered — re-issuing is the only way back. Continue?")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        std::string err, apiKey;
        if (!platform_.agentRotateKey(ah::kManagerName, name.toStdString(), apiKey, err)) {
            ui::Toast::show(this, i18n::trs("重新生成失败：", "Rotate failed: ") +
                                      QString::fromStdString(err),
                            false);
            return;
        }
        const QString key = QString::fromStdString(apiKey);
        QApplication::clipboard()->setText(key);
        QMessageBox box(this);
        box.setWindowTitle(i18n::trs("新密钥（仅显示一次）", "New key (shown once)"));
        box.setIcon(QMessageBox::Information);
        box.setText(i18n::trs("「%1」的新密钥已生成，并已复制到剪贴板：", "New key for \"%1\" (copied to clipboard):")
                        .arg(name));
        box.setInformativeText(
            key + "\n\n" +
            i18n::trs("请立刻填入该 Agent 端并重启它；此密钥不会再次展示。",
                      "Paste it into that agent now and restart it; this key is never shown again."));
        box.setStandardButtons(QMessageBox::Ok);
        box.exec();
        ui::Toast::show(this, i18n::trs("新密钥已签发 ✓", "New key issued ✓"));
        refreshAgents();
    });
    row->addWidget(refreshBtn);
    row->addWidget(onboardBtn);
    row->addWidget(connectBtn);
    row->addWidget(rotateBtn);
    row->addWidget(removeBtn);
    row->addStretch(1);
    lay->addLayout(row);

    auto* hint = thLabel("font-size:11px; color:@muted@;", page);
    hint->setText(i18n::trs("移除后该 Agent 的 API Key 立即失效（操作记入审计日志）。"
                            "密钥明文只在首次签发时展示，丢失后用「重新生成密钥」恢复。",
                            "Removing an agent invalidates its API key (audited). A key is shown only "
                            "when it is issued; if it is lost, use \"Rotate API key\" to recover."));
    lay->addWidget(hint);
    return page;
}

void SettingsDialog::refreshBackupList() {
    if (!backupTable_) return;
    std::vector<std::string> snaps;
    std::string err;
    if (!platform_.backupList(snaps, err)) return;
    backupTable_->setRowCount(static_cast<int>(snaps.size()));
    for (size_t i = 0; i < snaps.size(); ++i) {
        const QString name = QString::fromStdString(snaps[i]);
        const qint64 sz = QFileInfo(QString::fromStdString(platform_.homeDir()) +
                                    "/backup/" + name)
                              .size();
        setRow(backupTable_, static_cast<int>(i), {name, formatNum(sz) + " B"});
    }
}

void SettingsDialog::refreshAgents() {
    if (!agentsTable_) return;
    std::vector<ah::AgentInfo> agents;
    std::string err;
    if (!platform_.listAgents(agents, err)) return;
    agentsTable_->setRowCount(static_cast<int>(agents.size()));
    for (size_t i = 0; i < agents.size(); ++i) {
        const auto& a = agents[i];
        setRow(agentsTable_, static_cast<int>(i),
               {QString::fromStdString(a.name), QString::fromStdString(a.role),
                QString::fromStdString(a.status), QString::fromStdString(a.current_task),
                a.last_seen_at.empty() ? QString("—")
                                       : relTime(QString::fromStdString(a.last_seen_at))});
    }
}

// ---- Agent API：端点清单 + 一键复制 ----
QWidget* SettingsDialog::buildApiPage() {
    auto* page = new QWidget();
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(10);

    auto* addr = thLabel("font-size:13px; font-weight:600; color:@text@;", page);
    addr->setText(QString("HTTP  ·  http://127.0.0.1:%1").arg(platform_.httpPort()));
    lay->addWidget(addr);

    auto* auth = thLabel("font-size:11px; color:@muted@;", page);
    auth->setText(i18n::trs(
        "鉴权：业务接口需请求头 X-Agent-Name + X-Api-Key；"
        "预算设置等管理接口需 X-Master-Key。",
        "Auth: agent endpoints require X-Agent-Name + X-Api-Key headers; "
        "admin endpoints such as budget require X-Master-Key."));
    auth->setWordWrap(true);
    lay->addWidget(auth);

    // 关键端点列表：点击「复制」把完整 URL 放入剪贴板，便于接入新 Agent
    struct Ep {
        const char* method;
        const char* path;
    };
    const Ep eps[] = {
        {"GET", "/api/health"},
        {"POST", "/api/agents/register"},
        {"POST", "/api/agents/heartbeat"},
        {"POST", "/api/agents/remove"},
        {"GET/POST", "/api/memory"},
        {"POST", "/api/knowledge"},
        {"POST", "/api/knowledge/search"},
        {"POST", "/api/skills"},
        {"POST", "/api/skills/{name}/invoke"},
        {"POST", "/api/messages"},
        {"GET", "/api/messages"},
        {"POST", "/api/errors"},
        {"POST", "/api/usage/report"},
        {"GET", "/api/usage/summary"},
        {"GET", "/api/usage/daily"},
        {"GET", "/api/usage/models"},
        {"GET", "/api/audit"},
    };
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(5);
    for (size_t i = 0; i < std::size(eps); ++i) {
        const int r = static_cast<int>(i);
        auto* m = thLabel("color:@accent@; font-size:11px; font-family:@mono@,monospace;", page);
        m->setText(QString::fromLatin1(eps[i].method));
        auto* pth = thLabel("color:@text@; font-size:11px; font-family:@mono@,monospace;", page);
        pth->setText(QString::fromLatin1(eps[i].path));
        auto* cp = new QToolButton(page);
        cp->setText(i18n::trs("复制", "Copy"));
        cp->setCursor(Qt::PointingHandCursor);
        const QString full = QString("http://127.0.0.1:%1%2")
                                 .arg(platform_.httpPort())
                                 .arg(QString::fromLatin1(eps[i].path));
        connect(cp, &QToolButton::clicked, this, [full] {
            QApplication::clipboard()->setText(full);
            ui::Toast::show(qApp->activeWindow(), i18n::trs("已复制", "Copied"));
        });
        grid->addWidget(m, r, 0);
        grid->addWidget(pth, r, 1);
        grid->addWidget(cp, r, 2);
        grid->setColumnStretch(1, 1);
    }
    lay->addLayout(grid);
    lay->addStretch(1);

    auto* scroll = new QScrollArea(stack_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    return scroll;
}

// ---- 关于：品牌触点（口号定稿见 docs/brand.md）----
QWidget* SettingsDialog::buildAboutPage() {
    auto* page = new QWidget(stack_);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->addStretch(2);

    auto* logo = new QLabel(page);
    QPixmap pm(":/brand/logo.png");
    logo->setPixmap(pm.scaled(84, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    lay->addWidget(logo);

    auto* name = thLabel("font-size:20px; font-weight:800; color:@text@;", page);
    name->setText("MiderHive");
    name->setAlignment(Qt::AlignCenter);
    lay->addWidget(name);

    auto* slogan = thLabel("font-size:13px; font-weight:600; color:@brand@;", page);
    slogan->setText(i18n::trs("单体成长，蜂巢共享", "Grow alone, thrive together."));
    slogan->setAlignment(Qt::AlignCenter);
    lay->addWidget(slogan);

    auto* ver = thLabel("font-size:11px; color:@muted@;", page);
    ver->setText(QString("v%1  ·  %2")
                     .arg(ah::kPlatformVersion)
                     .arg(i18n::trs("本地优先 · 数据不出本机", "local-first · data stays local")));
    ver->setAlignment(Qt::AlignCenter);
    lay->addWidget(ver);
    lay->addSpacing(10);

    // 快捷键速查：随主题重涂的富文本标签（键帽用内联样式，QSS 管不到富文本内部）
    auto* keys = new QLabel(page);
    keys->setTextFormat(Qt::RichText);
    keys->setAlignment(Qt::AlignCenter);
    keys->setStyleSheet(ui::th("font-size:11px; color:@muted@;"));
    const auto kbd = [](const QString& k) {
        return ui::th(QString("<span style='background:@field@; color:@text@;"
                              " border:1px solid @line@; border-radius:4px;"
                              " padding:0 4px; font-family:@mono@,monospace;'>%1</span>")
                          .arg(k));
    };
    keys->setText(QString(
        "%1 %2　%3 %4　%5 %6　%7 %8")
        .arg(kbd("Ctrl 1-7"), i18n::trs("切换面板", "switch panel"))
        .arg(kbd("F5"), i18n::trs("刷新", "refresh"))
        .arg(kbd("Ctrl+F"), i18n::trs("聚焦过滤框", "focus filter"))
        .arg(kbd("Ctrl+,"), i18n::trs("打开设置", "open settings")));
    lay->addWidget(keys);
    lay->addSpacing(2);

    auto* row = new QHBoxLayout();
    row->addStretch(1);
    auto mk = [this, page, row](const QString& text, const char* url) {
        auto* b = new QPushButton(text, page);
        connect(b, &QPushButton::clicked, this,
                [url] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(url))); });
        row->addWidget(b);
        return b;
    };
    mk(i18n::trs("GitHub 仓库", "GitHub"), "https://github.com/SiliconCoderJames/MiderHive");
    mk(i18n::trs("问题反馈", "Issues"), "https://github.com/SiliconCoderJames/MiderHive/issues");
    mk(i18n::trs("☕ 赞助", "Sponsor"), "https://www.buymeacoffee.com/zwj8jc5rrgp");
    row->addStretch(1);
    lay->addLayout(row);

    auto* lic = thLabel("font-size:10px; color:@muted@;", page);
    lic->setText("© 2026 SiliconCoderJames · MIT License");
    lic->setAlignment(Qt::AlignCenter);
    lay->addWidget(lic);
    lay->addStretch(3);
    return page;
}

// ---- 更新：自动检查 + 一键升级（走 release 资产的 latest.json，见 update_checker.h）----
QWidget* SettingsDialog::buildUpdatePage() {
    auto* page = new QWidget(stack_);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(10);

    auto* cur = thLabel("font-size:13px; font-weight:600; color:@text@;", page);
    cur->setText(QString(i18n::trs("当前版本", "Current version")) +
                 QString("  v%1").arg(ah::kPlatformVersion));
    lay->addWidget(cur);

    autoCheckBox_ = new QCheckBox(
        i18n::trs("每天自动检查一次更新", "Check for updates automatically (once a day)"), page);
    autoCheckBox_->setChecked(ui::UpdateChecker::autoCheckEnabled());
    connect(autoCheckBox_, &QCheckBox::toggled, this,
            [](bool on) { ui::UpdateChecker::setAutoCheckEnabled(on); });
    lay->addWidget(autoCheckBox_);

    // 网络韧性：GitHub 直连经常超时/断流。这里可填加速前缀，或允许直连失败后自动换公共镜像。
    auto* mirrorRow = new QHBoxLayout();
    auto* mirrorLabel = thLabel("font-size:12px; color:@muted@;", page);
    mirrorLabel->setText(i18n::trs("镜像加速前缀", "Mirror prefix"));
    mirrorEdit_ = new QLineEdit(page);
    mirrorEdit_->setPlaceholderText("https://ghfast.top/");
    mirrorEdit_->setText(ui::UpdateChecker::mirrorPrefix());
    mirrorEdit_->setClearButtonEnabled(true);
    mirrorEdit_->setToolTip(i18n::trs(
        "填写 GitHub 加速前缀（形如 https://ghfast.top/）；留空表示只用直连。\n"
        "镜像仅用于下载安装包，更新清单与 SHA256 始终直连 GitHub。",
        "A GitHub accelerator prefix such as https://ghfast.top/; leave empty for direct only.\n"
        "Mirrors are used for the installer download only - the manifest and its SHA256 always "
        "come straight from GitHub."));
    connect(mirrorEdit_, &QLineEdit::editingFinished, this,
            [this] { ui::UpdateChecker::setMirrorPrefix(mirrorEdit_->text()); });
    mirrorRow->addWidget(mirrorLabel);
    mirrorRow->addWidget(mirrorEdit_, 1);
    lay->addLayout(mirrorRow);

    autoMirrorBox_ = new QCheckBox(
        i18n::trs("GitHub 直连失败时自动尝试公共加速镜像",
                  "Try public mirrors automatically when GitHub is unreachable"),
        page);
    autoMirrorBox_->setChecked(ui::UpdateChecker::autoMirrorEnabled());
    connect(autoMirrorBox_, &QCheckBox::toggled, this,
            [](bool on) { ui::UpdateChecker::setAutoMirrorEnabled(on); });
    lay->addWidget(autoMirrorBox_);

    if (!ui::UpdateChecker::isInstalledCopy()) {
        auto* portable = thLabel("font-size:11px; color:@muted@;", page);
        portable->setText(i18n::trs(
            "当前是便携版：不会自动安装（否则系统里会多出一份），请到下载页手动替换。",
            "Portable build: automatic install is disabled (it would add a second copy) — "
            "download the new archive manually."));
        portable->setWordWrap(true);
        lay->addWidget(portable);
    }

    auto* row = new QHBoxLayout();
    auto* checkBtn = new QPushButton(i18n::trs("检查更新", "Check for updates"), page);
    installBtn_ = new QPushButton(i18n::trs("下载并安装", "Download & install"), page);
    installBtn_->setObjectName("primary");
    installBtn_->setEnabled(false);
    skipBtn_ = new QPushButton(i18n::trs("跳过此版本", "Skip this version"), page);
    skipBtn_->setVisible(false);
    auto* pageBtn = new QPushButton(i18n::trs("打开下载页", "Open releases page"), page);
    row->addWidget(checkBtn);
    row->addWidget(installBtn_);
    row->addWidget(skipBtn_);
    row->addWidget(pageBtn);
    row->addStretch(1);
    lay->addLayout(row);

    progress_ = new QProgressBar(page);
    progress_->setRange(0, 100);
    progress_->setVisible(false);
    lay->addWidget(progress_);

    latest_ = thLabel("font-size:12px; color:@muted@;", page);
    latest_->setWordWrap(true);
    lay->addWidget(latest_);

    auto* note = thLabel("font-size:11px; color:@muted@;", page);
    note->setText(i18n::trs(
        "更新从 GitHub Releases 获取：下载后先校验 SHA256 再安装，校验不通过会删除文件。"
        "网络不稳时会自动重试并断点续传，直连失败后按需切换镜像；镜像只用于下载安装包，"
        "更新清单与校验和始终直连 GitHub。纯本地运行、无遥测；安装包为 per-user，"
        "不需要管理员权限。",
        "Updates come from GitHub Releases: the download is verified against its SHA256 before "
        "installing, and deleted if the checksum does not match. Flaky networks are handled with "
        "retries and resume, with mirrors as a fallback; mirrors only serve the installer, while "
        "the manifest and its checksums always come straight from GitHub. No telemetry; the "
        "installer is per-user and needs no administrator rights."));
    note->setWordWrap(true);
    lay->addWidget(note);
    lay->addStretch(1);

    updater_ = new ui::UpdateChecker(this);
    connect(updater_, &ui::UpdateChecker::checking, this, [this] {
        latest_->setText(i18n::trs("正在检查…", "Checking…"));
        installBtn_->setEnabled(false);
    });
    connect(updater_, &ui::UpdateChecker::upToDate, this, [this](const QString& cur) {
        pending_ = {};
        installBtn_->setEnabled(false);
        skipBtn_->setVisible(false);
        latest_->setText(i18n::trs("已是最新版本。", "You're up to date.") + "  v" + cur);
    });
    connect(updater_, &ui::UpdateChecker::updateAvailable, this,
            [this](const ui::UpdateInfo& info) {
                pending_ = info;
                const bool skipped = ui::UpdateChecker::isSkippedVersion(info.version);
                latest_->setText(i18n::trs("发现新版本 ", "New version available ") + "v" +
                                 info.version +
                                 (skipped ? i18n::trs("（此前被跳过）", " (previously skipped)") : ""));
                installBtn_->setEnabled(ui::UpdateChecker::isInstalledCopy());
                skipBtn_->setVisible(true);
            });
    connect(updater_, &ui::UpdateChecker::failed, this, [this](const QString& why) {
        progress_->setVisible(false);
        latest_->setText(i18n::trs("更新失败：", "Update failed: ") + why);
    });
    connect(updater_, &ui::UpdateChecker::downloadProgress, this,
            [this](qint64 got, qint64 total) {
                progress_->setVisible(true);
                progress_->setRange(0, 100);
                progress_->setValue(total > 0 ? int(got * 100 / total) : 0);
                latest_->setText(i18n::trs("正在下载更新…", "Downloading update…") + "  " +
                                 QString::number(got / 1048576.0, 'f', 1) + " / " +
                                 (total > 0 ? QString::number(total / 1048576.0, 'f', 1) : "?") +
                                 " MB");
            });
    connect(updater_, &ui::UpdateChecker::verifying, this, [this] {
        progress_->setVisible(false);
        latest_->setText(i18n::trs("正在校验 SHA256…", "Verifying SHA256…"));
    });
    connect(updater_, &ui::UpdateChecker::installing, this, [this] {
        latest_->setText(i18n::trs("已启动安装程序，工作台即将退出…",
                                  "Installer launched - the workbench will now exit…"));
        ui::Toast::show(this, i18n::trs("正在安装更新", "Installing update"));
        // 让出时间给安装程序读取文件，然后退出（MSI 自身也会关闭仍在运行的实例）
        QTimer::singleShot(1500, qApp, [] { QCoreApplication::quit(); });
    });

    connect(checkBtn, &QPushButton::clicked, this, [this] { updater_->check(); });
    connect(installBtn_, &QPushButton::clicked, this, [this] {
        if (pending_.valid()) updater_->downloadAndInstall(pending_);
    });
    connect(skipBtn_, &QPushButton::clicked, this, [this] {
        if (pending_.valid()) {
            ui::UpdateChecker::skipVersion(pending_.version);
            latest_->setText(i18n::trs("已跳过 v", "Skipped v") + pending_.version +
                             i18n::trs("，下次检查仍会显示。", " - it will still show up next time."));
            skipBtn_->setVisible(false);
        }
    });
    connect(pageBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoPage)));
    });
    return page;
}
