/**
 * @file ReliableUdpChannel.h
 * @brief 可靠 UDP 通道 —— NAT 打洞后的数据面
 *
 * 对上层伪装成一条可靠字节流（仿 Session 接口），内部用 UDP datagram 承载：
 *   - 上层 send() 的字节流切成 ≤kMaxPayload 的 datagram，每个带递增序号
 *   - 接收方回 ACK，发送方超时未确认自动重传
 *   - 接收方乱序缓冲 + 按序号拼回连续字节流，交付 onData
 *   - FIN 包通知对端关闭
 *
 * 不做拥塞控制（家庭宽带场景够用），只保证可靠有序字节流。
 * 打洞成功后，玩家↔房主之间的 MC 流量经此通道传输（替代中继 TCP 隧道）。
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yunyi {
class TransportCore;

class ReliableUdpChannel : public std::enable_shared_from_this<ReliableUdpChannel> {
public:
    using OnDataCallback  = std::function<void(const char*, size_t)>;
    using OnCloseCallback = std::function<void(int)>;

    ReliableUdpChannel();
    ~ReliableUdpChannel();

    /**
     * @brief 初始化
     * @param transport TransportCore 实例（需已 init）
     * @param udpSock   TransportCore::createUdpSocket 创建的 UDP socket
     * @param peer      对端地址（打洞成功后的公网映射端点）
     * @return true 成功
     */
    bool init(TransportCore* transport, SOCKET udpSock, const sockaddr_storage& peer);

    void setOnData(OnDataCallback cb) { _onData = std::move(cb); }
    void setOnClose(OnCloseCallback cb) { _onClose = std::move(cb); }

    /** 上层写入字节流（异步发送，内部切包 + 重传） */
    void send(const char* data, size_t len);

    /** 关闭通道（发 FIN + 停止重传线程 + 触发 onClose） */
    void close();

    /** TransportCore UDP 接收回调入口（IOCP worker 线程） */
    void onUdpRecv(const char* data, size_t len, const sockaddr_storage& peer);

    bool isClosed() const { return _closed.load(std::memory_order_acquire); }

private:
    enum : uint8_t {
        FLAG_DATA = 0x01,
        FLAG_ACK  = 0x02,
        FLAG_FIN  = 0x04
    };
    /** 单包 payload 上限（避开 IPv6 最小 MTU 1280 减去头开销，避免 IP 分片） */
    static constexpr size_t kMaxPayload = 1200;
    /** 重传超时（ms） */
    static constexpr uint32_t kRetransmitMs = 200;
    /** 最大重传次数，超时判死 */
    static constexpr int kMaxRetries = 30;
    /** 重传循环 tick 间隔（ms） */
    static constexpr uint32_t kTickMs = 40;

    void processSend();                       // 串行发送队列下一个 datagram
    void onSendDone();                        // 发送完成，继续队列
    void handleAck(uint32_t seq);             // 处理 ACK
    void handleData(uint32_t seq, const char* data, size_t len);
    void retransmitLoop();                    // 重传线程
    void doClose(int err);                    // 触发 onClose + 清理（外部线程调用，会 join 重传线程）
    void finishFromRetransmitThread(int err); // 仅供 retransmitLoop 内部收尾：不 join 自己

    static std::string pack(uint8_t flags, uint32_t seq, const char* payload, size_t len);
    static void unpack(const char* data, size_t len, uint8_t& flags,
                       uint32_t& seq, const char*& payload, size_t& payloadLen);

    TransportCore* _transport = nullptr;
    SOCKET _sock = INVALID_SOCKET;
    sockaddr_storage _peer{};
    bool _init = false;

    std::mutex _mutex;
    std::thread _retransmitThread;
    std::atomic<bool> _closed{false};

    /** 待切包字节流（上层 send 追加） */
    std::string _stream;
    /** 待发送 datagram 队列（新数据 + 重传） */
    std::deque<std::string> _sendQueue;
    /** 是否有 datagram 在途发送（串行） */
    bool _sending = false;
    uint32_t _nextSeq = 0;
    struct PendingPacket {
        std::string data;
        uint64_t sendTickMs = 0;
        int retries = 0;
    };
    /** 未确认包表（seq -> 包） */
    std::map<uint32_t, PendingPacket> _unacked;

    /** 接收乱序缓冲（seq -> payload） */
    std::map<uint32_t, std::string> _recvBuf;
    /** 期望接收的下一个序号 */
    uint32_t _expectSeq = 0;

    OnDataCallback _onData;
    OnCloseCallback _onClose;
};

} // namespace yunyi
