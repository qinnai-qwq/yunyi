/**
 * @file TlsPskContext.cpp
 * @brief OpenSSL TLS-PSK 封装实现
 *
 * 双路径编译:
 *   - 未定义 YUNYI_HAS_OPENSSL: 全部返回 false/Failed（桩实现）
 *   - 定义 YUNYI_HAS_OPENSSL:  使用 OpenSSL memory BIO 实现完整 TLS-PSK
 *
 * 使用方式: 在项目预处理器定义中添加 YUNYI_HAS_OPENSSL 并链接 OpenSSL 库。
 */
#include "Logger.h"
#include "TlsPskContext.h"
#include <cstring>
#include <iostream>

// 如需启用 OpenSSL，取消下面这行注释并确保链接了 OpenSSL 库
// #define YUNYI_HAS_OPENSSL

#ifdef YUNYI_HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#endif

namespace yunyi {

// ============================================================
//  桩实现（未启用 OpenSSL 时使用）
// ============================================================

#ifndef YUNYI_HAS_OPENSSL

TlsPskContext::TlsPskContext() = default;

TlsPskContext::~TlsPskContext() {}

bool TlsPskContext::init(
    const std::string& /*psk*/,
    const std::string& /*identity*/,
    TlsRole /*role*/) {
    return false;
}

TlsPskContext::HandshakeResult TlsPskContext::doHandshake(
    const uint8_t* /*input*/, size_t /*inputLen*/,
    std::vector<uint8_t>& /*output*/) {
    return HandshakeResult::Failed;
}

bool TlsPskContext::encrypt(
    const uint8_t* /*plaintext*/, size_t /*len*/,
    std::vector<uint8_t>& /*ciphertext*/) {
    return false;
}

bool TlsPskContext::decrypt(
    const uint8_t* /*ciphertext*/, size_t /*len*/,
    std::vector<uint8_t>& /*plaintext*/) {
    return false;
}

void TlsPskContext::cleanup() {}

#else // YUNYI_HAS_OPENSSL

// ============================================================
//  完整 OpenSSL 实现（启用 YUNYI_HAS_OPENSSL 时使用）
// ============================================================

namespace {

/**
 * @brief TLS-PSK 客户端回调
 *
 * 通过 SSL_get_app_data 获取 TlsPskContext*，避免 thread_local 跨线程问题。
 */
unsigned int pskClientCallback(
    SSL* ssl, const char* /*hint*/,
    char* identity, unsigned int maxIdentityLen,
    unsigned char* psk, unsigned int maxPskLen) {
    auto* ctx = static_cast<TlsPskContext*>(SSL_get_app_data(ssl));
    if (!ctx) return 0;
    if (ctx->_psk.empty()) return 0;

    size_t idLen = ctx->_pskIdentity.size();
    if (idLen >= maxIdentityLen) idLen = maxIdentityLen - 1;
    memcpy(identity, ctx->_pskIdentity.c_str(), idLen);
    identity[idLen] = '\0';

    size_t pskLen = ctx->_psk.size();
    if (pskLen > maxPskLen) pskLen = maxPskLen;
    memcpy(psk, ctx->_psk.c_str(), pskLen);
    return static_cast<unsigned int>(pskLen);
}

/**
 * @brief TLS-PSK 服务端回调
 */
unsigned int pskServerCallback(
    SSL* ssl, const char* identity,
    unsigned char* psk, unsigned int maxPskLen) {
    auto* ctx = static_cast<TlsPskContext*>(SSL_get_app_data(ssl));
    if (!ctx) return 0;
    if (!identity || ctx->_psk.empty()) return 0;
    if (ctx->_pskIdentity != identity) return 0;

    size_t pskLen = ctx->_psk.size();
    if (pskLen > maxPskLen) pskLen = maxPskLen;
    memcpy(psk, ctx->_psk.c_str(), pskLen);
    return static_cast<unsigned int>(pskLen);
}

} // anonymous namespace

TlsPskContext::TlsPskContext() = default;

TlsPskContext::~TlsPskContext() {
    cleanup();
}

bool TlsPskContext::init(
    const std::string& psk,
    const std::string& identity,
    TlsRole role) {
    // PSK 最少 16 字节
    if (psk.size() < 16) return false;

    _role        = role;
    _psk         = psk;
    _pskIdentity = identity.empty() ? "yunyi" : identity;

    // 创建 SSL_CTX
    const SSL_METHOD* method = (role == TlsRole::Server)
        ? TLS_server_method() : TLS_client_method();
    _sslCtx = SSL_CTX_new(method);
    if (!_sslCtx) return false;

    // 最低 TLS 1.3
    SSL_CTX_set_min_proto_version(_sslCtx, TLS1_3_VERSION);

    // 注册 PSK 回调
    if (role == TlsRole::Client) {
        SSL_CTX_set_psk_client_callback(_sslCtx, pskClientCallback);
    } else {
        SSL_CTX_set_psk_server_callback(_sslCtx, pskServerCallback);
    }

    // 创建 SSL 对象
    _ssl = SSL_new(_sslCtx);
    if (!_ssl) {
        SSL_CTX_free(_sslCtx);
        _sslCtx = nullptr;
        return false;
    }

    // 将 this 存到 SSL 对象，PSK 回调通过 SSL_get_app_data 取回
    // (替代 thread_local，解决 IOCP 跨线程回调问题)
    SSL_set_app_data(_ssl, this);

    // 创建 memory BIO 对（不直接操作 socket）
    _readBio  = BIO_new(BIO_s_mem());
    _writeBio = BIO_new(BIO_s_mem());
    if (!_readBio || !_writeBio) {
        cleanup();
        return false;
    }

    SSL_set_bio(_ssl, _readBio, _writeBio);

    // 设置握手状态
    if (role == TlsRole::Server) {
        SSL_set_accept_state(_ssl);
    } else {
        SSL_set_connect_state(_ssl);
    }
    return true;
}

TlsPskContext::HandshakeResult TlsPskContext::doHandshake(
    const uint8_t* input, size_t inputLen,
    std::vector<uint8_t>& output) {
    std::lock_guard<std::mutex> lock(_sslMutex);
    if (_handshakeDone) return HandshakeResult::Done;
    if (!_ssl || !_readBio || !_writeBio) return HandshakeResult::Failed;

    output.clear();

    // 将对端发来的数据写入 read BIO
    if (input && inputLen > 0) {
        int written = BIO_write(_readBio, input,
            static_cast<int>(inputLen));
        if (written <= 0) return HandshakeResult::Failed;
    }

    // 执行握手
    int ret = SSL_do_handshake(_ssl);
    if (ret == 1) {
        // 握手完成
        _handshakeDone = true;
        size_t pending = BIO_ctrl_pending(_writeBio);
        if (pending > 0) {
            output.resize(pending);
            BIO_read(_writeBio, output.data(), static_cast<int>(pending));
        }
        return HandshakeResult::Done;
    }

    int err = SSL_get_error(_ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // 需要更多数据：读取 write BIO 中待发送的握手数据
        size_t pending = BIO_ctrl_pending(_writeBio);
        if (pending > 0) {
            output.resize(pending);
            BIO_read(_writeBio, output.data(), static_cast<int>(pending));
        }
        return HandshakeResult::NeedMoreData;
    }

    // 握手失败
    unsigned long sslErr = ERR_get_error();
    char errBuf[256]{};
    ERR_error_string_n(sslErr, errBuf, sizeof(errBuf));
    CC_LOG(std::string("[TlsPskContext] handshake failed: ") + errBuf);
    return HandshakeResult::Failed;
}

bool TlsPskContext::encrypt(
    const uint8_t* plaintext, size_t len,
    std::vector<uint8_t>& ciphertext) {
    std::lock_guard<std::mutex> lock(_sslMutex);
    if (!_ssl || !_writeBio || !_handshakeDone) return false;

    ciphertext.clear();

    // SSL_write 将明文写入 SSL，密文输出到 write BIO
    int written = SSL_write(_ssl, plaintext, static_cast<int>(len));
    if (written <= 0) return false;

    // 从 write BIO 读取密文
    size_t pending = BIO_ctrl_pending(_writeBio);
    if (pending > 0) {
        ciphertext.resize(pending);
        BIO_read(_writeBio, ciphertext.data(), static_cast<int>(pending));
    }
    return true;
}

bool TlsPskContext::decrypt(
    const uint8_t* ciphertext, size_t len,
    std::vector<uint8_t>& plaintext) {
    std::lock_guard<std::mutex> lock(_sslMutex);
    if (!_ssl || !_readBio || !_handshakeDone) return false;

    plaintext.clear();

    // 将密文写入 read BIO，SSL_read 从中解密出明文
    int written = BIO_write(_readBio, ciphertext, static_cast<int>(len));
    if (written <= 0) return false;

    uint8_t buf[65536];
    int total = 0;
    int n = 0;
    while ((n = SSL_read(_ssl, buf + total,
        static_cast<int>(sizeof(buf) - total))) > 0) {
        total += n;
    }
    if (total > 0) {
        plaintext.assign(buf, buf + total);
        return true;
    }

    // 检查是否需要更多数据（非错误情况）
    int err = SSL_get_error(_ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return true;  // 不是错误，只是需要更多数据
    }
    return false;
}

void TlsPskContext::cleanup() {
    if (_ssl) {
        SSL_shutdown(_ssl);
        SSL_free(_ssl);
        _ssl = nullptr;
    }
    // readBio/writeBio 由 SSL_free 内部释放（SSL_set_bio 后 BIO 由 SSL 持有）
    _readBio  = nullptr;
    _writeBio = nullptr;
    if (_sslCtx) {
        SSL_CTX_free(_sslCtx);
        _sslCtx = nullptr;
    }
    _handshakeDone = false;
}

#endif // YUNYI_HAS_OPENSSL

} // namespace yunyi
