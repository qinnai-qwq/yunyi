/**
 * @file PortPool.cpp
 * @brief 端口池实现 —— 线性扫描分配 / 回收
 */
#include "yunyi/log/Logger.h"
#include "yunyi/pool/PortPool.h"
#include <iostream>

namespace yunyi {

PortPool::PortPool(uint16_t rangeStart, uint16_t rangeEnd)
    : _start(rangeStart)
    , _end(rangeEnd)
    , _total(rangeEnd >= rangeStart
          ? static_cast<size_t>(rangeEnd - rangeStart + 1) : 0)
{
    if (rangeEnd < rangeStart) {
        CC_LOG(std::string("[PortPool] WARNING: rangeEnd (") + std::to_string(rangeEnd) + ") < rangeStart (" + std::to_string(rangeStart) + "), pool will be empty!");
    }
}

uint16_t PortPool::acquire() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_used.size() >= _total) {
        return 0;   // 池已耗尽
    }
    for (uint16_t port = _start; port <= _end; ++port) {
        if (_used.find(port) == _used.end()) {
            _used.insert(port);
            return port;
        }
    }
    return 0;
}

void PortPool::release(uint16_t port) {
    std::lock_guard<std::mutex> lock(_mutex);
    _used.erase(port);
}

bool PortPool::isFree(uint16_t port) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _used.find(port) == _used.end();
}

size_t PortPool::usedCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _used.size();
}

} // namespace yunyi
