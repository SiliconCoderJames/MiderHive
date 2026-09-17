#pragma once
// 接入向导（唯一入口）：把"接入一个 Agent"压缩成 选工具 → 点一下 → 重启该工具。
//
// 设计要点（面向新手，尽量少概念）：
//   * 选完工具就自动预配身份（幂等：已存在则轮换），不必先理解"注册/密钥"；
//   * 直接告诉用户配置文件在哪、能不能替他写进去（能写就写，并留 .miderhive.bak）；
//   * 面板上实时显示该 Agent 是否已上线——点完就知道成没成，不用去猜；
//   * 每个工具都有"复制片段 / 打开所在文件夹"兜底，自动写入失败也不至于卡死。
//
// 本对话框每次打开都新建（模态、短生命周期），因此所有文案在构造/交互时用
// i18n::trs 现取即可，不注册 i18n::listeners()——监听器不可撤销，短命对象捕获
// this 会在下次切语言时悬空。
#include <QDialog>
#include <QString>
#include <QVector>

#include "core/platform.h"
#include "integrations.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;

class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(ah::Platform& platform, QWidget* parent = nullptr);

private:
    // 每个工具一行：注册表条目 + 本机检测结果 + 界面上的状态区
    struct Row {
        ui::integrations::Tool tool;
        bool detected = false;
        QLabel* badge = nullptr;   // 左侧列表项里的状态点（已连接/未连接）
    };

    void selectRow(int index);
    void recheck();          // 重新检测本机安装
    void provision();        // 一键接入：预配身份 + 生成片段
    void writeToConfig();    // 把片段合并进该工具的真实配置文件
    void writeProjectConfig();  // Claude Code：写进用户选定项目根的 .mcp.json
    void copySnippet();
    void copyCommand();
    void openConfigFolder();
    void tick();             // 轮询平台：该身份是否已上线

    QString mcpExePath() const;
    // 该工具的主写入按钮是否可用（没有固定配置文件的工具，如 Claude Code，走项目选择）
    bool hasFixedConfigTarget() const;

    ah::Platform& platform_;
    QVector<Row> rows_;
    int current_ = -1;
    QString issuedKey_;      // 本次签发的明文密钥（只留到对话框关闭）
    QString actionNotice_;   // 上一次操作的结果文案（与连接状态分行显示）

    QListWidget* list_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* desc_ = nullptr;
    QLabel* detect_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* provisionBtn_ = nullptr;
    QPlainTextEdit* snippet_ = nullptr;
    QPushButton* writeBtn_ = nullptr;
    QPushButton* writeProjectBtn_ = nullptr;  // Claude Code：选项目文件夹写入 .mcp.json
    QPushButton* copyBtn_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QPushButton* copyCmdBtn_ = nullptr;
    QLabel* cmdHint_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* timer_ = nullptr;
};
