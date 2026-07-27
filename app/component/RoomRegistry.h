/**
 * @file RoomRegistry.h
 * @brief 房间注册表 —— 中继服务器用
 *
 * 管理所有活跃房间的信息：房间 ID、名称、端口映射、心跳时间等。
 * 支持超时检测：定期调用 findTimeoutRooms() 查找心跳过期的房间。
 */
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace yunyi {

/**
 * @brief 单个房间记录
 */
struct RoomRecord {
    /** 房间唯一 ID */
    uint32_t roomId = 0;
    /** 房间名称 */
    std::string roomName;
    /** 连接码（"IP:Port" 格式） */
    std::string connectionCode;
    /** 分配的公网端口 */
    uint16_t assignedPort = 0;
    /** 房主本地 MC 端口 */
    uint16_t localMcPort = 0;
    /** 创建时间戳（毫秒） */
    uint64_t createdAt = 0;
    /** 最后一次心跳时间戳（毫秒） */
    uint64_t lastHeartbeat = 0;
    /** 是否活跃 */
    bool active = true;
};

/**
 * @class RoomRegistry
 * @brief 房间注册表
 *
 * 维护 roomId -> RoomRecord 和 port -> roomId 的双向索引。
 * 线程安全，所有公有方法内部加锁。
 */
class RoomRegistry {
public:
    RoomRegistry() = default;

    /**
     * @brief 添加房间
     * @param id 房间 ID
     * @param name 房间名称
     * @param port 分配的公网端口
     * @param mcPort 房主本地 MC 端口
     * @param code 连接码
     */
    void addRoom(uint32_t id, const std::string& name,
                 uint16_t port, uint16_t mcPort,
                 const std::string& code);

    /**
     * @brief 移除房间
     * @param id 房间 ID
     * @return true 成功移除，false 房间不存在
     */
    bool removeRoom(uint32_t id);

    /**
     * @brief 按房间 ID 查找
     * @param id 房间 ID
     * @return RoomRecord 指针，未找到返回 nullptr
     */
    RoomRecord* findRoom(uint32_t id);

    /**
     * @brief 按端口查找房间
     * @param port 公网端口
     * @return RoomRecord 指针，未找到返回 nullptr
     */
    RoomRecord* findByPort(uint16_t port);

    /**
     * @brief 获取所有活跃房间列表
     * @return 活跃房间的 RoomRecord 副本列表
     */
    std::vector<RoomRecord> listActive() const;

    /**
     * @brief 更新房间心跳时间
     * @param id 房间 ID
     * @param ts 当前时间戳（毫秒）
     */
    void updateHeartbeat(uint32_t id, uint64_t ts);

    /**
     * @brief 查找心跳超时的房间
     * @param now 当前时间戳（毫秒）
     * @param ms 超时阈值（毫秒），距上次心跳超过此值视为超时
     * @return 超时房间的 ID 列表
     */
    std::vector<uint32_t> findTimeoutRooms(uint64_t now, uint64_t ms) const;

    /**
     * @brief 活跃房间数量
     * @return 当前 active == true 的房间数
     */
    size_t activeCount() const;

private:
    /** 互斥锁 */
    mutable std::mutex _mutex;
    /** roomId -> RoomRecord */
    std::unordered_map<uint32_t, RoomRecord> _rooms;
    /** port -> roomId 反向索引 */
    std::unordered_map<uint16_t, uint32_t> _portIndex;
};

}
