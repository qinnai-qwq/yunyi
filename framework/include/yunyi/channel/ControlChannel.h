/**
 * @file ControlChannel.h
 * @brief 控制连接状态机 —— 房主端用
 *
 * 管理房主到中继的 TLS-PSK 控制连接。
 *
 * 状态迁移:
 *   DISCONNECTED -> CONNECTING -> REGISTERING -> ACTIVE
 *   ACTIVE -> RECONNECTING -> ACTIVE（宽限期内重连成功）
 *   任意状态 -> CLOSED -> DISCONNECTED
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace yunyi {
namespace protocol {
enum class FrameType : uint8_t;
enum class ErrorCode : uint8_t;
}
class Session;
class TlsPskContext;
class TransportCore;

/**
 * @brief 控制连接状态枚举
 */
enum class ControlState {
    /** 未连接 */
    Disconnected,
    /** TCP 连接中 */
    Connecting,
    /** 正在注册房间 */
    Registering,
    /** 活跃（正常通信） */
    Active,
    /** 重连中 */
    Reconnecting,
    /** 已关闭 */
    Closed
};

/**
 * @brief 状态变更回调
 * @param oldState 旧状态
 * @param newState 新状态
 */
using StateChangeCallback = std::function<void(ControlState, ControlState)>;

/**
 * @brief 控制帧接收回调
 * @param type 帧类型
 * @param data 负载数据
 * @param len 负载长度
 */
using ControlFrameCallback = std::function<void(protocol::FrameType, const uint8_t*, uint32_t)>;

/**
 * @brief 房间注册结果回调
 * @param ok 是否成功
 * @param roomId 房间 ID（成功时有效）
 * @param assignedPort 分配的端口（成功时有效）
 * @param errorMsg 错误消息（失败时有效）
 */
using RegisterAckCallback = std::function<void(bool ok, uint32_t roomId, uint16_t assignedPort, std::string errorMsg)>;

/**
 * @class ControlChannel
 * @brief 控制连接状态机
 *
 * 封装 TLS-PSK 控制连接的生命周期管理，包括:
 *   - TCP 连接建立
 *   - TLS 握手
 *   - REGISTER/DEREGISTER 帧收发
 *   - 心跳维护
 *   - 断线重连（最多 5 次，间隔 3 秒）
 */
class ControlChannel {
public:
    ControlChannel();
    ~ControlChannel();

    /**
     * @brief 连接到中继服务器
     * @param host 中继服务器主机名或 IP
     * @param port 控制连接端口
     * @param psk 预共享密钥
     * @param id PSK 身份标识
     * @param t TransportCore 实例指针
     * @return true 连接成功，false 失败
     */
    bool connect(const std::string& host, uint16_t port,
                 const std::string& psk, const std::string& id,
                 TransportCore* t);

    /**
     * @brief 注册房间
     * @param name 房间名称
     * @param mcPort 本地 MC 服务端口
     * @return true 发送成功，false 失败
     * @pre 状态为 ACTIVE
     */
    bool registerRoom(const std::string& name, uint16_t mcPort);

    /**
     * @brief 注册房间（异步，等待 REGISTER_ACK）
     * @param name 房间名称
     * @param mcPort 本地 MC 服务端口
     * @param cb 结果回调（在 IOCP 线程中调用）
     * @return true REGISTER 帧发送成功，false 发送失败（cb 不会被调用）
     * @pre 状态为 ACTIVE
     *
     * cb 在 REGISTER_ACK 到达或超时（5 秒）后调用。
     * 超时情况下 cb(false, 0, 0, "timeout")。
     */
    bool registerRoomAsync(const std::string& name, uint16_t mcPort,
                           RegisterAckCallback cb);

    /**
     * @brief 注销房间
     * @return true 发送成功
     */
    bool deregister();

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 查询当前状态
     * @return ControlState 枚举值
     */
    ControlState state() const { return _state.load(); }

