#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStatusBar>
#include <QSystemTrayIcon>

#include <utility>

#include "gui_util.h"
#include "panels/dashboard_panel.h"
#include "panels/errors_panel.h"
#include "panels/knowledge_panel.h"
#include "panels/logs_panel.h"
#include "panels/memory_panel.h"
#include "panels/messages_panel.h"
#include "panels/skills_panel.h"
#include "panels/usage_panel.h"
#include "settings_dialog.h"
#include "theme.h"
#include "update_checker.h"
#include "welcome_dialog.h"

MainWindow::MainWindow(ah::Platform& platform, QWidget* parent)
    : QMainWindow(parent), platform_(platform) {
    setWindowTitle("MiderHive · 多 Agent 协作工作台");
    // 最小尺寸必须先于几何恢复设定：否则会恢复出比最小尺寸还小的窗口
    // （原先在 main 里调用时为时已晚，布局被压到裁切）
    setMinimumSize(1080, 680);
    // 恢复上次窗口几何（位置/大小/最大化状态；关闭到托盘前已保存）
    QSettings s;
    const QByteArray geo = s.value("ui/geometry").toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);

    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- 侧边栏：品牌区 + 图标导航 + 版本脚注（样式统一在 applyChrome，随主题重涂）----
    side_ = new QWidget(central);
    side_->setFixedWidth(190);
    auto* sideLay = new QVBoxLayout(side_);
    sideLay->setContentsMargins(0, 0, 0, 10);
    sideLay->setSpacing(0);

    auto* brand = new QWidget(side_);
    auto* brandLay = new QVBoxLayout(brand);
    brandLay->setContentsMargins(16, 16, 12, 14);
    brandLay->setSpacing(2);
    // 品牌区：仓库品牌图标（与窗口/托盘图标同源）+ 字标，替代 emoji 蜜蜂
    // （emoji 在不同 Windows 版本上字形差异大，且与新的矢量图标体系不一致）
    auto* brandRow = new QHBoxLayout();
    brandRow->setSpacing(8);
    logoMark_ = new QLabel(brand);
    logoMark_->setFixedSize(24, 24);
    {
        QPixmap mark(":/brand/logo.png");
        logoMark_->setPixmap(mark.isNull()
                                 ? ui::makeIcon("overview", ui::brand(), 22).pixmap(22, 22)
                                 : mark.scaled(24, 24, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
    }
    logo_ = new QLabel("MiderHive", brand);
    logo_->setTextFormat(Qt::RichText);  // 字标为双色艺术字（Mider 中性 + Hive 品牌色），随主题重涂
    brandRow->addWidget(logoMark_);
    brandRow->addWidget(logo_);
    brandRow->addStretch(1);
    brandLay->addLayout(brandRow);
    tagline_ = new QLabel(brand);
    brandLay->addWidget(tagline_);
    sideLay->addWidget(brand);

    // 品牌分隔线：品牌色→强调色横向渐变，蜂巢品牌签名
    brandLine_ = new QFrame(side_);
    brandLine_->setFixedHeight(2);
    sideLay->addWidget(brandLine_);

    nav_ = new QListWidget(side_);
    nav_->setFocusPolicy(Qt::NoFocus);  // 去除选中项虚线焦点框；Ctrl+1..7 仍可切换面板
    sideLay->addWidget(nav_, 1);

    // 脚注：版本号 + 设置 + 语言切换（主题/字号在 ⚙ 设置的外观页，此处不再重复入口）
    auto* foot = new QWidget(side_);
    auto* footLay = new QHBoxLayout(foot);
    footLay->setContentsMargins(10, 0, 10, 0);
    ver_ = new QLabel(QString("v%1").arg(ah::kPlatformVersion), foot);
    ver_->setToolTip(i18n::trs("MiderHive 版本 %1", "MiderHive %1")
                         .arg(QString::fromUtf8(ah::kPlatformVersion)));
    settingsBtn_ = new QToolButton(foot);
    settingsBtn_->setIcon(ui::makeIcon("gear", ui::muted(), 16));
    settingsBtn_->setToolTip(i18n::trs("设置", "Settings"));
    // 纯图标按钮必须补可访问名称：读屏与 UIA 自动化都依赖 Name（tooltip 不算）
    settingsBtn_->setAccessibleName(i18n::trs("设置", "Settings"));
    connect(settingsBtn_, &QToolButton::clicked, this, &MainWindow::openSettings);
    langBtn_ = new QToolButton(foot);
    connect(langBtn_, &QToolButton::clicked, this, [] { i18n::toggle(); });
    footLay->addWidget(ver_);
    footLay->addStretch(1);
    footLay->addWidget(settingsBtn_);
    footLay->addWidget(langBtn_);
    sideLay->addWidget(foot);
    layout->addWidget(side_);

    stack_ = new QStackedWidget(central);
    layout->addWidget(stack_, 1);

    panelFactories_ = {
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new DashboardPanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new UsagePanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new KnowledgePanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new SkillsPanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new MemoryPanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new MessagesPanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new ErrorsPanel(pl, parent)); },
        [](ah::Platform& pl, QWidget* parent) { return static_cast<PanelBase*>(new LogsPanel(pl, parent)); },
    };
    rebuildPanels();

    setCentralWidget(central);
    buildNav();
    buildStatusBar();
    applyChrome();
    applyLanguage();

    connect(nav_, &QListWidget::currentRowChanged, this, &MainWindow::onNavChanged);
    nav_->setCurrentRow(0);

    // 语言切换：重译铬层（导航/页头/状态栏），面板各自处理自有文案
    i18n::listeners().push_back([this] { applyLanguage(); });
    // 主题/字号切换：重生成 QSS、重涂铬层、重建面板
    ui::themeListeners().push_back([this] { applyTheme(); });
    // 界面偏好（刷新频率/错误提醒）由设置对话框写入后经同一通知重读
    ui::themeListeners().push_back([this] { applyUiPrefs(); });

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onRefresh);
    applyUiPrefs();  // 读取刷新频率（默认 3s）与错误提醒偏好
    timer_->start(timer_->interval());

    setupTray();
    scheduleUpdateCheck();  // 启动后延迟做一次自动检查（每天最多一次，可在设置里关闭）

    // 首次运行引导：欢迎 + 三步接入 + 选主题（完成后不再弹出）
    {
        QSettings s;
        if (!s.value("ui/welcomeSeen", false).toBool()) {
            QTimer::singleShot(400, this, [this] {
                WelcomeDialog w(platform_, this);
                w.exec();
                QSettings s;
                s.setValue("ui/welcomeSeen", true);
            });
        }
    }

    // 快捷键：Ctrl+1..8 切面板，F5 手动刷新，Ctrl+F 聚焦本面板即时过滤，Ctrl+, 打开设置
    for (int i = 0; i < 8; ++i) {
        auto* sc = new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i + 1)), this);
        connect(sc, &QShortcut::activated, this, [this, i] { nav_->setCurrentRow(i); });
    }
    auto* refreshSc = new QShortcut(QKeySequence("F5"), this);
    connect(refreshSc, &QShortcut::activated, this, &MainWindow::onRefresh);
    auto* filterSc = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(filterSc, &QShortcut::activated, this, [this] {
        int idx = stack_->currentIndex();
        if (idx >= 0 && idx < static_cast<int>(panels_.size()))
            panels_[static_cast<size_t>(idx)]->focusFilter();
    });
    auto* settingsSc = new QShortcut(QKeySequence("Ctrl+,"), this);
    connect(settingsSc, &QShortcut::activated, this, &MainWindow::openSettings);
}

