/**
 * @file TransportCore.cpp
 * @brief Windows IOCP 异步网络传输核心实现
 *
 * 核心架构:
 *   1. init() 加载 AcceptEx/ConnectEx/GetAcceptExSockaddrs 扩展函数指针
 *   2. 创建 IOCP + 启动工作线程池
 *   3. workerLoop() 循环调用 GetQueuedCompletionStatus
 *   4. handleCompletion() 按 IoOpType 分发到对应回调
 *   5. 通过 ResourcePool<IoContext> 高频复用 IoContext 对象
 */
#include "TransportCore.h"
#include "NetUtil.h"
#include "ResourcePool.h"
#ifdef _WIN32
#include <windows.h>
#include <mstcpip.h>
#endif
#include <cassert>
#include <cstring>
#include <iostream>

namespace yunyi {

/** IoContext 全局资源池: 预分配 128，最大 4096 */
static ResourcePool<IoContext> s_ioCtxPool(128, 4096);

TransportCore::TransportCore() = default;

TransportCore::~TransportCore() {
    shutdown();
}

bool TransportCore::init(uint32_t ioThreads) {
    if (_running.load(std::memory_order_acquire)) return true;

    // 初始化 Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

    // 创建 IOCP
    _iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!_iocp) { WSACleanup(); return false; }

    // 加载 AcceptEx / GetAcceptExSockaddrs / ConnectEx 扩展函数指针
    SOCKET tmp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tmp == INVALID_SOCKET) {
        CloseHandle(_iocp);
        _iocp = nullptr;
        WSACleanup();
        return false;
    }

    GUID g1 = WSAID_ACCEPTEX;
    GUID g2 = WSAID_GETACCEPTEXSOCKADDRS;
    GUID g3 = WSAID_CONNECTEX;
    DWORD bytes = 0;

    WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &g1, sizeof(g1), &_fnAcceptEx, sizeof(_fnAcceptEx),
        &bytes, nullptr, nullptr);
    WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &g2, sizeof(g2), &_fnGetAcceptExSockaddrs,
        sizeof(_fnGetAcceptExSockaddrs), &bytes, nullptr, nullptr);
    WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &g3, sizeof(g3), &_fnConnectEx, sizeof(_fnConnectEx),
        &bytes, nullptr, nullptr);

    closesocket(tmp);

    if (!_fnAcceptEx || !_fnGetAcceptExSockaddrs || !_fnConnectEx) {
        CloseHandle(_iocp);
        _iocp = nullptr;
        WSACleanup();
        return false;
    }

    // 自动检测线程数
    if (ioThreads == 0) {
        ioThreads = std::thread::hardware_concurrency();
        if (ioThreads == 0) ioThreads = 2;
    }

    // 启动工作线程
    _running.store(true, std::memory_order_release);
    for (uint32_t i = 0; i < ioThreads; ++i) {
        _workers.emplace_back(&TransportCore::workerLoop, this);
    }
    return true;
}

void TransportCore::shutdown() {
    if (!_running.load(std::memory_order_acquire)) return;
    _running.store(false, std::memory_order_release);

    // 向每个工作线程发送退出信号
    for (size_t i = 0; i < _workers.size(); ++i) {
        PostQueuedCompletionStatus(_iocp, 0, 0, nullptr);
    }
    for (auto& t : _workers) {
        if (t.joinable()) t.join();
    }
    _workers.clear();

    if (_iocp) {
        CloseHandle(_iocp);
        _iocp = nullptr;
    }
    WSACleanup();
}

void TransportCore::workerLoop() {
    while (_running.load(std::memory_order_acquire)) {
        DWORD bt = 0;
        ULONG_PTR ck = 0;
        OVERLAPPED* ov = nullptr;

        // 100ms 超时，确保能及时响应 shutdown()
        BOOL ok = GetQueuedCompletionStatus(
            _iocp, &bt, &ck, &ov, 100);
        if (!ov) continue;

        // 从 OVERLAPPED 反查 IoContext
        IoContext* ctx = CONTAINING_RECORD(ov, IoContext, overlapped);
        handleCompletion(ctx, bt, ok ? 0 : WSAGetLastError());
    }
}

