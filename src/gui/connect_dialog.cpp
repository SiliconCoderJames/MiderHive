#include "connect_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <string>
#include <vector>

#include "core/types.h"
#include "gui_util.h"
#include "i18n.h"
#include "theme.h"
#include "widgets.h"

namespace {

// 已上线身份集合（status 由平台在心跳时置 online；120s 无心跳判离线）
QStringList onlineAgents(ah::Platform& platform) {
    std::string err;
    std::vector<ah::AgentInfo> agents;
    QStringList online;
    if (!platform.listAgents(agents, err)) return online;
    for (const auto& a : agents)
        if (a.status == "online") online << QString::fromStdString(a.name);
    return online;
}

}  // namespace

ConnectDialog::ConnectDialog(ah::Platform& platform, QWidget* parent)
    : QDialog(parent), platform_(platform) {
    setWindowTitle(i18n::trs("接入 Agent", "Connect an agent"));
    setModal(true);
    resize(880, 620);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(22, 20, 22, 18);
    lay->setSpacing(12);

    auto* intro = new QLabel(
        i18n::trs("选一个工具 → 点「一键接入」→ 重启那个工具。凭据由工作台自动签发，"
                  "配置文件能写就替你写好（原文件备份为 .miderhive.bak）。"
                  "该 Agent 上线后，右边会亮起「已连接」。",
                  "Pick a tool → click Connect → restart that tool. Credentials are issued for "
                  "you, and the config file is filled in automatically when that is safe (the "
                  "original is backed up as .miderhive.bak). The right side turns “Connected” "
                  "once that agent comes online."),
        this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto* body = new QHBoxLayout();
    body->setSpacing(14);

    list_ = new QListWidget(this);
    list_->setFixedWidth(210);
    for (const auto& t : ui::integrations::tools()) {
        Row row;
        row.tool = t;
        row.detected = ui::integrations::installed(t.id);
        auto* item = new QListWidgetItem(i18n::trs(t.nameZh, t.nameEn));
        if (!row.detected) item->setForeground(ui::muted());
        list_->addItem(item);
        rows_.push_back(row);
    }
    list_->setCurrentRow(0);
    body->addWidget(list_);

    auto* right = new QVBoxLayout();
    right->setSpacing(8);

    title_ = new QLabel(this);
    title_->setStyleSheet(ui::th("font-size:15px; font-weight:700; color:@text@;"));
    right->addWidget(title_);

    desc_ = new QLabel(this);
    desc_->setWordWrap(true);
    desc_->setStyleSheet(ui::th("color:@muted@;"));
    right->addWidget(desc_);

    detect_ = new QLabel(this);
    detect_->setWordWrap(true);
    detect_->setStyleSheet(ui::th("color:@muted@; font-size:11px;"));
    right->addWidget(detect_);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel(i18n::trs("接入身份名", "Agent name"), this));
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setFixedWidth(200);
    nameEdit_->setToolTip(i18n::trs("该名字就是它在蜂巢里的身份，可随意改。",
                                    "This is the agent's identity in the hive; change it freely."));
    nameRow->addWidget(nameEdit_);

    provisionBtn_ = new QPushButton(i18n::trs("一键接入", "Connect"), this);
    provisionBtn_->setObjectName("primary");
    provisionBtn_->setCursor(Qt::PointingHandCursor);
    nameRow->addWidget(provisionBtn_);

    auto* recheckBtn = new QPushButton(i18n::trs("重新检测", "Re-check"), this);
    nameRow->addWidget(recheckBtn);
    nameRow->addStretch(1);
    right->addLayout(nameRow);

    cmdHint_ = new QLabel(this);
    cmdHint_->setWordWrap(true);
    cmdHint_->setTextFormat(Qt::PlainText);
    cmdHint_->setStyleSheet(ui::th("font-family:@mono@,monospace; font-size:11px; color:@accenthi@;"));
    cmdHint_->setVisible(false);
    right->addWidget(cmdHint_);

    snippet_ = new QPlainTextEdit(this);
    snippet_->setReadOnly(true);
    snippet_->setFont(QApplication::font("QPlainTextEdit"));
    snippet_->setStyleSheet(
        ui::th("QPlainTextEdit { font-family:@mono@,monospace; font-size:11px; color:@text@; }"));
    snippet_->setPlaceholderText(
        i18n::trs("点「一键接入」后，这里出现该工具的接入配置……",
                  "Click Connect and this tool's config appears here…"));
    right->addWidget(snippet_, 1);

    auto* btnRow = new QHBoxLayout();
    writeBtn_ = new QPushButton(i18n::trs("写入配置文件", "Write to config file"), this);
    writeBtn_->setObjectName("primary");
    // Claude Code 没有固定配置文件可写：改为让用户挑一次项目根，向导把 JSON 写进那里的
    // .mcp.json（Claude Code 的官方项目作用域落点）。这是新手最容易卡住的"手工建点文件"步骤。
    writeProjectBtn_ =
        new QPushButton(i18n::trs("写入项目 .mcp.json…", "Write project .mcp.json…"), this);
    writeProjectBtn_->setObjectName("primary");
    writeProjectBtn_->setToolTip(
        i18n::trs("选择你的项目文件夹，向导会把 miderhive 条目合并进那里的 .mcp.json"
                  "（已有内容会保留并先备份）。之后在该目录运行一次 claude 批准即可。",
                  "Pick your project folder; the wizard merges the miderhive entry into the "
                  ".mcp.json there (existing content is kept and backed up first). Then run "
                  "`claude` once in that folder to approve it."));
    copyBtn_ = new QPushButton(i18n::trs("复制配置", "Copy config"), this);
    openBtn_ = new QPushButton(i18n::trs("打开所在文件夹", "Open folder"), this);
    copyCmdBtn_ = new QPushButton(i18n::trs("复制命令", "Copy command"), this);
    for (auto* b : {writeBtn_, writeProjectBtn_, copyBtn_, openBtn_, copyCmdBtn_}) {
        b->setCursor(Qt::PointingHandCursor);
        b->setEnabled(false);
        btnRow->addWidget(b);
    }
    btnRow->addStretch(1);
    right->addLayout(btnRow);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    right->addWidget(status_);

    body->addLayout(right, 1);
    lay->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { selectRow(row); });
    connect(recheckBtn, &QPushButton::clicked, this, [this] { recheck(); });
    connect(provisionBtn_, &QPushButton::clicked, this, [this] { provision(); });
    connect(writeBtn_, &QPushButton::clicked, this, [this] { writeToConfig(); });
    connect(writeProjectBtn_, &QPushButton::clicked, this, [this] { writeProjectConfig(); });
    connect(copyBtn_, &QPushButton::clicked, this, [this] { copySnippet(); });
    connect(copyCmdBtn_, &QPushButton::clicked, this, [this] { copyCommand(); });
    connect(openBtn_, &QPushButton::clicked, this, [this] { openConfigFolder(); });

    // 上线观察：每 2s 问一次平台，让"接没接上"当场可见（无需手动刷新）
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });
    timer_->start(2000);

    selectRow(0);
    tick();
}

