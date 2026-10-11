// platformd：无 GUI 的后台守护进程，只跑平台核心 + HTTP API。
// 用于无 Qt 环境的部署或测试；工作台 GUI 内置同款服务器。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "core/platform.h"
#include "core/util.h"

namespace {
// 信号处理器只置标志、绝不做事：主循环负责检查并优雅停机。
// 此前装的是空处理器且主循环从不检查任何标志——33 行宣传的
// "press Ctrl+C to stop" 整体失效，服务方式运行就是个只能外部强杀的死循环。
// （Windows 上的已知边界：交互模式下 Ctrl+C 返回后，阻塞中的 getline 不会
// 自动醒来，需再按一次回车让循环轮询到标志；服务方式（stdin 已关闭）每秒
// 轮询，Ctrl+C 一到即优雅退出。）
std::atomic<bool> g_stopRequested{false};
void requestStop(int) { g_stopRequested.store(true); }
}  // namespace

int main() {
    ah::Platform platform(ah::defaultHomeDir());
    std::string err;
    if (!platform.bootstrap(err)) {
        std::cerr << "[platformd] bootstrap failed: " << err << "\n";
        return 1;
    }
    int port = 8787;
    {
        std::string portEnv =
            ah::envOr({"MIDERHIVE_PORT", "AGENTHIVE_PORT", "ZCODE_PLATFORM_PORT"});
        // 校验与 agent-cli resolvePort 同一口径：atoi 无校验——"abc"/"0" 静默绑到
        // OS 临时端口（bind_to_port(0) 必成功）、"8787xyz" 前缀截断静默改端口、
        // 99999 16 位截断错绑，服务"在跑"却无人能连且零报错
        if (!portEnv.empty()) {
            bool ok = !portEnv.empty();
            for (char ch : portEnv)
                if (ch < '0' || ch > '9') ok = false;
            if (ok) {
                const long v = std::strtol(portEnv.c_str(), nullptr, 10);
                if (v < 1 || v > 65535) ok = false;
            }
            if (!ok) {
                std::cerr << "[platformd] invalid MIDERHIVE_PORT: " << portEnv
                          << " (expected 1..65535)\n";
                return 2;
            }
            port = std::atoi(portEnv.c_str());
        }
    }
    if (!platform.startHttpServer(port, err)) {
        std::cerr << "[platformd] cannot start server: " << err << "\n";
        return 1;
    }
    std::cout << "[platformd] listening on http://127.0.0.1:" << platform.httpPort() << "\n";
    std::cout << "[platformd] data dir: " << platform.homeDir() << "\n";
    std::cout << "[platformd] press Ctrl+C to stop\n";
    std::cout.flush();

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    // 等待退出条件：stdin 的 quit/exit、Ctrl+C/SIGTERM（置停止标志）、
    // 或 HTTP 服务异常退出（每秒轮询）
    while (true) {
        if (g_stopRequested.load()) break;
        std::string line;
        if (!std::getline(std::cin, line)) {
            // stdin 关闭（服务方式运行）→ 睡眠等待信号
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!platform.httpRunning()) break;
        } else if (line == "quit" || line == "exit") {
            break;
        }
    }
    platform.stopHttpServer();
    std::cout << "[platformd] stopped\n";
    return 0;
}