void MainWindow::hideEvent(QHideEvent* e) {
    // 任何隐藏路径（关闭到托盘 / 真正退出前）都保存窗口几何，下次启动原样恢复
    QSettings s;
    s.setValue("ui/geometry", saveGeometry());
    QMainWindow::hideEvent(e);
}

void MainWindow::buildNav() {
    nav_->clear();
    nav_->setStyleSheet(ui::th(
        "QListWidget { background:@deep@; border:none; padding:2px 8px; font-size:13px; }"
        "QListWidget::item { padding:12px 14px; margin:2px 8px; border-radius:8px;"
        " color:@muted@; border-left:3px solid transparent; }"
        "QListWidget::item:hover { background:@card@; color:@text@; }"
        "QListWidget::item:selected { background:@selbg@; color:@seltext@;"
        " font-weight:600; border-left:3px solid @brand@; }"));
    // 程序化线性图标：随主题着色，错误项恒红、选中态换亮色（替代大小不一的 emoji）
    const std::vector<std::tuple<const char*, const char*, const char*>> items{
        {"overview", "总览", "Overview"},
        {"usage", "用量分析", "Usage"},
        {"knowledge", "知识库", "Knowledge"},
        {"skills", "技能库", "Skills"},
        {"memory", "用户记忆", "Memory"},
        {"messages", "Agent 交流", "Messaging"},
        {"errors", "错误报告", "Errors"},
        {"audit", "操作日志", "Audit"},
    };
    for (const auto& [kind, zh, en] : items) {
        // 图标与文字一律中性色；「错误报告」有未解决错误时由 updateStatusBar
        // 换红色图标 + 追加计数，避免侧栏在没有错误时也长期处于告警色
        auto* it = new QListWidgetItem(ui::makeIcon(kind, ui::muted(), 18, ui::selText()),
                                       "  " + i18n::trs(zh, en));
        nav_->addItem(it);
    }
}

