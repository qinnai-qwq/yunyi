/**
 * @file NetUtil.cpp
 * @brief IPv4/IPv6 自适应网络工具实现
 */
#include "NetUtil.h"
#include <cstring>
#include <iostream>

namespace yunyi {

bool NetUtil::isIPv6(std::string_view ip) {
    // 含 ':' 即为 IPv6（双栈地址如 ::ffff:1.2.3.4 也走 IPv6 socket）
    return ip.find(':') != std::string_view::npos;
}

SOCKET NetUtil::createSocket(std::string_view ip) {
    int family = isIPv6(ip) ? AF_INET6 : AF_INET;
    SOCKET sock = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    // IPv6 设置双栈模式（允许通过 IPv4-mapped 地址接受 IPv4 连接）
    if (family == AF_INET6) {
        int opt = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
            reinterpret_cast<const char*>(&opt), sizeof(opt));
    }
    return sock;
}

int NetUtil::fillAddr(const char* ip, uint16_t port, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));

    if (isIPv6(ip)) {
        auto* addr6 = reinterpret_cast<sockaddr_in6*>(&out);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port   = htons(port);
        if (inet_pton(AF_INET6, ip, &addr6->sin6_addr) != 1)
            return 0;
        return sizeof(sockaddr_in6);
    } else {
        auto* addr4 = reinterpret_cast<sockaddr_in*>(&out);
        addr4->sin_family = AF_INET;
        addr4->sin_port   = htons(port);
        if (inet_pton(AF_INET, ip, &addr4->sin_addr) != 1)
            return 0;
        return sizeof(sockaddr_in);
    }
}

std::string NetUtil::addrToIP(const sockaddr_storage& addr) {
    char buf[INET6_ADDRSTRLEN]{};
    if (addr.ss_family == AF_INET6) {
        auto& a6 = reinterpret_cast<const sockaddr_in6&>(addr);
        inet_ntop(AF_INET6, &a6.sin6_addr, buf, sizeof(buf));
    } else {
        auto& a4 = reinterpret_cast<const sockaddr_in&>(addr);
        inet_ntop(AF_INET, &a4.sin_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

uint16_t NetUtil::addrToPort(const sockaddr_storage& addr) {
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6&>(addr).sin6_port);
    } else {
        return ntohs(reinterpret_cast<const sockaddr_in&>(addr).sin_port);
    }
}

std::string NetUtil::addrToString(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

uint16_t NetUtil::addrToPort(const sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

} // namespace yunyi
