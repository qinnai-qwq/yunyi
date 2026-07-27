/**
 * @file RelayApp.h
 * @brief 中继服务器主程序 —— 唯一挂载 HTTP 服务和 WebUI 的角色
 *
 * 职责:
 *   - 启动 Director 中继服务
 *   - 注册控制帧处理器
 *   - 运行主循环（定时器 tick + 过期房间清理）
 *   - 挂载 HTTP API 路由（对接 WebUI 前端）
 *   - 在独立线程中运行 cpp-httplib HTTP 服务器
 */
#pragma once
#include "../common/Config.h"
#include "../component/RoomRegistry.h"
#include <memory>
#include <string>
#include <thread>
#include <atomic>

// forward
namespace httplib { class Server; }

namespace yunyi {

class Director;
class HttpApiRouter;

/**
 * @class RelayApp
 * @brief 中继服务器应用程序
 *
 * 生命周期:
 *   1. 构造 RelayApp
 *   2. init(cfg) 初始化引擎 + HTTP 服务
 *   3. run() 进入主循环（在当前线程）
 *   4. stop() 退出主循环 + 停止 HTTP 线程
 */
class RelayApp {
public:
    RelayApp();
    ~RelayApp();

    /**
     * @brief 初始化中继服务
     * @param cfg 应用配置
     * @return true 成功，false 失败
     */
    bool init(const AppConfig& cfg);

    /**
     * @brief 运行主循环（阻塞当前线程）
     *
     * 循环执行: 引擎定时器 tick、过期房间检查。
     * HTTP 服务器在独立线程中运行，不阻塞主循环。
     */
    void run();

    /**
     * @brief 停止中继服务（主循环 + HTTP 线程）
     */
    void stop();

private:
    /** 注册控制帧处理器（REGISTER, HEARTBEAT, DEREGISTER） */
    void setupFrameHandlers();

    /** 每帧定时器回调 */
    void onTick(uint64_t deltaMs);

    /** 检查并清理心跳超时的房间 */
    void checkStaleRooms();

    /** 启动 HTTP 服务器（在独立线程） */
    void startHttpServer();

    /** 停止 HTTP 服务器 */
    void stopHttpServer();

    /** Director 引擎实例 */
    Director* _director = nullptr;
    /** HTTP API 路由器 */
    std::unique_ptr<HttpApiRouter> _httpRouter;
    /** 房间注册表 */
    std::unique_ptr<RoomRegistry> _roomRegistry;
    /** 应用配置副本 */
    AppConfig _config;
    /** 运行标志 */
    std::atomic<bool> _running{false};

    /** HTTP 服务器实例 */
    std::unique_ptr<httplib::Server> _httpServer;
    /** HTTP 服务器线程 */
    std::thread _httpThread;
};

} // namespace yunyi
