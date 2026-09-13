#pragma once
// 启动自检（防呆）：在平台初始化前后各跑一遍的可读性检查。
// 目标不是替代 Platform::bootstrap 的错误处理，而是把"静默失败/生硬报错"翻译成
// 用户能看懂的语言，并附上"下一步该怎么做"。所有文案经 i18n::trs 双语。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTcpServer>
#include <QString>
#include <QVector>

#include "i18n.h"

namespace ui::startup {

struct Issue {
    QString code;  // 稳定标识（诊断/测试用）：home_not_writable / agents_json_unreadable /
                   // port_occupied / db_file_missing / db_file_empty
    QString text;  // 已按当前语言格式化的问题说明（含下一步建议）
};

// 平台初始化前的本机环境预检。home = 数据目录绝对路径；port = 计划监听端口。
// dbPath 非空且文件存在时检查基本完整性（可读、非空）。
inline QVector<Issue> preflight(const QString& home, int port, const QString& dbPath,
                                const QString& agentsJsonPath) {
    QVector<Issue> issues;

    // 1) 数据目录存在且可写（bootstrap 会创建，这里提前探测并给出可读的解释）
    QDir dir(home);
    if (!dir.exists()) dir.mkpath(home);
    const QString probe = home + "/.write-probe";
    if (!QFile(probe).open(QIODevice::WriteOnly)) {
        issues.push_back({"home_not_writable",
                          i18n::trs("数据目录不可写：%1\n工作台无法保存配置与数据（数据库也建不起来）。"
                                    "\n下一步：请确认目录未被杀毒软件/权限策略锁定，或以管理员身份运行一次。",
                                    "Data directory is not writable: %1\nThe workbench cannot save "
                                    "settings or data (the database cannot be created either).\n"
                                    "Next: make sure the folder is not locked by antivirus/permissions, "
                                    "or run the app as administrator once.")
                              .arg(home)});
    } else {
        QFile::remove(probe);
    }

    // 2) 配置文件 agents.json 可读（存在时才检查；损坏在 bootstrap 会被拒并留痕）
    if (QFileInfo::exists(agentsJsonPath)) {
        QFile f(agentsJsonPath);
        if (!f.open(QIODevice::ReadOnly)) {
            issues.push_back({"agents_json_unreadable",
                              i18n::trs("配置文件不可读：%1\nAgent 密钥缓存读不出来，已接入的 Agent 会连接失败。"
                                        "\n下一步：关闭占用该文件的程序（如编辑器/同步盘），或在设置里轮换密钥。",
                                        "Config file is not readable: %1\nAgent key cache cannot be "
                                        "read; connected agents will fail to connect.\n"
                                        "Next: close whatever locks the file (editor/sync tool), or "
                                        "rotate keys in Settings.")
                                  .arg(agentsJsonPath)});
        }
    }

    // 3) 端口可用性：单实例互斥已在前（同一数据目录只跑一份工作台），
    //    这里若仍绑不上，说明是"别的程序"占了端口——直接给出人话与出路。
    QTcpServer portProbe;
    if (!portProbe.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        issues.push_back({"port_occupied",
                          i18n::trs("端口 %1 已被其他程序占用，Agent 将无法接入本机服务。"
                                    "\n下一步：关闭占用程序，或在启动前设置环境变量 MIDERHIVE_PORT 换一个端口"
                                    "（工作台与 Agent 两侧要用同一个端口）。",
                                    "Port %1 is used by another program, so agents cannot reach the "
                                    "local service.\nNext: stop the other program, or set MIDERHIVE_PORT "
                                    "before starting (the same port must be used on both sides).")
                              .arg(port)});
    } else {
        portProbe.close();
    }

    // 4) 数据库文件基本完整（存在时：可读且非空；深层校验由 bootstrap + /api/diagnostics 负责）
    if (!dbPath.isEmpty() && QFileInfo::exists(dbPath)) {
        QFile f(dbPath);
        if (!f.open(QIODevice::ReadOnly)) {
            issues.push_back({"db_file_missing",
                              i18n::trs("数据库文件无法读取：%1\n可能被其他程序锁住，或磁盘出现问题。"
                                        "\n下一步：关闭其他正在使用该文件的程序后重启工作台；仍失败时用最近一次备份恢复。",
                                        "Cannot read the database file: %1\nIt may be locked by another "
                                        "program, or the disk has a problem.\nNext: close other programs "
                                        "using it and restart; if it still fails, restore from the latest "
                                        "backup.")
                                  .arg(dbPath)});
        } else if (f.size() == 0) {
            issues.push_back({"db_file_empty",
                              i18n::trs("数据库文件是空的：%1\n上次运行可能被强制中断。"
                                        "\n下一步：重启工作台会自动重建空库；需要找回旧数据请用备份目录中的最近一次备份。",
                                        "The database file is empty: %1\nThe previous run may have been "
                                        "force-killed.\nNext: restarting recreates an empty database; "
                                        "restore from the latest backup if you need the old data.")
                                  .arg(dbPath)});
        }
    }
    return issues;
}

// 把问题列表拼成可关闭对话框的正文（每条一个问题 + 建议，空列表返回空串）。
inline QString joinIssues(const QVector<Issue>& issues) {
    QString out;
    for (const auto& it : issues) out += "• " + it.text + "\n\n";
    return out.trimmed();
}

}  // namespace ui::startup
