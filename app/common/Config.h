/**
 * @file Config.h
 * @brief 全局配置 —— 从命令行读取，三个角色共用
 */
#pragma once
#include <cstdint>
#include <string>
namespace yunyi {

/**
 * @brief 应用全局配置
 *
 * 从命令行参数解析，包含网络地址、端口范围、TLS 密钥、
 * 心跳/超时参数等。三个角色（Relay / HostAgent / Auto）共用。
 */
struct AppConfig {
    /** 公网 IPv4 地址 */
    std::string publicIp;
    /** 控制连接端口 */
    uint16_t controlPort = 40000;
    /** 端口池起始 */
    uint16_t portPoolStart = 40001;
    /** 端口池结束 */
    uint16_t portPoolEnd = 41000;
    /** HTTP API 端口（仅中继模式） */
    uint16_t httpPort = 8080;
    /** 房主端 HTTP API 端口 */
    uint16_t hostHttpPort = 8081;
    /** Host 模式启动时是否自动连接中继（false = 等待 WebUI 指令） */
    bool autoConnect = true;
    /** PSK 预共享密钥 */
    std::string psk;
    /** PSK 身份标识 */
    std::string pskIdentity = "yunyi";
    /** 心跳间隔（毫秒） */
    uint32_t heartbeatIntervalMs = 25000;
    /** 心跳超时（毫秒），超时后房间被清理 */
    uint32_t heartbeatTimeoutMs = 75000;
    /** 房间宽限期（毫秒），房主断连后保留时间 */
    uint32_t roomGracePeriodMs = 45000;
    /** 单房间最大玩家数 */
    uint32_t maxPlayersPerRoom = 10;
    /** 本地 MC 服务端口（仅房主模式） */
    uint16_t localMcPort = 25565;
    /** 日志级别: 0=off, 1=error, 2=warn, 3=info, 4=debug, 5=trace */
    int logLevel = 3;
    /** 日志输出路径，空字符串 = stderr */
    std::string logPath;

    /**
     * @brief 从命令行参数解析配置
     * @param argc 参数数量
     * @param argv 参数数组
     * @return true 解析成功，false 参数错误（自动打印帮助）
     */
    bool parseFromArgs(int argc, char* argv[]);
};

/**
 * @brief 运行模式
 */
enum class RunMode {
    /** 中继服务器 */
    Relay,
    /** 房主端 */
    HostAgent,
    /** 自动检测 */
    Auto
};

}
