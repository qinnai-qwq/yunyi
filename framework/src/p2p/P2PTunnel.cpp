/**
 * @file P2PTunnel.cpp
 * @brief P2P 打洞隧道实现
 */
#include "yunyi/p2p/P2PTunnel.h"
#include "yunyi/p2p/P2PCoordinator.h"
#include "yunyi/util/StunClient.h"
#include "yunyi/udp/ReliableUdpChannel.h"
#include "yunyi/eventloop/TransportCore.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include <chrono>
#include <cstring>
#include <thread>

namespace yunyi {

namespace {
/** 打洞探测包标志（ReliableUdpChannel 不使用此标志，收到会忽略） */
constexpr uint8_t kPunchFlag = 0x08;
/** 打洞探测包：1B 标志 + 4B magic */
constexpr uint32_t kPunchMagic = 0x594E55;  // "YNU"

std::string packPunch() {
    std::string p;
    p.push_back(static_cast<char>(kPunchFlag));
    uint32_t magic = htonl(kPunchMagic);
    p.append(reinterpret_cast<const char*>(&magic), 4);
    return p;
}
bool isPunch(const char* data, size_t len) {
    if (!data || len < 5) return false;
    if (static_cast<uint8_t>(data[0]) != kPunchFlag) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data + 1, 4);
    return ntohl(magic) == kPunchMagic;
}
} // anonymous namespace

P2PTunnel::P2PTunnel() = default;

P2PTunnel::~P2PTunnel() {
    stop();
}

bool P2PTunnel::init(TransportCore* transport, P2PCoordinator* coordinator,
                     const std::string& stunHost, uint16_t stunPort) {
    if (!transport || !coordinator || stunHost.empty()) return false;
    _transport = transport;
    _coordinator = coordinator;
    _stunHost = stunHost;
    _stunPort = stunPort;
    return true;
}

void P2PTunnel::setState(State s, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _state = s;
    }
    if (_onStatus) _onStatus(s, detail);
}

void P2PTunnel::fail(const std::string& detail) {
    setState(State::Failed, detail);
}

bool P2PTunnel::start(const std::string& roomId, bool isHost,
                      const std::string& localIp, uint16_t localPort) {
    if (_thread.joinable()) return false;
    _roomId = roomId;
    _isHost = isHost;
    _localIp = localIp;
    _localPort = localPort;
    _stop.store(false, std::memory_order_release);
    _thread = std::thread(&P2PTunnel::run, this);
    return true;
}

void P2PTunnel::stop() {
    _stop.store(true, std::memory_order_release);
    if (_thread.joinable()) _thread.join();
    if (_relayFromMcThread.joinable()) _relayFromMcThread.join();
    _channel.reset();
    if (_mcSock != INVALID_SOCKET) {
        closesocket(_mcSock);
        _mcSock = INVALID_SOCKET;
    }
    if (_udpSock != INVALID_SOCKET) {
        closesocket(_udpSock);
        _udpSock = INVALID_SOCKET;
    }
    setState(State::Idle, "已停止");
}

bool P2PTunnel::gatherCandidate() {
    setState(State::Gathering, "正在获取公网候选端点...");
    if (!StunClient::query(_stunHost.c_str(), _stunPort, _candidateIp, _candidatePort)) {
        fail("STUN 获取公网映射失败");
        return false;
    }
    return true;
}

bool P2PTunnel::registerPeer() {
    setState(State::Registering, "正在上报候选到协调服务器...");
    if (!_coordinator->registerPeer(_roomId, _candidateIp, _candidatePort)) {
        fail("协调服务器注册失败");
        return false;
    }
    return true;
}

bool P2PTunnel::pollPeerCandidate() {
    std::string peerIp;
    uint16_t peerPort = 0;
    if (!_coordinator->pollPeer(_roomId, peerIp, peerPort)) return false;

    // 存对端候选
    _peerAddr = {};
    if (peerIp.find(':') != std::string::npos) {
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&_peerAddr);
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(peerPort);
        inet_pton(AF_INET6, peerIp.c_str(), &a6->sin6_addr);
    } else {
        auto* a4 = reinterpret_cast<sockaddr_in*>(&_peerAddr);
        a4->sin_family = AF_INET;
        a4->sin_port = htons(peerPort);
        inet_pton(AF_INET, peerIp.c_str(), &a4->sin_addr);
    }
    return true;
}

