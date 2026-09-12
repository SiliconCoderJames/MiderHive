#include "update_checker.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include "core/types.h"   // ah::kPlatformVersion
#include "core/util.h"
#include "core/version_util.h"
#include "i18n.h"

namespace {

// GUI 子系统是 WIN32 可执行文件，没有可用的 stderr：更新检查失败时若只靠弹窗，
// 用户报"没有任何提示"就无从查起，因此把过程写入数据目录下的日志文件。
void trace(const QString& msg) {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/MiderHive";
    QDir().mkpath(dir);
    QFile f(dir + "/update-check.log");
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << ' ' << msg << '\n';
}

}  // namespace

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace ui {

namespace {

constexpr const char* kRepoSlug = "SiliconCoderJames/MiderHive";
constexpr const char* kReleasesPage = "https://github.com/SiliconCoderJames/MiderHive/releases";

// 韧性参数：GitHub 直连在国内经常超时/断流，一次失败就报错太脆。
constexpr int kManifestAttempts = 3;   // 清单只有几百字节，多试几次几乎不花代价
constexpr int kAssetAttempts = 3;      // 每个候选地址的重试次数（配合 Range 只补差额）
constexpr int kManifestTimeoutMs = 15000;
constexpr int kAssetTimeoutMs = 25000;  // 资产下载放宽空闲超时：慢链路起步慢 ≠ 死链

// 退避：0.8s → 1.6s → 3.2s（瞬时故障多半几秒内自愈，也不必让用户干等）
int backoffMs(int attempt) { return 800 * (1 << qBound(0, attempt - 1, 4)); }

// 公共加速镜像：把原始 GitHub URL 当路径拼接（形如 https://ghfast.top/https://github.com/...）。
// 只用于**产物下载**；清单与其内的 SHA256 始终直连 GitHub，可信锚点不经过第三方——
// 因此镜像最坏只能让下载失败（DoS），无法替换内容（哈希不匹配会被拒绝安装）。
const QStringList& builtinMirrors() {
    static const QStringList k = {
        QStringLiteral("https://ghfast.top/"),
        QStringLiteral("https://gh-proxy.com/"),
        QStringLiteral("https://ghproxy.net/"),
    };
    return k;
}

QUrl applyMirror(const QString& prefix, const QUrl& original) {
    QString p = prefix.trimmed();
    if (p.isEmpty()) return original;
    if (!p.endsWith('/')) p += '/';
    return QUrl(p + original.toString());
}

// 清单地址：默认走 releases/latest 的固定资产 URL；可用环境变量覆盖（便于本地联调与 fork）
QUrl manifestUrl() {
    const std::string override =
        ah::envOr({"MIDERHIVE_UPDATE_URL", "AGENTHIVE_UPDATE_URL", "ZCODE_UPDATE_URL"});
    if (!override.empty()) return QUrl(QString::fromStdString(override));
    return QUrl(QString("https://github.com/%1/releases/latest/download/latest.json")
                    .arg(QString::fromLatin1(kRepoSlug)));
}

QNetworkRequest makeRequest(const QUrl& url, int timeoutMs) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString("MiderHive/%1").arg(QString::fromLatin1(ah::kPlatformVersion)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);  // release 资产会 302 到 CDN
    req.setTransferTimeout(timeoutMs);
    return req;
}

QString tempMsiPath(const QString& version) {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QString("MiderHive-%1-x64.msi").arg(version));
}

// 校验通过后剥掉"网络来源标记"：否则 msiexec 会因文件来自互联网再弹一次 SmartScreen
void stripMarkOfTheWeb(const QString& path) {
#ifdef _WIN32
    const QString stream = path + ":Zone.Identifier";
    DeleteFileW(reinterpret_cast<const wchar_t*>(stream.utf16()));
#else
    Q_UNUSED(path);
#endif
}

}  // namespace

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {}

bool UpdateChecker::autoCheckEnabled() {
    QSettings s;
    return s.value("ui/autoCheck", true).toBool();
}
void UpdateChecker::setAutoCheckEnabled(bool on) {
    QSettings s;
    s.setValue("ui/autoCheck", on);
}

