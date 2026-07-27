/**
 * @file Director.cpp
 * @brief 引擎门面单例实现 —— pimpl 模式，所有模块的创建/销毁/协调
 */
#include "Director.h"
#include "../app/component/ConnectionCode.h"
#include "AutoreleasePool.h"
#include "FrameCodec.h"
#include "FrameDispatcher.h"
#include "NetUtil.h"
#include "PortPool.h"
#include "Ref.h"
#include "ResourcePool.h"
#include "Scheduler.h"
#include "Session.h"
#include "TlsPskContext.h"
#include "TransportCore.h"
#include "TunnelManager.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace yunyi {

// ============================================================
//  Director 实现体 (pimpl)
// ============================================================

struct DirectorImpl {
    // 核心模块
    std::unique_ptr<TransportCore>              transport;
    std::unique_ptr<Scheduler>                  scheduler;
    std::unique_ptr<PortPool>                   portPool;
    std::unique_ptr<TunnelManager>              tunnelMgr;
    std::unique_ptr<protocol::FrameDispatcher>  frameDispatcher;
    std::unique_ptr<AutoreleasePool>            autoreleasePool;

    // 配置
    EngineConfig   engineCfg;
    PortPoolConfig portCfg;
    TlsConfig      tlsCfg;
    bool           isRelay     = false;
    RelayAddress   relayAddr;
    bool           isHostAgent = false;
    std::string    publicIp;     // 中继公网 IP（WebUI 可更新）

    /** 后端日志缓冲（WebUI 轮询展示） */
    struct LogEntry { std::string time; std::string msg; };
    std::vector<LogEntry> logBuffer;
    mutable std::mutex logMutex;
    static constexpr size_t kMaxLogEntries = 100;
    /** 日志文件输出流（由 App 层注入，同时写入 .log 文件） */
    std::shared_ptr<std::ostream> logWriter;

    // 控制连接（房主端用）
    Session*                        controlSession = nullptr;
    std::unique_ptr<TlsPskContext>  controlTls;
    protocol::FrameDispatcher       controlDispatcher;

    // 中继监听 socket
    SOCKET listenSock = INVALID_SOCKET;

    // 统计
    EngineStats stats;
    mutable std::shared_mutex statsMutex;
    std::chrono::steady_clock::time_point startTime;

    // 状态
    std::atomic<bool> initialized{false};
    std::atomic<bool> running{false};

    // 错误信息
    mutable std::mutex errorMutex;
    std::string lastError;

    // 房间条目
    struct RoomEntry {
        uint32_t    roomId          = 0;
        uint16_t    assignedPort    = 0;
        uint16_t    tunnelPort       = 0;
        uint16_t    localMcPort     = 0;
        std::string roomName;
        std::string connectionCode;
        Session*    controlSession  = nullptr;
        std::shared_ptr<std::unique_ptr<TlsPskContext>> hostTls;
        SOCKET      playerListenSock = INVALID_SOCKET;
        SOCKET      tunnelListenSock = INVALID_SOCKET;
        bool        active          = true;
        bool        hostConnected   = false;
        std::string hostAddress;      // 房主控制连接的远端地址
        std::chrono::steady_clock::time_point lastHeartbeat;

        /** 已连接的玩家 */
        struct PlayerSession {
            uint32_t playerConnId = 0;
            Session* session = nullptr;
            uint64_t bytesSent = 0, bytesRecv = 0;
        };
        std::vector<PlayerSession> players;
    };
    std::unordered_map<uint32_t, RoomEntry> rooms;
    mutable std::mutex roomsMutex;
    uint32_t nextRoomId = 1;

    /** 监听 socket → 房间 ID 反向映射（accept 回调查找用） */
    std::unordered_map<SOCKET, uint32_t> listenSockToRoomId;

    /** 待发送队列 + accept 事件队列 */
    struct PendingSend {
        Session* session;
        TlsPskContext* tls;
        std::vector<uint8_t> frame;
    };
    std::vector<PendingSend> pendingSends;
    mutable std::mutex pendingSendsMutex;

    enum class AcceptEventType : uint8_t { Player, Tunnel };
    struct AcceptEvent {
        AcceptEventType type;
        uint32_t roomId;
        SOCKET clientSock;
        sockaddr_storage addr;
    };
    std::vector<AcceptEvent> acceptEvents;
    mutable std::mutex acceptEventsMutex;

    /** 自增玩家连接 ID */
    std::atomic<uint32_t> nextPlayerConnId{1};

    /** 当前正在处理帧的控制会话（用于 handler 回复路由） */
    Session* currentReplySession = nullptr;
    std::unique_ptr<TlsPskContext>* currentReplyTls = nullptr;
    /** 共享持有的 TLS context（避免 linkRoom 把 TLS 从 session onData 中移走导致崩溃） */
    std::shared_ptr<std::unique_ptr<TlsPskContext>> currentReplyTlsShared;
    /** 当前控制会话的远端地址（accept 时记录，link 时写入房间） */
    std::string currentReplyHostAddr;
};

// accept 回调前向声明（定义在文件末尾）
static void onControlAccepted(Director* director, DirectorImpl* dimpl,
    SOCKET clientSock, const sockaddr_storage& addr);
static void onPlayerAccepted(Director* director, DirectorImpl* dimpl,
    uint32_t roomId, SOCKET clientSock, const sockaddr_storage& addr);
static void onTunnelAccepted(Director* director, DirectorImpl* dimpl,
    uint32_t roomId, SOCKET clientSock, const sockaddr_storage& addr);

// ============================================================
//  单例
// ============================================================

static Director* s_instance = nullptr;
static std::mutex s_instanceMutex;

Director::Director()
    : _d(std::make_unique<DirectorImpl>())
{}

Director::~Director() = default;

Director& Director::instance() {
    // 双重检查锁定（线程安全单例）
    if (!s_instance) {
        std::lock_guard<std::mutex> lock(s_instanceMutex);
        if (!s_instance) {
            s_instance = new Director();
        }
    }
    return *s_instance;
}

// ============================================================
//  生命周期
// ============================================================