QString ConnectDialog::mcpExePath() const {
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + "/miderhive-mcp.exe";
#else
    return QCoreApplication::applicationDirPath() + "/miderhive-mcp";
#endif
}

void ConnectDialog::selectRow(int index) {
    if (index < 0 || index >= rows_.size()) return;
    current_ = index;
    const Row& row = rows_[index];
    const ui::integrations::Tool& t = row.tool;

    title_->setText(i18n::trs(t.nameZh, t.nameEn));
    snippet_->clear();
    issuedKey_.clear();
    actionNotice_.clear();
    writeBtn_->setEnabled(false);
    writeProjectBtn_->setEnabled(false);
    copyBtn_->setEnabled(false);
    copyCmdBtn_->setEnabled(false);
    cmdHint_->setVisible(false);
    cmdHint_->clear();

    // 主写入按钮二选一：有固定配置文件 → "写入配置文件"；
    // 没有固定落点（Claude Code）→ "写入项目 .mcp.json…"（用户挑一次项目文件夹）
    const bool fixedTarget = hasFixedConfigTarget();
    writeBtn_->setVisible(fixedTarget);
    writeProjectBtn_->setVisible(!fixedTarget);

    // 说明 = 该工具特有的注意点 + 配置落点
    QString desc = i18n::trs(t.noteZh, t.noteEn);
    const QString path = ui::integrations::configPath(t.id);
    if (!path.isEmpty()) {
        desc += "\n" + i18n::trs("配置文件：", "Config file: ") + path;
    } else if (!fixedTarget) {
        desc += "\n" + i18n::trs("配置文件：<你的项目根>/.mcp.json（点下方按钮选择文件夹）",
                                 "Config file: <your project root>/.mcp.json (pick the folder "
                                 "with the button below)");
    }
    desc_->setText(desc);

    nameEdit_->setText(t.defaultAgentName);

    // 安装检测：检测不到不影响接入（先配后装同样可行），所以只提示不拦截
    const bool found = ui::integrations::installed(t.id);
    if (found) {
        detect_->setText(i18n::trs("✓ 已检测到本机安装", "✓ detected on this machine"));
        detect_->setStyleSheet(ui::th("color:@ok@; font-size:11px;"));
    } else {
        detect_->setText(i18n::trs(t.installZh, t.installEn));
        detect_->setStyleSheet(ui::th("color:@warn@; font-size:11px;"));
    }
    // "打开所在文件夹"在没有配置文件的工具上无意义
    openBtn_->setVisible(!ui::integrations::configDir(t.id).isEmpty());
    tick();
}

