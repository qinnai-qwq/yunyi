/**
 * @file StunClient.cpp
 * @brief STUN 客户端实现（RFC 8489 最小子集）
 */
#include "yunyi/util/StunClient.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include <cstring>
#include <random>

namespace yunyi {

namespace {

// STUN 常量
constexpr uint16_t kBindingRequest   = 0x0001;
constexpr uint16_t kBindingResponse  = 0x0101;
constexpr uint32_t kMagicCookie      = 0x2112A442;
constexpr uint16_t kAttrXorMapped    = 0x0020;
constexpr uint16_t kAttrXorMappedOld = 0x0020;
constexpr size_t   kTxnIdLen         = 12;

// IPv4 XOR 掩码：magic cookie 32 位
// IPv6 XOR 掩码：magic cookie(32) + transaction id(96)

/**
 * @brief 解析 XOR-MAPPED-ADDRESS 属性
 * @param attr 属性数据（不含属性头）
 * @param attrLen 属性长度
 * @param cookie 请求中的 magic cookie
 * @param txnId 请求中的 transaction id（IPv6 用）
 * @param[out] ip 解析出的 IP
 * @param[out] port 解析出的端口
 * @return true 解析成功
 */
bool parseXorMapped(const uint8_t* attr, uint16_t attrLen,
                    uint32_t cookie, const uint8_t* txnId,
                    std::string& ip, uint16_t& port) {
    if (attrLen < 4) return false;
    uint8_t family = attr[1];
    if (family == 0x01) {  // IPv4
        if (attrLen < 8) return false;
        uint16_t xport = (static_cast<uint16_t>(attr[2]) << 8) | attr[3];
        port = static_cast<uint16_t>(xport ^ static_cast<uint16_t>(cookie >> 16));
        uint32_t xip = (static_cast<uint32_t>(attr[4]) << 24)
                     | (static_cast<uint32_t>(attr[5]) << 16)
                     | (static_cast<uint32_t>(attr[6]) << 8)
                     | attr[7];
        uint32_t realIp = xip ^ cookie;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &realIp, buf, sizeof(buf));
        ip = buf;
        return true;
    } else if (family == 0x02) {  // IPv6
        if (attrLen < 20) return false;
        uint16_t xport = (static_cast<uint16_t>(attr[2]) << 8) | attr[3];
        port = static_cast<uint16_t>(xport ^ static_cast<uint16_t>(cookie >> 16));
        unsigned char xip[16];
        memcpy(xip, attr + 4, 16);
        // XOR with (cookie + txnId) 大端
        uint8_t mask[16];
        mask[0] = static_cast<uint8_t>(cookie >> 24);
        mask[1] = static_cast<uint8_t>(cookie >> 16);
        mask[2] = static_cast<uint8_t>(cookie >> 8);
        mask[3] = static_cast<uint8_t>(cookie);
        memcpy(mask + 4, txnId, kTxnIdLen);
        unsigned char realIp[16];
        for (int i = 0; i < 16; ++i) realIp[i] = xip[i] ^ mask[i];
        char buf[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, realIp, buf, sizeof(buf));
        ip = buf;
        return true;
    }
    return false;
}

} // anonymous namespace

bool StunClient::query(const char* serverHost, uint16_t serverPort,
                       std::string& mappedIp, uint16_t& mappedPort,
                       uint32_t timeoutMs) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    // 解析服务器地址
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", serverPort);
    if (getaddrinfo(serverHost, portStr, &hints, &res) != 0) {
        WSACleanup();
        return false;
    }

    SOCKET sock = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        WSACleanup();
        return false;
    }

    // 接收超时
    DWORD t = timeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&t), sizeof(t));

    // 构造 Binding 请求
    uint8_t txn[kTxnIdLen];
    std::random_device rd;
    for (int i = 0; i < static_cast<int>(kTxnIdLen); ++i) {
        txn[i] = static_cast<uint8_t>(rd());
    }
    uint8_t req[20];
    req[0] = static_cast<uint8_t>(kBindingRequest >> 8);
    req[1] = static_cast<uint8_t>(kBindingRequest & 0xFF);
    req[2] = 0; req[3] = 0;  // message length = 0
    req[4] = static_cast<uint8_t>(kMagicCookie >> 24);
    req[5] = static_cast<uint8_t>(kMagicCookie >> 16);
    req[6] = static_cast<uint8_t>(kMagicCookie >> 8);
    req[7] = static_cast<uint8_t>(kMagicCookie);
    memcpy(req + 8, txn, kTxnIdLen);

    bool ok = false;
    if (sendto(sock, reinterpret_cast<const char*>(req), sizeof(req), 0,
            res->ai_addr, static_cast<int>(res->ai_addrlen)) != SOCKET_ERROR) {
        // 等待响应（同步阻塞，带超时）
        uint8_t buf[1024];
        sockaddr_storage from{};
        int fromLen = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n >= 20) {
            uint16_t type = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
            uint32_t cookie = (static_cast<uint32_t>(buf[4]) << 24)
                            | (static_cast<uint32_t>(buf[5]) << 16)
                            | (static_cast<uint32_t>(buf[6]) << 8)
                            | buf[7];
            if (type == kBindingResponse && cookie == kMagicCookie) {
                // 校验 transaction id
                if (memcmp(buf + 8, txn, kTxnIdLen) == 0) {
                    // 解析属性
                    int pos = 20;
                    int msgLen = static_cast<int>(n);
                    while (pos + 4 <= msgLen) {
                        uint16_t atype = (static_cast<uint16_t>(buf[pos]) << 8) | buf[pos + 1];
                        uint16_t alen  = (static_cast<uint16_t>(buf[pos + 2]) << 8) | buf[pos + 3];
                        if (atype == kAttrXorMapped && alen >= 4
                            && pos + 4 + alen <= msgLen) {
                            std::string ip;
                            uint16_t port = 0;
                            if (parseXorMapped(buf + pos + 4, alen, cookie, txn, ip, port)) {
                                mappedIp = ip;
                                mappedPort = port;
                                ok = true;
                                break;
                            }
                        }
                        pos += 4 + alen;
                        // 属性按 4 字节对齐
                        if (alen % 4 != 0) pos += 4 - (alen % 4);
                    }
                }
            }
        }
    }

    closesocket(sock);
    freeaddrinfo(res);
    WSACleanup();
    return ok;
}

} // namespace yunyi
