#pragma once
// GUI 小工具函数：表格填充、即时过滤、右键菜单（复制/详情/导出 CSV）、数字格式化、
// 相对时间。时间统一由后端以 UTC ISO8601 下发，展示层在此转成本地时区与人类可读文案。
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTableWidget>
#include <QTimeZone>
#include <QTreeWidget>

#include <functional>
#include <initializer_list>
#include <utility>

#include "i18n.h"

namespace ui {

// ---- 报错翻译（防呆）：把后端的技术报错翻译成用户能懂的一句话 + 下一步操作。 ----
// 匹配按"关键片段"进行（错误文本可能是英文/中文/带路径），逐条试，命中即返回；
// 都不命中时返回原文 + 通用指引，绝不吞信息。
inline QString humanError(const QString& raw) {
    const QString s = raw.toLower();
    auto say = [](const QString& zh, const QString& en) { return i18n::trs(zh, en); };
    if (s.contains("keyfile") || s.contains("master key") || s.contains("invalid master")) {
        return say("检测到密钥问题：Agent 密钥或主密钥丢失/不匹配，已接入的 Agent 会连接失败。\n"
                   "下一步：在设置 →「Agent 管理」里轮换密钥，然后把新密钥更新到 Agent 的配置文件并重启该 Agent。",
                   "Key problem: an agent key or the master key is missing or does not match, so "
                   "connected agents will fail.\nNext: rotate the key in Settings → Agents, update "
                   "the agent's config with the new key, then restart that agent.");
    }
    if (s.contains("database is locked")) {
        return say("数据库正被占用（可能有另一个工作台/平台进程同时在写）。\n"
                   "下一步：关闭其他 MiderHive 实例后重试；若仍失败，稍等几秒再试（写入会自动等待最多 3 秒）。",
                   "The database is locked (another workbench/platform process may be writing).\n"
                   "Next: close other MiderHive instances and retry; otherwise wait a few seconds "
                   "(writes already wait up to 3 s).");
    }
    if (s.contains("malformed") || s.contains("disk image") || s.contains("corrupt") ||
        s.contains("not a database")) {
        return say("数据库文件疑似损坏，无法继续读写。\n"
                   "下一步：关闭工作台，用备份目录里最近一次备份恢复 platform.db，再重新启动。",
                   "The database file appears to be corrupted.\nNext: close the workbench, restore "
                   "platform.db from the latest backup, then start again.");
    }
    if (s.contains("in use") || s.contains("bind") || s.contains("listen") ||
        s.contains("address")) {
        return say("网络端口被占用，本机服务起不来，Agent 将无法接入。\n"
                   "下一步：关闭占用该端口的程序，或设置环境变量 MIDERHIVE_PORT 换端口（两侧端口要一致）。",
                   "The port is occupied, so the local service cannot start and agents cannot "
                   "connect.\nNext: stop the program using the port, or set MIDERHIVE_PORT to "
                   "switch (both sides must match).");
    }
    if (s.contains("already registered")) {
        return say("该名称已被注册。\n下一步：换一个名字，或在设置 →「Agent 管理」里轮换这个 Agent 的密钥。",
                   "This name is already registered.\nNext: pick another name, or rotate that "
                   "agent's key in Settings → Agents.");
    }
    if (s.contains("version conflict")) {
        return say("内容已被他人更新（版本冲突），为避免覆盖未予写入。\n"
                   "下一步：刷新查看最新内容，基于新版本重新提交。",
                   "The content was updated by someone else (version conflict); the write was "
                   "refused to avoid overwriting.\nNext: refresh to see the latest version and "
                   "re-submit on top of it.");
    }
    if (s.contains("not registered") || s.contains("not found")) {
        return say("目标不存在（可能已被删除或名字拼写不符）。\n下一步：刷新列表确认名称后再试。",
                   "The target does not exist (it may have been removed, or the name is "
                   "misspelled).\nNext: refresh the list and check the name.");
    }
    if (s.contains("only zcode") || s.contains("manager") || s.contains("not allowed") ||
        s.contains("forbidden")) {
        return say("权限不足：该操作仅限管理者（zcode）。\n下一步：用管理者账号操作，或让管理者代为处理。",
                   "Not allowed: this action is manager-only (zcode).\nNext: do it as the manager, "
                   "or ask the manager to handle it.");
    }
    if (s.contains("too long")) {
        return say("内容超出长度限制。\n下一步：精简内容后重试（限制见报错原文中的说明）。",
                   "Content exceeds the length limit.\nNext: shorten it and retry (the limit is in "
                   "the original message).");
    }
    if (s.contains("invalid") || s.contains("must be") || s.contains("required") ||
        s.contains("missing")) {
        return say("提交的内容不完整或格式不对。\n下一步：按提示补全/修正后重试；反复失败请截图报错原文反馈。",
                   "The submitted content is incomplete or malformed.\nNext: fix it as suggested "
                   "and retry; if it keeps failing, report the original message.");
    }
    if (s.contains("cannot reach platform") || s.contains("connection refused") ||
        s.contains("timed out")) {
        return say("连不上本机服务（可能工作台未启动或端口被改）。\n"
                   "下一步：确认工作台正在运行、端口与 Agent 配置一致后重试。",
                   "Cannot reach the local service (the workbench may be down or the port "
                   "changed).\nNext: make sure the workbench is running and the port matches the "
                   "agent config, then retry.");
    }
    return say("操作失败：", "Operation failed: ") + raw +
           say("\n下一步：按原文提示处理；若与密钥/端口/数据库有关，可在总览页顶部的健康横幅查看修复入口。",
               "\nNext: follow the original message; for key/port/database issues, use the health "
               "banner at the top of the Overview page.");
}

}  // namespace ui