void TransportCore::handleCompletion(IoContext* ctx, DWORD bt, int err) {
    switch (ctx->opType) {
    case IoOpType::Accept:
        onAcceptComplete(ctx, bt);
        break;

    case IoOpType::Connect:
        onConnectComplete(ctx, bt);
        break;

    case IoOpType::Recv: {
        auto* cb = getCallbacks(ctx->socket);
        if (cb && cb->onRecv) {
            if (err == 0 && bt > 0) {
                // 正常接收数据
                cb->onRecv(ctx->socket,
                    reinterpret_cast<const char*>(ctx->buffer), bt);
            } else if (cb->onClose) {
                // 对端关闭或出错，触发 onClose
                cb->onClose(ctx->socket, err);
                s_ioCtxPool.release(ctx);
                return;  // 提前返回，不归还 ctx（已由 onClose 处理）
            }
        }
        break;
    }

    case IoOpType::Send: {
        auto* cb = getCallbacks(ctx->socket);
        if (cb && cb->onSend) {
            cb->onSend(ctx->socket, err);
        }
        break;
    }
    }
    s_ioCtxPool.release(ctx);
}

SOCKET TransportCore::createListenSocket(uint16_t port, const char* bindIp, SOCKET* outIpv4Sock) {
    (void)outIpv4Sock;  // 保留参数兼容性，实际由调用方自行创建双 socket
    const char* ip = (bindIp && bindIp[0]) ? bindIp : "::";
    SOCKET sock = NetUtil::createSocket(ip);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_storage addr{};
    int addrLen = sizeof(addr);

    if (NetUtil::isIPv6(ip)) {
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
        a6->sin6_family = AF_INET6;
        a6->sin6_port   = htons(port);
        inet_pton(AF_INET6, ip, &a6->sin6_addr);
        addrLen = sizeof(sockaddr_in6);
    } else {
        auto* a4 = reinterpret_cast<sockaddr_in*>(&addr);
        a4->sin_family = AF_INET;
        a4->sin_port   = htons(port);
        inet_pton(AF_INET, ip, &a4->sin_addr);
        addrLen = sizeof(sockaddr_in);
    }

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), addrLen)
        == SOCKET_ERROR) {
        if (NetUtil::isIPv6(ip)) {
            closesocket(sock);
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) return INVALID_SOCKET;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const char*>(&opt), sizeof(opt));
            sockaddr_in a4{};
            a4.sin_family = AF_INET;
            a4.sin_port   = htons(port);
            a4.sin_addr.s_addr = INADDR_ANY;
            if (bind(sock, reinterpret_cast<sockaddr*>(&a4), sizeof(a4)) == SOCKET_ERROR) {
                closesocket(sock);
                return INVALID_SOCKET;
            }
        } else {
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }
    if (listen(sock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    if (!bindToIocp(sock)) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

bool TransportCore::startAccept(SOCKET listenSock, OnAcceptCallback onAccept) {
    if (listenSock == INVALID_SOCKET) return false;

    CB cbs;
    cbs.onAccept = std::move(onAccept);
    setCallbacks(listenSock, cbs);

    // AcceptEx 需要预先创建好客户端 socket
    IoContext* ctx = s_ioCtxPool.acquire();
    ctx->opType = IoOpType::Accept;
    ctx->listenSocket = listenSock;
    ctx->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ctx->socket == INVALID_SOCKET) {
        s_ioCtxPool.release(ctx);
        return false;
    }

    // 预留 buffer 空间: 2 * (取 IPv4/IPv6 较大值 + 16)，兼容双栈
    DWORD addrLen = NetUtil::kAcceptAddrLen;
    DWORD br = 0;
    BOOL ok = _fnAcceptEx(listenSock, ctx->socket,
        ctx->buffer, 0, addrLen, addrLen, &br, &ctx->overlapped);
    if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
        closesocket(ctx->socket);
        ctx->socket = INVALID_SOCKET;
        s_ioCtxPool.release(ctx);
        return false;
    }
    return true;
}