void P2PTunnel::punch() {
    setState(State::Punching, "正在向对端打洞...");
    // UDP socket 已由 run() 创建，这里同步打洞
    std::string ping = packPunch();
    int peerLen = (_peerAddr.ss_family == AF_INET6)
        ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);

    DWORD tmo = 500;
    setsockopt(_udpSock, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&tmo), sizeof(tmo));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool peerConfirmed = false;

    while (!_stop.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        // 向对端发探测包
        sendto(_udpSock, ping.data(), static_cast<int>(ping.size()), 0,
            reinterpret_cast<sockaddr*>(&_peerAddr), peerLen);
        // 等待对端探测包
        char buf[256];
        sockaddr_storage from{};
        int fromLen = sizeof(from);
        int n = recvfrom(_udpSock, buf, sizeof(buf), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n >= 5 && isPunch(buf, static_cast<size_t>(n))) {
            // 收到对端探测包 → NAT 双向映射建立
            _peerAddr = from;
            peerConfirmed = true;
            break;
        }
    }

    if (!peerConfirmed && !_stop.load(std::memory_order_acquire)) {
        fail("打洞超时，NAT 可能不支持直连");
        return;
    }
    onConnected();
}

void P2PTunnel::onConnected() {
    setState(State::Connected, "直连建立，开始数据转发");

    // 用同一 UDP socket 建立可靠通道（shared_ptr：ReliableUdpChannel 接收循环需要 weak_from_this）
    _channel = std::make_shared<ReliableUdpChannel>();
    _channel->setOnData([this](const char* d, size_t l) {
        relayToMc();
        if (_mcSock != INVALID_SOCKET && l > 0) {
            send(_mcSock, d, static_cast<int>(l), 0);
        }
    });
    if (!_channel->init(_transport, _udpSock, _peerAddr)) {
        fail("可靠通道初始化失败");
        return;
    }

    // 连接本地 MC
    if (_isHost) {
        // 房主：连本地 MC 服务器
        _mcSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_mcSock == INVALID_SOCKET) { fail("本地 socket 创建失败"); return; }
        sockaddr_in mcAddr{};
        mcAddr.sin_family = AF_INET;
        mcAddr.sin_port = htons(_localPort);
        inet_pton(AF_INET, _localIp.c_str(), &mcAddr.sin_addr);
        if (connect(_mcSock, reinterpret_cast<sockaddr*>(&mcAddr), sizeof(mcAddr))
            == SOCKET_ERROR) {
            fail("无法连接本地 MC 服务器 " + _localIp + ":" + std::to_string(_localPort));
            return;
        }
    } else {
        // 玩家：监听本地端口，MC 客户端连入
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) { fail("本地监听 socket 创建失败"); return; }
        int opt = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(_localPort);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR
            || listen(ls, 1) == SOCKET_ERROR) {
            closesocket(ls);
            fail("本地端口 " + std::to_string(_localPort) + " 监听失败");
            return;
        }
        _mcSock = accept(ls, nullptr, nullptr);
        closesocket(ls);
        if (_mcSock == INVALID_SOCKET) { fail("MC 客户端未接入"); return; }
    }

    // 启动 MC → UDP 转发线程
    _relayFromMcThread = std::thread(&P2PTunnel::relayFromMc, this);
}

void P2PTunnel::relayToMc() {
    // 预留：UDP 数据已在上层转发；此处无需操作
}

void P2PTunnel::relayFromMc() {
    // 本地 MC TCP → UDP 通道
    char buf[8192];
    while (!_stop.load(std::memory_order_acquire) && _mcSock != INVALID_SOCKET) {
        int n = recv(_mcSock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (_channel && !_channel->isClosed()) {
            _channel->send(buf, static_cast<size_t>(n));
        }
    }
}

void P2PTunnel::run() {
    // 创建 UDP 打洞 socket
    _udpSock = _transport->createUdpSocket(0, "::");
    if (_udpSock == INVALID_SOCKET) { fail("UDP socket 创建失败"); return; }

    if (!gatherCandidate()) return;
    if (_stop.load(std::memory_order_acquire)) return;
    if (!registerPeer()) return;

    // 轮询对端候选（最多 15 秒）
    bool found = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!_stop.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        if (pollPeerCandidate()) { found = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    if (!found && !_stop.load(std::memory_order_acquire)) {
        fail("未发现对端，请确认对方已加入同一房间");
        return;
    }
    if (_stop.load(std::memory_order_acquire)) return;
    punch();
}

} // namespace yunyi