// 填充表格一行（按列顺序），并去重设置 item；全文同时挂到 tooltip，
// 列宽不足被截断时悬停即可看到完整内容。
inline void setRow(QTableWidget* table, int row, std::initializer_list<QString> cells) {
    int col = 0;
    for (const auto& text : cells) {
        auto* item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        if (!text.isEmpty()) item->setToolTip(text);
        table->setItem(row, col++, item);
    }
}

// 数量格式化：1234567 -> "1,234,567"
inline QString formatNum(qint64 n) {
    QString s = QString::number(n);
    for (int i = s.size() - 3; i > 0; i -= 3) s.insert(i, ',');
    return s;
}

// UTC ISO8601 -> 本地 QDateTime（无 'Z' 后缀时按 UTC 解释；解析失败返回无效值）
inline QDateTime parseUtcIso(const QString& iso) {
    if (iso.isEmpty()) return {};
    return QDateTime::fromString(iso.endsWith('Z') ? iso : iso + "Z", Qt::ISODate);
}

// 相对时间：把 "2026-09-10T14:07:04Z" 转成「刚刚 / 12 分钟前 / 3 小时前 / 2 天前」，
// 超过一周回落为本地 "MM-dd HH:mm"。解析失败时原样返回，不吞信息。
inline QString relTime(const QString& iso) {
    const QDateTime t = parseUtcIso(iso);
    if (!t.isValid()) return iso;
    const QDateTime local = t.toLocalTime();
    qint64 secs = local.secsTo(QDateTime::currentDateTime());
    if (secs < 0) secs = 0;  // 时钟偏差时不显示"未来"
    if (secs < 45) return i18n::trs("刚刚", "just now");
    if (secs < 3600) return i18n::trs("%1 分钟前", "%1 min ago").arg(secs / 60);
    if (secs < 86400) return i18n::trs("%1 小时前", "%1 h ago").arg(secs / 3600);
    if (secs < 7 * 86400) return i18n::trs("%1 天前", "%1 d ago").arg(secs / 86400);
    return local.toString("MM-dd HH:mm");
}

// 绝对本地时间（tooltip 用）：悬停即可看到精确时刻，兼顾相对时间的易读性。
inline QString localStamp(const QString& iso) {
    const QDateTime t = parseUtcIso(iso);
    return t.isValid() ? t.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : iso;
}

// 表格列自适应并铺满可用宽度：指定一列吃剩余空间，其余按内容宽。
// 不这么做时，列宽之和一旦超过视口就会出现"空表也有横向滚动条"，
// 时间列之类的窄列还会被压成 "06:51:..." 看不出完整时刻。
inline void fitTableColumns(QTableWidget* table, int stretchColumn = -1) {
    auto* h = table->horizontalHeader();
    for (int c = 0; c < table->columnCount(); ++c)
        h->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    if (stretchColumn >= 0 && stretchColumn < table->columnCount()) {
        h->setSectionResizeMode(stretchColumn, QHeaderView::Stretch);
        h->setStretchLastSection(false);
    } else {
        h->setStretchLastSection(true);
    }
}

