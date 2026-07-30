/**
 * @file RelayApp.cpp
 * @brief 中继服务器主程序实现 —— 引擎 + HTTP 服务
 *
 * 依赖:
 *   - third_party/httplib/httplib.h (cpp-httplib 单头文件)
 */
#include "RelayApp.h"
#include "HttpApiRouter.h"
#include "../../NetEngine/Director.h"
#include "../../NetEngine/FrameCodec.h"
#include "../../NetEngine/FrameDispatcher.h"
#include "../../NetEngine/Scheduler.h"
#include "../../NetEngine/TunnelManager.h"
#include "../component/ConnectionCode.h"
#include "third_party/httplib/httplib.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace yunyi {

RelayApp::RelayApp()
    : _director(&Director::instance())
    , _roomRegistry(std::make_unique<RoomRegistry>())
{}

RelayApp::~RelayApp() {
    stop();
}

bool RelayApp::init(const AppConfig& cfg) {
    _config = cfg;

    // ── 创建统一日志文件（logs/yunyi_YYYY-MM-DD_HH-MM-SS.log）──
    std::shared_ptr<std::ofstream> logFile;
    {
        std::filesystem::path logDir = std::filesystem::current_path() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);

        auto t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::ostringstream fname;
        fname << "yunyi_"
              << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".log";
        auto logPath = (logDir / fname.str()).string();

        logFile = std::make_shared<std::ofstream>(
            logPath, std::ios::out | std::ios::app);
        if (logFile->is_open()) {
            *logFile << "========== 云驿 中继服务器 v1.1.0 ==========\n"
                     << "启动时间: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
                     << "公网IP: " << cfg.publicIp << "  控制端口: " << cfg.controlPort
                     << "  HTTP端口: " << cfg.httpPort << "\n"
                     << "端口池: " << cfg.portPoolStart << "-" << cfg.portPoolEnd << "\n"
                     << "==========================================\n" << std::flush;
        }
    }

    // 注入日志流到 Director（所有 addLog / cout 统一落盘）
    _director->setLogWriter(logFile);

    // ── 初始化引擎 ──
    EngineConfig engineCfg;
    engineCfg.logLevel = cfg.logLevel;
    engineCfg.logPath  = cfg.logPath;

    if (!_director->init(engineCfg)) {
        std::cerr << "[RelayApp] Engine init failed: "
                  << _director->getLastError() << std::endl;
        return false;
    }

    PortPoolConfig portPool;
    portPool.controlPort       = cfg.controlPort;
    portPool.rangeStart        = cfg.portPoolStart;
    portPool.rangeEnd          = cfg.portPoolEnd;
    portPool.maxPlayersPerRoom = cfg.maxPlayersPerRoom;

    TlsConfig tls;
    tls.psk         = cfg.psk;
    tls.pskIdentity = cfg.pskIdentity;

    if (!_director->startRelayService(portPool, tls)) {
        std::cerr << "[RelayApp] startRelayService failed: "
                  << _director->getLastError() << std::endl;
        return false;
    }

    setupFrameHandlers();

    // 初始 IP 存入 Director
    _director->setPublicIp(cfg.publicIp);
    _director->addLog("中继服务器已启动, 公网IP: " + cfg.publicIp);
    _director->addLog("控制端口: " + std::to_string(cfg.controlPort)
        + " HTTP端口: " + std::to_string(cfg.httpPort));

    // ── 创建 HTTP API 路由器（传入共享日志流）──
    _httpRouter = std::make_unique<HttpApiRouter>(
        *_director, *_roomRegistry, cfg.publicIp, cfg.httpPort,
        cfg.portPoolStart, cfg.portPoolEnd, cfg.controlPort, logFile);

    // ── 打印启动信息 ──
    std::cout << "[RelayApp] ======================================" << std::endl;
    std::cout << "[RelayApp] 云驿中继服务器已启动" << std::endl;
    std::cout << "[RelayApp] 公网 IP:    " << cfg.publicIp << std::endl;
    std::cout << "[RelayApp] 控制端口:  " << cfg.controlPort << std::endl;
    std::cout << "[RelayApp] WebUI:     http://127.0.0.1:"
              << cfg.httpPort << std::endl;
    std::cout << "[RelayApp] REST API:  http://127.0.0.1:"
              << cfg.httpPort << "/api/v1/" << std::endl;
    std::cout << "[RelayApp] ======================================" << std::endl;

    // ── 启动 HTTP 服务器 ──
    startHttpServer();

    return true;
}

