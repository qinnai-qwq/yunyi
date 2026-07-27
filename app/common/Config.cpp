/**
 * @file Config.cpp
 * @brief 命令行参数解析实现
 */
#include "Config.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace yunyi {

bool AppConfig::parseFromArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--public-ip" && i + 1 < argc) {
            publicIp = argv[++i];
        } else if (arg == "--psk" && i + 1 < argc) {
            psk = argv[++i];
        } else if (arg == "--psk-identity" && i + 1 < argc) {
            pskIdentity = argv[++i];
        } else if (arg == "--control-port" && i + 1 < argc) {
            controlPort = static_cast<uint16_t>(
                std::stoul(argv[++i]));
        } else if (arg == "--port-start" && i + 1 < argc) {
            portPoolStart = static_cast<uint16_t>(
                std::stoul(argv[++i]));
        } else if (arg == "--port-end" && i + 1 < argc) {
            portPoolEnd = static_cast<uint16_t>(
                std::stoul(argv[++i]));
        } else if (arg == "--http-port" && i + 1 < argc) {
            httpPort = static_cast<uint16_t>(
                std::stoul(argv[++i]));
        } else if (arg == "--host-http-port" && i + 1 < argc) {
            hostHttpPort = static_cast<uint16_t>(
                std::stoul(argv[++i]));
        } else if (arg == "--no-auto-connect") {
            autoConnect = false;
        } else if (arg == "--heartbeat-interval" && i + 1 < argc) {
            heartbeatIntervalMs = std::stoul(argv[++i]);
        } else if (arg == "--room-grace" && i + 1 < argc) {
            roomGracePeriodMs = std::stoul(argv[++i]);
        } else if (arg == "--max-players" && i + 1 < argc) {
            maxPlayersPerRoom = std::stoul(argv[++i]);
        } else if (arg == "--log-level" && i + 1 < argc) {
            logLevel = std::stoi(argv[++i]);
        } else if (arg == "--log-path" && i + 1 < argc) {
            logPath = argv[++i];
        } else if (arg == "--local-mc-port" && i + 1 < argc) {
            localMcPort = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--relay-addr" && i + 1 < argc) {
            // 由 HostAgentApp 内部处理
        } else if (arg == "--help" || arg == "-h") {
            std::cout << R"(Usage:
  --relay                  Run as relay server
  --host                   Run as host agent
  --public-ip <ip>         Public IP address (required)
  --psk <key>              Pre-shared key (min 16 bytes)
  --local-mc-port <port>   Local Minecraft port (host only)
  --control-port <port>    Control connection port (default 40000)
  --port-start <port>      Port pool start (default 40001)
  --port-end <port>        Port pool end (default 41000)
  --http-port <port>       HTTP port for WebUI (default 8080)
  --host-http-port <port>  Host agent HTTP port (default 8081)
  --no-auto-connect        Don't auto-connect host to relay
  --max-players <n>        Max players per room (default 10)
  --log-level <0-5>        Log level (default 3)
  -h, --help               Show this help
)" << std::endl;
            return false;
        }
    }

    // PSK 安全检查：过短则使用默认测试密钥
    if (psk.size() < 16) {
        std::cerr
            << "[Config] WARNING: PSK is less than 16 bytes, "
            << "using default test key" << std::endl;
        psk = "yunyi-test-key-16bytes-min!";
    }

    // 端口范围合法性检查
    if (portPoolStart > portPoolEnd) {
        std::cerr << "[Config] WARNING: port-start (" << portPoolStart
                  << ") > port-end (" << portPoolEnd
                  << "), swapping" << std::endl;
        std::swap(portPoolStart, portPoolEnd);
    }
    if (controlPort >= portPoolStart && controlPort <= portPoolEnd) {
        std::cerr << "[Config] WARNING: control port (" << controlPort
                  << ") is within port pool range, this may cause conflicts"
                  << std::endl;
    }

    return true;
}

} // namespace yunyi