bool UpdateChecker::autoCheckDue(int intervalHours) {
    QSettings s;
    const QString last = s.value("ui/lastCheckUtc").toString();
    if (last.isEmpty()) return true;
    const QDateTime t = QDateTime::fromString(last, Qt::ISODate);
    if (!t.isValid()) return true;
    return t.secsTo(QDateTime::currentDateTimeUtc()) >= qint64(intervalHours) * 3600;
}
void UpdateChecker::markChecked() {
    QSettings s;
    s.setValue("ui/lastCheckUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

QString UpdateChecker::skippedVersion() {
    QSettings s;
    return s.value("ui/skippedVersion").toString();
}
void UpdateChecker::skipVersion(const QString& version) {
    QSettings s;
    s.setValue("ui/skippedVersion", version);
}
bool UpdateChecker::isSkippedVersion(const QString& version) {
    const QString skipped = skippedVersion();
    if (skipped.isEmpty() || version.isEmpty()) return false;
    // 数值等价（v/V 前缀与缺位都归一），避免裸字符串比较两边口径漂移
    return ah::compareVersions(version.toStdString(), skipped.toStdString()) == 0;
}

QString UpdateChecker::mirrorPrefix() {
    QSettings s;
    return s.value("ui/updateMirror").toString().trimmed();
}
void UpdateChecker::setMirrorPrefix(const QString& prefix) {
    QSettings s;
    s.setValue("ui/updateMirror", prefix.trimmed());
}
bool UpdateChecker::autoMirrorEnabled() {
    QSettings s;
    return s.value("ui/updateAutoMirror", true).toBool();
}
void UpdateChecker::setAutoMirrorEnabled(bool on) {
    QSettings s;
    s.setValue("ui/updateAutoMirror", on);
}

bool UpdateChecker::isInstalledCopy() {
    // 安装版固定落在 %LOCALAPPDATA%\MiderHive（GenericDataLocation 在 Windows 上即 %LOCALAPPDATA%），
    // 其余位置（解压出来的便携版、开发时的 build 目录）都按便携版处理。
    // 兼容旧品牌安装目录：从 AgentHive 1.0.x 升级后首次运行前，exe 仍可能在旧目录里。
    const QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString localRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (localRoot.isEmpty()) return false;
    if (appDir.compare(QDir::cleanPath(localRoot + "/MiderHive"), Qt::CaseInsensitive) == 0)
        return true;
    return appDir.compare(QDir::cleanPath(localRoot + "/AgentHive"), Qt::CaseInsensitive) == 0;
}

void UpdateChecker::check() {
    emit checking();
    trace(QString("check() start, url=%1").arg(manifestUrl().toString()));
    fetchManifest(1);
}

// 清单拉取带退避重试：网络抖动不该直接变成"更新失败"
void UpdateChecker::fetchManifest(int attempt) {
    if (!net_) net_ = new QNetworkAccessManager(this);
    QNetworkReply* reply = net_->get(makeRequest(manifestUrl(), kManifestTimeoutMs));
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QString why = reply->errorString();
            trace(QString("check() attempt %1/%2 failed: %3")
                      .arg(attempt)
                      .arg(kManifestAttempts)
                      .arg(why));
            if (attempt < kManifestAttempts) {
                QTimer::singleShot(backoffMs(attempt), this,
                                   [this, attempt] { fetchManifest(attempt + 1); });
                return;
            }
            emit failed(i18n::trs("无法获取更新信息（连接 GitHub 失败，已重试 %1 次）：%2\n"
                                  "可稍后再试，或点「打开下载页」用浏览器查看。",
                                  "Could not reach GitHub for update info (retried %1x): %2\n"
                                  "Try again later, or use \"Open releases page\" in a browser.")
                            .arg(kManifestAttempts)
                            .arg(why));
            return;
        }
        const QByteArray raw = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            emit failed(i18n::trs("更新清单格式无法解析", "malformed update manifest"));
            return;
        }
        const QJsonObject o = doc.object();
        UpdateInfo info;
        info.version = o.value("version").toString();
        info.tag = o.value("tag").toString();
        info.notesUrl = o.value("notes_url").toString(kReleasesPage);
        const QJsonObject msi = o.value("msi").toObject();
        info.msiUrl = QUrl(msi.value("url").toString());
        info.msiSha256 = msi.value("sha256").toString().toLower();
        info.msiSize = static_cast<qint64>(msi.value("size").toDouble());
        const QJsonObject portable = o.value("portable").toObject();
        info.portableUrl = QUrl(portable.value("url").toString());
        info.portableSha256 = portable.value("sha256").toString().toLower();

        markChecked();
        const QString current = QString::fromLatin1(ah::kPlatformVersion);
        trace(QString("manifest bytes=%1 version='%2' msi='%3' sha=%4chars current='%5' valid=%6 newer=%7")
                  .arg(raw.size())
                  .arg(info.version, info.msiUrl.toString())
                  .arg(info.msiSha256.size())
                  .arg(current)
                  .arg(info.valid() ? 1 : 0)
                  .arg(ah::isNewerVersion(info.version.toStdString(), current.toStdString()) ? 1 : 0));
        if (!info.valid()) {
            emit failed(i18n::trs("更新清单缺少必要字段", "manifest is missing required fields"));
            return;
        }
        if (ah::isNewerVersion(info.version.toStdString(), current.toStdString()))
            emit updateAvailable(info);
        else
            emit upToDate(current);
    });
}

