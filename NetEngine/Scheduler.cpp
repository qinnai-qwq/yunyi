/**
 * @file Scheduler.cpp
 * @brief 定时器调度器实现 —— tick 驱动、惰性删除
 */
#include "Scheduler.h"
#include <algorithm>

namespace yunyi {

uint64_t Scheduler::addTimer(uint64_t intervalMs, TimerCallback callback, bool repeating) {
    // 最小间隔 1ms，防止除零和过于频繁触发
    if (intervalMs == 0) intervalMs = 1;

    auto entry = std::make_shared<TimerEntry>();
    entry->id          = _nextId++;
    entry->intervalMs  = intervalMs;
    entry->nextFireMs  = _elapsedMs + intervalMs;
    entry->repeating   = repeating;
    entry->callback    = std::move(callback);

    std::lock_guard<std::mutex> lock(_mutex);
    _timers.push_back(std::move(entry));
    return _nextId - 1;
}

void Scheduler::cancelTimer(uint64_t id) {
    std::lock_guard<std::mutex> lock(_mutex);
    // 惰性删除：标记 cancelled + 清空 callback 防止 tick() 竞态执行
    for (auto& t : _timers) {
        if (t->id == id) {
            t->cancelled = true;
            t->callback = nullptr;  // 即使 tick() 已入列 toFire，执行时也是 no-op
            return;
        }
    }
}

void Scheduler::tick(uint64_t deltaMs) {
    _elapsedMs += deltaMs;

    // 收集到期的定时器（加锁范围最小化）
    std::vector<std::shared_ptr<TimerEntry>> toFire;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& t : _timers) {
            if (!t->cancelled && _elapsedMs >= t->nextFireMs) {
                toFire.push_back(t);
            }
        }
    }

    // 触发回调（不加锁）
    for (auto& t : toFire) {
        if (t->cancelled) continue;
        if (t->callback) t->callback();
        // 重复定时器：重新调度；一次性：标记取消
        if (t->repeating && !t->cancelled) {
            t->nextFireMs = _elapsedMs + t->intervalMs;
        } else {
            t->cancelled = true;
        }
    }

    // 清理已取消的定时器
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _timers.erase(
            std::remove_if(_timers.begin(), _timers.end(),
                [](const auto& t) { return t->cancelled; }),
            _timers.end());
    }
}

size_t Scheduler::activeCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _timers.size();
}

} // namespace yunyi
