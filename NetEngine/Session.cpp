/**
 * @file Session.cpp
 * @brief TCP 会话基类实现 —— 异步收发 + 引用计数自毁
 */
#include "Session.h"
#include "TransportCore.h"

namespace yunyi {

Session::Session(SOCKET sock, TransportCore* transport)
    : Ref()
    , _sock(sock)
    , _transport(transport)
{
}

Session::~Session() {
    close();
}

void Session::start() {
    if (_transport && _sock != INVALID_SOCKET) {
        // 投递第一个 recv 请求，后续在 _onRecvInternal 中循环投递
        _transport->postRecv(_sock, [this](SOCKET s, const char* d, size_t l) {
            _onRecvInternal(s, d, l);
        });
    }
}

bool Session::send(const char* data, size_t len) {
    if (!_transport || _sock == INVALID_SOCKET || !_connected.load()) {
        return false;
    }
    return _transport->postSend(_sock, data, len, [this](SOCKET s, int e) {
        _onSendInternal(s, e);
    });
}

void Session::close() {
    if (_transport && _sock != INVALID_SOCKET) {
        _connected.store(false);
        _transport->closeSocket(_sock, [this](SOCKET s, int e) {
            _onCloseInternal(s, e);
        });
        _sock = INVALID_SOCKET;
    }
}

std::string Session::peerAddr() const {
    if (_sock == INVALID_SOCKET) return "";
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(_sock, (sockaddr*)&addr, &len) == 0) {
        return TransportCore::addrToString(addr);
    }
    return "";
}

uint16_t Session::peerPort() const {
    if (_sock == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(_sock, (sockaddr*)&addr, &len) == 0) {
        return TransportCore::addrToPort(addr);
    }
    return 0;
}

void Session::_onRecvInternal(SOCKET, const char* data, size_t len) {
    if (!_connected.load()) return;

    // 通知上层
    if (_onData) {
        _onData(_sock, data, len);
    }

    // 继续投递 recv 请求，形成异步循环
    if (_transport && _connected.load()) {
        _transport->postRecv(_sock, [this](SOCKET s, const char* d, size_t l) {
            _onRecvInternal(s, d, l);
        });
    }
}

void Session::_onSendInternal(SOCKET, int) {
    // 发送完成，当前无需额外处理
    // 若发送失败，TransportCore 会触发 onClose 回调
}

void Session::_onCloseInternal(SOCKET, int error) {
    _connected.store(false);
    _sock = INVALID_SOCKET;

    // 通知上层
    if (_onClose) {
        _onClose(_sock, error);
    }

    // 自毁：release() 会递减引用计数，归零时 delete this
    release();
}

} // namespace yunyi
