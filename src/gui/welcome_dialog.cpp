#include "welcome_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "core/platform.h"
#include "gui_util.h"
#include "i18n.h"
#include "theme.h"

WelcomeDialog::WelcomeDialog(ah::Platform& platform, QWidget* parent)
    : QDialog(parent), platform_(platform) {
    setWindowTitle(i18n::trs("欢迎来到 MiderHive", "Welcome to MiderHive"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    // 不锁死尺寸：字号调到 14px 或系统字体放大时，固定尺寸会把内容挤掉
    setMinimumSize(620, 540);
    resize(660, 640);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(8);

    // 品牌头：logo + 名称 + 定稿口号
    auto* logo = new QLabel(this);
    QPixmap pm(":/brand/logo.png");
    logo->setPixmap(pm.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    root->addWidget(logo);
    auto* name = new QLabel("MiderHive", this);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet(ui::th("font-size:24px; font-weight:800; color:@text@;"));
    root->addWidget(name);
    auto* slogan = new QLabel(this);
    slogan->setText(QString("%1  ·  %2")
                        .arg(i18n::trs("单体成长，蜂巢共享", "Grow alone, thrive together."),
                             i18n::trs("本地优先，数据不出本机", "local-first, data stays local")));
    slogan->setAlignment(Qt::AlignCenter);
    slogan->setStyleSheet(ui::th("font-size:12px; color:@brand@; font-weight:600;"));
    root->addWidget(slogan);
    root->addSpacing(6);

    // 三步接入卡片（图标用与侧栏同源的矢量图标，替代 emoji：跨平台字形一致）
    struct Step {
        const char* kind;
        const char* zh;
        const char* en;
        const char* zhDesc;
        const char* enDesc;
    };
    const Step steps[] = {
        {"overview", "蜂巢已就绪", "Hive is ready", "工作台内置本地服务，127.0.0.1 仅本机可达",
         "The workbench ships a local-only service on 127.0.0.1"},
        {"key", "接入你的 Agent", "Connect your agents",
         "下方一键生成 MCP 配置，粘贴进你的 AI 工具即可入巢",
         "Generate the MCP config below, paste it into your AI tool"},
        {"knowledge", "共享与成长", "Share and grow",
         "经验进知识库、技能进市场，所有 Agent 越用越顺",
         "Knowledge and skills are shared by every agent"},
    };
    for (const auto& s : steps) {
        auto* row = new QHBoxLayout();
        row->setSpacing(12);
        auto* icon = new QLabel(this);
        icon->setPixmap(ui::makeIcon(s.kind, ui::brand(), 20).pixmap(20, 20));
        icon->setStyleSheet("background:transparent;");
        icon->setFixedWidth(28);
        auto* text = new QLabel(this);
        text->setTextFormat(Qt::RichText);
        text->setText(ui::th(QString(
            "<b style='color:@text@; font-size:13px;'>%1</b><br>"
            "<span style='color:@muted@; font-size:11px;'>%2</span>")
                                 .arg(i18n::trs(s.zh, s.en))
                                 .arg(i18n::trs(s.zhDesc, s.enDesc))));
        text->setWordWrap(true);
        row->addWidget(icon, 0, Qt::AlignTop);
        row->addWidget(text, 1);
        root->addLayout(row);
    }
    root->addSpacing(6);

    // ---- 一键接入：选择 AI 编码工具（检测安装 → 预配身份 → 生成 MCP 配置）----
    auto* toolTitle = new QLabel(this);
    toolTitle->setText(i18n::trs("一键接入：选择你的 AI 编码工具",
                                 "One-click connect: pick your AI coding tool"));
    toolTitle->setStyleSheet(ui::th("font-size:12px; color:@muted@; font-weight:600;"));
    root->addWidget(toolTitle);

    auto* toolRow = new QHBoxLayout();
    toolRow->setSpacing(8);
    for (const auto& t : ui::integrations::tools()) {
        auto* b = new QPushButton(t.nameEn, this);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(30);
        b->setProperty("toolId", t.id);
        const QString id = t.id;
        connect(b, &QPushButton::clicked, this, [this, id] {
            if (const auto* t = ui::integrations::toolById(id)) pickTool(*t);
        });
        toolBtns_.append(b);
        toolRow->addWidget(b, 1);
    }
    root->addLayout(toolRow);

    // 检测结果 + 结果面板（选中工具后出现）
    detectLabel_ = new QLabel(this);
    detectLabel_->setStyleSheet(ui::th("font-size:11px; color:@muted@;"));
    detectLabel_->setWordWrap(true);
    root->addWidget(detectLabel_);

    resultBox_ = new QFrame(this);
    resultBox_->setVisible(false);
    resultBox_->setStyleSheet(ui::th(
        "QFrame { background:@field@; border:1px solid @line@; border-radius:8px; }"));
    auto* rl = new QVBoxLayout(resultBox_);
    rl->setContentsMargins(12, 10, 12, 10);
    rl->setSpacing(6);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel(i18n::trs("接入身份名", "Agent name"), resultBox_));
    nameEdit_ = new QLineEdit(resultBox_);
    nameEdit_->setStyleSheet(ui::th("QLineEdit { background:@card@; color:@text@; }"));
    nameEdit_->setToolTip(i18n::trs("该名字会成为 Agent 在蜂巢里的身份（限字母数字-_）",
                                    "This becomes the agent's identity in the hive"));
    nameRow->addWidget(nameEdit_, 1);
    rl->addLayout(nameRow);

    configView_ = new QPlainTextEdit(resultBox_);
    configView_->setReadOnly(true);
    configView_->setFont(QApplication::font("QPlainTextEdit"));
    configView_->setStyleSheet(ui::th(
        "QPlainTextEdit { font-family:@mono@,monospace; font-size:11px; color:@text@; }"));
    configView_->setFixedHeight(132);
    rl->addWidget(configView_);

    auto* cfgRow = new QHBoxLayout();
    copyBtn_ = new QPushButton(i18n::trs("复制配置", "Copy config"), resultBox_);
    copyBtn_->setObjectName("primary");
    copyBtn_->setCursor(Qt::PointingHandCursor);
    connect(copyBtn_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(configView_->toPlainText());
        ui::Toast::show(this, i18n::trs("已复制，去粘贴并重启该工具", "Copied — paste it and "
                                                                     "restart the tool"));
    });
    cfgRow->addWidget(copyBtn_);
    cfgRow->addStretch(1);
    rl->addLayout(cfgRow);

    pathHint_ = new QLabel(resultBox_);
    pathHint_->setTextFormat(Qt::PlainText);
    pathHint_->setWordWrap(true);
    pathHint_->setStyleSheet(ui::th("font-size:11px; color:@muted@;"));
    rl->addWidget(pathHint_);
    stepsLabel_ = new QLabel(resultBox_);
    stepsLabel_->setWordWrap(true);
    stepsLabel_->setStyleSheet(ui::th("font-size:11px; color:@muted@;"));
    rl->addWidget(stepsLabel_);
    root->addWidget(resultBox_);
    root->addSpacing(4);

    // 注册命令（不含密钥本体，路径指向本机主密钥文件）：给愿意手动的用户兜底
    auto* cmdTitle = new QLabel(this);
    cmdTitle->setText(i18n::trs("手动接入命令（可选）", "Manual connect command (optional)"));
    cmdTitle->setStyleSheet(ui::th("font-size:11px; color:@muted@;"));
    root->addWidget(cmdTitle);
    auto* cmdRow = new QHBoxLayout();
    auto* cmd = new QLabel(this);
    const QString cmdText =
        QString("agent-cli register --name myagent --master-key "
                "(Get-Content \"%1/config/master.key\")")
            .arg(QString::fromStdString(platform.homeDir()));
    cmd->setText(ui::th(QString("<span style='color:@accenthi@; font-family:@mono@,monospace; "
                                "font-size:11px;'>%1</span>")
                            .arg(cmdText.toHtmlEscaped())));
    cmd->setWordWrap(true);
    cmdRow->addWidget(cmd, 1);
    auto* copyBtn = new QToolButton(this);
    copyBtn->setText(i18n::trs("复制", "Copy"));
    copyBtn->setCursor(Qt::PointingHandCursor);
    connect(copyBtn, &QToolButton::clicked, this, [cmdText, this] {
        QApplication::clipboard()->setText(cmdText);
        ui::Toast::show(this, i18n::trs("已复制", "Copied"));
    });
    cmdRow->addWidget(copyBtn);
    root->addLayout(cmdRow);
    root->addSpacing(4);

    // 主题色卡：第一印象自己选
    auto* themeTitle = new QLabel(this);
    themeTitle->setText(i18n::trs("选一套配色（随时在设置里换）",
                                  "Pick a theme (change anytime in Settings)"));
    themeTitle->setStyleSheet(ui::th("font-size:11px; color:@muted@;"));
    root->addWidget(themeTitle);
    auto* grid = new QGridLayout();
    grid->setSpacing(8);
    for (int i = 0; i < static_cast<int>(ui::themes().size()); ++i) {
        const auto& t = ui::themes()[static_cast<std::size_t>(i)];
        auto* sw = new ui::ThemeSwatch(i18n::trs(t.zh, t.en), t.bg, t.deep, t.accent, t.brand,
                                       t.text, [i] { ui::setThemeIndex(i); }, this);
        sw->setFixedSize(92, 68);
        swatches_.push_back(sw);
        grid->addWidget(sw, i / 5, i % 5);
    }
    root->addLayout(grid);
    root->addStretch(1);

    auto* row = new QHBoxLayout();
    row->addStretch(1);
    auto* go = new QPushButton(i18n::trs("开始使用 →", "Get started →"), this);
    go->setObjectName("primary");
    go->setFixedHeight(34);
    go->setFixedWidth(140);
    connect(go, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(go);
    root->addLayout(row);

    connect(this, &QDialog::finished, this, [](int) {
        QSettings s;
        s.setValue("ui/welcomeSeen", true);
    });
}

void WelcomeDialog::pickTool(const ui::integrations::Tool& tool) {
    current_ = tool;
    for (auto* b : toolBtns_) b->setChecked(b->property("toolId") == tool.id);

    // 1) 本地检测：PATH 或常见安装路径（检测不到不影响配置——先装后配同样可行）
    const bool found = ui::integrations::installed(tool.id);
    detectLabel_->setText(found ? i18n::trs("✓ 已检测到本机安装", "✓ detected on this machine")
                                : tool.installZh + "\n" + tool.installEn);
    detectLabel_->setStyleSheet(ui::th(found ? "font-size:11px; color:@ok@;"
                                             : "font-size:11px; color:@warn@;"));

    // 2) 预配接入身份（名字可改；重名/非法名直接给出可读解释）
    const QString agentName = nameEdit_->text().trimmed();
    if (agentName.isEmpty()) {
        QMessageBox::warning(this, i18n::trs("接入失败", "Connect failed"),
                             i18n::trs("请先填写接入身份名（例如 claude-code）。",
                                       "Enter an agent name first (e.g. claude-code)."));
        return;
    }
    std::string err, apiKey;
    if (!platform_.agentProvision("zcode", agentName.toStdString(), apiKey, err)) {
        QMessageBox::warning(this, i18n::trs("接入失败", "Connect failed"),
                             ui::humanError(QString::fromStdString(err)));
        return;
    }

    // 3) 生成该工具的 MCP 配置片段并展示 + 复制
    const QString exe = QCoreApplication::applicationDirPath() + "/miderhive-mcp.exe";
    configView_->setPlainText(ui::integrations::generateConfig(
        tool.id, exe, agentName, QString::fromStdString(apiKey)));
    pathHint_->setText(i18n::trs("配置文件位置：%1",
                                 "Config file location: %1")
                           .arg(i18n::trs(tool.configPathZh, tool.configPathEn)));
    stepsLabel_->setText(i18n::trs(
        "下一步：① 粘贴到上面的配置文件（没有就新建）② 完全退出并重启该工具。"
        "Agent 上线后工作台会提示「接入成功」，并自动写入一条欢迎记忆。",
        "Next: 1) paste into the config file above (create it if missing) 2) fully quit and "
        "restart the tool. When the agent comes online, the workbench shows \"Connected\" and "
        "writes a welcome memory entry."));
    resultBox_->setVisible(true);

    // 4) 登记待观察身份：MainWindow 轮询发现其上线 → 弹「接入成功」+ 写欢迎记忆（仅一次）
    QSettings s;
    s.setValue("ui/onboardingPending/" + agentName, tool.id);
}
