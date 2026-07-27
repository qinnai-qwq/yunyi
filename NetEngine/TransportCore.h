/**
 * @file TransportCore.h
 * @brief IOCP 重叠 I/O 传输层 —— 平台相关网络 I/O 的唯一封装点
 *
 * 职责: 创建/绑定/监听/接受连接、发起出站连接、异步收发、工作线程池。
 * 不管: TLS 加解密、帧协议、业务逻辑。
 *
 * 平台: Windows IOCP（已实现）| Linux epoll（预留）| macOS kqueue（预留）
 */
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"mswsock.lib")
#endif
namespace yunyi {
class Session;

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
 * @brief IOCP 异步操作类型
 */
enum class IoOpType : uint8_t {
    Accept,  /** AcceptEx 完成 */
    Connect, /** ConnectEx 完成 */
    Recv,    /** WSARecv 完成 */
    Send     /** WSASend 完成 */
};

/**
 * @brief IOCP 异步 I/O 上下文
 *
 * 每次异步操作携带一个 IoContext，包含 OVERLAPPED、缓冲区、操作类型等。
 * 通过 ResourcePool 高频复用，避免频繁 new/delete。
 */
struct IoContext {
    /** Windows OVERLAPPED 结构，IOCP 核心 */
    OVERLAPPED overlapped{};
    /** 本次操作类型 */
    IoOpType opType = IoOpType::Recv;
    /** WSABUF，指向内嵌 buffer */
    WSABUF wsaBuf{};
    /** 内嵌 64KB 缓冲区，避免外部内存管理 */
    uint8_t buffer[65536]{};
    /** 关联的 socket */
    SOCKET socket = INVALID_SOCKET;
    /** AcceptEx 所属的监听 socket（仅 Accept 操作有效） */
    SOCKET listenSocket = INVALID_SOCKET;
    /** 实际传输字节数 */
    DWORD bytesTransferred = 0;
    /**
     * @brief 重置上下文到初始状态（从资源池取出后调用）
     */
    void reset() {
        ZeroMemory(&overlapped, sizeof(overlapped));
        opType = IoOpType::Recv;
        wsaBuf.buf = (CHAR*)buffer;
        wsaBuf.len = sizeof(buffer);
        socket = INVALID_SOCKET;
        listenSocket = INVALID_SOCKET;
        bytesTransferred = 0;
    }
};

/**
 * @class TransportCore
 * @brief Windows IOCP 异步网络传输核心
 *
 * 管理 IOCP 句柄、工作线程池、socket 回调映射。
 * 所有异步 I/O 操作通过此类发起，回调在工作线程中执行。
 *
 * 生命周期:
 *   1. 构造 TransportCore 实例
 *   2. init(ioThreads) 创建 IOCP + 启动工作线程
 *   3. createListenSocket() / startConnect() 建立连接
 *   4. postRecv() / postSend() 异步收发
 *   5. closeSocket() 关闭连接
 *   6. shutdown() 停止所有工作线程 + 关闭 IOCP
 *
 * @note 所有回调在工作线程中执行，注意线程安全。
 */
class TransportCore {
public:
    TransportCore();
    ~TransportCore();

    /**
     * @brief 初始化传输层
     * @param ioThreads IO 工作线程数，0 = 自动检测 CPU 核心数
     * @return true 成功，false 失败
     * @pre 未调用 init()，或上次已 shutdown()
     * @post IOCP 创建，工作线程启动
     */
    bool init(uint32_t ioThreads = 0);

    /**
     * @brief 关闭传输层
     * @post 所有工作线程退出，IOCP 关闭，所有未完成 I/O 被取消
     */
    void shutdown();

    /**
     * @brief 创建监听 socket
     * @param port 监听端口
     * @param bindIp 绑定 IP 地址，默认 :: （IPv6 双栈，同时接受 IPv4/IPv6）
     * @return 有效的 SOCKET，失败返回 INVALID_SOCKET
     */
    SOCKET createListenSocket(uint16_t port, const char* bindIp = "::", SOCKET* outIpv4Sock = nullptr);

    /**
     * @brief 开始异步接受连接（AcceptEx）
     * @param ls 监听 socket（由 createListenSocket 创建）
     * @param cb 接受完成回调
     * @return true 成功投递，false 失败
     * @pre ls 必须是已绑定并监听的 socket
     */
    bool startAccept(SOCKET ls, OnAcceptCallback cb);

    /**
     * @brief 开始异步出站连接（ConnectEx）
     * @param host 目标主机名或 IP
     * @param port 目标端口
     * @param cb 连接完成回调
     * @return true 成功投递，false 失败
     */
    bool startConnect(const char* host, uint16_t port, OnConnectCallback cb);

