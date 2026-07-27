/**
 * @file Ref.cpp
 * @brief 引用计数基类实现
 */
#include "Ref.h"
#include "AutoreleasePool.h"

namespace yunyi {

AutoreleasePool* Ref::s_autoreleasePool = nullptr;

Ref::Ref()
    : _refCount(1)
{
}

void Ref::retain() {
    _refCount.fetch_add(1, std::memory_order_relaxed);
}

void Ref::release() {
    if (_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

void Ref::autorelease() {
    if (s_autoreleasePool) {
        s_autoreleasePool->add(this);
    } else {
        release();
    }
}

void Ref::setAutoreleasePool(AutoreleasePool* pool) {
    s_autoreleasePool = pool;
}

} // namespace yunyi
