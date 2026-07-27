/**
 * @file HttpApiRouter.cpp
 * @brief HTTP API 路由实现 —— cpp-httplib 路由注册 + JSON 响应
 *
 * 依赖:
 *   - third_party/httplib/httplib.h    (cpp-httplib 单头文件)
 *   - third_party/nlohmann_json/json.hpp (nlohmann/json 单头文件)
 *   - NetEngine/Director.h
 *
 * @see docs/rest-api.md 完整 API 文档
 */
#include "HttpApiRouter.h"
#include "../component/RoomRegistry.h"
#include "../../NetEngine/Director.h"
#include "third_party/httplib/httplib.h"
#include "third_party/nlohmann_json/json.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace yunyi {

using json = nlohmann::json;

// ============================================================
//  辅助
// ============================================================

namespace {

std::string isoNow() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &t);
    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

void setJsonHeaders(httplib::Response& res) {
    res.set_header("Content-Type", "application/json; charset=utf-8");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods",
                   "GET, POST, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

void sendError(httplib::Response& res, int status,
               const std::string& code, const std::string& message) {
    json body = {
        {"error", {
            {"code",    code},
            {"message", message}
        }}
    };
    setJsonHeaders(res);
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

/**
 * @brief 安全解析 URL 中的 room ID，失败返回 false
 */
bool parseRoomId(const std::string& s, uint32_t& out) {
    if (s.empty() || s.size() > 10) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    try {
        auto v = std::stoul(s);
        if (v > 0xFFFFFFFFULL) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

json roomInfoToJson(const RoomInfo& info) {
    return {
        {"roomId",         info.roomId},
        {"roomName",       info.roomName},
        {"connectionCode", info.connectionCode},
        {"assignedPort",   info.assignedPort},
        {"localMcPort",    info.localMcPort},
        {"status",         info.status},
        {"playerCount",    info.playerCount},
        {"createdAt",      info.createdAt.empty() ? isoNow() : info.createdAt},
        {"hostAddress",    info.hostAddress}
    };
}

json playerInfoToJson(const PlayerInfo& p) {
    return {
        {"id",           p.id},
        {"connectedAt",  p.connectedAt.empty() ? isoNow() : p.connectedAt},
        {"bytesSent",    p.bytesSent},
        {"bytesRecv",    p.bytesRecv}
    };
}

} // anonymous namespace

// ============================================================
//  构造
// ============================================================

void HttpApiRouter::setPublicIp(const std::string& ip) {
    _publicIp = ip;
    _director.setPublicIp(ip);  // 保持 Director 与路由器 IP 一致
}

HttpApiRouter::HttpApiRouter(Director& director,
                             RoomRegistry& registry,
                             const std::string& publicIp,
                             uint16_t httpPort,
                             uint16_t portStart,
                             uint16_t portEnd,
                             uint16_t ctrlPort,
                             std::shared_ptr<std::ostream> logWriter)
    : _director(director)
    , _roomRegistry(registry)
    , _publicIp(publicIp)
    , _httpPort(httpPort)
    , _portStart(portStart)
    , _portEnd(portEnd)
    , _ctrlPort(ctrlPort)
    , _logWriter(std::move(logWriter))
{}

// ============================================================
//  路由注册
// ============================================================

void HttpApiRouter::registerRoutes(httplib::Server& svr) {

    // ── 请求/响应日志（使用 App 层注入的共享日志流）──
    if (_logWriter) {
        auto t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        *_logWriter << "========== HTTP 日志 "
                    << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
                    << " ==========\n" << std::flush;

        svr.set_logger([this](const httplib::Request& req,
                               const httplib::Response& res) {
            if (!_logWriter || !_logWriter->good()) return;
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            std::tm t{};
            localtime_s(&t, &tt);
            std::ostringstream line;
            line << std::put_time(&t, "%H:%M:%S")
                 << " | " << req.method << " " << req.path
                 << " | " << res.status;
            if (!req.body.empty()) {
                line << " | body:" << req.body.substr(0, 200);
            }
            std::string lineStr = line.str();
            if (lineStr == _lastHttpLogLine) return;  // 去重：相同不写
            _lastHttpLogLine = lineStr;
            *_logWriter << lineStr << "\n" << std::flush;
        });
    }

    // ---- CORS 预检 ----
    svr.Options(R"(/api/v1/.*)", [](const auto&, auto& res) {
        setJsonHeaders(res);
        res.status = 204;
    });

    // ========================================================
    //  POST /api/v1/rooms  —  创建房间
    // ========================================================
    svr.Post("/api/v1/rooms", [this](const httplib::Request& req,
                                      httplib::Response& res) {
        setJsonHeaders(res);

        // 解析请求体
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            sendError(res, 400, "INVALID_PARAMS", "invalid JSON body");
            return;
        }

        // 校验字段类型
        if (!body.contains("roomName") || !body["roomName"].is_string()) {
            sendError(res, 400, "INVALID_PARAMS",
                      "roomName must be a string (1-64 chars)");
            return;
        }
        if (body.contains("localMcPort") && !body["localMcPort"].is_number()) {
            sendError(res, 400, "INVALID_PARAMS",
                      "localMcPort must be a number (1-65535)");
            return;
        }

        std::string roomName = body.value("roomName", "");
        int portVal = body.value("localMcPort", 25565);
        if (portVal < 1 || portVal > 65535) {
            sendError(res, 400, "INVALID_PARAMS",
                      "localMcPort must be 1-65535, got " + std::to_string(portVal));
            return;
        }
        uint16_t localMcPort = static_cast<uint16_t>(portVal);

        auto result = _director.createRoomRelay(
            roomName, localMcPort, _publicIp);

        if (!result.ok) {
            sendError(res, 409, result.errorCode, result.errorMessage);
            return;
        }

        // 登记到 RoomRegistry（用于超时清理）
        _roomRegistry.addRoom(result.roomId, roomName,
            result.assignedPort, localMcPort, result.connectionCode);

        json resp = {
            {"roomId",         result.roomId},
            {"roomName",       roomName.empty() ? "未命名房间" : roomName},
            {"connectionCode", result.connectionCode},
            {"assignedPort",   result.assignedPort},
            {"localMcPort",    localMcPort},
            {"status",         "waiting"},
            {"createdAt",      isoNow()}
        };

        res.status = 201;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  GET /api/v1/rooms  —  列出所有活跃房间
    // ========================================================
    svr.Get("/api/v1/rooms", [this](const httplib::Request&,
                                     httplib::Response& res) {
        setJsonHeaders(res);

        auto rooms = _director.listRooms();
        json arr = json::array();
        for (const auto& r : rooms) {
            arr.push_back(roomInfoToJson(r));
        }

        json resp = {
            {"rooms", arr},
            {"total", rooms.size()}
        };

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  GET /api/v1/rooms/{id}  —  单个房间详情
    // ========================================================
    svr.Get(R"(/api/v1/rooms/(\d+))", [this](const httplib::Request& req,
                                               httplib::Response& res) {
        setJsonHeaders(res);

        uint32_t roomId = 0;
        if (!parseRoomId(req.matches[1], roomId)) {
            sendError(res, 400, "INVALID_PARAMS", "invalid room ID");
            return;
        }

        auto info = _director.getRoomInfo(roomId);
        if (info.roomId == 0) {
            sendError(res, 404, "ROOM_NOT_FOUND",
                      "room " + std::to_string(roomId) + " not found");
            return;
        }

        auto players = _director.getRoomPlayers(roomId);
        json playerArr = json::array();
        for (const auto& p : players) {
            playerArr.push_back(playerInfoToJson(p));
        }

        json resp = roomInfoToJson(info);
        resp["players"] = playerArr;

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  GET /api/v1/rooms/{id}/players  —  房间玩家列表
    // ========================================================
    svr.Get(R"(/api/v1/rooms/(\d+)/players)",
            [this](const httplib::Request& req, httplib::Response& res) {
        setJsonHeaders(res);

        uint32_t roomId = 0;
        if (!parseRoomId(req.matches[1], roomId)) {
            sendError(res, 400, "INVALID_PARAMS", "invalid room ID");
            return;
        }

        auto info = _director.getRoomInfo(roomId);
        if (info.roomId == 0) {
            sendError(res, 404, "ROOM_NOT_FOUND",
                      "room " + std::to_string(roomId) + " not found");
            return;
        }

        auto players = _director.getRoomPlayers(roomId);
        json arr = json::array();
        for (const auto& p : players) {
            arr.push_back(playerInfoToJson(p));
        }

        json resp = {
            {"roomId",  roomId},
            {"players", arr}
        };

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  DELETE /api/v1/rooms/{id}  —  关闭房间
    // ========================================================
    svr.Delete(R"(/api/v1/rooms/(\d+))", [this](const httplib::Request& req,
                                                  httplib::Response& res) {
        setJsonHeaders(res);

        uint32_t roomId = 0;
        if (!parseRoomId(req.matches[1], roomId)) {
            sendError(res, 400, "INVALID_PARAMS", "invalid room ID");
            return;
        }

        bool ok = _director.forceCloseRoom(roomId);
        if (!ok) {
            sendError(res, 404, "ROOM_NOT_FOUND",
                      "room " + std::to_string(roomId)
                      + " not found or already closed");
            return;
        }

        // 同步清理 RoomRegistry
        _roomRegistry.removeRoom(roomId);

        json resp = {
            {"roomId",   roomId},
            {"status",   "closed"},
            {"closedAt", isoNow()}
        };

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  GET /api/v1/stats  —  服务器统计
    // ========================================================
    svr.Get("/api/v1/stats", [this](const httplib::Request&,
                                     httplib::Response& res) {
        setJsonHeaders(res);

        auto stats = _director.getStats();

        json resp = {
            {"uptimeSeconds",    stats.uptimeSeconds},
            {"activeRooms",      stats.activeRooms},
            {"totalRooms",       stats.totalRooms},
            {"activeTunnels",    stats.activeTunnels},
            {"totalConnections", stats.totalConnections},
            {"portPoolUsed",     stats.portPoolUsed},
            {"portPoolTotal",    stats.portPoolTotal},
            {"bytesRelayed",     stats.bytesRelayed},
            {"version",          "1.0.1"},
            {"protocolVersion",  1}
        };

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  GET /api/v1/logs  —  后端日志（WebUI 轮询）
    // ========================================================
    svr.Get("/api/v1/logs", [this](const httplib::Request&,
                                    httplib::Response& res) {
        setJsonHeaders(res);
        auto logs = _director.getRecentLogs(0);  // 0 = 返回全部
        json arr = json::array();
        for (auto& [t, msg] : logs) {
            arr.push_back({{"time", t}, {"msg", msg}});
        }
        json resp = {{"logs", arr}};
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    //  GET /api/v1/config  —  服务器配置（只读）
    // ========================================================
    svr.Get("/api/v1/config", [this](const httplib::Request&,
                                      httplib::Response& res) {
        setJsonHeaders(res);

        // 从 Director 读取真实公网 IP（支持 WebUI 动态更新）
        std::string realIp = _director.getPublicIp();
        if (realIp.empty()) realIp = _publicIp;

        json resp = {
            {"role",                "relay"},
            {"publicIp",            realIp},
            {"portPoolStart",       _portStart},
            {"portPoolEnd",         _portEnd},
            {"controlPort",         _ctrlPort},
            {"httpPort",            _httpPort},
            {"heartbeatIntervalMs", 25000},
            {"roomGracePeriodMs",   45000},
            {"maxPlayersPerRoom",   10}
        };

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  POST /api/v1/config  —  更新公网 IP（WebUI 调用）
    // ========================================================
    svr.Post("/api/v1/config", [this](const httplib::Request& req,
                                       httplib::Response& res) {
        setJsonHeaders(res);

        json body;
        try { body = json::parse(req.body); } catch (...) {
            sendError(res, 400, "INVALID_PARAMS", "invalid JSON body");
            return;
        }

        if (body.contains("publicIp") && body["publicIp"].is_string()) {
            std::string newIp = body["publicIp"];
            setPublicIp(newIp);  // 同步更新 _publicIp 和 Director
            std::cout << "[HttpApi] Public IP changed via WebUI: " << newIp << std::endl;

            json resp = {{"publicIp", newIp}, {"status", "ok"}};
            res.status = 200;
            res.set_content(resp.dump(), "application/json; charset=utf-8");
        } else {
            sendError(res, 400, "INVALID_PARAMS", "publicIp must be a string");
        }
    });

    // ========================================================
    //  GET /api/v1/ping  —  服务器握手（验证可用性）
    // ========================================================
    svr.Get("/api/v1/ping", [this](const httplib::Request&,
                                    httplib::Response& res) {
        json resp = {
            {"status",    "ok"},
            {"role",      "relay"},
            {"version",   "1.0.1"},
            {"protocol",  1},
            {"serverIp",  _publicIp},
            {"serverTime", isoNow()}
        };
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // ========================================================
    //  GET /  —  WebUI 静态页面
    // ========================================================
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        // 按优先级查找 yunyi.html:
        //   1. webview/yunyi.html — 发布目录（exe 同级）
        //   2. app/relay/webview/yunyi.html — 开发目录（项目根）
        const char* searchPaths[] = {
            "webview/yunyi.html",
            "app/relay/webview/yunyi.html",
        };
        std::ifstream file;
        for (auto* p : searchPaths) {
            file.open(p);
            if (file.is_open()) break;
        }
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            res.set_content(content, "text/html; charset=utf-8");
        } else {
            res.status = 404;
            res.set_content("<h1>yunyi.html not found</h1>",
                            "text/html; charset=utf-8");
        }
    });

    // ========================================================
    //  GET /Icon.png  —  图标资源
    // ========================================================
    svr.Get("/Icon.png", [](const httplib::Request&, httplib::Response& res) {
        const char* searchPaths[] = {
            "webview/Icon.png",
            "app/relay/webview/Icon.png",
        };
        std::ifstream file;
        for (auto* p : searchPaths) { file.open(p, std::ios::binary); if (file.is_open()) break; }
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            res.set_content(content, "image/png");
        } else {
            res.status = 404;
        }
    });

    // ========================================================
    //  404 catch-all — 仅在真正路由未匹配时返回错误
    // ========================================================
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404 && res.body.empty()) {
            setJsonHeaders(res);
            json body = {
                {"error", {
                    {"code",    "NOT_FOUND"},
                    {"message", "endpoint not found"}
                }}
            };
            res.set_content(body.dump(), "application/json; charset=utf-8");
        }
    });
}

} // namespace yunyi