void ConnectDialog::recheck() {
    for (int i = 0; i < rows_.size(); ++i) {
        rows_[i].detected = ui::integrations::installed(rows_[i].tool.id);
        QListWidgetItem* item = list_->item(i);
        if (!item) continue;
        item->setForeground(rows_[i].detected ? ui::text() : ui::muted());
    }
    selectRow(current_);
    ui::Toast::show(this, i18n::trs("已重新检测本机安装 ✓", "Re-checked local installs ✓"));
}

void ConnectDialog::provision() {
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        ui::Toast::show(this, i18n::trs("请先填写接入身份名", "Enter an agent name first"), false);
        return;
    }
    if (current_ < 0) return;
    const ui::integrations::Tool& t = rows_[current_].tool;

    // 1) 签发身份（幂等：不存在则注册，已存在则轮换）。明文只在这一次可读。
    std::string key, err;
    if (!platform_.agentProvision(ah::kManagerName, name.toStdString(), key, err)) {
        status_->setText(i18n::trs("接入失败：", "Connect failed: ") + ui::humanError(QString::fromStdString(err)));
        status_->setStyleSheet(ui::th("color:@danger@;"));
        return;
    }
    issuedKey_ = QString::fromStdString(key);

    // 2) 生成该工具真正能用的片段（JSON / TOML / YAML / 环境变量 / 指令块）
    const QString exe = mcpExePath();
    const QString snippet = ui::integrations::generateConfig(t.id, exe, name, issuedKey_);
    snippet_->setPlainText(snippet);
    QApplication::clipboard()->setText(snippet);

    // 3) 有手工命令的工具（Claude Code / Droid）额外给出命令——比改文件更稳
    const QString cmd = ui::integrations::cliCommand(t.id, exe, name, issuedKey_);
    if (!cmd.isEmpty()) {
        cmdHint_->setText(cmd);
        cmdHint_->setVisible(true);
        cmdHint_->setToolTip(i18n::trs(
            "请把命令粘进 cmd.exe 运行。PowerShell 调用 npm 版 claude 时会吞掉 -- 之后的命令，"
            "报 missing required argument 'commandOrUrl'（已实测）。",
            "Paste this into cmd.exe. In PowerShell the npm `claude` shim swallows everything "
            "after `--` and reports \"missing required argument 'commandOrUrl'\" (verified)."));
        copyCmdBtn_->setEnabled(true);
    }

    const bool hasFile = !ui::integrations::configPath(t.id).isEmpty();
    writeBtn_->setEnabled(hasFile);
    // Claude Code：没有固定落点，等用户挑项目文件夹后写入 .mcp.json
    writeProjectBtn_->setEnabled(!hasFile && issuedKey_ != QString());
    copyBtn_->setEnabled(true);
    openBtn_->setVisible(!ui::integrations::configDir(t.id).isEmpty());

    // 4) 缺少 MCP 可执行文件时如实说明（从源码树跑起来时很常见）
    if (hasFile && !QFileInfo::exists(exe)) {
        actionNotice_ =
            i18n::trs("注意：没找到 %1，请确认使用的是安装版（配置文件里的路径要能被该工具找到）。",
                      "Note: %1 was not found — use the installed build, so the path in the config "
                      "is reachable by that tool.")
                .arg(exe);
    } else {
        actionNotice_ = i18n::trs("凭据已签发，接入配置已复制到剪贴板。", "Credentials issued; the "
                                                                        "config is on your "
                                                                        "clipboard.");
    }

    // 5) 登记待观察身份：该 Agent 一上线，主窗口就弹「接入成功」并写一条欢迎记忆
    QSettings s;
    s.setValue("ui/onboardingPending/" + name, t.id);

    ui::Toast::show(this, i18n::trs("一键接入完成 ✓", "Connected ✓"));
    tick();
}

void ConnectDialog::writeToConfig() {
    if (current_ < 0 || issuedKey_.isEmpty()) return;
    const ui::integrations::Tool& t = rows_[current_].tool;
    const QString name = nameEdit_->text().trimmed();
    const ui::integrations::ApplyResult r = ui::integrations::applyConfig(
        t.id, mcpExePath(), name, issuedKey_);
    actionNotice_ = i18n::trs(r.detailZh, r.detailEn);
    if (r.ok) actionNotice_ += "\n" + i18n::trs("下一步：完全退出并重启该工具。",
                                                "Next: fully quit and restart that tool.");
    ui::Toast::show(this, r.ok ? i18n::trs("已写入配置文件 ✓", "Config written ✓")
                               : i18n::trs("未能自动写入", "Could not write automatically"),
                    r.ok);
    tick();
}

