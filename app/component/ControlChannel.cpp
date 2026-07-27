/**
 * @file ControlChannel.cpp
 * @brief 控制连接状态机实现
 */
#include "ControlChannel.h"
#include "../../NetEngine/FrameCodec.h"
#include "../../NetEngine/NetUtil.h"
#include "../../NetEngine/Scheduler.h"
#include "../../NetEngine/Session.h"
#include "../../NetEngine/TlsPskContext.h"
#include "../../NetEngine/TransportCore.h"

#include <chrono>
#include <iostream>

namespace yunyi {

ControlChannel::ControlChannel() = default;

ControlChannel::~ControlChannel() {
    disconnect();
}

#define CC_LOG(msg) do { if (_logCb) _logCb(msg); } while(0)

bool ControlChannel::connect(
    const std::string& relayHost, uint16_t relayPort,
    const std::string& psk, const std::string& pskIdentity,
    TransportCore* transport) {
    auto s = _state.load();
    if (s != ControlState::Disconnected && s != ControlState::Reconnecting) {
        CC_LOG("CC:connect rejected state=" + std::to_string(static_cast<int>(s)));
        return false;
    }

    _relayHost   = relayHost;
    _relayPort   = relayPort;
    _psk         = psk;
    _pskIdentity = pskIdentity;
    _transport   = transport;

    setState(ControlState::Connecting);

    // 初始化 TLS 上下文
    _tls = std::make_unique<TlsPskContext>();
    if (!_tls->init(psk, pskIdentity, TlsRole::Client)) {
        CC_LOG("CC:TLS init failed");
        setState(ControlState::Disconnected);
        return false;
    }

    // 建立 TCP 连接
    SOCKET sock = NetUtil::createSocket(relayHost);
    if (sock == INVALID_SOCKET) {
        CC_LOG("CC:socket create failed");
        setState(ControlState::Disconnected);
        return false;
    }

    sockaddr_storage addr{};
    int addrLen = NetUtil::fillAddr(relayHost.c_str(), relayPort, addr);
    if (addrLen == 0) {
        CC_LOG("CC:fillAddr failed");
        closesocket(sock);
        setState(ControlState::Disconnected);
        return false;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr),
            addrLen) == SOCKET_ERROR) {
        CC_LOG("CC:TCP connect failed");
        closesocket(sock);
        setState(ControlState::Disconnected);
        return false;
    }

    // 同步 connect 创建的 socket 需手动绑定到 IOCP
    if (!transport->bindSocket(sock)) {
        CC_LOG("CC:bindSocket failed");
        closesocket(sock);
        setState(ControlState::Disconnected);
        return false;
    }

    CC_LOG("CC:TCP connected, starting TLS");

    // 创建 Session 并设置回调
    _session = new Session(sock, transport);
    _session->retain();

    _session->setOnData([this](SOCKET, const char* data, size_t len) {
        handleReceivedData(data, len);
    });

    _session->setOnClose([this](SOCKET, int err) {
        CC_LOG("CC:session closed err=" + std::to_string(err));
        setState(ControlState::Disconnected);
        stopHeartbeat();
        _session = nullptr;
        _tls.reset();
        _tlsHandshakeDone = false;
    });

    _session->start();

    // TLS 握手（客户端发起）
    std::vector<uint8_t> handshakeOutput;
    auto result = _tls->doHandshake(nullptr, 0, handshakeOutput);

    if (result == TlsPskContext::HandshakeResult::Failed) {
        CC_LOG("CC:TLS handshake init failed");
        _session->close();
        _session = nullptr;
        setState(ControlState::Disconnected);
        return false;
    }

    // 发送握手数据（ClientHello）
    if (!handshakeOutput.empty()) {
        _session->send(
            reinterpret_cast<const char*>(handshakeOutput.data()),
            handshakeOutput.size());
        CC_LOG("CC:ClientHello sent " + std::to_string(handshakeOutput.size()) + "B");
    }

    if (result == TlsPskContext::HandshakeResult::Done) {
        _tlsHandshakeDone = true;
        setState(ControlState::Active);
        CC_LOG("CC:Handshake done (immediate)");
    } else {
        CC_LOG("CC:Waiting for ServerHello");
    }

    return true;
}

bool ControlChannel::registerRoom(
    const std::string& roomName, uint16_t localMcPort) {
    if (_state.load() != ControlState::Active) return false;

    setState(ControlState::Registering);

    // 保存房间信息用于重连后重注册
    _lastRoomName    = roomName;
    _lastLocalMcPort = localMcPort;

    protocol::RegisterPayload payload;
    payload.roomName    = roomName;
    payload.localMcPort = localMcPort;

    auto frame = protocol::FrameCodec::encodeRegister(payload);

    if (!sendFrame(frame)) {
        setState(ControlState::Active);
        return false;
    }
    return true;
}

