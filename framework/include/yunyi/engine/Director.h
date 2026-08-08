/**
 * @file Director.h
 * @brief 引擎唯一门面 —— 全局单例，外部代码的唯一入口
 *
 * @see docs/director-api.md 完整接口手册
 */
#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace yunyi {
struct DirectorImpl;
class Scheduler;
class Session;
class TlsPskContext;
class TransportCore;
class TunnelManager;
namespace protocol { class FrameDispatcher; }
/**
 * @brief 引擎全局配置
 *
 * 在调用 Director::init() 时传入，初始化后不可更改。
 */
struct EngineConfig {
    /** 日志级别: 0=off, 1=error, 2=warn, 3=info, 4=debug, 5=trace */
    int logLevel = 3;
    /** 日志输出路径，空字符串 = stderr */
    std::string logPath;
    /** IO 工作线程数，0 = 自动检测（CPU 核心数） */
    uint32_t ioThreads = 0;
};
/**
 * @brief 端口池配置
 *
 * 中继服务器为每个房间分配一个独立端口，端口从该池中取用。
 */
struct PortPoolConfig {
    /** 控制连接监听端口，默认 40000 */
    uint16_t controlPort = 40000;
    /** 端口池起始（含），默认 40001 */
    uint16_t rangeStart = 40001;
    /** 端口池结束（含），默认 41000 */
    uint16_t rangeEnd = 41000;
    /** 单个房间最大玩家数，超出后拒绝新连接 */
    uint32_t maxPlayersPerRoom = 10;
};
/**
 * @brief TLS-PSK 配置
 *
 * 当前仅支持 TLS-PSK，以结构体封装以便未来扩展其他加密方式。
 */
struct TlsConfig {
    /** PSK 预共享密钥（UTF-8），最少 16 字节 */
    std::string psk;
    /** PSK 身份标识（可选，为空则用默认值 "yunyi"） */
    std::string pskIdentity;
    /** 最低 TLS 版本: 0 = TLS 1.2, 1 = TLS 1.3，默认 1 */
    int minTlsVersion = 1;
};
/**
 * @brief 中继服务器地址
 */
struct RelayAddress {
    /** 公网 IPv4 地址 */
    std::string host;
    /** 端口 */
    uint16_t port = 40000;
};
/**
 * @brief 房间创建结果
 */
struct RoomResult {
    /** 是否成功 */
    bool ok = false;
    /** 房间 ID（成功时有效） */
    uint32_t roomId = 0;
    /** 分配到的公网端口（成功时有效） */
    uint16_t assignedPort = 0;
    /** 连接码字符串，格式 "IP:Port"（成功时有效） */
    std::string connectionCode;
    /** 错误码（失败时有效） */
    std::string errorCode;
    /** 错误描述（失败时有效） */
    std::string errorMessage;
};
struct EngineStats {
    uint64_t uptimeSeconds = 0;
    uint32_t activeRooms = 0, totalRooms = 0;
    uint32_t activeTunnels = 0, totalConnections = 0;
    uint32_t portPoolUsed = 0, portPoolTotal = 0;
    uint64_t bytesRelayed = 0;
};
struct RoomInfo {
    uint32_t roomId = 0;
    std::string roomName, connectionCode;
    uint16_t assignedPort = 0, localMcPort = 0;
    /** 状态: "waiting" | "active" | "closed" */
    std::string status;
    uint32_t playerCount = 0;
    /** 创建时间，ISO 8601 */
    std::string createdAt;
    /** 房主（控制连接）的远端地址，仅中继端有效 */
    std::string hostAddress;
};
struct PlayerInfo {
    uint32_t id = 0;
    /** 连接时间，ISO 8601 */
    std::string connectedAt;
    uint64_t bytesSent = 0, bytesRecv = 0;
};
/**
 * @class Director
 * @brief 引擎唯一门面，全局单例
 *
 * 生命周期:
 *   1. Director::instance()    获取单例
 *   2. init(cfg)               初始化引擎
 *   3. startRelayService() 或 connectToRelay()  选择角色
 *   4. 运行期间通过公开方法操作房间/隧道
 *   5. shutdown()              关闭引擎
 *
 * @note 外部代码严禁直接引用 Session、Scheduler、TunnelManager、
 *       TransportCore 等内部头文件。所有操作必须通过 Director。
 */
class Director {
public:
    static Director& instance();
    bool init(const EngineConfig& cfg);
    void shutdown();
    bool isInitialized() const;
    bool startRelayService(const PortPoolConfig& ports, const TlsConfig& tls);
    bool connectToRelay(const RelayAddress& addr, const TlsConfig& tls);
    RoomResult createRoom(std::string_view roomName, uint16_t localMcPort);
    bool closeRoom(uint32_t roomId);

