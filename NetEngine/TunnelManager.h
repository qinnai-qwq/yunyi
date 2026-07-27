/**
 * @file TunnelManager.h
 * @brief 数据隧道生命周期管理 —— 跟踪活跃隧道、配对、清理
 *
 * 每条隧道连接一个玩家（明文 TCP）和一个房主数据连接（TLS-PSK），
 * 实现双向字节转发。隧道由中继服务器管理。
 *
 * 生命周期:
 *   1. createPendingTunnel() 创建待配对隧道（已有玩家连接）
 *   2. pairTunnel() 绑定房主数据连接，开始转发
 *   3. removeTunnel() 关闭隧道并清理
 */
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace yunyi {

class Session;

/**
 * @brief 单条隧道信息
 */
struct TunnelInfo {
    /** 玩家连接 ID */
    uint32_t playerConnId = 0;
    /** 隧道端 Session（房主数据连接，TLS-PSK） */
    Session* tunnelSession = nullptr;
    /** 玩家端 Session（明文 TCP） */
    Session* playerSession = nullptr;
    /** 已转发字节数 */
    uint64_t bytesRelayed = 0;
    /** 是否已配对（双端就绪） */
    bool paired = false;
    /** 隧道 TLS 上下文（用于加密玩家→房主方向数据） */
    std::shared_ptr<std::unique_ptr<class TlsPskContext>> tunnelTls;
    /** TLS 操作互斥锁（保护 encrypt/decrypt 并发调用） */
    std::shared_ptr<std::mutex> tlsMutex;
    /** 隧道配对前的玩家数据缓冲 */
    std::vector<std::vector<uint8_t>> pendingPlayerData;
};

/**
 * @class TunnelManager
 * @brief 隧道管理器
 *
 * 管理所有活跃隧道，支持创建、配对、查询、移除。
 * 线程安全，所有公有方法内部加锁。
 */
class TunnelManager {
public:
    TunnelManager() = default;

    /**
     * @brief 创建待配对隧道（只含玩家端）
     * @param id 玩家连接 ID（关联的 OPEN_STREAM playerConnId）
     * @param ps 玩家端 Session（明文 TCP）
     * @return 创建的 TunnelInfo 指针
     *
     * 此时隧道仅有玩家端，等待 pairTunnel() 绑定房主数据连接。
     */
    TunnelInfo* createPendingTunnel(uint32_t id, Session* ps);

    /**
     * @brief 绑定房主数据连接，完成隧道配对
     * @param id 玩家连接 ID
     * @param ts 房主数据连接 Session（TLS-PSK）
     * @return 配对后的 TunnelInfo 指针，nullptr 表示未找到或已配对
     *
     * 配对后双端数据开始互相转发。
     */
    TunnelInfo* pairTunnel(uint32_t id, Session* ts);

    /**
     * @brief 移除隧道
     * @param id 玩家连接 ID
     *
     * 关闭并释放双端 Session，从映射表中移除。
     */
    void removeTunnel(uint32_t id);

    /**
     * @brief 查找隧道
     * @param id 玩家连接 ID
     * @return TunnelInfo 指针，未找到返回 nullptr
     */
    TunnelInfo* findTunnel(uint32_t id);

    /**
     * @brief 活跃隧道数量
     * @return 当前隧道总数
     */
    size_t activeCount() const;

    /**
     * @brief 获取累计转发总字节数
     * @return 所有隧道的 bytesRelayed 总和
     */
    uint64_t totalBytesRelayed() const;

    /**
     * @brief 获取隧道快照
     * @return 所有隧道信息的副本
     */
    std::vector<TunnelInfo> snapshot() const;

private:
    /** 互斥锁 */
    mutable std::mutex _mutex;
    /** playerConnId -> TunnelInfo */
    std::unordered_map<uint32_t, std::unique_ptr<TunnelInfo>> _tunnels;
};

}