bool ControlChannel::registerRoomAsync(
    const std::string& roomName, uint16_t localMcPort,
    RegisterAckCallback cb) {
    if (_state.load() != ControlState::Active) {
        if (cb) cb(false, 0, 0, "not connected");
        return false;
    }
    if (!cb) return registerRoom(roomName, localMcPort);

    // 保存回调（加锁保护，与 handleReceivedData / 超时回调互斥）
    {
        std::lock_guard<std::mutex> lock(_registerCbMutex);
        _pendingRegisterCb = std::move(cb);
    }

    // 设置 5 秒超时
    if (_scheduler) {
        _registerTimeoutId = _scheduler->addTimer(5000, [this]() {
            RegisterAckCallback timedOut;
            {
                std::lock_guard<std::mutex> lock(_registerCbMutex);
                _registerTimeoutId = 0;
                timedOut = std::move(_pendingRegisterCb);
                _pendingRegisterCb = nullptr;
            }
            if (timedOut) {
                if (_state.load() == ControlState::Registering) {
                    setState(ControlState::Active);
                }
                timedOut(false, 0, 0, "REGISTER_ACK timeout");
            }
        }, false);
    }

    return registerRoom(roomName, localMcPort);
}

bool ControlChannel::deregister() {
    auto frame = protocol::FrameCodec::encodeDeregister();
    return sendFrame(frame);
}

void ControlChannel::disconnect() {
    setState(ControlState::Closed);
    stopHeartbeat();
    // 取消重连定时器
    if (_scheduler && _reconnectTimerId != 0) {
        _scheduler->cancelTimer(_reconnectTimerId);
        _reconnectTimerId = 0;
    }
    if (_session) {
        _session->close();
        _session = nullptr;
    }
    _tls.reset();
    _tlsHandshakeDone = false;
    _recvBuffer.clear();
    _reconnectAttempts = 0;
    setState(ControlState::Disconnected);
}

bool ControlChannel::sendFrame(const std::vector<uint8_t>& frame) {
    if (!_session) { CC_LOG("SF:no_sess"); return false; }
    if (!_tls)    { CC_LOG("SF:no_tls"); return false; }
    if (!_tlsHandshakeDone) { CC_LOG("SF:no_hs"); return false; }

    std::lock_guard<std::recursive_mutex> lock(_tlsMutex);
    std::vector<uint8_t> encrypted;
    if (!_tls->encrypt(frame.data(), frame.size(), encrypted)) {
        CC_LOG("SF:enc_fail len=" + std::to_string(frame.size()));
        return false;
    }

    bool ok = _session->send(
        reinterpret_cast<const char*>(encrypted.data()),
        encrypted.size());
    if (!ok) CC_LOG("SF:send_fail");
    return ok;
}

void ControlChannel::setState(ControlState newState) {
    auto old = _state.exchange(newState);
    if (old != newState && _onStateChange) {
        _onStateChange(old, newState);
    }
}

