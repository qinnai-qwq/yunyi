/**
 * @file ReliableUdpChannel.cpp
 * @brief 可靠 UDP 通道实现
 */
#include "ReliableUdpChannel.h"
#include "TransportCore.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstring>

namespace yunyi {

ReliableUdpChannel::ReliableUdpChannel() = default;

ReliableUdpChannel::~ReliableUdpChannel() {
    _closed.store(true, std::memory_order_release);
    if (_retransmitThread.joinable()) _retransmitThread.join();
    if (_init && _sock != INVALID_SOCKET && _transport) {
        _transport->closeSocket(_sock, nullptr, 0);
    }
}

bool ReliableUdpChannel::init(TransportCore* transport, SOCKET udpSock,
                              const sockaddr_storage& peer) {
    if (!transport || udpSock == INVALID_SOCKET) return false;
    _transport = transport;
    _sock = udpSock;
    _peer = peer;
    _init = true;

    // 启动重传线程
    _retransmitThread = std::thread(&ReliableUdpChannel::retransmitLoop, this);

    // 投递 UDP 接收并持续循环。
    // 用 weak_from_this 持有本对象：IOCP 完成包可能在本对象析构后才回调，
    // 此时 lock() 失败即放弃，避免 use-after-free（本类必须以 shared_ptr 管理）。
    auto weakSelf = weak_from_this();
    auto arm = std::make_shared<std::function<void()>>();
    *arm = [weakSelf, arm]() {
        auto self = weakSelf.lock();
        if (!self || self->isClosed()) return;
        self->_transport->postRecvFrom(self->_sock,
            [weakSelf, arm](SOCKET, const char* d, size_t l,
                            const sockaddr_storage& src) {
                auto self2 = weakSelf.lock();
                if (!self2 || self2->isClosed()) return;
                self2->onUdpRecv(d, l, src);
                (*arm)();  // 重新投递，形成接收循环
            });
    };
    (*arm)();
    return true;
}

void ReliableUdpChannel::send(const char* data, size_t len) {
    if (isClosed() || !data || len == 0) return;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stream.append(data, len);
    }
    processSend();
}

void ReliableUdpChannel::processSend() {
    if (isClosed() || !_transport) return;
    std::string dgram;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // 先把 stream 切成 datagram 入队
        while (!_stream.empty()) {
            size_t take = _stream.size() > kMaxPayload ? kMaxPayload : _stream.size();
            uint32_t seq = _nextSeq++;
            std::string pkt = pack(FLAG_DATA, seq, _stream.data(), take);
            _stream.erase(0, take);
            _sendQueue.push_back(pkt);
            PendingPacket pp;
            pp.data = pkt;
            pp.sendTickMs = GetTickCount64();
            _unacked[seq] = std::move(pp);
        }
        // 串行：无在途且队列非空 → 取一个发送
        if (_sending || _sendQueue.empty()) return;
        _sending = true;
        dgram = _sendQueue.front();
    }
    _transport->postSendTo(_sock, dgram.data(), dgram.size(), _peer,
        [this](SOCKET, int) { onSendDone(); });
}

void ReliableUdpChannel::onSendDone() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_sendQueue.empty()) _sendQueue.pop_front();
        _sending = false;
    }
    processSend();
}

void ReliableUdpChannel::handleAck(uint32_t seq) {
    std::lock_guard<std::mutex> lock(_mutex);
    _unacked.erase(seq);
}

void ReliableUdpChannel::handleData(uint32_t seq, const char* data, size_t len) {
    std::vector<std::string> toDeliver;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (seq < _expectSeq) {
            // 已交付过的重复包，忽略（但仍回 ACK）
            return;
        }
        _recvBuf[seq] = std::string(data, len);
        // 拼连续数据
        std::string batch;
        auto it = _recvBuf.find(_expectSeq);
        while (it != _recvBuf.end()) {
            batch.append(it->second);
            _recvBuf.erase(it);
            ++_expectSeq;
            it = _recvBuf.find(_expectSeq);
        }
        if (!batch.empty()) toDeliver.push_back(std::move(batch));
    }
    // 锁外交付，避免回调重入 send 死锁
    for (auto& b : toDeliver) {
        if (_onData) _onData(b.data(), b.size());
    }
}