    /**
     * @brief 投递异步接收请求
     * @param s 目标 socket
     * @param cb 接收完成回调
     * @return true 成功投递，false 失败
     * @pre 同一 socket 不得同时有多个未完成的 recv 请求
     */
    bool postRecv(SOCKET s, OnRecvCallback cb);

    /**
     * @brief 投递异步发送请求
     * @param s 目标 socket
     * @param d 数据指针
     * @param len 数据长度
     * @param cb 发送完成回调
     * @return true 成功投递，false 失败
     * @pre d 指向的内存必须在回调触发前保持有效
     */
    bool postSend(SOCKET s, const char* d, size_t len, OnSendCallback cb);

    /**
     * @brief 将已有 socket 绑定到 IOCP（同步 connect 后必须调用）
     * @param s 要绑定的 socket
     * @return true 成功
     *
     * 使用 createListenSocket / startConnect 创建的 socket 已自动绑定。
     * 仅当外部自行创建 socket（如 socket() + connect()）后才需手动调用。
     */
    bool bindSocket(SOCKET s) { return bindToIocp(s); }

    /**
     * @brief 关闭 socket 并触发关闭回调
     * @param s 要关闭的 socket
     * @param cb 关闭完成回调（同步触发）
     * @param err 关闭原因错误码，默认 0
     */
    void closeSocket(SOCKET s, OnCloseCallback cb, int err = 0);

    /**
     * @brief 将 sockaddr_in 转为 IP 字符串（IPv4 兼容旧接口）
     */
    static std::string addrToString(const sockaddr_in& a);

    /**
     * @brief 从 sockaddr_in 提取端口号（IPv4 兼容旧接口）
     */
    static uint16_t addrToPort(const sockaddr_in& a);

    /**
     * @brief 将 sockaddr_storage 转为 IP 字符串（双栈）
     */
    static std::string addrToString(const sockaddr_storage& a);

    /**
     * @brief 从 sockaddr_storage 提取端口号（双栈）
     */
    static uint16_t addrToPort(const sockaddr_storage& a);

private:
    /**
     * @brief IOCP 工作线程主循环
     *
     * 循环调用 GetQueuedCompletionStatus，将完成事件分发给 handleCompletion()。
     */
    void workerLoop();

    /**
     * @brief IOCP 完成事件处理核心
     * @param ctx 关联的 I/O 上下文
     * @param bt 传输字节数
     * @param err 0 = 成功，非 0 = 操作失败
     */
    void handleCompletion(IoContext* ctx, DWORD bt, int err);

    /**
     * @brief 将 socket 绑定到 IOCP
     * @param s 要绑定的 socket
     * @return true 成功
     */
    bool bindToIocp(SOCKET s);

    /**
     * @brief 配置 TCP keepalive 参数（SO_KEEPALIVE + SIO_KEEPALIVE_VALS）
     * @param s 目标 socket
     *
     * keepalive 空闲 30 秒后开始探测，探测间隔 10 秒。
     * 用于快速检测对端崩溃/网络中断的死连接。
     */
    void configureKeepalive(SOCKET s);

    /** AcceptEx 完成处理: 提取客户端地址，更新 socket 上下文 */
    void onAcceptComplete(IoContext* ctx, DWORD bt);

    /** ConnectEx 完成处理: 设置 SO_UPDATE_CONNECT_CONTEXT */
    void onConnectComplete(IoContext* ctx, DWORD bt);

    /** IOCP 句柄 */
    HANDLE _iocp = nullptr;
    /** 运行标志，false 时工作线程退出 */
    std::atomic<bool> _running{false};
    /** 工作线程池 */
    std::vector<std::thread> _workers;
    /** 回调映射互斥锁 */
    mutable std::mutex _cbMutex;

    /** socket 回调集合 */
    struct CB {
        OnAcceptCallback onAccept;
        OnConnectCallback onConnect;
        OnRecvCallback onRecv;
        OnSendCallback onSend;
        OnCloseCallback onClose;
    };
    CB* getCallbacks(SOCKET s);
    void setCallbacks(SOCKET s, CB c);
    void removeCallbacks(SOCKET s);
    /** SOCKET -> 回调映射 */
    std::unordered_map<SOCKET, CB> _callbacks;

    /** AcceptEx 函数指针（运行时从 mswsock 加载） */
    LPFN_ACCEPTEX _fnAcceptEx = nullptr;
    /** GetAcceptExSockaddrs 函数指针 */
    LPFN_GETACCEPTEXSOCKADDRS _fnGetAcceptExSockaddrs = nullptr;
    /** ConnectEx 函数指针 */
    LPFN_CONNECTEX _fnConnectEx = nullptr;
    /** 用于加载 AcceptEx/ConnectEx 函数指针的虚拟监听 socket */
    SOCKET _dummyListenSock = INVALID_SOCKET;
};
}