void MainWindow::applyLanguage() {
    setWindowTitle(i18n::trs("MiderHive · 多 Agent 协作工作台",
                             "MiderHive · Multi-Agent Collaboration Workbench"));
    tagline_->setText(i18n::trs("多 Agent 协作工作台", "multi-agent collaboration hub"));
    langBtn_->setText(i18n::g_lang == i18n::Lang::Zh ? "EN" : "中文");
    langBtn_->setToolTip(i18n::trs("切换语言", "Switch language"));
    settingsBtn_->setToolTip(i18n::trs("设置", "Settings"));
    settingsBtn_->setAccessibleName(i18n::trs("设置", "Settings"));
    int row = nav_->currentRow();
    buildNav();
    if (row >= 0) nav_->setCurrentRow(row);
    rebuildPanels();  // 面板整体重建：ctor 里的 trs() 随新语言重新求值
    updateStatusBar();
}

void MainWindow::rebuildPanels() {
    int row = nav_ ? nav_->currentRow() : 0;
    // 拆除旧页面：面板已交给滚动容器持有，直接按栈内条目销毁即可
    panels_.clear();
    while (stack_->count() > 0) {
        QWidget* w = stack_->widget(0);
        stack_->removeWidget(w);
        w->deleteLater();
    }
    for (auto& f : panelFactories_) {
        PanelBase* panel = f(platform_, this);
        // 每个面板套一层滚动容器：窗口偏小或字号调大时内容可滚动可达。
        // 原先面板直接进 QStackedWidget，最小高度之和超出视口时底部内容会被裁掉。
        auto* scroll = new QScrollArea(stack_);
        scroll->setWidget(panel);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->viewport()->setAutoFillBackground(false);
        panels_.push_back(panel);
        stack_->addWidget(scroll);
    }
    if (row >= 0 && row < static_cast<int>(panels_.size())) {
        stack_->setCurrentIndex(row);
        panels_[static_cast<size_t>(row)]->refresh();
    }
}