bool Director::init(const EngineConfig& cfg) {
    auto& d = _impl();
    if (d.initialized.load()) return true;

    d.engineCfg       = cfg;
    d.transport       = std::make_unique<TransportCore>();
    d.scheduler       = std::make_unique<Scheduler>();
    d.tunnelMgr       = std::make_unique<TunnelManager>();
    d.autoreleasePool = std::make_unique<AutoreleasePool>();

    // 设置全局 AutoreleasePool，此后 Ref::autorelease() 将对象加入此池
    Ref::setAutoreleasePool(d.autoreleasePool.get());

    if (!d.transport->init(cfg.ioThreads)) {
        d.lastError = "TransportCore init failed";
        return false;
    }

    d.initialized.store(true);
    d.startTime = std::chrono::steady_clock::now();
    return true;
}

void Director::shutdown() {
    auto& d = _impl();
    if (!d.initialized.load()) return;
    d.running.store(false);

    // 关闭所有房间（先收集资源再释放，避免 onClose 回调重入 roomsMutex）
    {
        std::vector<Session*> sessionsToClose;
        std::vector<SOCKET> socketsToClose;
        {
            std::lock_guard<std::mutex> lock(d.roomsMutex);
            for (auto& kv : d.rooms) {
                if (kv.second.controlSession) {
                    sessionsToClose.push_back(kv.second.controlSession);
                    kv.second.controlSession = nullptr;  // 防止 onClose 重入
                    kv.second.hostTls.reset();
                }
                if (kv.second.playerListenSock != INVALID_SOCKET) {
                    socketsToClose.push_back(kv.second.playerListenSock);
                }
                if (kv.second.tunnelListenSock != INVALID_SOCKET) {
                    socketsToClose.push_back(kv.second.tunnelListenSock);
                }
            }
            d.rooms.clear();
        }
        // 锁外关闭
        for (auto* s : sessionsToClose) s->close();
        for (auto sock : socketsToClose) d.transport->closeSocket(sock, nullptr);
    }

    // 关闭控制连接
    if (d.controlSession) {
        d.controlSession->close();
        d.controlSession = nullptr;
    }

    // 排空 autorelease 池
    if (d.autoreleasePool) {
        d.autoreleasePool->drain();
    }

    // 停止 TransportCore（阻塞等待所有 IO 完成）
    if (d.transport) {
        d.transport->shutdown();
    }

    // 析构所有子模块（顺序重要：transport 最后析构）
    d.frameDispatcher.reset();
    d.portPool.reset();
    d.tunnelMgr.reset();
    d.scheduler.reset();
    d.transport.reset();
    d.autoreleasePool.reset();

    d.initialized.store(false);
}

bool Director::isInitialized() const {
    return _impl().initialized.load();
}

// ============================================================
//  中继角色
// ============================================================

bool Director::startRelayService(
    const PortPoolConfig& ports, const TlsConfig& tls) {
    auto& d = _impl();
    if (!d.initialized.load()) {
        d.lastError = "not initialized";
        return false;
    }
    if (d.isHostAgent) {
        d.lastError = "already in host-agent mode";
        return false;
    }

    d.portCfg = ports;
    d.tlsCfg  = tls;
    d.isRelay = true;
    // publicIp 由 RelayApp 在 init 时设置（从 CLI --public-ip 或 WebUI）
    if (d.publicIp.empty()) d.publicIp = "127.0.0.1";

    d.portPool = std::make_unique<PortPool>(
        ports.rangeStart, ports.rangeEnd);
    d.frameDispatcher = std::make_unique<protocol::FrameDispatcher>();

    // 创建控制连接监听 socket
    d.listenSock = d.transport->createListenSocket(ports.controlPort);
    if (d.listenSock == INVALID_SOCKET) {
        d.lastError = "failed to create listen socket";
        return false;
    }

    // 开始接受控制连接
    auto* dimpl = &d;
    if (!d.transport->startAccept(d.listenSock,
            [this, dimpl](SOCKET clientSock, const sockaddr_storage& addr) {
                onControlAccepted(this, dimpl, clientSock, addr);
            })) {
        d.lastError = "failed to start accept on control socket";
        return false;
    }

    d.running.store(true);
    return true;
}

// ============================================================
//  房主角色
// ============================================================

bool Director::connectToRelay(
    const RelayAddress& addr, const TlsConfig& tls) {
    auto& d = _impl();
    if (!d.initialized.load()) {
        d.lastError = "not initialized";
        return false;
    }
    if (d.isRelay) {
        d.lastError = "already in relay mode";
        return false;
    }

    d.relayAddr   = addr;
    d.tlsCfg      = tls;
    d.isHostAgent = true;

    d.frameDispatcher = std::make_unique<protocol::FrameDispatcher>();
    d.controlTls = std::make_unique<TlsPskContext>();

    if (!d.controlTls->init(tls.psk, tls.pskIdentity,
            TlsRole::Client)) {
        d.lastError = "TlsPskContext init failed";
        return false;
    }

    // 同步 connect（简单实现，后续改为异步 ConnectEx）
    SOCKET sock = NetUtil::createSocket(addr.host);
    if (sock == INVALID_SOCKET) {
        d.lastError = "socket creation failed";
        return false;
    }

    sockaddr_storage remote{};
    int remoteLen = NetUtil::fillAddr(addr.host.c_str(), addr.port, remote);
    if (remoteLen == 0) {
        closesocket(sock);
        d.lastError = "invalid address";
        return false;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&remote),
            remoteLen) == SOCKET_ERROR) {
        closesocket(sock);
        d.lastError = "connect failed";
        return false;
    }

    // 同步 connect 创建的 socket 需手动绑定到 IOCP
    if (!d.transport->bindSocket(sock)) {
        closesocket(sock);
        d.lastError = "bindToIocp failed";
        return false;
    }

    d.controlSession = new Session(sock, d.transport.get());
    d.controlSession->retain();
    d.running.store(true);
    return true;
}

