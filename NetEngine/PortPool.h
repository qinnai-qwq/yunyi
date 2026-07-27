/**
 * @file PortPool.h
 * @brief 端口池 —— 自动分配/回收监听端口
 *
 * 中继服务器为每个房间分配独立端口，从端口池中取用。
 * 默认范围 40000-41000，共 1001 个可用端口。
 */
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_set>
namespace yunyi {

/**
 * @class PortPool
 * @brief 端口分配池
 *
 * 管理一个端口范围，支持 acquire（分配）/ release（回收）/ isFree（查询）。
 * 线程安全，内部加锁。
 */
class PortPool {
public:
    /**
     * @brief 构造端口池
     * @param start 起始端口（含），默认 40000
     * @param end 结束端口（含），默认 41000
     * @pre start <= end
     */
    PortPool(uint16_t start = 40000, uint16_t end = 41000);

    /**
     * @brief 分配一个端口
     * @return 分配的端口号，0 表示池已耗尽
     *
     * 采用线性扫描策略，从 start 到 end 查找第一个未使用端口。
     */
    uint16_t acquire();

    /**
     * @brief 回收端口
     * @param port 要回收的端口号
     *
     * 若端口不在池范围内或未被使用，静默忽略。
     */
    void release(uint16_t port);

    /**
     * @brief 查询端口是否空闲
     * @param port 端口号
     * @return true 空闲
     */
    bool isFree(uint16_t port) const;

    /**
     * @brief 已分配端口数量
     * @return 当前已使用端口数
     */
    size_t usedCount() const;

    /**
     * @brief 端口池总容量
     * @return end - start + 1
     */
    size_t totalCount() const { return _total; }

    /**
     * @brief 端口池是否已耗尽
     * @return true 无可用端口
     */
    bool isExhausted() const { return usedCount() >= _total; }

private:
    /** 起始端口 */
    uint16_t _start;
    /** 结束端口 */
    uint16_t _end;
    /** 总端口数 */
    size_t _total;
    /** 互斥锁 */
    mutable std::mutex _mutex;
    /** 已分配端口集合 */
    std::unordered_set<uint16_t> _used;
};

}
