#pragma once
// 应用内更新：从 GitHub Releases 抓最新版并安装。
//
// 为什么走 release 资产里的 latest.json 而不是 GitHub API：
//   * 固定 URL https://github.com/<repo>/releases/latest/download/latest.json 可直接下载，
//     不需要 token，也不受未认证 API「每小时 60 次」的限流；
//   * 清单里带每个产物的 SHA256，正是安装前要校验的东西；
//   * `releases/latest` 天然排除预发布版本，不会把 rc 推给正式版用户。
//
// 安全取向（刻意的取舍，不是没做完）：
//   * **默认不静默安装**。本程序持有用户全部协作数据、且当前产物未代码签名，
//     静默替换可执行文件的风险远大于收益；这里做的是"自动检查 + 一键升级"。
//   * 下载后**必须校验 SHA256** 才安装，且安装前剥掉下载文件的"网络来源标记"(MOTW)，
//     否则会二次弹 SmartScreen 警告。
//   * 便携版不走 MSI 安装（否则会装出第二份），只提示去下载页。
//   * 诚实说明：哈希来自同一分发渠道（HTTPS + GitHub），能防传输损坏与镜像篡改，
//     但不等于代码签名；签名后应改成校验签名。
//
// 网络韧性（GitHub 在国内经常超时、断流）：
//   * 清单拉取失败按退避重试若干次，不因一次抖动就报错；
//   * 产物下载失败按退避重试，且**断点续传**（Range）——只补没下完的差额；
//   * 直连失败后可切换到镜像前缀重试（用户自定义镜像 + 可选公共镜像）；
//   * 仅产物走镜像：清单与其中的 SHA256 始终直连 GitHub，可信锚点不经过第三方，
//     因此镜像最坏只能让下载失败，无法替换内容（哈希不匹配会被拒绝安装）。
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;

namespace ui {

struct UpdateInfo {
    QString version;      // 1.0.1
    QString tag;          // v1.0.1
    QString notesUrl;     // release 页
    QUrl msiUrl;
    QString msiSha256;
    qint64 msiSize = 0;
    QUrl portableUrl;
    QString portableSha256;
    bool valid() const { return !version.isEmpty() && msiUrl.isValid() && !msiSha256.isEmpty(); }
};

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);

    // ---- 偏好（存 QSettings，键名与设置页共用）----
    static bool autoCheckEnabled();
    static void setAutoCheckEnabled(bool on);
    static bool autoCheckDue(int intervalHours = 24);  // 距上次检查是否已超过间隔
    static void markChecked();
    static QString skippedVersion();
    static void skipVersion(const QString& version);
    // 跳过判定用数值等价而不是字符串相等：清单 version 与历史保存值可能一边带
    // v/V 前缀一边不带（旧版本保存的、或不同构建口径），裸比较会让"跳过此版本"
    // 失效、弹窗反复出现
    static bool isSkippedVersion(const QString& version);

    // 运行的是安装版（%LOCALAPPDATA%\MiderHive，兼容旧品牌 %LOCALAPPDATA%\AgentHive）还是便携版
    static bool isInstalledCopy();

    // ---- 网络韧性偏好（与设置页共用；GitHub 可达性差时的兜底）----
    static QString mirrorPrefix();                 // 用户自定义镜像前缀（空 = 只用直连）
    static void setMirrorPrefix(const QString& prefix);
    static bool autoMirrorEnabled();               // 直连失败后是否自动尝试公共镜像（默认开）
    static void setAutoMirrorEnabled(bool on);

    void check();                                       // 拉清单并比对版本
    void downloadAndInstall(const UpdateInfo& info);    // 下载 → 校验 → 安装 → 退出
    // 检查或下载是否正在进行（设置页据此禁用按钮；重入会被忽略并留痕）
    bool busy() const { return inFlight_; }

signals:
    void checking();
    void upToDate(const QString& currentVersion);
    void updateAvailable(const ui::UpdateInfo& info);
    void failed(const QString& reason);
    void downloadProgress(qint64 received, qint64 total);
    void verifying();
    void installing();   // 已启动安装程序，调用方应准备退出

private:
    void fetchManifest(int attempt);   // 清单拉取（带退避重试）
    void startAttempt();               // 单次下载尝试（带 Range 续传）
    void verifyAndInstall(const UpdateInfo& info, const QString& filePath);

    QNetworkAccessManager* net_ = nullptr;
    // 韧性下载状态：候选地址（直连 → 自定义镜像 → 公共镜像）、当前地址与尝试次数、已收字节
    UpdateInfo dlInfo_;
    QString dlPath_;
    QStringList dlUrls_;
    int dlUrlIdx_ = 0;
    int dlAttempt_ = 0;
    int dlTries_ = 0;
    qint64 dlGot_ = 0;
    // 单飞保护：同实例重复触发（双击按钮、设置页与弹窗并发）此前会并发写同一个
    // 按版本固定的临时 MSI（Qt 的文件共享模式允许第二个句柄），产出损坏文件并被
    // 误报"可能被篡改"。检查与下载全程置位，到任一终态信号复位。
    bool inFlight_ = false;
    // 下载写盘失败（磁盘满/配额/被锁）：网络层一切正常，必须在写盘处如实失败，
    // 否则截断的文件一路走到 SHA256 被误诊为"可能被篡改"
    bool dlWriteFailed_ = false;
};

}  // namespace ui
