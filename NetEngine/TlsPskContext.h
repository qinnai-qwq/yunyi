/**
 * @file TlsPskContext.h
 * @brief OpenSSL TLS-PSK 封装 —— 通过 memory BIO 与 IOCP 对接
 *
 * IOCP 只管密文字节收发，本模块在内存中完成加解密转换。
 * 采用 memory BIO 模式，不直接操作 socket，由上层喂入密文/取出明文。
 *
 * @note 当前支持 TLS-PSK（预共享密钥），结构体预留扩展其他加密方式。
 */
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
struct ssl_ctx_st;
struct ssl_st;
struct bio_st;
namespace yunyi {

/**
 * @brief TLS 角色
 */
enum class TlsRole {
    /** TLS 服务端（中继服务器） */
    Server,
    /** TLS 客户端（房主端） */
    Client
};

/**
 * @class TlsPskContext
 * @brief OpenSSL TLS-PSK 上下文
 *
 * 封装 OpenSSL SSL_CTX/SSL 对象，使用 memory BIO 实现 TLS 加解密。
 * 不直接操作 socket —— 上层负责从 socket 读取密文喂入，取出明文后处理。
 *
 * 握手流程:
 *   1. init(psk, identity, role) 创建 SSL 上下文
 *   2. 循环调用 doHandshake() 直到返回 Done
 *   3. 握手完成后用 encrypt()/decrypt() 加解密数据
 *
 * 生命周期:
 *   1. 构造 TlsPskContext
 *   2. init(psk, identity, role) 初始化
 *   3. doHandshake() 完成 TLS 握手
 *   4. encrypt()/decrypt() 收发数据
 *   5. cleanup() 或析构释放资源
 */
class TlsPskContext {
public:
    TlsPskContext();
    ~TlsPskContext();

    /**
     * @brief 初始化 TLS 上下文
     * @param psk 预共享密钥（UTF-8），最少 16 字节
     * @param identity PSK 身份标识
     * @param role TLS 角色（Server/Client）
     * @return true 成功，false 失败（PSK 过短或 OpenSSL 初始化失败）
     */
    bool init(const std::string& psk, const std::string& identity, TlsRole role);

    /**
     * @brief 查询 TLS 握手是否已完成
     * @return true 握手完成，可以开始加解密
     */
    bool isHandshakeDone() const { return _handshakeDone; }

    /**
     * @brief TLS 握手结果
     */
    enum class HandshakeResult {
        /** 握手完成，可以开始加解密数据 */
        Done,
        /** 需要更多数据，请继续喂入对端发来的握手数据 */
        NeedMoreData,
        /** 握手失败（证书/密钥不匹配等） */
        Failed
    };

    /**
     * @brief 执行 TLS 握手
     * @param input 对端发来的握手数据（可为 nullptr 表示无输入）
     * @param inputLen 输入数据长度
     * @param[out] output 本端需要发送给对端的握手数据（追加到此 vector）
     * @return Done = 握手完成，NeedMoreData = 需要继续握手，Failed = 失败
     *
     * 握手期间需反复调用：每次将从 socket 收到的数据传入 input，
     * 将 output 中的数据发送给对端，直到返回 Done。
     */
    HandshakeResult doHandshake(const uint8_t* input, size_t inputLen,
                                 std::vector<uint8_t>& output);

    /**
     * @brief TLS 加密（明文 -> 密文）
     * @param pt 明文数据指针
     * @param len 明文长度
     * @param[out] ct 密文输出（追加到此 vector）
     * @return true 成功，false 失败
     * @pre isHandshakeDone() == true
     */
    bool encrypt(const uint8_t* pt, size_t len, std::vector<uint8_t>& ct);

    /**
     * @brief TLS 解密（密文 -> 明文）
     * @param ct 密文数据指针
     * @param len 密文长度
     * @param[out] pt 明文输出（追加到此 vector）
     * @return true 成功，false 失败
     * @pre isHandshakeDone() == true
     */
    bool decrypt(const uint8_t* ct, size_t len, std::vector<uint8_t>& pt);

    /**
     * @brief 手动清理 OpenSSL 资源
     *
     * 析构函数自动调用，也可提前调用以便尽早释放资源。
     */
    void cleanup();

    TlsPskContext(const TlsPskContext&) = delete;
    TlsPskContext& operator=(const TlsPskContext&) = delete;

private:
    ssl_ctx_st* _sslCtx = nullptr;
    ssl_st*     _ssl = nullptr;
    bio_st*      _readBio = nullptr;
    bio_st*      _writeBio = nullptr;
    bool         _handshakeDone = false;
    TlsRole      _role = TlsRole::Client;
    /** SSL 操作互斥锁（OpenSSL SSL 对象非线程安全） */
    mutable std::mutex _sslMutex;

public:
    /** PSK 凭据（供 PSK 回调通过 SSL_get_app_data 跨线程访问） */
    std::string  _psk;
    std::string  _pskIdentity;

private:
};

}
