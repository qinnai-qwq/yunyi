/**
 * @file HttpApiRouter.h
 * @brief HTTP API 路由 —— 对接 WebUI 前端，所有端点 /api/v1/ 前缀
 *
 * 提供 RESTful API 用于查询房间列表、玩家信息、引擎统计等。
 * 后端 cpp-httplib 接管 HTTP，本类只负责注册路由 + JSON 序列化。
 *
 * @see docs/rest-api.md 完整前后端通信契约
 */
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

// forward declare httplib types (避免头文件污染)
namespace httplib {
    class Server;
    struct Request;
    struct Response;
}

namespace yunyi {

class Director;
class RoomRegistry;

/**
 * @class HttpApiRouter
 * @brief HTTP API 路由注册器
 *
 * 端点列表 (详见 docs/rest-api.md):
 *   POST   /api/v1/rooms               - 创建房间
 *   GET    /api/v1/rooms                - 房间列表
 *   GET    /api/v1/rooms/{id}           - 单个房间信息
 *   GET    /api/v1/rooms/{id}/players   - 房间玩家列表
 *   DELETE /api/v1/rooms/{id}           - 关闭房间
 *   GET    /api/v1/stats                - 引擎运行统计
 *   GET    /api/v1/config               - 服务器配置
 */
class HttpApiRouter {
public:
    /**
     * @brief 构造函数
     * @param director Director 引擎单例引用
     * @param registry 房间注册表引用（用于超时清理）
     * @param publicIp 公网 IP 字符串（用于生成连接码）
     * @param httpPort HTTP 服务端口
     * @param portStart 端口池起始
     * @param portEnd 端口池结束
     * @param ctrlPort 控制连接端口
     */
    HttpApiRouter(Director& director,
                  RoomRegistry& registry,
                  const std::string& publicIp,
                  uint16_t httpPort,
                  uint16_t portStart = 40001,
                  uint16_t portEnd = 41000,
                  uint16_t ctrlPort = 40000,
                  std::shared_ptr<std::ostream> logWriter = nullptr);

    /**
     * @brief 注册所有 API 路由到 HTTP 服务器
     * @param svr httplib::Server 实例引用
     */
    void registerRoutes(httplib::Server& svr);

    /** 公网 IP getter */
    const std::string& publicIp() const { return _publicIp; }
    /** 更新公网 IP（同步 Director，保证连接码一致性） */
    void setPublicIp(const std::string& ip);

private:
    Director&      _director;
    RoomRegistry&  _roomRegistry;
    std::string    _publicIp;
    uint16_t       _httpPort;
    uint16_t       _portStart;
    uint16_t       _portEnd;
    uint16_t       _ctrlPort;
    std::shared_ptr<std::ostream> _logWriter;
    std::string _lastHttpLogLine;  // HTTP 日志去重：与上一行相同则跳过
};

} // namespace yunyi
