/**
 * @file INetEventLoop.h
 * @brief 跨平台网络事件循环抽象接口 —— Core 层对外暴露的稳定契约
 *
 * 所有异步网络 I/O 通过本接口发起，具体实现（Windows IOCP / 未来 Linux epoll）
 * 下沉到平台适配层，Framework/App 层只依赖本接口，不被平台 API 绑定。
 */
#pragma once
#include <cstdint>
#include <functional>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace yunyi {

/**
 * @brief 接受完成回调
 * @param sock 新连接的 socket
 * @param addr 客户端地址（IPv4 或 IPv6）
 */
using OnAcceptCallback = std::function<void(SOCKET, const sockaddr_storage&)>;

/**
 * @brief 连接完成回调
 * @param sock 已连接的 socket
 * @param err 0 = 成功，非 0 = 失败错误码
 */
using OnConnectCallback = std::function<void(SOCKET, int)>;

/**
 * @brief 接收完成回调
 * @param sock 数据来源 socket
 * @param data 接收到的数据指针（仅在回调期间有效）
 * @param len 数据长度（字节），0 表示对端关闭
 */
using OnRecvCallback = std::function<void(SOCKET, const char*, size_t)>;

/**
 * @brief UDP 接收完成回调（带对端地址）
 * @param sock 数据来源 socket
 * @param data 数据指针（仅在回调期间有效）
 * @param len 数据长度（字节）
 * @param peer 对端地址（NAT 打洞场景是打洞成功后的公网映射端点）
 */
using OnRecvFromCallback = std::function<void(SOCKET, const char*, size_t, const sockaddr_storage&)>;

/**
 * @brief UDP 发送完成回调
 * @param sock 目标 socket
 * @param err 0 = 成功，非 0 = 失败错误码
 */
using OnSendToCallback = std::function<void(SOCKET, int)>;

/**
 * @brief 发送完成回调
 * @param sock 目标 socket
 * @param err 0 = 成功，非 0 = 失败错误码
 */
using OnSendCallback = std::function<void(SOCKET, int)>;

/**
 * @brief 关闭完成回调
 * @param sock 已关闭的 socket
 * @param err 关闭原因错误码，0 = 正常关闭
 */
using OnCloseCallback = std::function<void(SOCKET, int)>;

/**
 * @class INetEventLoop
 * @brief 跨平台异步网络事件循环抽象
 *
 * 提供 accept/connect/recv/send/recvfrom/sendto/close 的异步契约。
 * 平台实现：Windows 由 TransportCore(IOCP) 提供；Linux/macOS 未来可实现 epoll/kqueue。
 */
class INetEventLoop {
public:
    virtual ~INetEventLoop() = default;

    /**
     * @brief 初始化事件循环
     * @param ioThreads IO 工作线程数，0 = 自动检测 CPU 核心数
     * @return true 成功，false 失败
     * @pre 未调用 init()，或上次已 shutdown()
     * @post 事件循环创建，工作线程启动
     */
    virtual bool init(uint32_t ioThreads = 0) = 0;

    /**
     * @brief 关闭事件循环
     * @post 所有工作线程退出，事件循环关闭，所有未完成 I/O 被取消
     */
    virtual void shutdown() = 0;

    /**
     * @brief 创建监听 socket
     * @param port 监听端口
     * @param bindIp 绑定 IP 地址，默认 :: （IPv6 双栈，同时接受 IPv4/IPv6）
     * @param outIpv4Sock 若提供，输出同时绑定的 IPv4 socket（IPv6 双栈监听时）
     * @return 有效的 SOCKET，失败返回 INVALID_SOCKET
     */
    virtual SOCKET createListenSocket(uint16_t port, const char* bindIp = "::",
                                      SOCKET* outIpv4Sock = nullptr) = 0;

    /**
     * @brief 开始异步接受连接
     * @param ls 监听 socket（由 createListenSocket 创建）
     * @param cb 接受完成回调
     * @return true 成功投递，false 失败
     * @pre ls 必须是已绑定并监听的 socket
     */
    virtual bool startAccept(SOCKET ls, OnAcceptCallback cb) = 0;

    /**
     * @brief 开始异步出站连接
     * @param host 目标主机名或 IP
     * @param port 目标端口
     * @param cb 连接完成回调
     * @return true 成功投递，false 失败
     */
    virtual bool startConnect(const char* host, uint16_t port, OnConnectCallback cb) = 0;

    /**
     * @brief 投递异步接收请求
     * @param s 目标 socket
     * @param cb 接收完成回调
     * @return true 成功投递，false 失败
     * @pre 同一 socket 不得同时有多个未完成的 recv 请求
     */
    virtual bool postRecv(SOCKET s, OnRecvCallback cb) = 0;

    /**
     * @brief 投递异步发送请求
     * @param s 目标 socket
     * @param d 数据指针
     * @param len 数据长度
     * @param cb 发送完成回调
     * @return true 成功投递，false 失败
     * @pre d 指向的内存必须在回调触发前保持有效
     */
    virtual bool postSend(SOCKET s, const char* d, size_t len, OnSendCallback cb) = 0;

    /**
     * @brief 创建 UDP socket（SOCK_DGRAM，绑事件循环）
     * @param localPort 本地绑定端口，0 = 系统自动分配
     * @param bindIp 绑定 IP，默认 "::"（IPv6 双栈）
     * @return 有效 SOCKET，失败 INVALID_SOCKET
     */
    virtual SOCKET createUdpSocket(uint16_t localPort = 0, const char* bindIp = "::") = 0;

    /**
     * @brief 投递异步 UDP 接收请求
     * @param s UDP socket
     * @param cb 接收完成回调（带对端地址）
     * @return true 成功投递
     * @note UDP 无连接，无"对端关闭"，recv 失败仅静默
     */
    virtual bool postRecvFrom(SOCKET s, OnRecvFromCallback cb) = 0;

    /**
     * @brief 投递异步 UDP 发送请求
     * @param s UDP socket
     * @param d 数据指针
     * @param len 数据长度
     * @param peer 目标对端地址
     * @param cb 发送完成回调
     * @return true 成功投递
     */
    virtual bool postSendTo(SOCKET s, const char* d, size_t len,
                            const sockaddr_storage& peer, OnSendToCallback cb) = 0;

    /**
     * @brief 将已有 socket 绑定到事件循环（同步 connect 后必须调用）
     * @param s 要绑定的 socket
     * @return true 成功
     *
     * 使用 createListenSocket / startConnect 创建的 socket 已自动绑定。
     * 仅当外部自行创建 socket（如 socket() + connect()）后才需手动调用。
     */
    virtual bool bindSocket(SOCKET s) = 0;

    /**
     * @brief 关闭 socket 并触发关闭回调
     * @param s 要关闭的 socket
     * @param cb 关闭完成回调（同步触发）
     * @param err 关闭原因错误码，默认 0
     */
    virtual void closeSocket(SOCKET s, OnCloseCallback cb, int err = 0) = 0;
};

} // namespace yunyi