    /**
     * @brief 在中继端直接创建房间（不通过控制帧）
     * @param roomName 房间名称
     * @param localMcPort 房主 MC 端口
     * @param publicIp 公网 IP（用于生成连接码）
     * @return 房间创建结果
     *
     * @pre 已 startRelayService()
     *
     * 用于 HTTP API 在服务端直接创建房间，跳过 REGISTER 帧交换。
     */
    RoomResult createRoomRelay(std::string_view roomName,
                               uint16_t localMcPort,
                               const std::string& publicIp);
    RoomInfo getRoomInfo(uint32_t roomId) const;
    std::vector<RoomInfo> listRooms() const;
    std::vector<PlayerInfo> getRoomPlayers(uint32_t roomId) const;
    bool forceCloseRoom(uint32_t roomId);
    EngineStats getStats() const;
    std::string getLastError() const;

    /** 获取帧分发器（用于注册 handler） */
    protocol::FrameDispatcher* frameDispatcher();
    /** 获取调度器（用于定时器） */
    class Scheduler* scheduler();
    /** 获取传输层（用于 ControlChannel） */
    class TransportCore* transport();

    /** 设置为房主模式 */
    void setAsHostAgent();

    /**
     * @brief 向控制会话发送加密帧
     * @param session 目标会话
     * @param tls TLS 上下文（用于加密）
     * @param frame 待发送的原始帧字节
     * @return true 发送成功
     */
    bool sendToControlSession(Session* session, TlsPskContext* tls,
                              const std::vector<uint8_t>& frame);

    /**
     * @brief 将房主控制会话链接到房间
     * @param roomId 房间 ID
     * @param session 控制会话
     * @param tls TLS 上下文（转移所有权）
     * @return true 成功
     */
    bool linkRoomToHost(uint32_t roomId, Session* session,
                        std::shared_ptr<std::unique_ptr<TlsPskContext>> tls);

    /**
     * @brief 向当前正在处理的帧的发送者回复帧
     * @param frame 原始帧字节（明文，由本方法加密）
     * @return true 成功
     * @pre 在 FrameDispatcher handler 回调中调用
     */
    bool sendReplyFrame(const std::vector<uint8_t>& frame);

    /**
     * @brief 入队待发送的加密帧（线程安全，供 accept 回调使用）
     */
    void enqueueControlSend(Session* session, TlsPskContext* tls,
                            const std::vector<uint8_t>& frame);

    /**
     * @brief 处理待发送队列（在主循环 onTick 中调用）
     */
    void flushPendingSends();

    /**
     * @brief 更新中继公网 IP（GUI WebUI 修改后调用）
     * @param ip 新的公网 IP 字符串
     */
    void setPublicIp(const std::string& ip);
    std::string getPublicIp() const;

    /** 获取最近 N 条后端日志（供 WebUI 轮询） */
    std::vector<std::pair<std::string, std::string>> getRecentLogs(size_t limit = 50);
    /** 添加后端日志（同时写入 log 文件和内存缓冲区） */
    void addLog(const std::string& msg);
    /** 设置日志文件输出流（由 App 层创建并注入） */
    void setLogWriter(std::shared_ptr<std::ostream> writer);

    /**
     * @brief 将当前控制会话链接到房间
     * @param roomId 房间 ID
     * @return true 成功
     * @pre 在 FrameDispatcher handler 回调中调用
     */
    bool linkCurrentSessionToRoom(uint32_t roomId);

    /**
     * @brief 更新当前控制会话所属房间的心跳时间
     * @return 房间 ID，0 表示未找到
     * @pre 在 FrameDispatcher handler 回调中调用
     */
    uint32_t touchRoomHeartbeat();

    /**
     * @brief 按控制会话查找房间 ID
     * @param session 控制会话指针
     * @return 房间 ID，0 表示未找到
     */
    uint32_t findRoomByControlSession(class Session* session) const;
    uint32_t findRoomByHostAddr(const std::string& hostAddr) const;
    std::string getCurrentReplyHostAddr() const;

    /**
     * @brief 获取隧道管理器（用于 STREAM_BIND 配对）
     */
    class TunnelManager* tunnelMgr();

    /**
     * @brief 配对数据隧道：将房主的 TLS 隧道会话绑定到玩家连接
     * @param playerConnId 玩家连接 ID（来自 OPEN_STREAM）
     * @param hostTunnelSession 房主新建立的 TLS 数据隧道会话
     * @return true 配对成功
     */
    bool pairHostTunnel(uint32_t playerConnId, Session* hostTunnelSession);

    Director(const Director&) = delete;
    Director& operator=(const Director&) = delete;
    Director(Director&&) = delete;
    Director& operator=(Director&&) = delete;
private:
    Director(); ~Director();
    DirectorImpl& _impl(); const DirectorImpl& _impl() const;
    std::unique_ptr<DirectorImpl> _d;
};
}
