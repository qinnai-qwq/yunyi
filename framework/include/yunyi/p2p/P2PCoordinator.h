/**
 * @file P2PCoordinator.h
 * @brief P2P 协调服务器抽象接口 —— Framework 层定义的契约
 *
 * P2PTunnel 通过本接口与协调服务器（云驿后端）交互，具体实现（WinHTTP / 未来其他）
 * 由 App 层注入，Framework 不依赖具体 HTTP 库。
 */
#pragma once
#include <cstdint>
#include <string>

namespace yunyi {

/**
 * @class P2PCoordinator
 * @brief P2P 打洞协调服务器客户端接口
 *
 * 抽象"上报候选"与"轮询对端候选"两个操作，把网络细节留给实现层。
 */
class P2PCoordinator {
public:
    virtual ~P2PCoordinator() = default;

    /**
     * @brief 上报本端公网候选端点
     * @param roomId 房间 ID
     * @param candidateIp 本端公网候选 IP
     * @param candidatePort 本端公网候选端口
     * @return true 上报成功
     */
    virtual bool registerPeer(const std::string& roomId,
                              const std::string& candidateIp,
                              uint16_t candidatePort) = 0;

    /**
     * @brief 轮询对端公网候选端点
     * @param roomId 房间 ID
     * @param[out] outIp 对端候选 IP
     * @param[out] outPort 对端候选端口
     * @return true 已获得对端候选，false 尚无
     */
    virtual bool pollPeer(const std::string& roomId,
                          std::string& outIp, uint16_t& outPort) = 0;
};

} // namespace yunyi
