/**
 * @file StunClient.h
 * @brief STUN 客户端 —— 获取本机公网 UDP 映射端点（NAT 打洞候选）
 *
 * 实现 RFC 8489 最小子集（Binding 请求/响应 + XOR-MAPPED-ADDRESS）：
 *   1. 向 STUN 服务器发 UDP Binding 请求
 *   2. 服务器回显"观察到的源地址"（XOR-MAPPED-ADDRESS）
 *   3. 解析出本机的公网映射 (ip:port)，作为 NAT 打洞候选端点
 *
 * 同步阻塞实现（带超时），用于打洞前的候选收集阶段。
 */
#pragma once
#include <cstdint>
#include <string>

namespace yunyi {

class StunClient {
public:
    /**
     * @brief 同步 STUN 查询，获取本机公网映射端点
     * @param serverHost STUN 服务器主机名或 IP（如 mc.qinnai.xyz）
     * @param serverPort STUN 服务器端口
     * @param[out] mappedIp   公网映射 IP
     * @param[out] mappedPort 公网映射端口
     * @param timeoutMs 超时（ms），默认 3000
     * @return true 成功获取映射
     * @note 阻塞调用。服务器需支持 STUN Binding 响应。
     */
    static bool query(const char* serverHost, uint16_t serverPort,
                      std::string& mappedIp, uint16_t& mappedPort,
                      uint32_t timeoutMs = 3000);
};

} // namespace yunyi