bool ConnectDialog::hasFixedConfigTarget() const {
    if (current_ < 0 || current_ >= rows_.size()) return false;
    const QString id = rows_[current_].tool.id;
    // 有固定配置文件 = 能解析出落点（Cursor/Codex/Droid/DSH/Hermes/Claude Desktop）。
    // Claude Code 落在项目根的 .mcp.json，由用户挑一次文件夹，不属于"固定落点"。
    return !ui::integrations::configPath(id).isEmpty();
}

void ConnectDialog::writeProjectConfig() {
    if (current_ < 0 || issuedKey_.isEmpty()) return;

    // 上次选过的项目目录作为默认位置：接第二个项目时不用重新找
    QSettings s;
    const QString lastDir = s.value("ui/lastProjectDir").toString();
    const QString dir = QFileDialog::getExistingDirectory(
        this, i18n::trs("选择要接入的项目（.mcp.json 会写在这里）",
                        "Pick the project to connect (.mcp.json will be written there)"),
        lastDir.isEmpty() ? QDir::homePath() : lastDir);
    if (dir.isEmpty()) return;  // 用户取消：不算失败，也不留任何提示噪音
    s.setValue("ui/lastProjectDir", dir);

    const QString name = nameEdit_->text().trimmed();
    const ui::integrations::ApplyResult r =
        ui::integrations::applyProjectConfig(dir, mcpExePath(), name, issuedKey_);
    actionNotice_ = i18n::trs(r.detailZh, r.detailEn);
    if (r.ok) {
        actionNotice_ += "\n" +
                         i18n::trs("下一步：在该项目目录运行一次 claude 并批准 miderhive。",
                                   "Next: run `claude` once in that project folder and approve "
                                   "miderhive.");
    }
    ui::Toast::show(this, r.ok ? i18n::trs("已写入 .mcp.json ✓", ".mcp.json written ✓")
                               : i18n::trs("未能自动写入", "Could not write automatically"),
                    r.ok);
    tick();
}

void ConnectDialog::copySnippet() {
    QApplication::clipboard()->setText(snippet_->toPlainText());
    ui::Toast::show(this, i18n::trs("已复制到剪贴板 ✓", "Copied to clipboard ✓"));
}

void ConnectDialog::copyCommand() {
    QApplication::clipboard()->setText(cmdHint_->text());
    ui::Toast::show(this, i18n::trs("命令已复制，粘进终端执行即可 ✓",
                                    "Command copied — paste it into a terminal ✓"));
}

void ConnectDialog::openConfigFolder() {
    if (current_ < 0) return;
    const QString dir = ui::integrations::configDir(rows_[current_].tool.id);
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ConnectDialog::tick() {
    if (current_ < 0) return;
    const QStringList online = onlineAgents(platform_);

    // 左侧列表：已上线的身份打勾（哪个工具接上了，一眼可见）
    for (int i = 0; i < rows_.size() && i < list_->count(); ++i) {
        QListWidgetItem* item = list_->item(i);
        if (!item) continue;
        const QString base = i18n::trs(rows_[i].tool.nameZh, rows_[i].tool.nameEn);
        // 该工具对应的身份名：当前选中行取输入框，其余取默认名
        const QString who = (i == current_) ? nameEdit_->text().trimmed()
                                            : rows_[i].tool.defaultAgentName;
        item->setText(online.contains(who) ? "● " + base : base);
    }

    const QString name = nameEdit_->text().trimmed();
    QString line;
    if (!name.isEmpty() && online.contains(name)) {
        line = i18n::trs("● 已连接：%1 正在蜂巢中在线工作。", "● Connected: %1 is online in the hive.")
                   .arg(name);
    } else if (!issuedKey_.isEmpty()) {
        line = i18n::trs("○ 等待上线：重启该工具后，这里会自动变成「已连接」。",
                         "○ Waiting: restart the tool and this turns into “Connected” "
                         "automatically.");
    } else {
        line = i18n::trs("○ 尚未接入：点「一键接入」签发凭据。",
                         "○ Not connected yet: click Connect to issue credentials.");
    }
    QString html = ui::th(QString("<span style='color:%1;'>%2</span>")
                              .arg(issuedKey_.isEmpty() ? ui::muted().name() : ui::accent().name())
                              .arg(line.toHtmlEscaped()));
    if (!actionNotice_.isEmpty())
        html += "<br>" + ui::th(QString("<span style='color:@muted@; font-size:11px;'>%1</span>")
                                    .arg(actionNotice_.toHtmlEscaped()));
    status_->setText(html);
}
