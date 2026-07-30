/**
 * @file main.cpp
 * @brief 程序入口 —— 解析命令行参数并启动对应 App
 *
 * 用法:
 *   云驿 --relay --public-ip 1.2.3.4 --psk mysecretkey
 *   云驿 --host  --public-ip 1.2.3.4 --psk mysecretkey
 */

#include "common/Config.h"
#include "relay/RelayApp.h"
#include "hostagent/HostAgentApp.h"

#include <iostream>
#include <cstring>
#include <thread>

namespace {

void printBanner() {
    std::cout << R"(
  +====================================+
  |       云驿 Yun Yi  v1.1.0          |
  |   Minecraft 中继服务 / 无需公网 IP |
  +====================================+
)" << std::endl;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    printBanner();

    // 解析运行模式
    yunyi::RunMode mode = yunyi::RunMode::Auto;
    yunyi::AppConfig config;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--relay") == 0) {
            mode = yunyi::RunMode::Relay;
        } else if (std::strcmp(argv[i], "--host") == 0) {
            mode = yunyi::RunMode::HostAgent;
        }
    }

    if (!config.parseFromArgs(argc, argv)) {
        return 1;
    }

    if (mode == yunyi::RunMode::Auto) {
        std::cerr << "Error: must specify --relay or --host" << std::endl;
        std::cerr << "Use --help for usage" << std::endl;
        return 1;
    }

    try {
        if (mode == yunyi::RunMode::Relay) {
            std::cout << "Starting as Relay Server..." << std::endl;

            yunyi::RelayApp app;
            if (!app.init(config)) {
                std::cerr << "Failed to initialize relay server" << std::endl;
                return 1;
            }
            app.run();
        } else {
            std::cout << "Starting as Host Agent..." << std::endl;

            yunyi::HostAgentApp app;
            if (!app.init(config)) {
                std::cerr << "Failed to initialize host agent" << std::endl;
                return 1;
            }
            app.run();
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
