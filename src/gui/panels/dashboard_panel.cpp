#include "dashboard_panel.h"

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

#include "../gui_util.h"
#include "../i18n.h"

DashboardPanel::DashboardPanel(ah::Platform& platform, QWidget* parent)
    : PanelBase(platform, parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);
    buildHeader(root, "overview", "总览", "Overview", "预算消耗 · Agent 状态 · 事件流与告警，一屏掌握蜂巢动态",
                "Budget, agent status, event stream and alerts at a glance");

    // ---- 健康横幅（防呆）：紧跟页头，出问题时用户第一眼就能看到修复入口 ----
    healthBox_ = new QWidget(this);
    healthBox_->setStyleSheet("background:transparent;");
    healthLay_ = new QVBoxLayout(healthBox_);
    healthLay_->setContentsMargins(0, 0, 0, 0);
    healthLay_->setSpacing(6);
    healthBox_->setVisible(false);
    root->addWidget(healthBox_);

    // ---- 第一行：KPI 磁贴（趋势小图 + 环比胶囊，先给结论再看图表）----
    auto* kpiRow = new QHBoxLayout();
    kpiRow->setSpacing(14);
    kpiTokens_ = new ui::MetricTile("overview", this);
    kpiAgents_ = new ui::MetricTile("plug", this);
    kpiToday_ = new ui::MetricTile("audit", this);
    kpiErrors_ = new ui::MetricTile("errors", this);
    // 首屏占位：骨架屏覆盖期间也不至于出现空白数字
    kpiTokens_->set(i18n::trs("本周 Token", "Weekly tokens"), "--",
                    i18n::trs("正在取数…", "loading…"));
    kpiAgents_->set(i18n::trs("Agent", "Agents"), "--", i18n::trs("正在取数…", "loading…"));
    kpiToday_->set(i18n::trs("今日消耗", "Today's tokens"), "--", i18n::trs("正在取数…", "loading…"));
    kpiErrors_->set(i18n::trs("未解决错误", "Unresolved errors"), "--",
                    i18n::trs("正在取数…", "loading…"));
    kpiRow->addWidget(kpiTokens_, 1);
    kpiRow->addWidget(kpiAgents_, 1);
    kpiRow->addWidget(kpiToday_, 1);
    kpiRow->addWidget(kpiErrors_, 1);
    root->addLayout(kpiRow);

    // ---- 第二行：紧凑预算卡 + 各 Agent 用量 + 逐日趋势（栅格重分配：环形图不再独占大块）----
    auto* mainRow = new QHBoxLayout();
    mainRow->setSpacing(14);

    budgetCard_ = new QGroupBox(i18n::trs("本周 Token 预算", "Weekly Token Budget"), this);
    budgetCard_->setObjectName("card");
    auto* bl = new QVBoxLayout(budgetCard_);
    bl->setContentsMargins(10, 16, 10, 8);
    bl->setSpacing(8);
    auto* budgetRow = new QHBoxLayout;
    budgetRow->setSpacing(12);
    ring_ = new ui::RingProgress(budgetCard_);
    ring_->setMinimumSize(112, 112);
    ring_->setMaximumWidth(132);
    budgetRow->addWidget(ring_, 0, Qt::AlignTop);
    auto* statsCol = new QVBoxLayout;
    statsCol->setSpacing(6);
    budgetStats_ = new QLabel(budgetCard_);
    budgetStats_->setTextFormat(Qt::RichText);
    budgetStats_->setStyleSheet(ui::th("font-family:'@mono@'; font-size:12px; color:@muted@;"));
    // 不换行：三行都是短数字，开 wordWrap 只会让 minimumSizeHint 虚高，
    // 把预算卡（进而整行）的最小高度撑大，底行被顶出视口（布局审计实测）
    budgetStats_->setWordWrap(false);
    statsCol->addWidget(budgetStats_);
    budgetSpark_ = new ui::Sparkline(budgetCard_);
    budgetSpark_->setToolTip(i18n::trs("最近 14 天逐日消耗", "daily tokens, last 14 days"));
    statsCol->addWidget(budgetSpark_);
    statsCol->addStretch(1);
    budgetRow->addLayout(statsCol, 1);
    bl->addLayout(budgetRow);
    editBudgetBtn_ = new QPushButton(i18n::trs("调整预算", "Adjust budget"), budgetCard_);
    editBudgetBtn_->setObjectName("editBudget");  // UI 自动化按稳定 id 定位（文案随语言变）
    editBudgetBtn_->setCursor(Qt::PointingHandCursor);
    editBudgetBtn_->setStyleSheet(ui::th("padding:5px 14px;"));
    connect(editBudgetBtn_, &QPushButton::clicked, this, [this] {
        std::string berr;
        const qint64 cur = platform_.usageBudget(berr);
        ui::BudgetEditDialog dlg(cur > 0 ? cur : 10'000'000, this);
        if (dlg.exec() != QDialog::Accepted) return;
        std::string err;
        if (platform_.usageSetBudget(ah::kManagerName, dlg.value(), err))
            refresh();  // 环形图与 KPI 立即反映新预算
        else
            QMessageBox::warning(this, i18n::trs("调整失败", "Adjustment failed"),
                                 QString::fromStdString(err));
    });
    bl->addWidget(editBudgetBtn_, 0, Qt::AlignHCenter);
    mainRow->addWidget(budgetCard_, 2);

    usageCard_ = new QGroupBox(i18n::trs("各 Agent 本周用量", "Per-Agent Usage This Week"), this);
    usageCard_->setObjectName("card");
    auto* ul = new QVBoxLayout(usageCard_);
    ul->setContentsMargins(8, 16, 8, 6);
    usageChart_ = new ui::HBarChart(usageCard_);
    // 点击某一行 → 用量页按该 Agent 过滤（图表下钻）
    usageChart_->setOnEntryClick([this](int idx) {
        if (idx >= 0 && idx < lastAgentBars_.size())
            drillTo("usage", "agent", lastAgentBars_[idx].first);
    });
    ul->addWidget(usageChart_, 1);
    mainRow->addWidget(usageCard_, 4);

    trendCard_ = new QGroupBox(i18n::trs("最近 14 天逐日消耗", "Daily Tokens (14 days)"), this);
    trendCard_->setObjectName("card");
    auto* trl = new QVBoxLayout(trendCard_);
    trl->setContentsMargins(8, 16, 8, 6);
    trendChart_ = new ui::VBarChart(trendCard_);
    trendChart_->setOnBarClick([this](int) { drillTo("usage", "range", "14"); });
    trl->addWidget(trendChart_, 1);
    mainRow->addWidget(trendCard_, 4);
    // 只有主行吸收多余高度（图表越高越好读）；KPI 与其余行都按内容取高，
    // 这样小窗口下不会把底行顶出视口，大窗口下也不会把事件流拉出死区
    root->addLayout(mainRow, 1);

    // ---- 第三行：Agent 状态网格 + 模型分布 ----
    auto* midRow = new QHBoxLayout();
    midRow->setSpacing(14);
    agentsCard_ = new QGroupBox(i18n::trs("Agent 状态", "Agent Status"), this);
    agentsCard_->setObjectName("card");
    auto* al = new QVBoxLayout(agentsCard_);
    al->setContentsMargins(8, 16, 8, 6);
    auto* gridHolder = new QWidget(agentsCard_);
    // 透明底色：否则全局 QSS 的 QWidget 背景会在卡片内再涂一层页面底色，形成"卡中卡"暗框
    gridHolder->setStyleSheet("background:transparent;");
    agentGrid_ = new QGridLayout(gridHolder);
    agentGrid_->setContentsMargins(0, 0, 0, 0);
    agentGrid_->setSpacing(8);
    al->addWidget(gridHolder);
    al->addStretch(1);
    agentsEmpty_ = new ui::InlineEmpty(
        "plug", i18n::trs("还没有 Agent 接入", "no agents yet"),
        i18n::trs("用右侧设置里的「接入命令」把第一个 Agent 拉进蜂巢",
                  "use the connect command in Settings to add your first agent"),
        agentsCard_);
    agentsEmpty_->setVisible(false);
    al->addWidget(agentsEmpty_);
    midRow->addWidget(agentsCard_, 3);

    modelCard_ = new QGroupBox(i18n::trs("模型用量累计", "Tokens by Model"), this);
    modelCard_->setObjectName("card");
    auto* ml = new QVBoxLayout(modelCard_);
    ml->setContentsMargins(8, 16, 8, 6);
    modelChart_ = new ui::HBarChart(modelCard_);
    modelChart_->setOnEntryClick([this](int idx) {
        if (idx >= 0 && idx < lastModelBars_.size())
            drillTo("usage", "model", lastModelBars_[idx].first);
    });
    ml->addWidget(modelChart_, 1);
    midRow->addWidget(modelCard_, 2);
    root->addLayout(midRow);          // 按内容取高（Agent 网格行数由数据决定）

    // ---- 第四行：事件流（类型过滤 + 点击下钻）+ 告警 ----
    auto* bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(14);

    tlCard_ = new QGroupBox(i18n::trs("事件流", "Event Stream"), this);
    tlCard_->setObjectName("card");
    // 显式最小高度会覆盖布局从子控件推算的最小值：事件流的换行标签曾把卡片最小高度
    // 撑到 384px，整页被顶出视口（事件流/告警掉到折页之下）。240 ≈ 过滤行 + 6 行事件。
    tlCard_->setMinimumHeight(240);
    auto* tl = new QVBoxLayout(tlCard_);
    tl->setContentsMargins(10, 16, 10, 8);
    tl->setSpacing(8);
    auto* tlFilter = new QHBoxLayout;
    eventFilter_ = new QComboBox(tlCard_);
    // 类型与审计动作同源（kindOfEvent），过滤即"按动作族筛选"
    eventFilter_->addItem(i18n::trs("全部类型", "All types"), QString());
    eventFilter_->addItem(i18n::trs("记忆", "Memory"), QString("memory"));
    eventFilter_->addItem(i18n::trs("知识", "Knowledge"), QString("knowledge"));
    eventFilter_->addItem(i18n::trs("技能", "Skills"), QString("skills"));
    eventFilter_->addItem(i18n::trs("消息", "Messages"), QString("messages"));
    eventFilter_->addItem(i18n::trs("错误", "Errors"), QString("errors"));
    eventFilter_->addItem(i18n::trs("Agent", "Agents"), QString("agents"));
    eventFilter_->setStyleSheet(ui::th("QComboBox { padding:3px 10px; font-size:11px; }"));
    connect(eventFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { lastTimeline_.clear(); refresh(); });
    tlFilter->addWidget(eventFilter_);
    tlFilter->addStretch(1);
    tl->addLayout(tlFilter);

    eventStack_ = new QStackedWidget(tlCard_);
    timeline_ = new QListWidget(eventStack_);
    timeline_->setAlternatingRowColors(true);
    // 封顶：事件流是"最近动态"的窗口，不该无限吃掉首屏高度（完整审计在「操作日志」面板）。
    // 高度取整行数（6 × 33px ≈ 200），避免最后一行被切成半行——布局审计会报 CLIPPED。
    timeline_->setMaximumHeight(200);
    // 经 ui::th() 生成：等宽字体族与字号随主题/字号档位缩放
    timeline_->setStyleSheet(ui::th(
        "QListWidget { font-family:@mono@,monospace; font-size:11px; }"
        "QListWidget::item { padding:3px 4px; }"));
    timeline_->setCursor(Qt::PointingHandCursor);
    // 双击事件行 → 跳到对应面板看细节（事件流是"线索"，不是终点）
    connect(timeline_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        if (!it) return;
        const QString kind = it->data(Qt::UserRole).toString();
        drillTo(kind.isEmpty() ? "audit" : kind);
    });
    eventEmpty_ = new ui::InlineEmpty("audit", i18n::trs("该类型暂无事件", "no events of this type"),
                                      i18n::trs("换个类型，或等 Agent 产生新的操作",
                                                "pick another type, or wait for new activity"),
                                      eventStack_);
    eventStack_->addWidget(timeline_);
    eventStack_->addWidget(eventEmpty_);
    tl->addWidget(eventStack_, 1);
    bottomRow->addWidget(tlCard_, 3);

    alertCard_ = new QGroupBox(i18n::trs("告警", "Alerts"), this);
    alertCard_->setObjectName("card");
    alertCard_->setMinimumHeight(240);  // 同 tlCard_：封住换行标签撑大的最小高度
    auto* wl = new QVBoxLayout(alertCard_);
    wl->setContentsMargins(10, 16, 10, 8);
    wl->setSpacing(8);
    // 空状态：说明"这里会出现什么 + 去哪里处理"，不再是一行干巴巴的提示
    emptyAlerts_ = new ui::InlineEmpty(
        "errors", i18n::trs("暂无错误，一切正常", "No errors — all clear"),
        i18n::trs("预算越线或 Agent 报错时会在这里分级告警",
                  "budget alerts and reported errors show up here"), alertCard_);
    wl->addWidget(emptyAlerts_);
    alertsLay_ = wl;
    bottomRow->addWidget(alertCard_, 2);
    // 底行按内容取高：此前它带 stretch，卡片被拉到 380px 而列表只有 168px，
    // 既留出大片死区，又把整页推高到必须滚动才能看到事件流/告警（布局审计实测越界 142px）
    root->addLayout(bottomRow);

    // ---- 首屏骨架屏：数据回来前铺一层占位，避免"空面板"被误读成"没有数据" ----
    skeleton_ = new ui::Skeleton(this);
    skeleton_->setRows(5);
    skeleton_->start();
}

void DashboardPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (skeleton_ && skeleton_->isVisible())
        skeleton_->setGeometry(rect().adjusted(16, 84, -16, -16));
}

QString DashboardPanel::kindOfEvent(const QString& action, const QString& target) {
    const QString s = action + " " + target;
    if (s.contains("error")) return "errors";
    if (s.contains("skill")) return "skills";
    if (s.contains("memory")) return "memory";
    if (s.contains("knowledge")) return "knowledge";
    if (s.contains("note") || s.contains("question") || s.contains("task") ||
        s.contains("message"))
        return "messages";
    if (s.contains("register") || s.contains("agent")) return "agents";
    return "audit";
}

void DashboardPanel::refreshHealth() {
    const ah::Diagnostics d = platform_.diagnostics();
    struct Row {
        QString text;
        QString fixAgent;  // 非空 → 该行的修复动作是"给这个 Agent 轮换密钥"
        QString detail;    // 非空 → 查看详情弹更完整的解释与出路
    };
    QVector<Row> rows;
    QStringList sig;
    if (!d.http_running) {
        rows.push_back({i18n::trs("本机服务未运行：Agent 现在无法接入工作台。",
                                  "Local service is not running: agents cannot reach the "
                                  "workbench right now."),
                        QString(), "http"});
        sig << "http";
    }
    if (!d.home_writable) {
        rows.push_back({i18n::trs("数据目录不可写：配置与数据无法保存。",
                                  "Data directory is not writable: settings and data cannot be "
                                  "saved."),
                        QString(), "dir"});
        sig << "dir";
    }
    if (!d.db_ok) {
        rows.push_back({i18n::trs("数据库异常：无法读取核心数据表。",
                                  "Database problem: core tables cannot be read."),
                        QString(), "db"});
        sig << "db";
    }
    if (!d.agents_json_readable) {
        rows.push_back({i18n::trs("密钥缓存损坏或不可读：已接入的 Agent 可能全部掉线。",
                                  "Key cache is corrupted or unreadable: connected agents may all "
                                  "go offline."),
                        QString(), "cache"});
        sig << "cache";
    }
    for (const auto& n : d.keyfile_missing) {
        const QString name = QString::fromStdString(n);
        rows.push_back({i18n::trs("%1 的明文密钥已丢失：无法再查看或补配；若 Agent 端配置也丢了将无法接入，建议轮换密钥。",
                                  "%1's plaintext key is gone: it cannot be viewed or re-shared; "
                                  "if the agent side also lost it, rotation is the only way back.")
                            .arg(name),
                        name, QString()});
        sig << "key:" + name;
    }
    const QString s = sig.join("|");
    // healthDirty_：修复动作（如轮换密钥）已让现场变好，但"健康"的签名恰好也是空串，
    // 与 clear() 后的 lastHealthSig_ 相等会短路返回——旧告警横幅将永远挂在页面上。
    if (!healthDirty_ && s == lastHealthSig_) return;
    healthDirty_ = false;
    lastHealthSig_ = s;

    // 重建横幅行（签名变化才会走到这里）
    const auto& kids = healthBox_->children();
    for (auto* c : kids)
        if (qobject_cast<QFrame*>(c)) qobject_cast<QFrame*>(c)->deleteLater();
    const QColor c = ui::warn();
    for (const auto& r : rows) {
        auto* row = new QFrame(healthBox_);
        row->setStyleSheet(QString("QFrame { background:rgba(%1,%2,%3,28);"
                                   " border-left:4px solid %4; border-radius:6px; }")
                               .arg(c.red())
                               .arg(c.green())
                               .arg(c.blue())
                               .arg(c.name()));
        auto* hl = new QHBoxLayout(row);
        hl->setContentsMargins(10, 6, 10, 6);
        hl->setSpacing(8);
        auto* ic = new QLabel(row);
        ic->setPixmap(ui::makeIcon("errors", c, 15).pixmap(15, 15));
        ic->setStyleSheet("background:transparent;");
        hl->addWidget(ic);
        auto* lb = new ui::WrappedLabel(2, r.text, row);
        lb->setStyleSheet(QString("color:%1; font-size:12px; background:transparent;").arg(c.name()));
        hl->addWidget(lb, 1);
        if (!r.fixAgent.isEmpty()) {
            auto* fix = new QPushButton(i18n::trs("轮换密钥修复", "Fix: rotate key"), row);
            fix->setCursor(Qt::PointingHandCursor);
            fix->setStyleSheet(ui::th("padding:3px 10px; font-size:11px;"));
            connect(fix, &QPushButton::clicked, this,
                    [this, name = r.fixAgent] { fixKeyfile(name); });
            hl->addWidget(fix);
        }
        if (!r.detail.isEmpty()) {
            auto* more = new QPushButton(i18n::trs("查看详情", "Details"), row);
            more->setCursor(Qt::PointingHandCursor);
            more->setStyleSheet(ui::th("padding:3px 10px; font-size:11px;"));
            const QString code = r.detail;
            connect(more, &QPushButton::clicked, this, [this, code] {
                const QString home = QString::fromStdString(platform_.homeDir());
                QString text;
                if (code == "http")
                    text = ui::humanError(i18n::trs("HTTP 服务未启动", "HTTP service not started"));
                else if (code == "dir")
                    text = ui::humanError(i18n::trs("数据目录不可写: %1", "data dir not writable: %1")
                                              .arg(home)) +
                           "\n" + home;
                else if (code == "db")
                    text = ui::humanError(
                        i18n::trs("数据库异常或损坏", "database missing or corrupted"));
                else
                    text = ui::humanError(i18n::trs("密钥缓存损坏", "key cache corrupted"));
                QMessageBox::information(this, i18n::trs("健康详情", "Health details"), text);
            });
            hl->addWidget(more);
        }
        healthLay_->addWidget(row);
    }
    healthBox_->setVisible(!rows.isEmpty());
}