RoomResult Director::createRoom(
    std::string_view roomName, uint16_t localMcPort) {
    auto& d = _impl();
    RoomResult result;

    if (!d.isHostAgent || !d.controlSession) {
        result.errorCode    = "NOT_CONNECTED";
        result.errorMessage = "not connected to relay";
        return result;
    }

    // 编码 REGISTER 帧
    protocol::RegisterPayload rp;
    rp.roomName    = std::string(roomName);
    rp.localMcPort = localMcPort;

    auto frame = protocol::FrameCodec::encodeRegister(rp);

    // TLS 加密后发送
    if (d.controlTls) {
        std::vector<uint8_t> encrypted;
        if (!d.controlTls->encrypt(
                frame.data(), frame.size(), encrypted)) {
            result.errorCode    = "TLS_ENCRYPT_FAILED";
            result.errorMessage = "TLS encryption failed";
            return result;
        }
        d.controlSession->send(
            reinterpret_cast<const char*>(encrypted.data()),
            encrypted.size());
    } else {
        d.controlSession->send(
            reinterpret_cast<const char*>(frame.data()),
            frame.size());
    }

    result.ok = true;
    return result;
}

RoomResult Director::createRoomRelay(
    std::string_view roomName, uint16_t localMcPort,
    const std::string& publicIp) {
    auto& d = _impl();
    RoomResult result;

    if (!d.isRelay) {
        result.errorCode    = "NOT_RELAY";
        result.errorMessage = "not in relay mode";
        return result;
    }

    // 校验房间名
    if (roomName.empty() || roomName.size() > 64) {
        result.errorCode    = "INVALID_PARAMS";
        result.errorMessage = "roomName must be 1-64 characters";
        return result;
    }

    // 校验 MC 端口
    if (localMcPort == 0 || localMcPort > 65535) {
        result.errorCode    = "INVALID_PARAMS";
        result.errorMessage = "localMcPort must be 1-65535";
        return result;
    }

    if (!d.portPool) {
        result.errorCode    = "NO_PORT_POOL";
        result.errorMessage = "port pool not initialized";
        return result;
    }

    // 从端口池分配两个端口: playerPort + tunnelPort
    uint16_t playerPort = d.portPool->acquire();
    if (playerPort == 0) {
        result.errorCode    = "PORT_POOL_EXHAUSTED";
        result.errorMessage = "port pool exhausted";
        return result;
    }
    uint16_t tunnelPort = d.portPool->acquire();
    if (tunnelPort == 0) {
        d.portPool->release(playerPort);
        result.errorCode    = "PORT_POOL_EXHAUSTED";
        result.errorMessage = "port pool exhausted (tunnel)";
        return result;
    }

    // 玩家监听 socket
    SOCKET playerSock = d.transport->createListenSocket(playerPort);
    if (playerSock == INVALID_SOCKET) {
        d.portPool->release(playerPort);
        d.portPool->release(tunnelPort);
        result.errorCode = "LISTEN_FAILED";
        result.errorMessage = "failed to listen on port " + std::to_string(playerPort);
        return result;
    }
    // 隧道监听 socket
    SOCKET tunnelSock = d.transport->createListenSocket(tunnelPort);
    if (tunnelSock == INVALID_SOCKET) {
        d.transport->closeSocket(playerSock, nullptr);
        d.portPool->release(playerPort);
        d.portPool->release(tunnelPort);
        result.errorCode = "LISTEN_FAILED";
        result.errorMessage = "failed to listen on tunnel port " + std::to_string(tunnelPort);
        return result;
    }

    uint32_t roomId = d.nextRoomId++;
    d.listenSockToRoomId[playerSock] = roomId;
    d.listenSockToRoomId[tunnelSock] = roomId;

    // accept → 事件队列（主线程处理，避免 IOCP 线程跨模块调用崩溃）
    if (!d.transport->startAccept(playerSock,
            [roomId](SOCKET clientSock, const sockaddr_storage& addr) {
                auto& dd = Director::instance()._impl();
                DirectorImpl::AcceptEvent ev;
                ev.type = DirectorImpl::AcceptEventType::Player;
                ev.roomId = roomId;
                ev.clientSock = clientSock;
                ev.addr = addr;
                { std::lock_guard<std::mutex> lk(dd.acceptEventsMutex);
                  dd.acceptEvents.push_back(ev); }
            })) { goto cleanup_room; }
    if (!d.transport->startAccept(tunnelSock,
            [roomId](SOCKET clientSock, const sockaddr_storage& addr) {
                auto& dd = Director::instance()._impl();
                DirectorImpl::AcceptEvent ev;
                ev.type = DirectorImpl::AcceptEventType::Tunnel;
                ev.roomId = roomId;
                ev.clientSock = clientSock;
                ev.addr = addr;
                { std::lock_guard<std::mutex> lk(dd.acceptEventsMutex);
                  dd.acceptEvents.push_back(ev); }
            })) { goto cleanup_room; }

    // 登记房间
    {
        std::lock_guard<std::mutex> lock(d.roomsMutex);
        DirectorImpl::RoomEntry entry;
        entry.roomId            = roomId;
        entry.assignedPort      = playerPort;
        entry.tunnelPort         = tunnelPort;
        entry.localMcPort       = localMcPort;
        entry.roomName          = std::string(roomName);
        entry.connectionCode    = ConnectionCode::generate(publicIp, playerPort);
        entry.playerListenSock  = playerSock;
        entry.tunnelListenSock  = tunnelSock;
        entry.active            = true;
        d.rooms[roomId] = std::move(entry);
    }

    result.ok             = true;
    result.roomId         = roomId;
    result.assignedPort   = playerPort;
    result.connectionCode = ConnectionCode::generate(publicIp, playerPort);
    return result;

cleanup_room:
    d.transport->closeSocket(playerSock, nullptr);
    d.transport->closeSocket(tunnelSock, nullptr);
    d.listenSockToRoomId.erase(playerSock);
    d.listenSockToRoomId.erase(tunnelSock);
    d.portPool->release(playerPort);
    d.portPool->release(tunnelPort);
    result.errorCode    = "ACCEPT_FAILED";
    result.errorMessage = "failed to start accept";
    return result;
}

bool Director::closeRoom(uint32_t roomId) {
    auto& d = _impl();
    if (!d.isHostAgent || !d.controlSession) return false;

    auto frame = protocol::FrameCodec::encodeDeregister();

    if (d.controlTls) {
        std::vector<uint8_t> encrypted;
        if (!d.controlTls->encrypt(
                frame.data(), frame.size(), encrypted)) {
            return false;
        }
        return d.controlSession->send(
            reinterpret_cast<const char*>(encrypted.data()),
            encrypted.size());
    }

    return d.controlSession->send(
        reinterpret_cast<const char*>(frame.data()),
        frame.size());
}

