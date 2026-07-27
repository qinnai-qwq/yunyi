/**
 * @file NetUtil.h
 * @brief IPv4/IPv6 自适应网络工具
 *
 * 自动检测 IP 版本，创建对应 socket，统一处理地址转换。
 * 所有需要网络 I/O 的模块通过此工具创建 socket，无需关心 IP 版本。
 */
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace yunyi {

struct NetUtil {
    /** 判断 IP 字符串是否为 IPv6 */
    static bool isIPv6(std::string_view ip);

    /**
     * @brief 创建 TCP socket，自动选择 AF_INET6 或 AF_INET
     * @param ip 目标 IP（空字符串 = IPv4 默认）
     */
    static SOCKET createSocket(std::string_view ip = "");

    /**
     * @brief 用 IP:Port 填充 sockaddr_storage，自动适配 IPv4/IPv6
     * @param ip   目标 IP 地址（文本格式）
     * @param port 目标端口（主机字节序）
     * @param out  输出 sockaddr_storage
     * @return 实际 sockaddr 大小（sizeof(sockaddr_in) 或 sizeof(sockaddr_in6)），0 = 失败
     */
    static int fillAddr(const char* ip, uint16_t port, sockaddr_storage& out);

    /**
     * @brief 从 sockaddr_storage 提取 IP 字符串
     */
    static std::string addrToIP(const sockaddr_storage& addr);

    /**
     * @brief 从 sockaddr_storage 提取端口号
     */
    static uint16_t addrToPort(const sockaddr_storage& addr);

    /** 将 sockaddr_in 转为 "IP:Port"（IPv4 兼容，沿用旧接口） */
    static std::string addrToString(const sockaddr_in& addr);
    static uint16_t addrToPort(const sockaddr_in& addr);

    /**
     * @brief AcceptEx 缓冲区大小 — 取 IPv4 和 IPv6 中较大者
     *
     * IPv4: sizeof(sockaddr_in) + 16 = 32 字节，两端共 64 字节
     * IPv6: sizeof(sockaddr_in6) + 16 = 44 字节，两端共 88 字节
     */
    static constexpr DWORD kAcceptAddrLen = sizeof(sockaddr_in6) + 16;
};

} // namespace yunyi
