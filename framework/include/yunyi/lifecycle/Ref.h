/**
 * @file Ref.h
 * @brief 引用计数基类 —— 管理堆上对象的生命周期
 *
 * 用法: 继承 Ref（构造时自动 _refCount = 1），传递时 retain()，
 * 不再需要时 release()。归零自动 delete。配合 AutoreleasePool 可延迟释放。
 *
 * @note release() 可能触发 delete this，调用后不得再访问该对象。
 */
#pragma once
#include <atomic>
#include <cstdint>
namespace yunyi {

class AutoreleasePool;

/**
 * @class Ref
 * @brief 引用计数基类
 *
 * 采用侵入式引用计数：对象构造时计数为 1，外部通过 retain()/release()
 * 管理生命周期。当引用计数归零时，release() 自动 delete this。
 *
 * 支持 autorelease 模式：调用 autorelease() 将对象加入 AutoreleasePool，
 * 在下一帧 drain() 时批量释放，适用于 "创建-传递-忘记" 场景。
 */
class Ref {
public:
    /**
     * @brief 构造 Ref 对象
     * @post _refCount = 1
     */
    Ref();

    /**
     * @brief 虚析构（由 release() 在计数归零时调用）
     */
    virtual ~Ref() = default;

    /**
     * @brief 增加引用计数
     * @post _refCount += 1
     */
    void retain();

    /**
     * @brief 减少引用计数，归零时 delete this
     * @post 若 _refCount == 0，对象被销毁
     * @note 调用后不得再访问该对象
     */
    void release();

    /**
     * @brief 将对象加入 AutoreleasePool 延迟释放
     *
     * 若全局 AutoreleasePool 已设置且当前引用计数为 1，
     * 则加入池中等待批量 drain()；否则直接 release()。
     */
    void autorelease();

    /**
     * @brief 查询当前引用计数
     * @return 当前引用计数（relaxed 内存序）
     */
    uint32_t refCount() const {
        return _refCount.load(std::memory_order_relaxed);
    }

    /**
     * @brief 设置全局 AutoreleasePool
     * @param pool AutoreleasePool 指针，nullptr 表示禁用
     *
     * 静态方法，影响所有 Ref 子类的 autorelease() 行为。
     */
    static void setAutoreleasePool(AutoreleasePool* pool);

    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;

protected:
    /** 引用计数，初始值为 1 */
    std::atomic<uint32_t> _refCount{1};

private:
    /** 全局 AutoreleasePool 单例指针 */
    static AutoreleasePool* s_autoreleasePool;
};

}