// ============================================================
//  房间查询（中继角色）
// ============================================================

RoomInfo Director::getRoomInfo(uint32_t roomId) const {
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.roomsMutex);

    RoomInfo info;
    auto it = d.rooms.find(roomId);
    if (it == d.rooms.end()) return info;

    info.roomId         = it->second.roomId;
    info.roomName       = it->second.roomName;
    info.connectionCode = it->second.connectionCode;
    info.assignedPort   = it->second.assignedPort;
    info.localMcPort    = it->second.localMcPort;
    info.status         = it->second.active ? "active" : "closed";
    info.hostAddress    = it->second.hostAddress;
    return info;
}

std::vector<RoomInfo> Director::listRooms() const {
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.roomsMutex);

    std::vector<RoomInfo> result;
    for (const auto& kv : d.rooms) {
        if (!kv.second.active) continue;
        RoomInfo info;
        info.roomId         = kv.second.roomId;
        info.roomName       = kv.second.roomName;
        info.connectionCode = kv.second.connectionCode;
        info.assignedPort   = kv.second.assignedPort;
        info.localMcPort    = kv.second.localMcPort;
        info.status         = kv.second.hostConnected ? "active" : "waiting";
        info.playerCount    = static_cast<uint32_t>(kv.second.players.size());
        info.hostAddress    = kv.second.hostAddress;
        result.push_back(info);
    }
    return result;
}

std::vector<PlayerInfo> Director::getRoomPlayers(uint32_t roomId) const {
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.roomsMutex);

    std::vector<PlayerInfo> result;
    auto it = d.rooms.find(roomId);
    if (it == d.rooms.end()) return result;

    for (const auto& ps : it->second.players) {
        PlayerInfo info;
        info.id          = ps.playerConnId;
        info.bytesSent   = ps.bytesSent;
        info.bytesRecv   = ps.bytesRecv;
        result.push_back(info);
    }
    return result;
}

uint32_t Director::touchRoomHeartbeat() {
    auto& d = _impl();
    if (!d.currentReplySession) return 0;

    std::lock_guard<std::mutex> lock(d.roomsMutex);
    // 第一优先：精确匹配 controlSession
    for (auto& kv : d.rooms) {
        if (kv.second.controlSession == d.currentReplySession) {
            kv.second.lastHeartbeat = std::chrono::steady_clock::now();
            return kv.second.roomId;
        }
    }
    // 第二优先：匹配 hostAddress（重连后 session 变了但 IP 没变）
    if (!d.currentReplyHostAddr.empty()) {
        for (auto& kv : d.rooms) {
            if (kv.second.hostAddress == d.currentReplyHostAddr
                && kv.second.hostConnected) {
                // 更新 controlSession 指向（重连后 session 变了）
                kv.second.controlSession = d.currentReplySession;
                kv.second.lastHeartbeat = std::chrono::steady_clock::now();
                return kv.second.roomId;
            }
        }
    }
    return 0;
}

uint32_t Director::findRoomByHostAddr(const std::string& hostAddr) const {
    if (hostAddr.empty()) return 0;
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.roomsMutex);
    for (auto& kv : d.rooms) {
        if (kv.second.hostAddress == hostAddr && kv.second.active) {
            return kv.second.roomId;
        }
    }
    return 0;
}

std::string Director::getCurrentReplyHostAddr() const {
    return _impl().currentReplyHostAddr;
}

uint32_t Director::findRoomByControlSession(Session* session) const {
    auto& d = _impl();
    if (!session) return 0;

    std::lock_guard<std::mutex> lock(d.roomsMutex);
    for (auto& kv : d.rooms) {
        if (kv.second.controlSession == session && kv.second.active) {
            return kv.second.roomId;
        }
    }
    return 0;
}

bool Director::forceCloseRoom(uint32_t roomId) {
    auto& d = _impl();

    // ── 先收集需要释放的资源（持锁），再在锁外执行 close 操作 ──
    // 避免 close → onClose 回调 → re-lock roomsMutex 死锁
    Session* ctrlSession = nullptr;
    SOCKET playerSock = INVALID_SOCKET;
    SOCKET tunnelSock = INVALID_SOCKET;
    uint16_t playerPort = 0;
    uint16_t tunnelPort = 0;

    {
        std::lock_guard<std::mutex> lock(d.roomsMutex);
        auto it = d.rooms.find(roomId);
        if (it == d.rooms.end()) return false;

        ctrlSession = it->second.controlSession;
        playerSock  = it->second.playerListenSock;
        tunnelSock  = it->second.tunnelListenSock;
        playerPort  = it->second.assignedPort;
        tunnelPort  = it->second.tunnelPort;

        // 清除房间持有的指针，防止 onClose 回调中再次操作已移除的房间
        it->second.controlSession = nullptr;
        it->second.hostTls.reset();

        // 从反向索引中移除
        if (playerSock != INVALID_SOCKET) d.listenSockToRoomId.erase(playerSock);
        if (tunnelSock != INVALID_SOCKET) d.listenSockToRoomId.erase(tunnelSock);

        it->second.active = false;
        d.rooms.erase(it);
    }
    // ── 锁已释放，安全执行可能触发回调的操作 ──

    // 回收端口
    if (d.portPool) {
        if (playerPort) d.portPool->release(playerPort);
        if (tunnelPort) d.portPool->release(tunnelPort);
    }
    // 关闭监听 socket
    if (playerSock != INVALID_SOCKET) {
        d.transport->closeSocket(playerSock, nullptr);
    }
    if (tunnelSock != INVALID_SOCKET) {
        d.transport->closeSocket(tunnelSock, nullptr);
    }
    // 关闭控制连接（onClose 回调可安全获取 roomsMutex）
    if (ctrlSession) {
        ctrlSession->close();
    }

    return true;
}

// ============================================================
//  统计与诊断
// ============================================================

