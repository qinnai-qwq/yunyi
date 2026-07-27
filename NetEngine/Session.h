/**
 * @file Session.h
 * @brief 网络会话基类 —— 对应一条 TCP 连接
 *
 * 继承 Ref，不关心业务，只知道收发数据。
 * 通过回调将数据/事件通知上层。
 *
 * 生命周期:
 *   1. 构造 Session(socket, transport)
 *   2. 设置回调 setOnData / setOnClose
 *   3. start() 开始异步接收
 *   4. 收发过程中 retain()/release() 管理引用
 *   5. close() 关闭连接，最终 release() 销毁
 *
 * @note Session 继承 Ref，close() 后由 _onCloseInternal 调用 release() 自毁。
 *       外部持有 Session* 时应先 retain()，不再需要时 release()。
 */
#pragma once
#include "Ref.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif
#include <functional>
#include <string>
namespace yunyi {

class TransportCore;

/**
 * @brief 数据接收回调
 * @param sock 数据来源 socket
 * @param data 数据指针（仅在回调期间有效）
 * @param len 数据长度（字节）
 */
using SessionDataCallback = std::function<void(SOCKET, const char*, size_t)>;

/**
 * @brief 连接关闭回调
 * @param sock 已关闭的 socket
 * @param err 关闭原因，0 = 正常
 */
using SessionCloseCallback = std::function<void(SOCKET, int)>;

/**
 * @class Session
 * @brief TCP 会话基类
 *
 * 封装单条 TCP 连接的异步收发。通过 TransportCore 投递 IOCP 请求，
 * 数据到达时回调 _onRecvInternal -> _onData，连接关闭时回调 _onCloseInternal -> _onClose。
 *
 * 子类可重写 _onRecvInternal / _onCloseInternal 实现自定义逻辑
 * （如 TLS 加解密后再回调上层）。
 */
class Session : public Ref {
public:
    /**
     * @brief 构造会话
     * @param sock 已连接的 TCP socket
     * @param transport TransportCore 实例指针
     * @pre sock 必须是有效的已连接 socket
     * @pre transport 必须在 Session 生命周期内保持有效
     */
    Session(SOCKET sock, TransportCore* transport);

    /**
     * @brief 虚析构，关闭 socket
     */
    virtual ~Session();

    /**
     * @brief 开始异步接收数据
     *
     * 投递第一个 WSARecv 请求，后续在 _onRecvInternal 中循环投递。
     * 必须在设置回调后调用。
     */
    void start();

    /**
     * @brief 异步发送数据
     * @param data 数据指针
     * @param len 数据长度
     * @return true 投递成功，false 失败（socket 已关闭或缓冲区满）
     * @pre data 指向的内存必须在发送回调触发前保持有效
     */
    bool send(const char* data, size_t len);

    /**
     * @brief 关闭连接
     *
     * 触发 TransportCore::closeSocket，最终回调 _onClose -> _onCloseInternal。
     */
    void close();

    /**
     * @brief 设置数据接收回调
     * @param cb 回调函数
     */
    void setOnData(SessionDataCallback cb) { _onData = std::move(cb); }

    /**
     * @brief 设置连接关闭回调
     * @param cb 回调函数
     */
    void setOnClose(SessionCloseCallback cb) { _onClose = std::move(cb); }

    /**
     * @brief 获取关联的 socket
     * @return SOCKET 句柄
     */
    SOCKET socket() const { return _sock; }

    /**
     * @brief 是否处于连接状态
     * @return true 已连接
     */
    bool isConnected() const { return _connected; }

    /**
     * @brief 获取关联的 TransportCore
     * @return TransportCore 指针
     */
    TransportCore* transport() const { return _transport; }

    /**
     * @brief 获取对端 IP 地址
     * @return "x.x.x.x" 格式字符串
     */
    std::string peerAddr() const;

    /**
     * @brief 获取对端端口
     * @return 主机字节序端口号
     */
    uint16_t peerPort() const;

protected:
    /** 关联的 socket */
    SOCKET _sock = INVALID_SOCKET;
    /** 关联的 TransportCore */
    TransportCore* _transport = nullptr;
    /** 连接状态 */
    std::atomic<bool> _connected{true};
    /** 数据回调 */
    SessionDataCallback _onData;
    /** 关闭回调 */
    SessionCloseCallback _onClose;

    /**
     * @brief 内部接收回调（从 TransportCore 投递）
     * @param sock 数据来源 socket
     * @param data 数据指针
     * @param len 数据长度
     *
     * 默认实现: 调用 _onData 后重新投递 recv 请求（保持异步循环）。
     * 子类可重写以插入 TLS 解密等逻辑。
     */
    void _onRecvInternal(SOCKET sock, const char* data, size_t len);

    /**
     * @brief 内部发送完成回调
     * @param sock 目标 socket
     * @param err 错误码
     */
    void _onSendInternal(SOCKET sock, int err);

    /**
     * @brief 内部关闭回调（从 TransportCore 投递）
     * @param sock 已关闭的 socket
     * @param err 关闭原因
     *
     * 默认实现: 调用 _onClose 后 release()（自毁）。
     * 子类可重写以添加清理逻辑。
     */
    void _onCloseInternal(SOCKET sock, int err);
};

}