void TransportCore::onAcceptComplete(IoContext* ctx, DWORD /*bt*/) {
    // 从 AcceptEx buffer 提取客户端地址
    sockaddr* localAddr = nullptr;
    sockaddr* remoteAddr = nullptr;
    int localAddrLen = 0, remoteAddrLen = 0;

    DWORD addrLen = NetUtil::kAcceptAddrLen;
    _fnGetAcceptExSockaddrs(
        ctx->buffer, 0, addrLen, addrLen,
        &localAddr, &localAddrLen,
        &remoteAddr, &remoteAddrLen);

    // 更新客户端 socket 的 accept 上下文
    setsockopt(ctx->socket, SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<char*>(&ctx->listenSocket),
        sizeof(ctx->listenSocket));
    bindToIocp(ctx->socket);

    // 转换为 sockaddr_storage 调用上层回调
    sockaddr_storage clientAddr{};
    if (remoteAddr && remoteAddrLen > 0) {
        size_t copyLen = (static_cast<size_t>(remoteAddrLen) < sizeof(clientAddr))
            ? static_cast<size_t>(remoteAddrLen) : sizeof(clientAddr);
        std::memcpy(&clientAddr, remoteAddr, copyLen);
    }

    auto* cb = getCallbacks(ctx->listenSocket);
    if (cb && cb->onAccept) {
        cb->onAccept(ctx->socket, clientAddr);
    }

    // 重新投递 AcceptEx，形成持续 accept 循环
    // 注意: ctx 由 handleCompletion 统一释放，这里不要 release
    SOCKET ls = ctx->listenSocket;
    OnAcceptCallback nextCb = cb ? cb->onAccept : nullptr;

    if (ls != INVALID_SOCKET && nextCb) {
        startAccept(ls, nextCb);
    }
}

bool TransportCore::startConnect(
    const char* host, uint16_t port, OnConnectCallback onConnect) {
    SOCKET sock = NetUtil::createSocket(host);
    if (sock == INVALID_SOCKET) return false;
    bindToIocp(sock);

    // 自动适配 IPv4/IPv6 本地绑定
    sockaddr_storage local{};
    int localLen = sizeof(sockaddr_in);
    if (NetUtil::isIPv6(host)) {
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&local);
        a6->sin6_family = AF_INET6;
        a6->sin6_port   = 0;
        a6->sin6_addr   = in6addr_any;
        localLen = sizeof(sockaddr_in6);
    } else {
        auto* a4 = reinterpret_cast<sockaddr_in*>(&local);
        a4->sin_family = AF_INET;
        a4->sin_port   = 0;
        a4->sin_addr.s_addr = INADDR_ANY;
    }
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), localLen)
        == SOCKET_ERROR) {
        closesocket(sock);
        return false;
    }

    CB cbs;
    cbs.onConnect = std::move(onConnect);
    setCallbacks(sock, cbs);

    // 填充目标地址
    sockaddr_storage remote{};
    int remoteLen = NetUtil::fillAddr(host, port, remote);
    if (remoteLen == 0) {
        removeCallbacks(sock);
        closesocket(sock);
        return false;
    }

    IoContext* ctx = s_ioCtxPool.acquire();
    ctx->opType = IoOpType::Connect;
    ctx->socket = sock;

    BOOL ok = _fnConnectEx(sock,
        reinterpret_cast<sockaddr*>(&remote), remoteLen,
        nullptr, 0, nullptr, &ctx->overlapped);
    if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
        removeCallbacks(sock);
        closesocket(sock);
        ctx->socket = INVALID_SOCKET;
        s_ioCtxPool.release(ctx);
        return false;
    }
    return true;
}

