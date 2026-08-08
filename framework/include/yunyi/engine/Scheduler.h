/**
 * @file Scheduler.h
 * @brief 定时器调度器 —— 心跳、超时检测、重连退避
 *
 * 由外部每帧调用 tick() 驱动，自身不创建线程。
 * 支持一次性定时器和重复定时器。
 */
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
namespace yunyi {

/**
 * @brief 定时器回调类型（无参数、无返回值）
 */
using TimerCallback = std::function<void()>;

/**
 * @brief 单个定时器条目
 */
struct TimerEntry {
    /** 定时器唯一 ID */
    uint64_t id;
    /** 触发间隔（毫秒） */
    uint64_t intervalMs;
    /** 下次触发时间点（基于 elapsed 累计毫秒） */
    uint64_t nextFireMs;
    /** true = 周期性触发，false = 一次性 */
    bool repeating;
    /** 取消标记，tick() 检测到此标记后移除 */
    bool cancelled = false;
    /** 触发时执行的回调 */
    TimerCallback callback;
};

/**
 * @class Scheduler
 * @brief 基于 tick 驱动的定时器调度器
 *
 * 不创建线程，由外部主循环周期性调用 tick(deltaMs)。
 * 每次 tick() 检查所有定时器，触发到期的回调。
 *
 * 用法:
 *   1. addTimer(ms, cb, repeat) 注册定时器
 *   2. 主循环每帧调用 tick(deltaMs)
 *   3. cancelTimer(id) 取消（惰性删除，下次 tick 清理）
 */
class Scheduler {
public:
    Scheduler() = default;

    /**
     * @brief 添加定时器
     * @param ms 触发间隔（毫秒），传入 0 会被提升为 1ms
     * @param cb 回调函数
     * @param repeat true = 周期性触发，false = 一次性
     * @return 定时器 ID（用于 cancelTimer）
     */
    uint64_t addTimer(uint64_t ms, TimerCallback cb, bool repeat = false);

    /**
     * @brief 取消定时器
     * @param id 由 addTimer() 返回的定时器 ID
     *
     * 惰性删除：标记 cancelled = true，在下次 tick() 时实际移除。
     */
    void cancelTimer(uint64_t id);

    /**
     * @brief 驱动定时器，每帧调用
     * @param deltaMs 距上次调用经过的毫秒数
     *
     * 检查所有未取消的定时器，触发已到期的回调。
     * 一次性定时器触发后自动移除，重复定时器重新调度。
     *
     * @pre 必须由外部周期性调用，否则定时器永不触发
     */
    void tick(uint64_t deltaMs);

    /**
     * @brief 当前活跃定时器数量
     * @return 未取消的定时器数（含待触发和已触发但未清理的重复定时器）
     */
    size_t activeCount() const;

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

private:
    /** 自增 ID 计数器 */
    uint64_t _nextId = 1;
    /** 累计经过的毫秒数 */
    uint64_t _elapsedMs = 0;
    /** 定时器列表互斥锁 */
    mutable std::mutex _mutex;
    /** 定时器列表 */
    std::vector<std::shared_ptr<TimerEntry>> _timers;
};

}