// GitHub 的 release 下载 URL 对 tag 大小写敏感（实测 v1.0.4 404、V1.0.4 206），
// 而清单里的 tag 与实际打点可能漂移（历史版本 v/V 混用）。把 "tag 段大小写翻转"
// 的变体作为候选地址，是零成本的兜底：主地址 404 时自动换写法再试。
static QString withTagCaseSwapped(const QString& url) {
    static const QRegularExpression re(
        "^(https?://[^/]+/[^/]+/[^/]+/releases/download/)([vV])([^/]*)(/.*)$");
    const QRegularExpressionMatch m = re.match(url);
    if (!m.hasMatch()) return url;
    const QString flipped = (m.captured(2) == QLatin1String("v")) ? QStringLiteral("V")
                                                                  : QStringLiteral("v");
    return m.captured(1) + flipped + m.captured(3) + m.captured(4);
}

void UpdateChecker::downloadAndInstall(const UpdateInfo& info) {
    if (!isInstalledCopy()) {
        // 便携版：装 MSI 会在系统里多出一份，交给用户自己选
        emit failed(i18n::trs("便携版请到下载页手动更新（避免装出第二份）",
                              "portable build: download manually to avoid a second copy"));
        return;
    }
    if (!info.msiUrl.isValid() || info.msiSha256.isEmpty()) {
        emit failed(i18n::trs("清单里没有可用的安装包信息", "manifest has no usable installer entry"));
        return;
    }
    // 候选地址顺序：直连 GitHub → 用户自定义镜像 → 公共镜像（仅在允许时）
    dlInfo_ = info;
    dlPath_ = tempMsiPath(info.version);
    dlUrls_.clear();
    dlUrls_ << info.msiUrl.toString();
    const QString caseSwapped = withTagCaseSwapped(info.msiUrl.toString());
    if (caseSwapped != info.msiUrl.toString())
        dlUrls_ << caseSwapped;  // tag 大小写漂移兜底（排在自定义镜像之前）
    const QString custom = mirrorPrefix();
    if (!custom.isEmpty()) dlUrls_ << applyMirror(custom, info.msiUrl).toString();
    if (autoMirrorEnabled())
        for (const QString& m : builtinMirrors()) dlUrls_ << applyMirror(m, info.msiUrl).toString();
    dlUrlIdx_ = 0;
    dlAttempt_ = 0;
    dlTries_ = 0;
    dlGot_ = 0;
    trace(QString("download start: v%1 candidates=%2 customMirror=%3 autoMirror=%4")
              .arg(info.version)
              .arg(dlUrls_.size())
              .arg(custom.isEmpty() ? "none" : custom)
              .arg(autoMirrorEnabled() ? 1 : 0));
    startAttempt();
}