void MainWindow::applyChrome() {
    side_->setStyleSheet(ui::th("QWidget { background:@deep@; }"));
    // 双色艺术字标：Mider 中性色 + Hive 品牌色，随主题重涂
    logo_->setText(ui::th("<span style='color:@text@;'>Mider</span>"
                          "<span style='color:@brand@;'>Hive</span>"));
    logo_->setStyleSheet(
        ui::th("font-size:19px; font-weight:800; color:@text@; background:transparent;"));
    tagline_->setStyleSheet(
        ui::th("font-size:11px; font-weight:500; color:@muted@; background:transparent;"));
    brandLine_->setStyleSheet(ui::th(
        "background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        "stop:0 @brand@, stop:0.55 @accent@, stop:1 transparent);"
        "border-radius:1px; margin:0 18px 8px 18px;"));
    ver_->setStyleSheet(ui::th(
        "font-family:'@mono@'; font-size:11px; color:@muted@; background:transparent;"));
    const QString btn = ui::th(
        "QToolButton { color:@muted@; font-size:11px; font-weight:600;"
        " border:1px solid @line@; border-radius:7px; padding:4px 10px; }"
        "QToolButton:hover { color:@text@; border-color:@accenthi@; background:@card@; }"
        "QToolButton:pressed { background:@fieldhover@; border-color:@accent@; }");
    langBtn_->setStyleSheet(btn);
    settingsBtn_->setStyleSheet(btn);
    settingsBtn_->setIcon(ui::makeIcon("gear", ui::muted(), 16));  // 随主题重涂齿轮
    // 手型光标：导航与 chrome 按钮都是可点击目标（产品化交互惯例）
    nav_->viewport()->setCursor(Qt::PointingHandCursor);
    langBtn_->setCursor(Qt::PointingHandCursor);
    settingsBtn_->setCursor(Qt::PointingHandCursor);
}

void MainWindow::applyUiPrefs() {
    QSettings s;
    int ms = s.value("ui/refreshMs", 3000).toInt();
    if (ms < 1000 || ms > 60000) ms = 3000;
    timer_->setInterval(ms);
    errorToast_ = s.value("ui/errorToast", false).toBool();
}

void MainWindow::scheduleUpdateCheck() {
    // 启动后延迟 5 秒（不跟首屏刷新抢资源），且每天最多一次
    if (!ui::UpdateChecker::autoCheckEnabled()) return;
    if (!ui::UpdateChecker::autoCheckDue(24)) return;
    QTimer::singleShot(5000, this, [this] {
        updater_ = new ui::UpdateChecker(this);
        connect(updater_, &ui::UpdateChecker::updateAvailable, this,
                [this](const ui::UpdateInfo& info) { promptUpdate(info); });
        // 启动时的自动检查失败要静默：断网/公司代理下不该弹错误框
        connect(updater_, &ui::UpdateChecker::failed, this, [](const QString&) {});
        updater_->check();
    });
}

void MainWindow::promptUpdate(const ui::UpdateInfo& info) {
    if (ui::UpdateChecker::isSkippedVersion(info.version)) return;  // 用户已选择跳过（数值等价比较）
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(i18n::trs("发现新版本", "Update available"));
    box.setText(i18n::trs("MiderHive 有新版本可用", "A new MiderHive version is available"));
    box.setInformativeText(
        i18n::trs("当前版本 v%1 → 最新版本 v%2\n\n下载后将校验 SHA256 再安装；"
                  "数据目录不受影响，可稍后再更新。",
                  "Current v%1 → latest v%2\n\nThe download is verified against its SHA256 "
                  "before installing. Your data directory is untouched.")
            .arg(QString::fromLatin1(ah::kPlatformVersion), info.version));
    QPushButton* install = box.addButton(i18n::trs("下载并安装", "Download & install"),
                                         QMessageBox::AcceptRole);
    QPushButton* later = box.addButton(i18n::trs("稍后", "Later"), QMessageBox::RejectRole);
    QPushButton* skip = box.addButton(i18n::trs("跳过此版本", "Skip this version"),
                                      QMessageBox::DestructiveRole);
    box.setDefaultButton(later);
    box.exec();
    if (box.clickedButton() == skip) {
        ui::UpdateChecker::skipVersion(info.version);
        return;
    }
    if (box.clickedButton() != install) return;
    if (!ui::UpdateChecker::isInstalledCopy()) {
        // 便携版不自动装：装 MSI 会在系统里多一份
        QMessageBox::information(
            this, i18n::trs("便携版", "Portable build"),
            i18n::trs("当前是便携版，请到下载页手动替换；自动安装会另装一份到系统目录。",
                      "This is the portable build - please update it manually from the releases "
                      "page; automatic install would add a second copy to the system."));
        QDesktopServices::openUrl(QUrl(info.notesUrl));
        return;
    }
    ui::Toast::show(this, i18n::trs("正在下载更新…", "Downloading update…"));
    connect(updater_, &ui::UpdateChecker::installing, this, [this] {
        ui::Toast::show(this, i18n::trs("正在安装更新", "Installing update"));
        quitForUpdate_ = true;   // 让 closeEvent 真退出而不是最小化到托盘
        QTimer::singleShot(1500, qApp, [] { QCoreApplication::quit(); });
    });
    connect(updater_, &ui::UpdateChecker::failed, this, [this](const QString& why) {
        QMessageBox::warning(this, i18n::trs("更新失败", "Update failed"), why);
    });
    updater_->downloadAndInstall(info);
}