void RelayApp::run() {
    _running.store(true);
    auto lastTick = std::chrono::steady_clock::now();

    std::cout << "[RelayApp] Main loop started (10ms tick)" << std::endl;

    // 主循环：10ms 间隔
    while (_running.load()) {
        auto now = std::chrono::steady_clock::now();
        auto deltaMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - lastTick).count();
        lastTick = now;

        onTick(static_cast<uint64_t>(deltaMs));

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[RelayApp] Main loop exited" << std::endl;
}

void RelayApp::stop() {
    if (!_running.load()) return;

    std::cout << "[RelayApp] Shutting down..." << std::endl;
    _running.store(false);

    stopHttpServer();

    _director->shutdown();

    std::cout << "[RelayApp] Shutdown complete" << std::endl;
}

// ============================================================
//  HTTP 服务器
// ============================================================

void RelayApp::startHttpServer() {
    _httpServer = std::make_unique<httplib::Server>();

    // 注册所有 API 路由
    _httpRouter->registerRoutes(*_httpServer);

    // 在独立线程启动 HTTP 服务器（双栈 :: 同时接受 IPv4/IPv6）
    uint16_t port = _config.httpPort;
    _httpThread = std::thread([this, port]() {
        // 先尝试 IPv6 双栈，失败则回退 IPv4
        if (!_httpServer->listen("::", port)) {
            std::cerr << "[HTTP] IPv6 dual-stack failed, trying IPv4..." << std::endl;
            if (!_httpServer->listen("0.0.0.0", port)) {
                std::cerr << "[HTTP] Failed to start server on port "
                          << port << std::endl;
            }
        }
        std::cout << "[HTTP] Server stopped" << std::endl;
    });
}

void RelayApp::stopHttpServer() {
    if (_httpServer) {
        _httpServer->stop();
    }
    if (_httpThread.joinable()) {
        _httpThread.join();
    }
}

// ============================================================
//  控制帧处理器（预留）
// ============================================================