// 单次下载尝试：已下部分用 Range 续传（服务器不支持 206 就自动从头来）。
// 半成品**不删**——断网后重试只补差额，13 MB 的包不必每次重下。
void UpdateChecker::startAttempt() {
    if (!net_) net_ = new QNetworkAccessManager(this);
    const QUrl url(dlUrls_.at(dlUrlIdx_));
    ++dlAttempt_;
    ++dlTries_;
    const qint64 total = dlInfo_.msiSize > 0 ? dlInfo_.msiSize : 0;
    qint64 have = QFileInfo(dlPath_).size();
    // 半成品已下满（上次多半是校验失败）：从头下，避免继续拼一个错文件
    if (total > 0 && have >= total) have = 0;
    QNetworkRequest req = makeRequest(url, kAssetTimeoutMs);
    if (have > 0)
        req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");
    trace(QString("download try %1: attempt %2/%3 url=%4 have=%5/%6")
              .arg(dlTries_)
              .arg(dlAttempt_)
              .arg(kAssetAttempts)
              .arg(url.toString())
              .arg(have)
              .arg(total));

    QNetworkReply* reply = net_->get(req);
    auto* out = new QFile(dlPath_, this);
    if (!out->open(have > 0 ? (QIODevice::WriteOnly | QIODevice::Append) : QIODevice::WriteOnly)) {
        out->deleteLater();
        reply->deleteLater();
        emit failed(i18n::trs("无法写入临时目录", "cannot write to the temp directory"));
        return;
    }
    dlGot_ = have;
    if (total > 0) emit downloadProgress(dlGot_, total);
    // 服务器忽略 Range 而返回 200 时，续传会拼出脏文件——发现即截断重写
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, out, have] {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (have > 0 && code == 200) {
            out->resize(0);
            out->seek(0);
            dlGot_ = 0;
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, out] {
        const QByteArray chunk = reply->readAll();
        out->write(chunk);
        dlGot_ += chunk.size();
        if (dlInfo_.msiSize > 0) emit downloadProgress(dlGot_, dlInfo_.msiSize);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, out] {
        out->write(reply->readAll());
        out->close();
        out->deleteLater();
        const QString usedUrl = dlUrls_.at(dlUrlIdx_);
        if (reply->error() == QNetworkReply::NoError) {
            reply->deleteLater();
            verifyAndInstall(dlInfo_, dlPath_);
            return;
        }
        const QString why = reply->errorString();
        reply->deleteLater();
        trace(QString("download try %1 failed (%2): %3").arg(dlTries_).arg(usedUrl).arg(why));
        if (dlAttempt_ < kAssetAttempts) {  // 同一地址再试（续传，只补差额）
            QTimer::singleShot(backoffMs(dlAttempt_), this, [this] { startAttempt(); });
            return;
        }
        dlAttempt_ = 0;
        ++dlUrlIdx_;
        if (dlUrlIdx_ < dlUrls_.size()) {  // 换下一个候选地址（镜像）
            trace(QString("download switching to url %1/%2: %3")
                      .arg(dlUrlIdx_ + 1)
                      .arg(dlUrls_.size())
                      .arg(dlUrls_.at(dlUrlIdx_)));
            QTimer::singleShot(300, this, [this] { startAttempt(); });
            return;
        }
        QFile::remove(dlPath_);  // 全部候选都失败：清掉半成品
        emit failed(i18n::trs("下载失败（已重试 %1 次、尝试 %2 个地址）：%3\n"
                              "可点「打开下载页」用浏览器下载，或在设置里填写镜像加速地址。",
                              "Download failed (retried %1x across %2 url(s)): %3\n"
                              "Use \"Open releases page\" to download in a browser, or set a "
                              "mirror prefix in Settings.")
                        .arg(dlTries_)
                        .arg(dlUrls_.size())
                        .arg(why));
    });
}

void UpdateChecker::verifyAndInstall(const UpdateInfo& info, const QString& filePath) {
    emit verifying();
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit failed(i18n::trs("下载的文件无法读取", "downloaded file cannot be read"));
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) {
        emit failed(i18n::trs("计算校验和失败", "failed to hash the download"));
        return;
    }
    f.close();
    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (actual.compare(info.msiSha256, Qt::CaseInsensitive) != 0) {
        QFile::remove(filePath);
        emit failed(i18n::trs("校验和不匹配，已删除下载文件（可能被篡改或传输损坏）",
                              "checksum mismatch - download deleted (tampered or corrupted)"));
        return;
    }
    stripMarkOfTheWeb(filePath);
    // /passive：显示进度但不打断用户；per-user 安装不需要管理员权限
    const bool started = QProcess::startDetached("msiexec",
                                                 {"/i", QDir::toNativeSeparators(filePath),
                                                  "/passive", "/norestart"});
    if (!started) {
        trace("verifyAndInstall: msiexec launch failed");
        emit failed(i18n::trs("无法启动安装程序", "failed to launch the installer"));
        return;
    }
    trace(QString("verifyAndInstall: hash ok, msiexec started for %1").arg(filePath));
    emit installing();
}

}  // namespace ui