// 审计日志的 detail 是紧凑 JSON。这里做轻量清洗（不引入 JSON 依赖），
// 让"改了哪个字段、值是什么"能一眼读出来：
//   {"call_type":"llm","tokens_in":2600}  ->  call_type=llm · tokens_in=2600
// 非 JSON 形态（或解析不出键值对）时原样返回，绝不吞信息。
inline QString prettyDetail(const QString& raw) {
    QString s = raw.trimmed();
    if (s.size() < 2 || !s.startsWith('{') || !s.endsWith('}')) return raw;
    s = s.mid(1, s.size() - 2);
    QString out;
    int depth = 0;
    bool inStr = false;
    QString field;
    auto flush = [&] {
        QString f = field.trimmed();
        field.clear();
        if (f.isEmpty()) return;
        // 去掉包裹的引号，逗号分隔的键值对换成分隔符
        if (f.startsWith('"') && f.endsWith('"') && f.size() >= 2) f = f.mid(1, f.size() - 2);
        if (!out.isEmpty()) out += " · ";
        out += f;
    };
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\')) {
            inStr = !inStr;
            if (depth == 0) continue;  // 扁平键值对去掉引号：call_type=llm 而非 call_type="llm"
        }
        if (!inStr) {
            if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') --depth;
            else if (c == ':' && depth == 0) { field += '='; continue; }
            else if (c == ',' && depth == 0) { flush(); continue; }
        }
        field += c;
    }
    flush();
    return out.isEmpty() ? raw : out;
}

// 表格统一抛光：隔行底色、隐藏垂直表头、整行选择、只读、舒适行高
inline void polishTable(QTableWidget* table) {
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(28);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setWordWrap(false);
}

// 即时过滤（纯函数）：按任意列、大小写不敏感隐藏不匹配的行，空文本恢复全部；
// 过滤只改可见性，不动数据。面板在 refresh() 重建行后需再次调用以保持过滤态。
inline void applyTableFilter(QTableWidget* table, const QString& text) {
    const QString needle = text.trimmed();
    for (int r = 0; r < table->rowCount(); ++r) {
        if (needle.isEmpty()) {
            table->setRowHidden(r, false);
            continue;
        }
        bool hit = false;
        for (int c = 0; c < table->columnCount() && !hit; ++c)
            if (auto* it = table->item(r, c); it && it->text().contains(needle, Qt::CaseInsensitive))
                hit = true;
        table->setRowHidden(r, !hit);
    }
}

namespace detail {
// 过滤输入框的 Esc 行为：按 Esc 清空过滤并恢复全部行（编辑中时优先消费）。
class FilterEditFilter : public QObject {
public:
    using Apply = std::function<void(const QString&)>;
    FilterEditFilter(QLineEdit* edit, Apply apply) : QObject(edit), apply_(std::move(apply)) {
        edit->installEventFilter(this);
    }
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::KeyPress) {
            if (auto* ke = static_cast<QKeyEvent*>(e); ke->key() == Qt::Key_Escape) {
                auto* edit = static_cast<QLineEdit*>(obj);
                if (!edit->text().isEmpty()) {
                    edit->clear();
                    apply_(QString());
                }
                return true;
            }
        }
        return QObject::eventFilter(obj, e);
    }

private:
    Apply apply_;
};
}  // namespace detail

// 即时过滤输入框：文本变化即过滤（任意列、大小写不敏感），Esc 清空恢复。
// 返回输入框交给面板放进工具栏；refresh() 重建行后请调用 applyTableFilter 保持过滤态。
inline QLineEdit* makeTableFilter(QTableWidget* table, QWidget* parent,
                                  const QString& placeholder = {}) {
    auto* edit = new QLineEdit(parent);
    edit->setPlaceholderText(placeholder.isEmpty()
                                 ? i18n::trs("输入即筛…", "Type to filter…")
                                 : placeholder);
    edit->setClearButtonEnabled(true);
    edit->setToolTip(i18n::trs("即时过滤（Ctrl+F 聚焦 · Esc 清空）",
                               "Live filter (Ctrl+F to focus · Esc to clear)"));
    auto apply = [table](const QString& text) { applyTableFilter(table, text); };
    QObject::connect(edit, &QLineEdit::textChanged, table, apply);
    new detail::FilterEditFilter(edit, std::move(apply));
    return edit;
}

// 树形即时过滤（纯函数）：叶子任一列命中则该行与所属分组可见，命中的分组自动展开，
// 其余分组隐藏；空文本恢复全部并收回到默认展开深度。
inline void applyTreeFilter(QTreeWidget* tree, const QString& text) {
    const QString needle = text.trimmed();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* group = tree->topLevelItem(i);
        if (needle.isEmpty()) {
            group->setHidden(false);
            for (int j = 0; j < group->childCount(); ++j)
                group->child(j)->setHidden(false);
            continue;
        }
        bool groupHit = false;
        for (int j = 0; j < group->childCount(); ++j) {
            auto* row = group->child(j);
            bool hit = false;
            for (int c = 0; c < tree->columnCount() && !hit; ++c)
                if (row->text(c).contains(needle, Qt::CaseInsensitive)) hit = true;
            row->setHidden(!hit);
            groupHit |= hit;
        }
        group->setHidden(!groupHit);
        group->setExpanded(groupHit);
    }
}