void MainWindow::setupTray() {
    tray_ = new QSystemTrayIcon(QIcon(":/brand/logo.png"), this);
    tray_->setToolTip(i18n::trs("MiderHive · 蜂巢运行中", "MiderHive · hive is running"));
    auto* menu = new QMenu(this);
    auto* showAct = menu->addAction(i18n::trs("显示 / 隐藏工作台", "Show / Hide workbench"));
    connect(showAct, &QAction::triggered, this, [this] {
        setVisible(!isVisible());
        if (isVisible()) {
            raise();
            activateWindow();
        }
    });
    menu->addSeparator();
    auto* quitAct = menu->addAction(i18n::trs("退出", "Quit"));
    connect(quitAct, &QAction::triggered, this, [] { QCoreApplication::exit(0); });
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    setVisible(!isVisible());
                    if (isVisible()) {
                        raise();
                        activateWindow();
                    }
                }
            });
    tray_->show();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // 自动更新：安装程序会让本进程关闭，此时必须真退出——否则"关闭即最小化到托盘"
    // 会让进程继续占着 exe，安装程序无法替换文件（实测踩到过）。
    if (quitForUpdate_) {
        QMainWindow::closeEvent(e);
        QCoreApplication::quit();
        return;
    }
    // 关闭 = 隐藏到托盘（HTTP 服务随进程常驻，Agent 不受影响）；托盘菜单退出才真正退出
    if (tray_ && tray_->isVisible()) {
        e->ignore();
        hide();
        if (!trayHinted_) {
            trayHinted_ = true;
            ui::Toast::show(this, i18n::trs("已最小化到托盘 · 服务仍在运行，托盘右键可退出",
                                            "Minimized to tray — the service keeps running; "
                                            "right-click the tray icon to quit"));
        }
        return;
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::openSettings() {
    if (!settings_) {
        settings_ = new SettingsDialog(platform_, this);
        settings_->setAttribute(Qt::WA_DeleteOnClose);
    }
    settings_->show();
    settings_->raise();
    settings_->activateWindow();
}

void MainWindow::applyTheme() {
    qApp->setStyleSheet(ui::themeQss());  // 全局 QSS 随主题重生成
    applyChrome();
    int row = nav_->currentRow();
    buildNav();
    if (row >= 0) nav_->setCurrentRow(row);
    rebuildPanels();  // 面板样式在构造时取色，整体重建
    updateStatusBar();
}

void MainWindow::buildStatusBar() {
    statusServer_ = new QLabel(this);
    statusUsage_ = new QLabel(this);
    statusUpdated_ = new QLabel(this);
    spin_ = new QLabel(this);
    spin_->setStyleSheet(ui::th("color:@accent@; font-size:14px;"));
    spin_->setToolTip(i18n::trs("正在刷新…", "refreshing…"));
    statusBar()->addWidget(spin_);
    statusBar()->addWidget(statusServer_);
    statusBar()->addPermanentWidget(statusUpdated_);
    statusBar()->addPermanentWidget(statusUsage_);
}

void MainWindow::onNavChanged(int row) {
    if (row >= 0 && row < stack_->count()) {
        stack_->setCurrentIndex(row);
        panels_[static_cast<size_t>(row)]->refresh();
    }
}

void MainWindow::onRefresh() {
    // 刷新微动画：状态栏旋转指示符
    static const QString kFrames = "◐◓◑◒";
    spin_->setText(kFrames[spinPhase_++ % 4]);
    int idx = stack_->currentIndex();
    if (idx >= 0 && idx < static_cast<int>(panels_.size())) panels_[static_cast<size_t>(idx)]->refresh();
    updateStatusBar();
}

void MainWindow::updateStatusBar() {
    std::string err;
    ah::UsageSummary sum;
    // 服务在线指示灯：HTTP 服务随进程常驻，绿点常亮即后端可用
    statusServer_->setText(QString("<span style='color:%1;'>●</span> HTTP: "
                                   "http://127.0.0.1:%2 · %3 %4")
                               .arg(ui::ok().name())
                               .arg(platform_.httpPort())
                               .arg(i18n::trs("数据", "data"))
                               .arg(QString::fromStdString(platform_.homeDir())));
    if (platform_.usageSummary(sum, err)) {
        double pct = sum.budget > 0 ? 100.0 * sum.total_tokens / sum.budget : 0.0;
        QString color = sum.alert_level == "none" ? ui::ok().name()
                        : (sum.alert_level == "warn" ? ui::warn().name() : ui::danger().name());
        statusUsage_->setText(
            QString("<span style='color:%1'>%2: %3 / %4 (%5%) · %6 %7</span>")
                .arg(color)
                .arg(i18n::trs("本周 Token", "Weekly tokens"))
                .arg(formatNum(sum.total_tokens))
                .arg(formatNum(sum.budget))
                .arg(pct, 0, 'f', 1)
                .arg(i18n::trs("剩余", "left"))
                .arg(formatNum(sum.budget - sum.total_tokens)));
    }
    // 导航徽标：未解决错误数附加在「错误报告」项上（图标随计数换色，不再用 emoji 前缀）
    std::vector<ah::ErrorReport> openErrors;
    if (platform_.errorList("open", "", 99, openErrors, err)) {
        openErrors_ = static_cast<int>(openErrors.size());
        // 偏好开启时，新增未解决错误弹提醒（首次采样不提醒）
        if (errorToast_ && seenOpenErrors_ >= 0 && openErrors_ > seenOpenErrors_) {
            ui::Toast::show(this, i18n::trs("新增 %1 条未解决错误", "%1 new unresolved "
                                            "error(s)").arg(openErrors_ - seenOpenErrors_),
                            false);
        }
        seenOpenErrors_ = openErrors_;
        const QString label = i18n::trs("错误报告", "Errors");
        if (auto* errItem = nav_->item(5)) {
            const bool has = openErrors_ > 0;
            errItem->setText(has ? QString("  %1   (%2)").arg(label).arg(openErrors_)
                                 : "  " + label);
            errItem->setIcon(ui::makeIcon("errors", has ? ui::danger() : ui::muted(), 18,
                                          has ? ui::danger() : ui::selText()));
            errItem->setForeground(QBrush(has ? ui::danger()
                                              : (nav_->currentRow() == 5 ? ui::selText()
                                                                         : ui::muted())));
            errItem->setToolTip(has ? i18n::trs("%1 条未解决错误", "%1 unresolved error(s)")
                                          .arg(openErrors_)
                                    : i18n::trs("暂无未解决错误", "no unresolved errors"));
        }
    }
    // 数据新鲜度：让用户知道当前显示是不是刚取的数
    if (statusUpdated_) {
        // 首次刷新发生在 timer_ 创建之前，这里必须容忍空指针
        const int secs = timer_ ? timer_->interval() / 1000 : 3;
        statusUpdated_->setText(i18n::trs("更新于 %1", "updated %1")
                                    .arg(QTime::currentTime().toString("HH:mm:ss")));
        statusUpdated_->setToolTip(
            i18n::trs("自动刷新间隔 %1 秒（设置中心可调）", "auto-refresh every %1 s (see Settings)")
                .arg(secs));
    }
}