EngineStats Director::getStats() const {
    auto& d = _impl();
    EngineStats s;
    s.portPoolUsed     = static_cast<uint32_t>(
        d.portPool ? d.portPool->usedCount() : 0);
    s.portPoolTotal    = static_cast<uint32_t>(
        d.portPool ? d.portPool->totalCount() : 0);
    s.activeTunnels    = static_cast<uint32_t>(
        d.tunnelMgr ? d.tunnelMgr->activeCount() : 0);

    // 从房间表计算活跃房间数
    {
        std::lock_guard<std::mutex> lock(d.roomsMutex);
        s.activeRooms  = static_cast<uint32_t>(d.rooms.size());
        s.totalRooms   = d.nextRoomId > 0 ? d.nextRoomId - 1 : 0;
    }
    s.totalConnections = d.nextPlayerConnId.load() > 0
        ? d.nextPlayerConnId.load() - 1 : 0;

    // 累计转发字节数（由隧道管理器维护）
    s.bytesRelayed = static_cast<uint64_t>(
        d.tunnelMgr ? d.tunnelMgr->totalBytesRelayed() : 0);

    // 运行时长: 用 init 时记录的起点
    s.uptimeSeconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - d.startTime).count());

    return s;
}

std::string Director::getLastError() const {
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.errorMutex);
    return d.lastError;
}

protocol::FrameDispatcher* Director::frameDispatcher() {
    return _impl().frameDispatcher.get();
}

Scheduler* Director::scheduler() {
    return _impl().scheduler.get();
}

TransportCore* Director::transport() {
    return _impl().transport.get();
}

void Director::setAsHostAgent() {
    _impl().isHostAgent = true;
}

bool Director::sendToControlSession(
    Session* session, TlsPskContext* tls,
    const std::vector<uint8_t>& frame) {
    if (!session || !tls) return false;
    std::vector<uint8_t> encrypted;
    if (!tls->encrypt(frame.data(), frame.size(), encrypted))
        return false;
    return session->send(
        reinterpret_cast<const char*>(encrypted.data()),
        encrypted.size());
}

bool Director::sendReplyFrame(const std::vector<uint8_t>& frame) {
    auto& d = _impl();
    if (!d.currentReplySession || !d.currentReplyTls || !*d.currentReplyTls)
        return false;
    return sendToControlSession(
        d.currentReplySession, (*d.currentReplyTls).get(), frame);
}

bool Director::linkCurrentSessionToRoom(uint32_t roomId) {
    auto& d = _impl();
    if (!d.currentReplySession || !d.currentReplyTls || !*d.currentReplyTls)
        return false;

    // 共享 TLS 所有权（不移动！tlsShared 仍被控制连接 onData 引用，移动会导致心跳崩溃）
    bool ok = linkRoomToHost(roomId, d.currentReplySession, d.currentReplyTlsShared);
    if (ok) {
        d.currentReplySession->retain();  // 房间持有引用
        // 保存房主地址
        std::lock_guard<std::mutex> lock(d.roomsMutex);
        auto it = d.rooms.find(roomId);
        if (it != d.rooms.end()) {
            it->second.hostAddress = d.currentReplyHostAddr;
        }
    }
    return ok;
}

bool Director::linkRoomToHost(
    uint32_t roomId, Session* session,
    std::shared_ptr<std::unique_ptr<TlsPskContext>> tls) {
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.roomsMutex);

    auto it = d.rooms.find(roomId);
    if (it == d.rooms.end()) return false;
    if (it->second.hostConnected) return false;  // 已有房主

    it->second.controlSession = session;
    it->second.hostTls = tls;  // 共享 shared_ptr（不移动，控制连接 onData 仍可用）
    it->second.hostConnected = true;
    it->second.lastHeartbeat = std::chrono::steady_clock::now();
    return true;
}

TunnelManager* Director::tunnelMgr() {
    return _impl().tunnelMgr.get();
}

bool Director::pairHostTunnel(uint32_t playerConnId, Session* hostTunnelSession) {
    auto& d = _impl();
    if (!d.tunnelMgr) return false;

    auto* tunnel = d.tunnelMgr->pairTunnel(playerConnId, hostTunnelSession);
    if (!tunnel) return false;

    // 设置隧道数据转发: hostTunnel → playerSession
    hostTunnelSession->setOnData(
        [&d, playerConnId](SOCKET, const char* data, size_t len) {
            auto* t = d.tunnelMgr->findTunnel(playerConnId);
            if (!t || !t->playerSession) return;
            t->playerSession->send(data, len);
            t->bytesRelayed += len;
        });

    hostTunnelSession->setOnClose(
        [&d, playerConnId](SOCKET, int) {
            d.tunnelMgr->removeTunnel(playerConnId);
        });

    hostTunnelSession->start();
    return true;
}

// ============================================================
//  pimpl 访问器
// ============================================================

DirectorImpl& Director::_impl() {
    return *_d;
}

const DirectorImpl& Director::_impl() const {
    return *_d;
}

void Director::enqueueControlSend(
    Session* session, TlsPskContext* tls,
    const std::vector<uint8_t>& frame) {
    auto& d = _impl();
    DirectorImpl::PendingSend ps;
    ps.session = session;
    ps.tls = tls;
    ps.frame = frame;
    std::lock_guard<std::mutex> lock(d.pendingSendsMutex);
    d.pendingSends.push_back(std::move(ps));
}

void Director::flushPendingSends() {
    auto& d = _impl();

    // 处理 accept 事件（从 IOCP 线程入队，主线程处理）
    {
        std::vector<DirectorImpl::AcceptEvent> events;
        {
            std::lock_guard<std::mutex> lock(d.acceptEventsMutex);
            events.swap(d.acceptEvents);
        }
        for (auto& ev : events) {
            try {
                if (ev.type == DirectorImpl::AcceptEventType::Player) {
                    onPlayerAccepted(this, &d, ev.roomId, ev.clientSock, ev.addr);
                } else {
                    onTunnelAccepted(this, &d, ev.roomId, ev.clientSock, ev.addr);
                }
            } catch (const std::exception& e) {
                std::cerr << "[Director] CRASH in accept handler: " << e.what() << std::endl;
                addLog("接受连接时崩溃: " + std::string(e.what()));
                d.transport->closeSocket(ev.clientSock, nullptr);
            } catch (...) {
                std::cerr << "[Director] CRASH in accept handler (unknown)" << std::endl;
                addLog("接受连接时崩溃: unknown exception");
                d.transport->closeSocket(ev.clientSock, nullptr);
            }
        }
    }

    // 发送待发队列
    std::vector<DirectorImpl::PendingSend> batch;
    {
        std::lock_guard<std::mutex> lock(d.pendingSendsMutex);
        batch.swap(d.pendingSends);
    }
    for (auto& ps : batch) {
        if (ps.session && ps.tls) {
            sendToControlSession(ps.session, ps.tls, ps.frame);
        }
    }
}