// 树形过滤输入框：配合 applyTreeFilter 使用，Esc 清空；refresh() 后需重调 applyTreeFilter。
inline QLineEdit* makeTreeFilter(QTreeWidget* tree, QWidget* parent,
                                 const QString& placeholder = {}) {
    auto* edit = new QLineEdit(parent);
    edit->setPlaceholderText(placeholder.isEmpty()
                                 ? i18n::trs("输入即筛…", "Type to filter…")
                                 : placeholder);
    edit->setClearButtonEnabled(true);
    edit->setToolTip(i18n::trs("即时过滤（Ctrl+F 聚焦 · Esc 清空）",
                               "Live filter (Ctrl+F to focus · Esc to clear)"));
    auto apply = [tree](const QString& text) { applyTreeFilter(tree, text); };
    QObject::connect(edit, &QLineEdit::textChanged, tree, apply);
    new detail::FilterEditFilter(edit, std::move(apply));
    return edit;
}

// 表格右键菜单：复制单元格/整行、导出 CSV。idColumn >= 0 时提供"复制 ID"。
inline void attachTableContextMenu(QTableWidget* table, int idColumn = -1) {
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(table, &QTableWidget::customContextMenuRequested,
                     table, [table, idColumn](const QPoint& pos) {
                         int row = table->rowAt(pos.y());
                         QMenu menu(table);
                         QAction* copyCell = menu.addAction(i18n::trs("复制单元格", "Copy cell"));
                         QAction* copyRow = menu.addAction(i18n::trs("复制整行", "Copy row"));
                         QAction* copyId = nullptr;
                         if (idColumn >= 0 && row >= 0)
                             copyId = menu.addAction(i18n::trs("复制 ID", "Copy ID"));
                         menu.addSeparator();
                         QAction* exportCsv = menu.addAction(i18n::trs("导出 CSV…", "Export CSV…"));
                         QAction* act = menu.exec(table->viewport()->mapToGlobal(pos));
                         if (!act || row < 0) return;
                         if (act == copyCell) {
                             auto* it = table->item(row, table->columnAt(pos.x()));
                             if (it) QApplication::clipboard()->setText(it->text());
                         } else if (act == copyRow) {
                             QStringList cells;
                             for (int c = 0; c < table->columnCount(); ++c) {
                                 auto* it = table->item(row, c);
                                 cells << (it ? it->text() : "");
                             }
                             QApplication::clipboard()->setText(cells.join("\t"));
                         } else if (copyId && act == copyId) {
                             auto* it = table->item(row, idColumn);
                             if (it) QApplication::clipboard()->setText(it->text());
                         } else if (act == exportCsv) {
                             QString path = QFileDialog::getSaveFileName(
                                 table, i18n::trs("导出 CSV", "Export CSV"), "export.csv",
                                 "CSV (*.csv)");
                             if (path.isEmpty()) return;
                             QFile f(path);
                             if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                                 QMessageBox::warning(table, i18n::trs("导出失败", "Export failed"),
                                                      i18n::trs("无法写入文件",
                                                                "cannot write the file"));
                                 return;
                             }
                             auto esc = [](QString v) {
                                 v.replace('"', "\"\"");
                                 return "\"" + v + "\"";
                             };
                             QTextStream out(&f);
                             out << "\xEF\xBB\xBF";  // Excel 兼容 BOM（Qt6 默认 UTF-8）
                             for (int c = 0; c < table->columnCount(); ++c)
                                 out << esc(table->horizontalHeaderItem(c)
                                                ? table->horizontalHeaderItem(c)->text()
                                                : "")
                                     << (c + 1 < table->columnCount() ? "," : "\n");
                             for (int r = 0; r < table->rowCount(); ++r) {
                                 for (int c = 0; c < table->columnCount(); ++c) {
                                     auto* it = table->item(r, c);
                                     out << esc(it ? it->text() : "")
                                         << (c + 1 < table->columnCount() ? "," : "\n");
                                 }
                             }
                             QMessageBox::information(table, i18n::trs("导出完成", "Export done"),
                                                      path);
                         }
                     });
}