void ControlChannel::handleReceivedData(
    const char* data, size_t len) {
    if (!_tls) return;

    std::lock_guard<std::recursive_mutex> lock(_tlsMutex);

    // 握手未完成 → 将数据喂给 doHandshake，不调 decrypt
    if (!_tlsHandshakeDone) {
        std::vector<uint8_t> handshakeOutput;
        auto result = _tls->doHandshake(
            reinterpret_cast<const uint8_t*>(data), len, handshakeOutput);

        if (!handshakeOutput.empty()) {
            _session->send(
                reinterpret_cast<const char*>(handshakeOutput.data()),
                handshakeOutput.size());
        }

        if (result == TlsPskContext::HandshakeResult::Done) {
            _tlsHandshakeDone = true;
            CC_LOG("CC:Handshake complete, Active");
            // 握手完成 → 推进到 Active（允许 registerRoom 调用）
            if (_state.load() == ControlState::Connecting) {
                setState(ControlState::Active);
            }
        } else if (result == TlsPskContext::HandshakeResult::Failed) {
            disconnect();
        }
        // NeedMoreData: 等待更多握手数据
        return;
    }

    // 握手已完成 → 正常 TLS 解密
    std::vector<uint8_t> plaintext;
    if (!_tls->decrypt(
            reinterpret_cast<const uint8_t*>(data), len, plaintext)) {
        return;
    }
    if (plaintext.empty()) return;

    // 追加到接收缓冲区（处理 TCP 粘包/拆包）
    _recvBuffer.insert(_recvBuffer.end(),
        plaintext.begin(), plaintext.end());

    // 循环解析完整的帧
    while (_recvBuffer.size() >= protocol::kHeaderSize) {
        // 校验 magic
        if (_recvBuffer[0] != protocol::kMagic) {
            _recvBuffer.clear();
            return;
        }

        protocol::FrameType type;
        uint8_t version;
        uint32_t payloadLen;

        if (!protocol::FrameCodec::decodeHeader(
                _recvBuffer.data(), type, version, payloadLen)) {
            _recvBuffer.clear();
            return;
        }

        // 版本不匹配 → 发送 ERROR 后断开
        if (version != protocol::kVersion) {
            auto errFrame = protocol::FrameCodec::encodeError(
                protocol::ErrorCode::PROTOCOL_VERSION_MISMATCH,
                "version mismatch");
            sendFrame(errFrame);
            disconnect();
            return;
        }

        // 数据不足，等待下次
        size_t frameSize = protocol::kHeaderSize + payloadLen;
        if (_recvBuffer.size() < frameSize) return;

        // 通知上层
        if (_onFrame) {
            _onFrame(type,
                _recvBuffer.data() + protocol::kHeaderSize,
                payloadLen);
        }

        // REGISTER_ACK 收到后启动心跳 + 触发异步回调
        if (type == protocol::FrameType::REGISTER_ACK
            && _state == ControlState::Registering) {
            // 解码 ACK payload
            auto ack = protocol::FrameCodec::decodeRegisterAckPayload(
                _recvBuffer.data() + protocol::kHeaderSize, payloadLen);
            setState(ControlState::Active);
            startHeartbeat();

            // 提取回调（加锁，防 IOCP 线程与主线程超时回调竞态）
            RegisterAckCallback cb;
            {
                std::lock_guard<std::mutex> lock(_registerCbMutex);
                if (_scheduler && _registerTimeoutId != 0) {
                    _scheduler->cancelTimer(_registerTimeoutId);
                    _registerTimeoutId = 0;
                }
                cb = std::move(_pendingRegisterCb);
                _pendingRegisterCb = nullptr;
            }
            // 锁外调用回调
            if (cb) {
                cb(true, ack.roomId, ack.assignedPort, "");
            }
        }

        // 消费已处理的帧
        _recvBuffer.erase(
            _recvBuffer.begin(),
            _recvBuffer.begin() + frameSize);
    }
}

void ControlChannel::startHeartbeat() {
    if (!_scheduler) return;
    if (_heartbeatTimerId != 0) return;  // 已在运行

    _heartbeatTimerId = _scheduler->addTimer(kHeartbeatIntervalMs,
        [this]() {
            if (_state.load() == ControlState::Active) {
                auto frame = protocol::FrameCodec::encodeHeartbeat();
                bool ok = sendFrame(frame);
                CC_LOG(ok ? "HB:ok" : "HB:FAIL");
            }
        }, true);  // 重复定时器

    std::cout << "[ControlChannel] Heartbeat started ("
              << kHeartbeatIntervalMs << "ms)" << std::endl;
}

void ControlChannel::stopHeartbeat() {
    if (_scheduler && _heartbeatTimerId != 0) {
        _scheduler->cancelTimer(_heartbeatTimerId);
        _heartbeatTimerId = 0;
    }
}

void ControlChannel::tryReconnect() {
    setState(ControlState::Reconnecting);

    // 超过最大重连次数 → 放弃
    if (++_reconnectAttempts > kMaxReconnectAttempts) {
        std::cerr << "[ControlChannel] Max reconnect attempts reached, giving up"
                  << std::endl;
        stopHeartbeat();
        setState(ControlState::Disconnected);
        return;
    }

    std::cout << "[ControlChannel] Reconnect attempt " << _reconnectAttempts
              << "/" << kMaxReconnectAttempts << " in "
              << kReconnectDelayMs << "ms" << std::endl;

    // 清理旧连接
    if (_session) {
        _session->close();
        _session = nullptr;
    }
    _tls.reset();
    _tlsHandshakeDone = false;
    _recvBuffer.clear();

    // 使用 Scheduler 延迟重连（不阻塞 IOCP 线程）
    if (_scheduler) {
        _reconnectTimerId = _scheduler->addTimer(kReconnectDelayMs,
            [this]() {
                _reconnectTimerId = 0;
                // 尝试重连
                if (connect(_relayHost, _relayPort,
                        _psk, _pskIdentity, _transport)) {
                    _reconnectAttempts = 0;
                    // 重连成功后重注册房间
                    if (!_lastRoomName.empty()) {
                        registerRoom(_lastRoomName, _lastLocalMcPort);
                    }
                } else {
                    tryReconnect();  // 下次重试（通过 Scheduler，不递归）
                }
            }, false);  // 一次性定时器
    } else {
        // 无 Scheduler 时回退到直接重连（阻塞，不推荐）
        if (connect(_relayHost, _relayPort,
                _psk, _pskIdentity, _transport)) {
            _reconnectAttempts = 0;
            if (!_lastRoomName.empty()) {
                registerRoom(_lastRoomName, _lastLocalMcPort);
            }
        } else {
            tryReconnect();
        }
    }
}

} // namespace yunyi