void ReliableUdpChannel::onUdpRecv(const char* data, size_t len,
                                   const sockaddr_storage& peer) {
    (void)peer;
    if (isClosed() || !data || len < 5) return;

    uint8_t flags = 0;
    uint32_t seq = 0;
    const char* payload = nullptr;
    size_t payloadLen = 0;
    unpack(data, len, flags, seq, payload, payloadLen);

    if (flags & FLAG_ACK) {
        handleAck(seq);
        return;
    }
    if (flags & FLAG_FIN) {
        doClose(0);
        return;
    }
    if (flags & FLAG_DATA) {
        // 立即回 ACK（旁路发送，不占串行数据队列）
        std::string ack = pack(FLAG_ACK, seq, nullptr, 0);
        if (_transport) {
            _transport->postSendTo(_sock, ack.data(), ack.size(), _peer, nullptr);
        }
        handleData(seq, payload, payloadLen);
    }
}

void ReliableUdpChannel::retransmitLoop() {
    while (!isClosed()) {
        Sleep(kTickMs);
        if (isClosed()) break;

        bool needSend = false;
        bool timedOut  = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            uint64_t now = GetTickCount64();
            auto it = _unacked.begin();
            while (it != _unacked.end()) {
                auto& pkt = it->second;
                if (now - pkt.sendTickMs >= kRetransmitMs) {
                    if (pkt.retries >= kMaxRetries) {
                        // 对端长时间无确认，判死：只标记，锁外再收尾（避免持锁回调重入 + 自 join）
                        timedOut = true;
                        break;
                    }
                    pkt.sendTickMs = now;
                    pkt.retries++;
                    _sendQueue.push_back(pkt.data);
                    needSend = true;
                }
                ++it;
            }
        }
        if (timedOut) {
            finishFromRetransmitThread(-1);
            return;  // 函数返回，线程自然结束，无需 join 自己
        }
        if (needSend) processSend();
    }
}

void ReliableUdpChannel::close() {
    if (isClosed()) return;
    _closed.store(true, std::memory_order_release);
    // 发 FIN 告知对端（尽力而为，不等待确认）
    uint32_t finSeq;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        finSeq = _nextSeq;
    }
    std::string fin = pack(FLAG_FIN, finSeq, nullptr, 0);
    if (_transport && _sock != INVALID_SOCKET) {
        _transport->postSendTo(_sock, fin.data(), fin.size(), _peer, nullptr);
    }
    if (_retransmitThread.joinable()) _retransmitThread.join();
    if (_onClose) _onClose(0);
}

void ReliableUdpChannel::doClose(int err) {
    _closed.store(true, std::memory_order_release);
    if (_retransmitThread.joinable()) _retransmitThread.join();
    if (_onClose) _onClose(err);
}

void ReliableUdpChannel::finishFromRetransmitThread(int err) {
    _closed.store(true, std::memory_order_release);
    if (_onClose) _onClose(err);
    // 不 join _retransmitThread —— 本方法就运行在该线程上，返回后线程自然结束
}

std::string ReliableUdpChannel::pack(uint8_t flags, uint32_t seq,
                                     const char* payload, size_t len) {
    std::string out;
    out.reserve(5 + len);
    out.push_back(static_cast<char>(flags));
    uint32_t be = htonl(seq);
    out.append(reinterpret_cast<const char*>(&be), 4);
    if (payload && len > 0) out.append(payload, len);
    return out;
}

void ReliableUdpChannel::unpack(const char* data, size_t len,
                                uint8_t& flags, uint32_t& seq,
                                const char*& payload, size_t& payloadLen) {
    flags = static_cast<uint8_t>(data[0]);
    uint32_t be = 0;
    std::memcpy(&be, data + 1, 4);
    seq = ntohl(be);
    payload = data + 5;
    payloadLen = len - 5;
}

} // namespace yunyi