void DashboardPanel::fixKeyfile(const QString& name) {
    const auto ret = QMessageBox::question(
        this, i18n::trs("轮换密钥", "Rotate key"),
        i18n::trs("将为 %1 生成新密钥并写回本机缓存。旧密钥立即失效；之后请把新密钥更新到该 Agent "
                  "的配置并重启它。继续吗？",
                  "A new key will be generated for %1 and written to the local cache. The old key "
                  "stops working immediately; afterwards update the agent's config with the new "
                  "key and restart it. Continue?")
            .arg(name));
    if (ret != QMessageBox::Yes) return;
    std::string key, err;
    if (!platform_.agentRotateKey(ah::kManagerName, name.toStdString(), key, err)) {
        QMessageBox::warning(this, i18n::trs("修复失败", "Fix failed"),
                             ui::humanError(QString::fromStdString(err)));
        return;
    }
    QMessageBox box(this);
    box.setWindowTitle(i18n::trs("密钥已轮换", "Key rotated"));
    box.setIcon(QMessageBox::Information);
    box.setText(i18n::trs("%1 的新密钥已生成并写入本机缓存：", "A new key for %1 was generated "
                                                         "and written to the local cache:")
                        .arg(name) +
                "\n\n" + QString::fromStdString(key) + "\n\n" +
                i18n::trs("下一步：把新密钥更新到该 Agent 的配置（MIDERHIVE_AGENT_KEY 或注册命令），"
                          "然后重启该 Agent。",
                          "Next: update the agent's config with the new key (MIDERHIVE_AGENT_KEY or "
                          "the register command), then restart the agent."));
    auto* copyBtn = box.addButton(i18n::trs("复制密钥", "Copy key"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == copyBtn) {
        QApplication::clipboard()->setText(QString::fromStdString(key));
        ui::Toast::show(this, i18n::trs("已复制，请粘贴到 Agent 配置并重启它",
                                        "Copied — paste it into the agent config and restart it"));
    }
    healthDirty_ = true;  // 强制下轮重建横幅（密钥条目已恢复；健康签名恰为空串，仅 clear 会撞相等）
}

void DashboardPanel::showAgentActions(const QString& name, bool online) {
    QMenu menu(this);
    if (online) {
        menu.addAction(i18n::trs("在线中", "online"))->setEnabled(false);
    } else {
        // 防呆：离线卡片先给"为什么离线"的具体原因，再给对应的修复入口
        const ah::Diagnostics d = platform_.diagnostics();
        const bool keyMissing =
            std::find(d.keyfile_missing.begin(), d.keyfile_missing.end(),
                      name.toStdString()) != d.keyfile_missing.end();
        const QString reason =
            keyMissing
                ? i18n::trs("离线排查：明文密钥缓存丢失（无法再查看/补配；轮换可重置）",
                            "Diagnosis: plaintext key cache missing (cannot be viewed or "
                            "re-shared; rotate to reset)")
                : i18n::trs("离线排查：密钥正常，多为 Agent 未运行或端口/网络配置不符",
                            "Diagnosis: key is fine — likely the agent is not running, or "
                            "port/network mismatch");
        menu.addAction("● " + reason)->setEnabled(false);
        if (keyMissing)
            menu.addAction(i18n::trs("轮换密钥修复", "Fix by rotating key"), this,
                           [this, name] { fixKeyfile(name); });
    }
    menu.addSeparator();
    // 离线 Agent 的出路：把接入命令直接复制走，用户不用去翻文档凑参数
    menu.addAction(i18n::trs("复制接入命令", "Copy connect command"), this, [this, name] {
        const QString exe = QCoreApplication::applicationDirPath() + "/miderhive-mcp.exe";
        // 命令本体是固定 CLI 语法，只有密钥占位符是给用户看的中文，按语言切换
        const QString cmd = i18n::trs("claude mcp add miderhive --env MIDERHIVE_AGENT_NAME=%1 "
                                      "--env MIDERHIVE_AGENT_KEY=<该 Agent 的密钥> -- \"%2\"",
                                      "claude mcp add miderhive --env MIDERHIVE_AGENT_NAME=%1 "
                                      "--env MIDERHIVE_AGENT_KEY=<this agent's key> -- \"%2\"")
                                .arg(name, exe);
        QApplication::clipboard()->setText(cmd);
        ui::Toast::show(this, i18n::trs("已复制接入命令", "connect command copied"));
    });
    menu.addAction(i18n::trs("复制 Agent 名称", "Copy agent name"), this, [this, name] {
        QApplication::clipboard()->setText(name);
        ui::Toast::show(this, i18n::trs("已复制名称", "name copied"));
    });
    menu.addAction(i18n::trs("查看该 Agent 的用量", "Usage for this agent"), this,
                   [this, name] { drillTo("usage", "agent", name); });
    if (!online)
        menu.addAction(i18n::trs("查看 Agent 交流", "Open messaging"), this,
                       [this] { drillTo("messages"); });
    menu.exec(QCursor::pos());
}

void DashboardPanel::refresh() {
    std::string err;

    // 健康横幅先行：问题（端口/目录/库/密钥）在用户看数据前就摆上桌面
    refreshHealth();

    // 逐日序列先取：KPI 的趋势小图与预算卡的迷你走势都用它（一次查询，两处复用）
    std::vector<ah::UsageDailyPoint> dailyPts;
    QVector<qint64> dailySeries;
    if (platform_.usageDaily(14, dailyPts, err)) {
        for (const auto& d : dailyPts) dailySeries << d.tokens;
    }

    ah::UsageSummary sum;
    if (platform_.usageSummary(sum, err)) {
        // 环内不再重复"剩余"（右侧数字列已经写了），环只留百分比与 已用/总额
        ring_->setValues(sum.total_tokens, sum.budget, QString());
        budgetAlertLevel_ = QString::fromStdString(sum.alert_level);

        QVector<QPair<QString, qint64>> bars;
        lastAgentBars_.clear();
        for (const auto& [name, tokens] : sum.per_agent) {
            bars.append({QString::fromStdString(name), tokens});
            lastAgentBars_.append({QString::fromStdString(name), tokens});
        }
        usageChart_->setEntries(bars);

        // 预算卡右侧数字列：已用 / 剩余 / 占比 三行等宽数字（原来只有环内一行，信息量太低）
        const double ratio = sum.budget > 0 ? double(sum.total_tokens) / double(sum.budget) : 0.0;
        const QColor c = ui::usageColor(ratio);
        // 注意占位符与参数一一对应：%2 是"用量语义色"，被 %3/%6/%8 三处复用；
        // 百分比必须自己作为最后一个参数填进 %8（曾把颜色串填进去，占比显示成了色值）
        budgetStats_->setText(
            QString("<div style='line-height:152%'>"
                    "%1 <b style='color:%2'>%3</b><br>"
                    "%4 <b style='color:%5'>%6</b><br>"
                    "%7 <b style='color:%2'>%8</b></div>")
                .arg(i18n::trs("已用", "used"), c.name(), formatNum(sum.total_tokens),
                     i18n::trs("剩余", "left"), ui::ok().name(),
                     formatNum(qMax<qint64>(0, sum.budget - sum.total_tokens)),
                     i18n::trs("占比", "share"))
                .arg(QString("%1%").arg(ratio * 100, 0, 'f', 1)));
        budgetStats_->setToolTip(i18n::trs("本周预算 %1 tokens", "weekly budget %1 tokens")
                                     .arg(formatNum(sum.budget)));
        if (!dailySeries.isEmpty()) budgetSpark_->setSeries(dailySeries, c);

        // KPI：本周 Token（等宽大数字 + 14 天趋势小图 + 环比胶囊）
        kpiTokens_->set(i18n::trs("本周 Token", "Weekly tokens"), ui::fmtCompact(sum.total_tokens),
                        i18n::trs("预算 %1 · 剩余 %2", "budget %1 · left %2")
                            .arg(ui::fmtCompact(sum.budget),
                                 ui::fmtCompact(qMax<qint64>(0, sum.budget - sum.total_tokens))),
                        ui::usageColor(ratio));
        kpiTokens_->setToolTipAll(i18n::trs("本周已用 %1 / %2（%3%）",
                                            "used %1 of %2 (%3%)")
                                      .arg(formatNum(sum.total_tokens))
                                      .arg(formatNum(sum.budget))
                                      .arg(ratio * 100, 0, 'f', 1));
        if (!dailySeries.isEmpty()) kpiTokens_->setSparkline(dailySeries, ui::usageColor(ratio));
        // 环比：最近 7 天 vs 前 7 天（口径与「用量分析」页一致）
        if (dailySeries.size() >= 14) {
            qint64 recent = 0, prior = 0;
            for (int i = 0; i < 7; ++i) prior += dailySeries[i];
            for (int i = 7; i < 14; ++i) recent += dailySeries[i];
            if (prior > 0) {
                const double d = 100.0 * double(recent - prior) / double(prior);
                kpiTokens_->setDelta(QString("%1%2%").arg(d >= 0 ? "▲ " : "▼ ")
                                         .arg(qAbs(d), 0, 'f', 0),
                                     d >= 0 ? ui::danger() : ui::ok());
            } else {
                kpiTokens_->setDelta(QString(), QColor());
            }
        }
        // 环比说明：胶囊只有百分比，tooltip 里给出"跟什么比"
        kpiTokens_->setDeltaTooltip(
            i18n::trs("最近 7 天 vs 前 7 天（成本类指标：涨为红、跌为绿）",
                      "last 7 days vs previous 7 days (cost metric: up = red, down = green)"));
    }

    // 今日消耗（环比昨日）
    if (!dailySeries.isEmpty()) {
        const qint64 today = dailySeries.back();
        const qint64 prev = dailySeries.size() >= 2 ? dailySeries[dailySeries.size() - 2] : 0;
        kpiToday_->set(i18n::trs("今日消耗", "Today's tokens"), ui::fmtCompact(today),
                       i18n::trs("较昨日", "vs yesterday"), ui::accent());
        if (prev > 0) {
            const double d = 100.0 * double(today - prev) / double(prev);
            kpiToday_->setDelta(QString("%1%2%").arg(d >= 0 ? "▲ " : "▼ ").arg(qAbs(d), 0, 'f', 0),
                                d >= 0 ? ui::danger() : ui::ok());
        } else {
            kpiToday_->setDelta(QString(), QColor());
        }
        kpiToday_->setToolTipAll(
            i18n::trs("今日已用 %1 tokens", "%1 tokens today").arg(formatNum(today)));
        kpiToday_->setDeltaTooltip(i18n::trs("今天 vs 昨天", "today vs yesterday"));
    }

    // 逐日趋势图（内容变化才重绘，避免 3s 刷新反复扫掠动画）
    {
        QVector<QPair<QString, qint64>> es;
        for (const auto& d : dailyPts)
            es.push_back({QString::fromStdString(d.day).mid(5), d.tokens});  // MM-DD
        if (es != lastDaily_) {
            lastDaily_ = es;
            trendChart_->setEntries(es);
        }
    }

    // 模型用量
    std::vector<ah::UsageModelRow> modelRows;
    if (platform_.usageByModel(modelRows, err)) {
        QVector<QPair<QString, qint64>> es;
        lastModelBars_.clear();
        for (size_t i = 0; i < modelRows.size() && i < 8; ++i) {
            es.push_back({QString::fromStdString(modelRows[i].model), modelRows[i].tokens});
            lastModelBars_.append({QString::fromStdString(modelRows[i].model), modelRows[i].tokens});
        }
        if (es != lastModel_) {
            lastModel_ = es;
            modelChart_->setEntries(es);
        }
    }

    // ---- Agent 状态网格（在线/离线微条 + 卡片点击操作）----
    std::vector<ah::AgentInfo> agents;
    if (platform_.listAgents(agents, err)) {
        int online = 0;
        for (const auto& a : agents)
            if (a.status == "online") ++online;
        const int total = static_cast<int>(agents.size());
        const int offline = total - online;
        const bool allOn = total > 0 && online == total;
        const QColor c = online > 0 ? ui::ok() : ui::muted();
        kpiAgents_->set(i18n::trs("Agent", "Agents"), QString("%1/%2").arg(online).arg(total),
                        allOn ? i18n::trs("全部在线", "all online")
                              : i18n::trs("%1 个离线", "%1 offline").arg(offline),
                        c, c);
        // 在线/离线构成微条：比文字更快看出"几个掉线了"
        QColor off = ui::line();
        off.setAlpha(200);
        kpiAgents_->setMicroStrip({{c, online}, {off, offline}},
                                  i18n::trs("在线 %1 · 离线 %2", "online %1 · offline %2")
                                      .arg(online)
                                      .arg(offline));
        kpiAgents_->setToolTipAll(
            i18n::trs("在线 %1 / 共 %2；离线 Agent 可在下方卡片上点击获取接入命令",
                      "%1 of %2 online; click an offline card for its connect command")
                .arg(online)
                .arg(total));

        size_t need = agents.size();
        while (agentCards_.size() < need) agentCards_.push_back(new ui::AgentCard(this));
        const int avail = agentsCard_->width() > 0 ? agentsCard_->width() : width();
        const int cols = qBound(1, avail / 300, 3);
        for (size_t i = 0; i < agentCards_.size(); ++i) {
            auto* card = agentCards_[i];
            agentGrid_->removeWidget(card);  // 列数变化时重新落位
            if (i < need) {
                agentGrid_->addWidget(card, static_cast<int>(i) / cols, static_cast<int>(i) % cols);
                card->setVisible(true);
                const auto& a = agents[i];
                const QString nm = QString::fromStdString(a.name);
                const QString rawSeen = QString::fromStdString(a.last_seen_at);
                card->setAgent(nm, QString::fromStdString(a.status),
                               QString::fromStdString(a.role),
                               QString::fromStdString(a.current_task),
                               rawSeen.isEmpty() ? QString() : relTime(rawSeen),
                               localStamp(rawSeen));
                const bool on = a.status == "online";
                card->setOnActivate([this, nm, on] { showAgentActions(nm, on); });
            } else {
                card->hide();
            }
        }
        agentsEmpty_->setVisible(total == 0);
    }

    // ---- 事件流：类型过滤 + 截断 + 空状态分页 ----
    std::vector<ah::AuditRecord> records;
    if (platform_.auditList("", "", "", 60, records, err)) {
        rebuildEventStream(records);
    }

    // ---- 告警：预算分级 + 未解决错误（分级卡片可点，直达处理面板）----
    for (auto* w : alertCards_) {
        alertsLay_->removeWidget(w);
        w->deleteLater();
    }
    alertCards_.clear();
    auto addAlert = [this](const QString& level, const QString& text, const QString& panel,
                           const QString& filterKey, const QString& filterValue) {
        auto* card = new ui::AlertCard(level, text, this);
        card->setOnClick([this, panel, filterKey, filterValue] {
            drillTo(panel, filterKey, filterValue);
        });
        alertCards_.push_back(card);
    };
    if (budgetAlertLevel_ == "warn")
        addAlert("warn",
                 i18n::trs("Token 用量已达预算 80%，请留意消耗",
                           "Token usage reached 80% of budget"),
                 "usage", QString(), QString());
    else if (budgetAlertLevel_ == "critical")
        addAlert("critical",
                 i18n::trs("Token 用量已达预算 95%！", "Token usage reached 95% of budget!"),
                 "usage", QString(), QString());
    else if (budgetAlertLevel_ == "over")
        addAlert("critical",
                 i18n::trs("Token 用量已超出本周预算！", "Token usage exceeded this week's budget!"),
                 "usage", QString(), QString());

    std::vector<ah::ErrorReport> allOpen;
    int openCount = 0;
    if (platform_.errorList("open", "", 999, allOpen, err)) {
        openCount = static_cast<int>(allOpen.size());
        // 严重度构成微条 + 新错误环比：错误卡也从"一个数"变成"构成 + 变化"
        int crit = 0, warn = 0, note = 0;
        for (const auto& e : allOpen) {
            if (e.severity == "critical") ++crit;
            else if (e.severity == "warning") ++warn;
            else ++note;
        }
        const QColor cc = openCount == 0 ? ui::ok() : ui::danger();
        kpiErrors_->set(i18n::trs("未解决错误", "Unresolved errors"), QString::number(openCount),
                        openCount == 0 ? i18n::trs("一切正常", "all clear")
                                       : i18n::trs("到「错误报告」处理", "resolve in Errors"),
                        cc, cc);
        if (openCount > 0)
            kpiErrors_->setMicroStrip({{ui::danger(), crit}, {ui::warn(), warn}, {ui::muted(), note}},
                                      i18n::trs("严重 %1 · 警告 %2 · 提示 %3",
                                                "critical %1 · warning %2 · note %3")
                                          .arg(crit)
                                          .arg(warn)
                                          .arg(note));
        kpiErrors_->setToolTipAll(
            i18n::trs("当前有 %1 条未解决错误；点击下方告警卡片直达处理",
                      "%1 unresolved error(s); click an alert card to resolve")
                .arg(openCount));
        // 会话内新增（首次采样只建立基线，不误报"新增"）
        if (seenOpenErrors_ >= 0 && openCount != seenOpenErrors_) {
            const int diff = openCount - seenOpenErrors_;
            kpiErrors_->setDelta(QString("%1%2").arg(diff > 0 ? "▲ " : "▼ ").arg(qAbs(diff)),
                                 diff > 0 ? ui::danger() : ui::ok());
            kpiErrors_->setDeltaTooltip(i18n::trs("相对上次刷新", "since last refresh"));
        }
        seenOpenErrors_ = openCount;

        // 告警清单只放最近 4 条：告警是"抬头看一眼"的信息，全量在「错误报告」面板
        const int show = qMin<int>(4, openCount);
        for (int i = 0; i < show; ++i) {
            const auto& e = allOpen[static_cast<size_t>(i)];
            const QString when = relTime(QString::fromStdString(e.created_at));
            addAlert(e.severity == "critical" ? "critical"
                                              : (e.severity == "warning" ? "warn" : "note"),
                     QString("%1 — %2 · %3")
                         .arg(QString::fromStdString(e.title))
                         .arg(QString::fromStdString(e.reporter))
                         .arg(when),
                     "errors", QString(), QString());
        }
    }

    int at = alertsLay_->indexOf(emptyAlerts_);
    for (auto* card : alertCards_) {
        const int insertAt = at < 0 ? alertsLay_->count() : at;
        alertsLay_->insertWidget(insertAt, card);
        ++at;
    }
    emptyAlerts_->setVisible(alertCards_.empty());

    // ---- 首屏骨架收尾 ----
    if (!firstLoadDone_) {
        firstLoadDone_ = true;
        if (skeleton_) skeleton_->stop();
    }
}

void DashboardPanel::rebuildEventStream(const std::vector<ah::AuditRecord>& records) {
    const QString want = eventFilter_->currentData().toString();
    QString joined = want;
    int shown = 0;
    QList<QListWidgetItem*> pending;   // 先构建待上屏条目：内容签名未变时整体丢弃，不清列表
    for (const auto& r : records) {
        const QString action = QString::fromStdString(r.action);
        const QString target = QString::fromStdString(r.target);
        const QString kind = kindOfEvent(action, target);
        if (!want.isEmpty() && kind != want) continue;
        if (shown >= 6) break;  // 事件流是窗口不是清单：6 条足够"抬头看一眼"
        const QString raw = QString::fromStdString(r.created_at);
        auto* item = new QListWidgetItem(
            ui::makeIcon(kind == "agents" ? "overview" : kind,
                         ui::muted(), 14, ui::selText()),
            QString("%1  %2  %3  %4")
                .arg(relTime(raw), -10)
                .arg(QString::fromStdString(r.actor))
                .arg(action)
                .arg(target));
        item->setData(Qt::UserRole, kind);
        item->setToolTip(i18n::trs("%1\n双击查看「%2」面板", "%1\ndouble-click to open the %2 panel")
                             .arg(localStamp(raw) + " · " + QString::fromStdString(r.actor) +
                                      " · " + action + " · " + target)
                             .arg(kind));
        pending.append(item);
        ++shown;
        joined += "|" + raw + action + target;
    }
    if (joined == lastTimeline_) {
        // 内容未变：丢弃待上屏条目、保留现有列表（3s 刷新不再闪烁，选中/滚动位置不丢）
        qDeleteAll(pending);
        eventStack_->setCurrentWidget(shown == 0 ? static_cast<QWidget*>(eventEmpty_)
                                                 : static_cast<QWidget*>(timeline_));
        return;
    }
    lastTimeline_ = joined;
    timeline_->clear();
    for (auto* item : pending) timeline_->addItem(item);  // addItems 只收 QStringList，条目须逐个上屏
    eventStack_->setCurrentWidget(shown == 0 ? static_cast<QWidget*>(eventEmpty_)
                                             : static_cast<QWidget*>(timeline_));
}

void DashboardPanel::retranslate() {
    PanelBase::retranslate();
    budgetCard_->setTitle(i18n::trs("本周 Token 预算", "Weekly Token Budget"));
    editBudgetBtn_->setText(i18n::trs("调整预算", "Adjust budget"));
    usageCard_->setTitle(i18n::trs("各 Agent 本周用量", "Per-Agent Usage This Week"));
    agentsCard_->setTitle(i18n::trs("Agent 状态", "Agent Status"));
    trendCard_->setTitle(i18n::trs("最近 14 天逐日消耗", "Daily Tokens (14 days)"));
    modelCard_->setTitle(i18n::trs("模型用量累计", "Tokens by Model"));
    tlCard_->setTitle(i18n::trs("事件流", "Event Stream"));
    alertCard_->setTitle(i18n::trs("告警", "Alerts"));
    eventFilter_->setItemText(0, i18n::trs("全部类型", "All types"));
    eventFilter_->setItemText(1, i18n::trs("记忆", "Memory"));
    eventFilter_->setItemText(2, i18n::trs("知识", "Knowledge"));
    eventFilter_->setItemText(3, i18n::trs("技能", "Skills"));
    eventFilter_->setItemText(4, i18n::trs("消息", "Messages"));
    eventFilter_->setItemText(5, i18n::trs("错误", "Errors"));
    eventFilter_->setItemText(6, i18n::trs("Agent", "Agents"));
    // 构造期一次性写入、不在 refresh() 里重生成的铬层文案
    budgetSpark_->setToolTip(i18n::trs("最近 14 天逐日消耗", "daily tokens, last 14 days"));
    agentsEmpty_->titleLabel()->setText(i18n::trs("还没有 Agent 接入", "no agents yet"));
    agentsEmpty_->hintLabel()->setText(
        i18n::trs("用右侧设置里的「接入命令」把第一个 Agent 拉进蜂巢",
                  "use the connect command in Settings to add your first agent"));
    eventEmpty_->titleLabel()->setText(i18n::trs("该类型暂无事件", "no events of this type"));
    eventEmpty_->hintLabel()->setText(i18n::trs("换个类型，或等 Agent 产生新的操作",
                                                "pick another type, or wait for new activity"));
    emptyAlerts_->titleLabel()->setText(i18n::trs("暂无错误，一切正常", "No errors — all clear"));
    emptyAlerts_->hintLabel()->setText(
        i18n::trs("预算越线或 Agent 报错时会在这里分级告警",
                  "budget alerts and reported errors show up here"));
    // 健康横幅由 refreshHealth() 按内容签名增量重建：签名不变会短路，这里显式置脏
    // 强制下一轮按新语言重建（与 fixKeyfile 后的处理同一条路径）
    healthDirty_ = true;
    lastTimeline_.clear();  // 强制事件流下次刷新重建（文案随语言变化）
}
