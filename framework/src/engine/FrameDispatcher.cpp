/**
 * @file FrameDispatcher.cpp
 * @brief 控制帧分发器实现
 */
#include "yunyi/engine/FrameDispatcher.h"

namespace yunyi {
namespace protocol {

void FrameDispatcher::registerHandler(FrameType type, FrameHandler handler) {
    std::lock_guard<std::mutex> lock(_mutex);
    _handlers[static_cast<uint8_t>(type)] = std::move(handler);
}

bool FrameDispatcher::dispatch(FrameType type, const uint8_t* data, uint32_t len) {
    FrameHandler handler;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _handlers.find(static_cast<uint8_t>(type));
        if (it == _handlers.end()) {
            // 未找到处理器，发送 UNKNOWN_FRAME_TYPE 错误
            if (_errorSender) {
                _errorSender(ErrorCode::UNKNOWN_FRAME_TYPE,
                    "no handler for frame type");
            }
            return false;
        }
        handler = it->second;
    }

    // 调用处理器（不加锁，避免死锁）
    if (!handler(type, data, len)) {
        if (_errorSender) {
            _errorSender(ErrorCode::INVALID_PAYLOAD,
                "handler rejected frame");
        }
        return false;
    }
    return true;
}

void FrameDispatcher::unregisterHandler(FrameType type) {
    std::lock_guard<std::mutex> lock(_mutex);
    _handlers.erase(static_cast<uint8_t>(type));
}

void FrameDispatcher::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _handlers.clear();
}

void FrameDispatcher::setErrorSender(
    std::function<void(ErrorCode, std::string)> sender) {
    std::lock_guard<std::mutex> lock(_mutex);
    _errorSender = std::move(sender);
}

} // namespace protocol
} // namespace yunyi
