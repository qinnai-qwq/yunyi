/**
 * @file AutoreleasePool.h
 * @brief 自动释放池 —— 管理 Ref 对象的释放时机
 *
 * 避免回调中途 delete 导致 use-after-free。
 * obj->autorelease() 将对象标记为待释放，pool.drain() 统一执行 release()。
 *
 * 典型用法:
 *   1. 帧开始时调用 drain() 清理上一帧的待释放对象
 *   2. 帧内各模块通过 autorelease() 标记不再需要的对象
 *   3. 下一帧 drain() 时统一释放
 *
 * @note 与 ResourcePool 职责不同:
 *       AutoreleasePool 管释放时机（延迟 delete），
 *       ResourcePool 管内存复用（对象池）。
 */
#pragma once
#include <mutex>
#include <vector>
namespace yunyi {

class Ref;

/**
 * @class AutoreleasePool
 * @brief 自动释放池
 *
 * 收集调用 autorelease() 的 Ref 对象，在 drain() 时统一 release()。
 * 线程安全：add() / drain() / size() 内部加锁。
 */
class AutoreleasePool {
public:
    AutoreleasePool() = default;

    /**
     * @brief 将对象加入待释放队列
     * @param obj 待释放的 Ref 对象
     * @pre obj 非空
     */
    void add(Ref* obj);

    /**
     * @brief 释放队列中所有对象
     * @post 队列清空，所有已加入的对象被 release()
     *
     * 通常每帧开始时调用一次。
     */
    void drain();

    /**
     * @brief 查询待释放对象数量
     * @return 队列中待释放的对象数
     */
    size_t size() const;

    AutoreleasePool(const AutoreleasePool&) = delete;
    AutoreleasePool& operator=(const AutoreleasePool&) = delete;

private:
    /** 互斥锁 */
    mutable std::mutex _mutex;
    /** 待释放对象队列 */
    std::vector<Ref*> _pending;
};

}
