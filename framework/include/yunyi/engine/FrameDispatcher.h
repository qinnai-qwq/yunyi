/**
 * @file FrameDispatcher.h
 * @brief 控制帧分发器 —— 按帧 type 注册处理器并分发
 *
 * 未注册的帧类型自动回复 ERROR(UNKNOWN_FRAME_TYPE)。
 * 若 handler 返回 false 表示处理失败，同样触发错误回复。
 */
#pragma once
#include "yunyi/protocol/FrameCodec.h"
#include <functional>
#include <mutex>
#include <unordered_map>
namespace yunyi {
namespace protocol {

/**
 * @brief 帧处理器回调
 * @param type 帧类型
 * @param data 负载数据指针
 * @param len 负载长度
 * @return true 处理成功，false 处理失败（将自动发送 ERROR 帧）
 */
using FrameHandler = std::function<bool(FrameType, const uint8_t*, uint32_t)>;

/**
 * @class FrameDispatcher
 * @brief 控制帧分发器
 *
 * 维护 FrameType -> FrameHandler 的映射表。
 * dispatch() 时查找对应 handler 并调用；未找到则通过 errorSender 发送错误。
 *
 * 线程安全：所有方法内部加锁。
 */
class FrameDispatcher {
public:
    FrameDispatcher() = default;

    /**
     * @brief 注册帧处理器
     * @param t 帧类型
     * @param h 处理器回调
     *
     * 若已存在同类型处理器，会被覆盖。
     */
    void registerHandler(FrameType t, FrameHandler h);

    /**
     * @brief 分发帧到对应处理器
     * @param t 帧类型
     * @param d 负载数据指针
     * @param len 负载长度
     * @return true 成功分发并处理，false 未找到处理器或处理器返回 false
     *
     * 若未找到处理器，自动调用 errorSender（若已设置）发送 UNKNOWN_FRAME_TYPE 错误。
     */
    bool dispatch(FrameType t, const uint8_t* d, uint32_t len);

    /**
     * @brief 取消注册帧处理器
     * @param t 帧类型
     */
    void unregisterHandler(FrameType t);

    /**
     * @brief 清除所有已注册的处理器
     */
    void clear();

    /**
     * @brief 设置错误帧发送回调
     * @param s 回调函数 (ErrorCode, message) -> void
     *
     * 当 dispatch() 找不到处理器或处理器返回 false 时，通过此回调发送 ERROR 帧。
     * 若不设置，错误将被静默忽略。
     */
    void setErrorSender(std::function<void(ErrorCode, std::string)> s);

    FrameDispatcher(const FrameDispatcher&) = delete;
    FrameDispatcher& operator=(const FrameDispatcher&) = delete;

private:
    /** 处理器映射互斥锁 */
    std::mutex _mutex;
    /** FrameType -> FrameHandler 映射表 */
    std::unordered_map<uint8_t, FrameHandler> _handlers;
    /** 错误帧发送回调 */
    std::function<void(ErrorCode, std::string)> _errorSender;
};

}
}
