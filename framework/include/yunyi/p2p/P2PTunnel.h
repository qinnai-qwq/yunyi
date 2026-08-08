/**
 * @file P2PTunnel.h
 * @brief P2P 打洞隧道 —— 无中继直连（云驿后端协调）
 *
 * 两个云驿客户端（房主 + 玩家）都运行本模块，通过云驿后端(mc.qinnai.xyz)
 * 互换公网候选端点后，双方同时向对方 UDP 打洞，打通后用 ReliableUdpChannel
 * 承载 MC 流量直连，绕过中继。
 *
 * 状态机: Idle → Gathering(STUN) → Registering(上报候选) → Punching(打洞) → Connected/Failed
 *
 * 数据流:
 *   [房主] 本地MC服务器 ◀─TCP─ ReliableUdpChannel ─UDP打洞─▶ ReliableUdpChannel ─TCP─▶ [玩家] MC客户端
 */
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yunyi {
class TransportCore;
class ReliableUdpChannel;
class P2PCoordinator;

class P2PTunnel {
public:
    enum class State {
        Idle,
        Gathering,     // STUN 获取公网候选
        Registering,   // 上报候选到协调服务器
        Punching,      // 双向 UDP 打洞
        Connected,     // 直连建立，数据流转
        Failed         // 打洞失败
    };

    using StatusCallback = std::function<void(State, const std::string& detail)>;

    P2PTunnel();
    ~P2PTunnel();

    /**
     * @brief 初始化
     * @param transport TransportCore（需已 init）
     * @param coordinator 协调服务器客户端（App 层注入，生命周期须长于本对象）
     * @param stunHost STUN 服务器地址（如 mc.qinnai.xyz）
     * @param stunPort STUN UDP 端口（默认 3478）
     */
    bool init(TransportCore* transport, P2PCoordinator* coordinator,
              const std::string& stunHost, uint16_t stunPort = 3478);

    void setOnStatus(StatusCallback cb) { _onStatus = std::move(cb); }

    /**
     * @brief 启动 P2P 隧道
     * @param roomId 房间 ID（双方需一致）
     * @param isHost true = 房主（数据转发到本地 MC 服务器），false = 玩家
     * @param localIp 本地 MC 地址（房主连 MC 服务器；玩家监听让 MC 客户端连）
     * @param localPort 本地 MC 端口
     */
    bool start(const std::string& roomId, bool isHost,
               const std::string& localIp, uint16_t localPort);

    void stop();

    State state() const { return _state; }
    std::string candidateIp() const { return _candidateIp; }
    uint16_t candidatePort() const { return _candidatePort; }

private:
    void run();                       // 主流程线程
    bool gatherCandidate();           // STUN 获取映射
    bool registerPeer();              // POST 上报候选
    bool pollPeerCandidate();         // GET 轮询对端候选
    void punch();                     // 双向打洞
    void onConnected();               // 建立 ReliableUdpChannel + 本地 MC 转发
    void relayToMc();                 // UDP → 本地 MC TCP
    void relayFromMc();               // 本地 MC TCP → UDP
    void fail(const std::string& detail);
    void setState(State s, const std::string& detail);

    TransportCore* _transport = nullptr;
    P2PCoordinator* _coordinator = nullptr;
    std::string _stunHost;
    uint16_t _stunPort = 3478;

    std::string _roomId;
    bool _isHost = false;
    std::string _localIp;
    uint16_t _localPort = 25565;

    SOCKET _udpSock = INVALID_SOCKET;
    sockaddr_storage _peerAddr{};
    std::string _candidateIp;
    uint16_t _candidatePort = 0;

    std::atomic<bool> _stop{false};
    std::thread _thread;
    // shared_ptr 管理：ReliableUdpChannel 接收循环闭包依赖 weak_from_this 保活
    std::shared_ptr<ReliableUdpChannel> _channel;

    // 本地 MC TCP（房主：连 MC 服务器；玩家：监听 MC 客户端）
    SOCKET _mcSock = INVALID_SOCKET;
    std::thread _relayFromMcThread;

    std::mutex _stateMutex;
    State _state = State::Idle;
    StatusCallback _onStatus;
};

} // namespace yunyi
