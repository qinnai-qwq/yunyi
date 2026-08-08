/**
 * @file AutoreleasePool.cpp
 * @brief 自动释放池实现
 */
#include "yunyi/lifecycle/AutoreleasePool.h"
#include "yunyi/lifecycle/Ref.h"

namespace yunyi {

void AutoreleasePool::add(Ref* obj) {
    if (!obj) return;
    _mutex.lock();
    _pending.push_back(obj);
    _mutex.unlock();
}

void AutoreleasePool::drain() {
    std::vector<Ref*> batch;
    _mutex.lock();
    batch.swap(_pending);
    _mutex.unlock();
    for (auto* obj : batch) {
        obj->release();
    }
}

size_t AutoreleasePool::size() const {
    _mutex.lock();
    size_t s = _pending.size();
    _mutex.unlock();
    return s;
}

} // namespace yunyi
