/**
 * @file ResourcePool.h
 * @brief 高频对象复用池（模板类）
 *
 * 适用场景: OVERLAPPED 上下文、收发缓冲区、Session 等高频创建/销毁对象。
 * 内部使用 std::stack 管理空闲对象，acquire() 优先从池中取用，
 * 池空时 new 新对象；release() 归还对象（reset() 后入池）。
 *
 * @tparam T 池化对象类型，需满足:
 *           - 默认构造函数（new T() 或 std::make_unique<T>()）
 *           - void reset() 方法（归还时调用以恢复初始状态）
 */
#pragma once
#include <memory>
#include <mutex>
#include <stack>
namespace yunyi {

template<typename T>
class ResourcePool {
public:
    /**
     * @brief 构造资源池
     * @param prealloc 预分配对象数量，默认 64
     * @param maxSize 池最大容量，0 = 无限制（默认）
     */
    explicit ResourcePool(size_t prealloc = 64, size_t maxSize = 0)
        : _maxSize(maxSize) {
        for (size_t i = 0; i < prealloc; ++i)
            _pool.push(std::make_unique<T>());
    }

    /**
     * @brief 获取一个对象
     * @return T* 指针，调用方负责最终释放（通过 release() 归还）
     *
     * 优先从池中取用，池空时 new T()。
     */
    T* acquire() {
        std::lock_guard<std::mutex> l(_mutex);
        T* obj;
        if (_pool.empty())
            obj = new T();
        else {
            auto p = std::move(_pool.top());
            _pool.pop();
            obj = p.release();
        }
        obj->reset();
        return obj;
    }

    /**
     * @brief 归还对象到池中
     * @param obj 待归还的对象指针（可为 nullptr，静默忽略）
     *
     * 调用 obj->reset() 后入池。若池已满（maxSize > 0 且池容量 >= maxSize），
     * 直接 delete obj。
     *
     * @pre obj 必须是通过此池的 acquire() 获取，或由 new T() 创建
     */
    void release(T* obj) {
        if (!obj) return;
        obj->reset();
        std::lock_guard<std::mutex> l(_mutex);
        if (_maxSize > 0 && _pool.size() >= _maxSize) {
            delete obj;
            return;
        }
        _pool.push(std::unique_ptr<T>(obj));
    }

    /**
     * @brief 查询池中可用对象数量
     * @return 池中空闲对象数
     */
    size_t available() const {
        std::lock_guard<std::mutex> l(_mutex);
        return _pool.size();
    }

private:
    /** 空闲对象栈 */
    std::stack<std::unique_ptr<T>> _pool;
    /** 池最大容量，0 = 无限制 */
    size_t _maxSize;
    /** 互斥锁 */
    mutable std::mutex _mutex;
};

}