    /**
     * @brief 发送控制帧
     * @param f 帧字节数据
     * @return true 发送成功
     */
    bool sendFrame(const std::vector<uint8_t>& f);

    /**
     * @brief 设置状态变更回调
     * @param cb 回调函数
     */
    void setOnStateChange(StateChangeCallback cb) { _onStateChange = std::move(cb); }
    void setLogCallback(std::function<void(const std::string&)> cb) { _logCb = std::move(cb); }

    /**
     * @brief 设置帧接收回调
     * @param cb 回调函数
     */
    void setOnFrame(ControlFrameCallback cb) { _onFrame = std::move(cb); }

    /**
     * @brief 获取 TLS 上下文
     * @return TlsPskContext 指针
     */
    TlsPskContext* tlsContext() const { return _tls.get(); }

    /**
     * @brief 获取底层 Session
     * @return Session 指针
     */
    Session* session() const { return _session; }

    /**
     * @brief 设置调度器（心跳和重连依赖）
     * @param s Scheduler 指针
     */
    void setScheduler(class Scheduler* s) { _scheduler = s; }

private:
    /** 设置状态（内部调用，触发回调） */
    void setState(ControlState s);
    /** 启动心跳定时器 */
    void startHeartbeat();
    /** 停止心跳定时器 */
    void stopHeartbeat();
    /** 处理接收到的 TLS 解密后数据 */
    void handleReceivedData(const char* d, size_t len);
    /** 尝试重连 */
    void tryReconnect();

    /** 当前状态 */
    std::atomic<ControlState> _state{ControlState::Disconnected};
    /** 底层 TCP 会话 */
    Session* _session = nullptr;
    /** TransportCore 指针 */
    TransportCore* _transport = nullptr;
    /** TLS 上下文 */
    std::unique_ptr<TlsPskContext> _tls;
    /** 中继主机名 */
    std::string _relayHost;
    /** 预共享密钥 */
    std::string _psk;
    /** PSK 身份标识 */
    std::string _pskIdentity;
    /** 中继端口 */
    uint16_t _relayPort = 0;
    /** 当前房间 ID */
    uint32_t _roomId = 0;
    /** 重连尝试次数 */
    uint32_t _reconnectAttempts = 0;
    /** TLS 握手是否已完成 */
    std::atomic<bool> _tlsHandshakeDone{false};
    /** TLS 操作递归互斥锁（handleReceivedData 内部调 sendFrame 需要可重入） */
    std::recursive_mutex _tlsMutex;
    /** 调度器（用于心跳和重连定时器） */
    class Scheduler* _scheduler = nullptr;
    /** 心跳定时器 ID */
    uint64_t _heartbeatTimerId = 0;
    /** 重连定时器 ID */
    uint64_t _reconnectTimerId = 0;
    /** 上次注册的房间名（用于重连后重注册） */
    std::string _lastRoomName;
    /** 上次注册的 MC 端口 */
    uint16_t _lastLocalMcPort = 0;
    /** 最大重连次数 */
    static constexpr uint32_t kMaxReconnectAttempts = 5;
    /** 重连间隔（毫秒） */
    static constexpr uint32_t kReconnectDelayMs = 3000;
    /** 心跳间隔（毫秒） */
    static constexpr uint32_t kHeartbeatIntervalMs = 25000;
    /** 接收缓冲区 */
    std::vector<uint8_t> _recvBuffer;
    /** 状态变更回调 */
    StateChangeCallback _onStateChange;
    std::function<void(const std::string&)> _logCb;
    /** 帧接收回调 */
    ControlFrameCallback _onFrame;
    /** 待处理的注册回调（等待 REGISTER_ACK） */
    RegisterAckCallback _pendingRegisterCb;
    /** 注册超时定时器 ID */
    uint64_t _registerTimeoutId = 0;
    /** 保护 _pendingRegisterCb 和 _registerTimeoutId 的互斥锁（IOCP 线程 vs 主线程） */
    mutable std::mutex _registerCbMutex;
};

}
