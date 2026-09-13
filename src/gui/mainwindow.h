#pragma once
#include <QListWidget>
#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <functional>
#include <vector>

#include "core/platform.h"
#include "i18n.h"
#include "panels/panel_base.h"
#include "update_checker.h"

class QFrame;
class QCloseEvent;
class QHideEvent;
class SettingsDialog;
class QSystemTrayIcon;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ah::Platform& platform, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent*) override;  // 关闭即隐藏到托盘，服务常驻
    void hideEvent(QHideEvent*) override;    // 任何隐藏路径都持久化窗口几何

private slots:
    void onNavChanged(int row);
    void onRefresh();

private:
    void buildNav();
    void buildStatusBar();
    void updateStatusBar();
    void applyLanguage();
    void rebuildPanels();
    void applyTheme();   // 主题/字号切换：重生成 QSS + 重涂铬层 + 重建面板
    void applyChrome();  // 侧边栏铬层样式（随主题重涂）
    void applyUiPrefs(); // 读取界面偏好（刷新频率/错误提醒）并应用
    void setupTray();    // 系统托盘：蜂巢常驻 + 显示/退出
    void openSettings(); // ⚙ 设置对话框（复用同一实例，关闭即删）
    void scheduleUpdateCheck();                // 启动后的自动检查（每天一次，可关）
    void promptUpdate(const ui::UpdateInfo&);  // 发现新版本时的选择框
    // 接入引导观察：轮询 ui/onboardingPending 里的接入身份，Agent 上线即弹
    // 「接入成功」并写入欢迎记忆（每个身份只发生一次，随后移除登记）
    void checkOnboarding();
    // 跨面板下钻：切到指定面板并可带筛选条件（面板只登记意图，落地由这里负责）
    void goToPanel(const QString& panelId, const QString& filterKey = QString(),
                   const QString& filterValue = QString());
    // 面板 id → 导航行号（按 id 查找，不用硬编码下标：插入新面板不会错位）
    int navRowOf(const QString& panelId) const;

    ah::Platform& platform_;
    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    // 导航项定义（id, 中文, English）——与 panelFactories_ 同序，是"面板清单"的单一来源
    struct NavItem {
        QString id;
        const char* zh;
        const char* en;
    };
    std::vector<NavItem> navItems_;
    std::vector<PanelBase*> panels_;
    std::vector<std::function<PanelBase*(ah::Platform&, QWidget*)>> panelFactories_;
    QWidget* side_ = nullptr;
    QFrame* brandLine_ = nullptr;
    QLabel* logoMark_ = nullptr;  // 品牌图标（仓库 logo.png，缺失时回退矢量蜂巢）
    QLabel* logo_ = nullptr;
    QLabel* statusServer_ = nullptr;
    QLabel* statusUsage_ = nullptr;
    QLabel* statusUpdated_ = nullptr;  // 最近一次刷新时刻（数据新鲜度）
    QLabel* spin_ = nullptr;
    QToolButton* settingsBtn_ = nullptr;
    QToolButton* langBtn_ = nullptr;
    QLabel* ver_ = nullptr;
    QLabel* tagline_ = nullptr;
    QPointer<SettingsDialog> settings_ = nullptr;
    ui::UpdateChecker* updater_ = nullptr;  // 启动时的自动更新检查（延迟创建）
    bool quitForUpdate_ = false;            // 自动更新中：closeEvent 要真退出而非最小化到托盘
    QSystemTrayIcon* tray_ = nullptr;
    bool trayHinted_ = false;
    int spinPhase_ = 0;
    QTimer* timer_ = nullptr;
    int openErrors_ = 0;      // 未解决错误数，用于导航徽标
    int seenOpenErrors_ = -1; // 上次已提醒的错误数（-1 = 尚未采样，启动不弹提醒）
    bool errorToast_ = false; // 偏好：新错误弹 Toast
};