void Director::setPublicIp(const std::string& ip) {
    auto& d = _impl();
    std::string oldIp = d.publicIp;
    d.publicIp = ip;
    std::cout << "[Director] Public IP updated: " << oldIp << " -> " << ip << std::endl;
    addLog("公网IP已更新: " + oldIp + " -> " + ip);
}

std::string Director::getPublicIp() const {
    return _impl().publicIp;
}

void Director::setLogWriter(std::shared_ptr<std::ostream> writer) {
    _impl().logWriter = std::move(writer);
}

void Director::addLog(const std::string& msg) {
    auto& d = _impl();
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

    // 写入内存缓冲区（WebUI 轮询用）
    {
        std::lock_guard<std::mutex> lock(d.logMutex);
        d.logBuffer.push_back({buf, msg});
        if (d.logBuffer.size() > DirectorImpl::kMaxLogEntries)
            d.logBuffer.erase(d.logBuffer.begin());
    }

    // 同时写入 .log 文件
    if (d.logWriter && d.logWriter->good()) {
        char timeFull[24];
        snprintf(timeFull, sizeof(timeFull), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        (*d.logWriter) << timeFull << " | " << msg << "\n" << std::flush;
    }
}

std::vector<std::pair<std::string, std::string>>
Director::getRecentLogs(size_t limit) {
    auto& d = _impl();
    std::lock_guard<std::mutex> lock(d.logMutex);
    std::vector<std::pair<std::string, std::string>> result;
    // limit=0 表示返回全部（缓冲区已上限 100 条）
    size_t start = (limit == 0 || d.logBuffer.size() <= limit)
        ? 0 : d.logBuffer.size() - limit;
    for (size_t i = start; i < d.logBuffer.size(); ++i)
        result.emplace_back(d.logBuffer[i].time, d.logBuffer[i].msg);
    return result;
}

// ============================================================
//  Accept 回调（静态函数，避免在头文件暴露 SOCKET 类型）
// ============================================================

static void onControlAccepted(Director* director, DirectorImpl* dimpl, SOCKET clientSock,
                               const sockaddr_storage& addr) {
    std::string peer = TransportCore::addrToString(addr) + ":"
        + std::to_string(TransportCore::addrToPort(addr));
    std::cout << "[Director] Control connection accepted from " << peer << std::endl;
    director->addLog("控制连接接入: " + peer);
    auto tls = std::make_unique<TlsPskContext>();
    if (!tls->init(dimpl->tlsCfg.psk, dimpl->tlsCfg.pskIdentity, TlsRole::Server)) {
        std::cerr << "[Director] TLS server init failed for " << peer << std::endl;
        dimpl->transport->closeSocket(clientSock, nullptr);
        return;
    }

    // 创建 Session
    auto* session = new Session(clientSock, dimpl->transport.get());
    session->retain();

    // 使用 shared_ptr 管理握手状态（lambda 捕获）
    auto handshakeDone = std::make_shared<bool>(false);
    auto tlsPtr = std::move(tls);  // 移到 shared_ptr 供 lambda 持有
    auto tlsShared = std::make_shared<std::unique_ptr<TlsPskContext>>(
        std::move(tlsPtr));
    auto recvBuf = std::make_shared<std::vector<uint8_t>>();

    auto peerShared = std::make_shared<std::string>(peer);
    session->setOnData([director, dimpl, tlsShared, handshakeDone, recvBuf, session, peerShared]
        (SOCKET, const char* data, size_t len) {
        auto& tls = *tlsShared;

        if (!*handshakeDone) {
            // ── TLS 握手阶段 ──
            std::vector<uint8_t> handshakeOutput;
            auto result = tls->doHandshake(
                reinterpret_cast<const uint8_t*>(data), len, handshakeOutput);

            if (!handshakeOutput.empty()) {
                session->send(
                    reinterpret_cast<const char*>(handshakeOutput.data()),
                    handshakeOutput.size());
            }

            if (result == TlsPskContext::HandshakeResult::Done) {
                *handshakeDone = true;
                std::cout << "[Director] TLS handshake done" << std::endl;
            } else if (result == TlsPskContext::HandshakeResult::Failed) {
                std::cerr << "[Director] TLS handshake failed" << std::endl;
                session->close();
            }
            return;
        }

        // ── 握手完成，解密 + 帧分发 ──
        std::vector<uint8_t> plaintext;
        if (!tls->decrypt(
                reinterpret_cast<const uint8_t*>(data), len, plaintext)) {
            return;
        }
        if (plaintext.empty()) return;

        recvBuf->insert(recvBuf->end(), plaintext.begin(), plaintext.end());

        auto* dispatcher = dimpl->frameDispatcher.get();

        // 循环解析帧
        while (recvBuf->size() >= protocol::kHeaderSize) {
            if ((*recvBuf)[0] != protocol::kMagic) {
                recvBuf->clear();
                return;
            }

            protocol::FrameType type;
            uint8_t version;
            uint32_t payloadLen;
            if (!protocol::FrameCodec::decodeHeader(
                    recvBuf->data(), type, version, payloadLen)) {
                recvBuf->clear();
                return;
            }

            if (version != protocol::kVersion) {
                auto errFrame = protocol::FrameCodec::encodeError(
                    protocol::ErrorCode::PROTOCOL_VERSION_MISMATCH,
                    "version mismatch");
                director->sendToControlSession(session, tls.get(), errFrame);
                session->close();
                return;
            }

            size_t frameSize = protocol::kHeaderSize + payloadLen;
            if (recvBuf->size() < frameSize) return;  // 等更多数据

            // 设置回复路由
            dimpl->currentReplySession = session;
            dimpl->currentReplyTls = tlsShared.get();
            dimpl->currentReplyTlsShared = tlsShared;
            dimpl->currentReplyHostAddr = *peerShared;

            // 分发帧
            if (dispatcher) {
                dispatcher->dispatch(type,
                    recvBuf->data() + protocol::kHeaderSize, payloadLen);
            }

            dimpl->currentReplySession = nullptr;
            dimpl->currentReplyTls = nullptr;
            dimpl->currentReplyTlsShared.reset();
            dimpl->currentReplyHostAddr.clear();

            recvBuf->erase(recvBuf->begin(), recvBuf->begin() + frameSize);
        }
    });

    session->setOnClose([dimpl, session](SOCKET, int err) {
        (void)err;
        // 清理：如果已关联房间，标记 hostConnected = false
        std::lock_guard<std::mutex> lock(dimpl->roomsMutex);
        for (auto& kv : dimpl->rooms) {
            if (kv.second.controlSession == session) {
                std::cout << "[Director] Host disconnected from room "
                          << kv.second.roomId << std::endl;
                kv.second.hostConnected = false;
                kv.second.controlSession = nullptr;
                kv.second.hostTls.reset();
                break;
            }
        }
    });

    session->start();
}

static void onPlayerAccepted(Director* director, DirectorImpl* dimpl, uint32_t roomId,
                               SOCKET clientSock, const sockaddr_storage& addr) {
    std::string peer = TransportCore::addrToString(addr) + ":"
        + std::to_string(TransportCore::addrToPort(addr));
    std::cout << "[Director] Player connection accepted for room #"
              << roomId << " from " << peer << std::endl;

    if (director) director->addLog("玩家连接: #" + std::to_string(roomId) + " " + peer);

    // 查找房间并复制关键字段（锁内完成，防止房间被清理时野指针）
    bool hostConnected = false;
    Session* controlSession = nullptr;
    TlsPskContext* hostTls = nullptr;
    uint16_t tunnelPort = 0;
    {
        std::lock_guard<std::mutex> lock(dimpl->roomsMutex);
        auto it = dimpl->rooms.find(roomId);
        if (it == dimpl->rooms.end() || !it->second.active) {
            dimpl->transport->closeSocket(clientSock, nullptr);
            return;
        }
        hostConnected  = it->second.hostConnected;
        controlSession = it->second.controlSession;
        hostTls        = it->second.hostTls ? (*it->second.hostTls).get() : nullptr;
        tunnelPort     = it->second.tunnelPort;
    }

    // 检查房主是否在线
    if (!hostConnected || !controlSession) {
        std::cerr << "[Director] Room #" << roomId
                  << " has no host, rejecting player" << std::endl;
        dimpl->transport->closeSocket(clientSock, nullptr);
        return;
    }

    // 创建玩家 Session
    auto* playerSession = new Session(clientSock, dimpl->transport.get());
    playerSession->retain();

    // 分配玩家连接 ID
    uint32_t playerConnId = dimpl->nextPlayerConnId.fetch_add(1);

    // 创建待配对隧道
    if (dimpl->tunnelMgr) {
        dimpl->tunnelMgr->createPendingTunnel(playerConnId, playerSession);
    }

    // 入队 OPEN_STREAM（主循环 flushPendingSends 时发送）
    if (controlSession && hostTls) {
        protocol::OpenStreamPayload osp;
        osp.playerConnId = playerConnId;
        osp.tunnelPort   = tunnelPort;
        osp.flags = 0;
        director->enqueueControlSend(
            controlSession, hostTls,
            protocol::FrameCodec::encodeOpenStream(osp));
    }

    // 设置玩家数据转发回调（含缓冲：隧道就绪前的数据暂存，就绪后发送）
    auto* dimplForLambda = dimpl;
    playerSession->setOnData([dimplForLambda, playerConnId, director, roomId](SOCKET, const char* data, size_t len) {
        auto* tunnel = dimplForLambda->tunnelMgr ? dimplForLambda->tunnelMgr->findTunnel(playerConnId) : nullptr;
        if (!tunnel) return;

        if (!tunnel->paired) {
            tunnel->pendingPlayerData.push_back(std::vector<uint8_t>(data, data + len));
            return;
        }

        if (!tunnel->tunnelSession) return;

        // 隧道已配对：先发缓冲，再发当前
        auto sendOne = [tunnel](const uint8_t* d, size_t l) {
            if (!tunnel->tunnelTls || !tunnel->tunnelTls->get()) return;
            std::lock_guard<std::mutex> lock(*tunnel->tlsMutex);
            std::vector<uint8_t> enc;
            if ((*tunnel->tunnelTls)->encrypt(d, l, enc)) {
                tunnel->tunnelSession->send(reinterpret_cast<const char*>(enc.data()), enc.size());
                tunnel->bytesRelayed += l;
            }
        };
        for (auto& buf : tunnel->pendingPlayerData) sendOne(buf.data(), buf.size());
        tunnel->pendingPlayerData.clear();
        sendOne(reinterpret_cast<const uint8_t*>(data), len);
    });

    playerSession->setOnClose([dimplForLambda, playerConnId](SOCKET, int /*err*/) {
        if (dimplForLambda->tunnelMgr) {
            dimplForLambda->tunnelMgr->removeTunnel(playerConnId);
        }
    });

    playerSession->start();

    // 记录到房间玩家列表（重新加锁查找）
    {
        std::lock_guard<std::mutex> lock(dimpl->roomsMutex);
        auto it = dimpl->rooms.find(roomId);
        if (it != dimpl->rooms.end() && it->second.active) {
            DirectorImpl::RoomEntry::PlayerSession ps;
            ps.playerConnId = playerConnId;
            ps.session = playerSession;
            it->second.players.push_back(ps);
        }
    }

    std::cout << "[Director] Player #" << playerConnId
              << " attached to room #" << roomId << std::endl;
}

static void onTunnelAccepted(Director* director, DirectorImpl* dimpl,
                             uint32_t roomId, SOCKET clientSock,
                             const sockaddr_storage& addr) {
    std::string peer = TransportCore::addrToString(addr) + ":"
        + std::to_string(TransportCore::addrToPort(addr));
    std::cout << "[Director] Tunnel connection accepted for room #"
              << roomId << " from " << peer << std::endl << std::flush;

    if (director) director->addLog("隧道连接: #" + std::to_string(roomId) + " " + peer);

    // 创建 TLS 服务端上下文
    auto tls = std::make_unique<TlsPskContext>();
    if (!tls->init(dimpl->tlsCfg.psk, dimpl->tlsCfg.pskIdentity, TlsRole::Server)) {
        std::cerr << "[Director] TLS server init failed for tunnel" << std::endl;
        dimpl->transport->closeSocket(clientSock, nullptr);
        return;
    }

    // 创建隧道 Session
    auto* tunnelSession = new Session(clientSock, dimpl->transport.get());
    tunnelSession->retain();

    // TLS 握手 + 配对状态
    auto handshakeDone = std::make_shared<bool>(false);
    auto connIdDone = std::make_shared<bool>(false);
    auto pairedConnId = std::make_shared<uint32_t>(0);
    auto connIdBuf = std::make_shared<std::vector<uint8_t>>();
    auto tlsShared = std::make_shared<std::unique_ptr<TlsPskContext>>(std::move(tls));
    auto tlsSendMutex = std::make_shared<std::mutex>();  // 保护 encrypt/decrypt 并发

    tunnelSession->setOnData([dimpl, tlsShared, handshakeDone, connIdDone,
                              pairedConnId, connIdBuf, tunnelSession, tlsSendMutex, director]
        (SOCKET, const char* data, size_t len) {
        try {
        auto& tls = *tlsShared;

        if (!*handshakeDone) {
            // ── TLS 握手阶段 ──
            std::vector<uint8_t> handshakeOutput;
            auto result = tls->doHandshake(
                reinterpret_cast<const uint8_t*>(data), len, handshakeOutput);

            if (!handshakeOutput.empty()) {
                tunnelSession->send(
                    reinterpret_cast<const char*>(handshakeOutput.data()),
                    handshakeOutput.size());
            }

            if (result == TlsPskContext::HandshakeResult::Done) {
                *handshakeDone = true;
                std::cout << "[Director] Tunnel TLS handshake done, waiting for playerConnId..."
                          << std::endl;
                if (director) director->addLog("TUN_HS_DONE");
            } else if (result == TlsPskContext::HandshakeResult::Failed) {
                std::cerr << "[Director] Tunnel TLS handshake failed" << std::endl;
                tunnelSession->close();
            }
            return;
        }

        // ── 握手完成 → 解密数据（加锁保护 SSL 对象并发）──
        std::vector<uint8_t> plaintext;
        {
            auto* t = dimpl->tunnelMgr && *pairedConnId != 0
                ? dimpl->tunnelMgr->findTunnel(*pairedConnId) : nullptr;
            std::unique_lock<std::mutex> lock(t && t->tlsMutex ? *t->tlsMutex : *tlsSendMutex);
            if (!tls->decrypt(
                    reinterpret_cast<const uint8_t*>(data), len, plaintext))
                return;
        }

        // 第一包数据 = playerConnId（4 字节大端）
        if (!*connIdDone) {
            connIdBuf->insert(connIdBuf->end(), plaintext.begin(), plaintext.end());
            if (connIdBuf->size() >= 4) {
                uint32_t pid =
                    (static_cast<uint32_t>((*connIdBuf)[0]) << 24) |
                    (static_cast<uint32_t>((*connIdBuf)[1]) << 16) |
                    (static_cast<uint32_t>((*connIdBuf)[2]) << 8)  |
                    static_cast<uint32_t>((*connIdBuf)[3]);
                *pairedConnId = pid;
                *connIdDone = true;

                std::cout << "[Director] Tunnel identified: playerConnId="
                          << pid << ", pairing..." << std::endl;

                // 配对隧道
                if (dimpl->tunnelMgr) {
                    if (director) director->addLog("TUN_PAIR pid=" + std::to_string(pid));
                    auto* t = dimpl->tunnelMgr->pairTunnel(pid, tunnelSession);
                    if (t) {
                        t->tunnelTls = tlsShared;
                        t->tlsMutex  = tlsSendMutex;
                        if (director) director->addLog("TUN_PAIRED: pending=" + std::to_string(t->pendingPlayerData.size()));
                        // 立即发送配对前缓冲的玩家数据
                        for (auto& buf : t->pendingPlayerData) {
                            if (!tlsShared || !tlsShared->get()) { director->addLog("FLUSH_NO_TLS"); continue; }
                            std::vector<uint8_t> enc;
                            if ((*tlsShared)->encrypt(buf.data(), buf.size(), enc)) {
                                tunnelSession->send(reinterpret_cast<const char*>(enc.data()), enc.size());
                                t->bytesRelayed += buf.size();
                            } else {
                                director->addLog("FLUSH_ENC_FAIL");
                            }
                        }
                        t->pendingPlayerData.clear();
                    }
                    std::cout << "[Director] Tunnel paired: " << pid << std::endl;
                }

                // 剩余数据（MC 握手等）转发给玩家
                if (connIdBuf->size() > 4) {
                    auto* t = dimpl->tunnelMgr
                        ? dimpl->tunnelMgr->findTunnel(pid) : nullptr;
                    if (t && t->playerSession) {
                        t->playerSession->send(
                            reinterpret_cast<const char*>(connIdBuf->data() + 4),
                            connIdBuf->size() - 4);
                    }
                }
            }
            return;
        }

        // 后续数据: TLS 解密后的 MC 字节流 → 转发给玩家
        auto* t = dimpl->tunnelMgr
            ? dimpl->tunnelMgr->findTunnel(*pairedConnId) : nullptr;
        if (t && t->playerSession) {
            static int fwdCount2 = 0;
            if (++fwdCount2 <= 2) {
                director->addLog("Host->Player: " + std::to_string(plaintext.size()) + "B");
            }
            t->playerSession->send(
                reinterpret_cast<const char*>(plaintext.data()),
                plaintext.size());
            t->bytesRelayed += plaintext.size();
        }
        } catch (const std::exception& e) {
            if (director) director->addLog("TUN_CB_ERR: " + std::string(e.what()));
        } catch (...) {
            if (director) director->addLog("TUN_CB_CRASH");
        }
    });

    tunnelSession->setOnClose([dimpl, pairedConnId](SOCKET, int) {
        if (dimpl->tunnelMgr && *pairedConnId != 0) {
            dimpl->tunnelMgr->removeTunnel(*pairedConnId);
        }
    });

    tunnelSession->start();
}

} // namespace yunyi