void RelayApp::setupFrameHandlers() {
    auto* dispatcher = _director->frameDispatcher();
    if (!dispatcher) return;

    // 错误发送器：向当前控制会话回复 ERROR 帧
    dispatcher->setErrorSender([this](protocol::ErrorCode code, std::string msg) {
        auto errFrame = protocol::FrameCodec::encodeError(code, msg);
        _director->sendReplyFrame(errFrame);
    });

    // ── REGISTER 处理器 ──
    dispatcher->registerHandler(protocol::FrameType::REGISTER,
        [this](protocol::FrameType, const uint8_t* data, uint32_t len) -> bool {
            auto payload = protocol::FrameCodec::decodeRegisterPayload(data, len);
            if (payload.roomName.empty()) return false;

            RoomResult result;
            if (payload.roomId != 0) {
                // 认领已有房间
                result.roomId = payload.roomId;
                result.ok = _director->getRoomInfo(payload.roomId).roomId != 0;
                result.assignedPort = _director->getRoomInfo(payload.roomId).assignedPort;
            } else {
                // 先检查同 host 是否已有房间（重连场景：复用已有、不创建新的）
                uint32_t existingRoomId = _director->findRoomByHostAddr(
                    _director->getCurrentReplyHostAddr());
                if (existingRoomId != 0) {
                    result.roomId = existingRoomId;
                    result.ok = true;
                    result.assignedPort = _director->getRoomInfo(existingRoomId).assignedPort;
                } else {
                    // 创建新房间
                    result = _director->createRoomRelay(
                        payload.roomName, payload.localMcPort,
                        _director->getPublicIp().empty() ? _config.publicIp : _director->getPublicIp());
                }
            }

            if (!result.ok) {
                auto errFrame = protocol::FrameCodec::encodeError(
                    protocol::ErrorCode::PORT_POOL_EXHAUSTED, result.errorMessage);
                _director->sendReplyFrame(errFrame);
                return false;
            }

            // 发送 REGISTER_ACK
            protocol::RegisterAckPayload ack;
            ack.assignedPort = result.assignedPort;
            ack.roomId = result.roomId;
            auto ackFrame = protocol::FrameCodec::encodeRegisterAck(ack);
            if (!_director->sendReplyFrame(ackFrame)) return false;

            // 将当前控制会话链接到房间（同时更新 session 绑定）
            _director->linkCurrentSessionToRoom(result.roomId);

            // 登记到 RoomRegistry（用于超时清理，已存在则更新心跳）
            _roomRegistry->addRoom(result.roomId, payload.roomName,
                result.assignedPort, payload.localMcPort, result.connectionCode);

            std::cout << "[RelayApp] Room registered: #" << result.roomId
                      << " \"" << payload.roomName << "\" on port "
                      << result.assignedPort << std::endl;
            _director->addLog("房间已注册: #" + std::to_string(result.roomId)
                + " \"" + payload.roomName + "\" 端口:" + std::to_string(result.assignedPort));
            return true;
        });

    // ── HEARTBEAT 处理器 ──
    dispatcher->registerHandler(protocol::FrameType::HEARTBEAT,
        [this](protocol::FrameType, const uint8_t*, uint32_t) -> bool {
            // 更新心跳（同时更新 Director 房间和 RoomRegistry）
            uint32_t roomId = _director->touchRoomHeartbeat();
            if (roomId != 0) {
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                _roomRegistry->updateHeartbeat(roomId, now);
            } else {
                // 心跳没匹配到房间，尝试从 RoomRegistry 反向查找
                std::cerr << "[RelayApp] Heartbeat from unknown room, trying registry..." << std::endl;
            }
            auto ack = protocol::FrameCodec::encodeHeartbeatAck();
            return _director->sendReplyFrame(ack);
        });

    // ── DEREGISTER 处理器 ──
    dispatcher->registerHandler(protocol::FrameType::DEREGISTER,
        [this](protocol::FrameType, const uint8_t*, uint32_t) -> bool {
            std::cout << "[RelayApp] Host requested deregister" << std::endl;
            // touchRoomHeartbeat() 是当前唯一可用的 session→roomId 查找方法
            // （currentReplySession 为 DirectorImpl 私有成员，无公开 getter）
            // 心跳副作用可忽略：房间即将被销毁
            uint32_t roomId = _director->touchRoomHeartbeat();
            if (roomId != 0) {
                _director->forceCloseRoom(roomId);
                _roomRegistry->removeRoom(roomId);
                _director->addLog("房间已注销: #" + std::to_string(roomId));
            }
            return true;
        });

    // ── STREAM_BIND 处理器 ──
    // 注：实际隧道配对在 Director::onTunnelAccepted 中完成（TLS 握手 + playerConnId 识别后）。
    // 此 handler 仅用于确认配对状态和诊断日志，不做实际绑定操作。
    dispatcher->registerHandler(protocol::FrameType::STREAM_BIND,
        [this](protocol::FrameType, const uint8_t* data, uint32_t len) -> bool {
            auto payload = protocol::FrameCodec::decodeStreamBindPayload(data, len);
            std::cout << "[RelayApp] Stream bind ACK: playerConnId="
                      << payload.playerConnId << std::endl;
            auto* tm = _director->tunnelMgr();
            if (!tm) return false;
            auto* tunnel = tm->findTunnel(payload.playerConnId);
            if (tunnel && tunnel->paired) {
                std::cout << "[RelayApp] Tunnel " << payload.playerConnId
                          << " confirmed paired" << std::endl;
            } else if (tunnel && !tunnel->paired) {
                std::cout << "[RelayApp] Tunnel " << payload.playerConnId
                          << " pending (waiting for host data connection)"
                          << std::endl;
            }
            return true;
        });
}

void RelayApp::onTick(uint64_t deltaMs) {
    try {
        if (auto* sched = _director->scheduler()) {
            sched->tick(deltaMs);
        }
        _director->flushPendingSends();
        checkStaleRooms();
    } catch (const std::exception& e) {
        std::cerr << "[RelayApp] CRASH in onTick: " << e.what() << std::endl;
        _director->addLog("主循环崩溃: " + std::string(e.what()));
    } catch (...) {
        std::cerr << "[RelayApp] CRASH in onTick (unknown)" << std::endl;
        _director->addLog("主循环崩溃: unknown exception");
    }
}

void RelayApp::checkStaleRooms() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto staleIds = _roomRegistry->findTimeoutRooms(
        now, _config.roomGracePeriodMs);

    for (auto id : staleIds) {
        std::cout << "[RelayApp] Room " << id
                  << " timed out, cleaning up" << std::endl;
        _director->addLog("房间超时清理: #" + std::to_string(id));
        _director->forceCloseRoom(id);
        _roomRegistry->removeRoom(id);
    }
}

} // namespace yunyi
