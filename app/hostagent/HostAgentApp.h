/**
 * @file HostAgentApp.h
 * @brief 房主端主程序 —— 暴露本地 MC 服务到中继
 *
 * 职责:
 *   - 通过 Director 连接到中继服务器
 *   - 管理 ControlChannel（控制连接状态机）
 *   - 注册控制帧处理器（REGISTER_ACK, OPEN_STREAM, HEARTBEAT_ACK, ERROR）
 *   - 接收 OPEN_STREAM 后建立到本地 MC 的数据隧道
 */
#pragma once
#include "../common/Config.h"
#include "../component/ControlChannel.h"
#include <memory>
#include <thread>
#include <unordered_map>

// forward
namespace httplib { class Server; }

namespace yunyi {

class Director;

/**
 * @class HostAgentApp
 * @brief 房主端应用程序
 *
 * 生命周期:
 *   1. 构造 HostAgentApp
 *   2. init(cfg) 连接到中继
 *   3. run() 进入主循环
 *   4. stop() 退出主循环
 */
class HostAgentApp {
public:
    HostAgentApp();
    ~HostAgentApp();

    /**
     * @brief 初始化房主端
     * @param cfg 应用配置（含中继地址、PSK 等）
     * @return true 成功，false 失败
     */
    bool init(const AppConfig& cfg);

    /**
     * @brief 连接到远程中继（当 _autoConnect=false 时由 WebUI 调用）
     * @param relayIp 中继公网 IP
     * @param controlPort 中继控制端口
     * @param psk 预共享密钥
     * @return true 连接成功，false 失败
     */
    bool connectToRelay(const std::string& relayIp, uint16_t controlPort,
                        const std::string& psk);

    /** @brief 断开与远程中继的连接 */
    void disconnectFromRelay();

    /** @brief 查询当前控制连接状态 */
    ControlState connectionState() const;

    /**
     * @brief 运行主循环（阻塞）
     */
    void run();

    /**
     * @brief 停止房主端
     */
    void stop();

private:
    /** 注册控制帧处理器 */
    void setupFrameHandlers();

    /**
     * @brief 处理来自中继的控制帧
     * @param t 帧类型
     * @param d 负载数据
     * @param len 负载长度
     */
    void onRelayFrame(protocol::FrameType t, const uint8_t* d, uint32_t len);

    /** Director 引擎实例 */
    Director* _director = nullptr;
    /** 控制连接状态机 */
    std::unique_ptr<ControlChannel> _controlChannel;
    /** 应用配置副本 */
    AppConfig _config;
    /** 运行标志 */
    bool _running = false;
    /** 是否在 init() 中自动连接中继 */
    bool _autoConnect = true;
    /** 本地 MC 服务端口 */
    uint16_t _localMcPort = 25565;
    /** 中继分配的房间端口 */
    uint16_t _assignedPort = 0;
    /** Host 创建的远程房间信息 */
    struct HostRoom {
        uint32_t roomId = 0;
        std::string roomName;
        uint16_t assignedPort = 0;
        uint16_t localMcPort = 0;
        std::string connectionCode;
        std::string createdAt;
        uint32_t playerCount = 0;
        uint32_t tunnelCount = 0;
        uint64_t bytesRelayed = 0;
    };
    /** 活跃的数据隧道（playerConnId → Session 映射） */
    std::unordered_map<uint32_t, class Session*> _tunnels;
    /** 已注册的房间列表（REGISTER_ACK 填充，供 WebUI 显示） */
    std::vector<HostRoom> _hostRooms;
    /** 累计隧道转发字节数（供 WebUI 显示流量） */
    uint64_t _totalBytesRelayed = 0;
    /** 最近一次 REGISTER 的房间名（等待 ACK 时暂存） */
    std::string _lastRegisterName;
    uint16_t _lastRegisterMcPort = 25565;

    /** 启动 HTTP 服务器（用于 WebUI） */
    void startHttpServer();
    void stopHttpServer();
    void registerHttpRoutes(httplib::Server& svr);

    /** HTTP 服务器 */
    std::unique_ptr<httplib::Server> _httpServer;
    std::thread _httpThread;
    uint16_t _httpPort = 8080;
};

}