void TransportCore::onConnectComplete(IoContext* ctx, DWORD) {
    auto* cb = getCallbacks(ctx->socket);
    if (cb && cb->onConnect) {
        // 更新连接上下文
        setsockopt(ctx->socket, SOL_SOCKET,
            SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
        cb->onConnect(ctx->socket, 0);
    }
    // ctx 由 handleCompletion 统一释放
}

bool TransportCore::postRecv(SOCKET sock, OnRecvCallback onRecv) {
    if (sock == INVALID_SOCKET) return false;

    // 确保 socket 在 callback map 中注册（首次 postRecv 时可能尚未注册）
    CB cbs;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        auto it = _callbacks.find(sock);
        if (it != _callbacks.end()) cbs = it->second;
    }
    cbs.onRecv = std::move(onRecv);
    setCallbacks(sock, cbs);

    IoContext* ctx = s_ioCtxPool.acquire();
    ctx->opType = IoOpType::Recv;
    ctx->socket = sock;

    DWORD flags = 0;
    int ret = WSARecv(sock, &ctx->wsaBuf, 1,
        nullptr, &flags, &ctx->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
        s_ioCtxPool.release(ctx);
        return false;
    }
    return true;
}

bool TransportCore::postSend(
    SOCKET sock, const char* data, size_t len, OnSendCallback onSend) {
    if (sock == INVALID_SOCKET || !data || len == 0
        || len > sizeof(IoContext::buffer)) {
        return false;
    }

    // 确保 socket 在 callback map 中注册
    CB cbs;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        auto it = _callbacks.find(sock);
        if (it != _callbacks.end()) cbs = it->second;
    }
    cbs.onSend = std::move(onSend);
    setCallbacks(sock, cbs);

    IoContext* ctx = s_ioCtxPool.acquire();
    ctx->opType = IoOpType::Send;
    ctx->socket = sock;
    memcpy(ctx->buffer, data, len);
    ctx->wsaBuf.buf = reinterpret_cast<CHAR*>(ctx->buffer);
    ctx->wsaBuf.len = static_cast<ULONG>(len);

    DWORD sent = 0;
    int ret = WSASend(sock, &ctx->wsaBuf, 1,
        &sent, 0, &ctx->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != ERROR_IO_PENDING) {
        s_ioCtxPool.release(ctx);
        return false;
    }
    return true;
}

void TransportCore::closeSocket(
    SOCKET sock, OnCloseCallback onClose, int error) {
    if (sock == INVALID_SOCKET) return;
    // 先触发回调，再清理
    if (onClose) onClose(sock, error);
    removeCallbacks(sock);
    closesocket(sock);
}

bool TransportCore::bindToIocp(SOCKET sock) {
    auto h = CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), _iocp, 0, 0);
    if (h) configureKeepalive(sock);
    return h != nullptr;
}

void TransportCore::configureKeepalive(SOCKET s) {
    if (s == INVALID_SOCKET) return;

    // 启用 TCP keepalive
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
        reinterpret_cast<const char*>(&opt), sizeof(opt));

    // 通过 WSAIoctl 配置 keepalive 计时参数 (tcp_keepalive 定义在 mstcpip.h)
    tcp_keepalive kav;
    kav.onoff = 1;
    kav.keepalivetime   = 30000;  // 空闲 30 秒后开始 keepalive 探测
    kav.keepaliveinterval = 10000; // 探测间隔 10 秒

    DWORD bytes = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &kav, sizeof(kav),
        nullptr, 0, &bytes, nullptr, nullptr);
}

TransportCore::CB* TransportCore::getCallbacks(SOCKET sock) {
    std::lock_guard<std::mutex> lock(_cbMutex);
    auto it = _callbacks.find(sock);
    return (it != _callbacks.end()) ? &it->second : nullptr;
}

void TransportCore::setCallbacks(SOCKET sock, CB cbs) {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _callbacks[sock] = std::move(cbs);
}

void TransportCore::removeCallbacks(SOCKET sock) {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _callbacks.erase(sock);
}

std::string TransportCore::addrToString(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

uint16_t TransportCore::addrToPort(const sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

std::string TransportCore::addrToString(const sockaddr_storage& a) {
    return NetUtil::addrToIP(a);
}

uint16_t TransportCore::addrToPort(const sockaddr_storage& a) {
    return NetUtil::addrToPort(a);
}

} // namespace yunyi
