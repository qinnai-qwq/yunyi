/**
 * @file HostAgentApp.cpp
 * @brief 房主端主程序实现
 */
#include "HostAgentApp.h"
#include "../../NetEngine/Director.h"
#include "../../NetEngine/FrameCodec.h"
#include "../../NetEngine/NetUtil.h"
#include "../../NetEngine/Scheduler.h"
#include "../../NetEngine/Session.h"
#include "../../NetEngine/TlsPskContext.h"
#include "../../NetEngine/TransportCore.h"
#include "third_party/httplib/httplib.h"
#include "third_party/nlohmann_json/json.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

// Windows <wingdi.h> 通过某些包含链定义 ERROR=0，与 FrameType::ERROR 冲突
#ifdef ERROR
#undef ERROR
#endif

namespace yunyi {

HostAgentApp::HostAgentApp()
    : _director(&Director::instance())
    , _controlChannel(std::make_unique<ControlChannel>())
{}

HostAgentApp::~HostAgentApp() {
    stop();
}

bool HostAgentApp::init(const AppConfig& cfg) {
    _config      = cfg;
    _localMcPort = cfg.localMcPort;  // 从命令行参数读取，默认 25565

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
            *logFile << "========== 云驿 房主端 v1.0.1 ==========\n"
                     << "启动时间: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
                     << "中继地址: " << cfg.publicIp << ":" << cfg.controlPort << "\n"
                     << "本地MC端口: " << _localMcPort
                     << "  HTTP端口: " << cfg.httpPort << "\n"
                     << "==========================================\n" << std::flush;
        }
    }

    // 注入日志流到 Director
    _director->setLogWriter(logFile);

    // 初始化引擎
    EngineConfig engineCfg;
    engineCfg.logLevel = cfg.logLevel;
    engineCfg.logPath  = cfg.logPath;

    if (!_director->init(engineCfg)) {
        std::cerr << "[HostAgent] Engine init failed: "
                  << _director->getLastError() << std::endl;
        return false;
    }

    _director->setAsHostAgent();

    setupFrameHandlers();

    // 设置日志回调
    _controlChannel->setLogCallback([this](const std::string& msg) {
        _director->addLog(msg);
    });

    // 设置 Scheduler（心跳和重连依赖）
    _controlChannel->setScheduler(_director->scheduler());

    // 保存连接参数（用于后续 connectToRelay()）
    _autoConnect = cfg.autoConnect;

    // 通过 ControlChannel 连接到中继（若 autoConnect = false 则跳过）
    if (_autoConnect) {
        if (!_controlChannel->connect(cfg.publicIp, cfg.controlPort,
                cfg.psk, cfg.pskIdentity, _director->transport())) {
            std::cerr << "[HostAgent] ControlChannel connect failed"
                      << std::endl;
            return false;
        }

        std::cout << "[HostAgent] Connected to relay, ready to create room"
                  << std::endl;
        _director->addLog("已连接中继 " + cfg.publicIp + ":" + std::to_string(cfg.controlPort));
    } else {
        std::cout << "[HostAgent] Idle mode — waiting for WebUI connect command"
                  << std::endl;
        _director->addLog("房主端已启动（空闲模式），等待连接指令");
    }

    // ── 启动 HTTP 服务器（WebUI）──
    _httpPort = cfg.httpPort;
    startHttpServer();

    // 状态变更日志（不再自动注册房间，由 WebUI 手动触发）
    _controlChannel->setOnStateChange(
        [this](ControlState oldState, ControlState newState) {
            std::cout << "[HostAgent] State: "
                      << static_cast<int>(oldState) << " -> "
                      << static_cast<int>(newState) << std::endl;
        });

    return true;
}

void HostAgentApp::run() {
    _running = true;
    auto lastTick = std::chrono::steady_clock::now();

    // 主循环：50ms 间隔，用实际 delta 驱动 Scheduler（与 RelayApp 一致）
    while (_running) {
        auto now = std::chrono::steady_clock::now();
        auto deltaMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - lastTick).count();
        lastTick = now;

        if (auto* sched = _director->scheduler()) {
            sched->tick(static_cast<uint64_t>(deltaMs));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void HostAgentApp::stop() {
    _running = false;
    stopHttpServer();
    // 关闭所有数据隧道
    for (auto& kv : _tunnels) {
        if (kv.second) {
            kv.second->close();
            kv.second->release();
        }
    }
    _tunnels.clear();
    if (_controlChannel) {
        // 仅在 Active 状态发送 DEREGISTER（Connecting/Registering/Closed 状态下 TLS 会话无效）
        if (_controlChannel->state() == ControlState::Active) {
            _controlChannel->deregister();
        }
        _controlChannel->disconnect();
    }
}

void HostAgentApp::setupFrameHandlers() {
    // 注册帧处理器
    _controlChannel->setOnFrame(
        [this](protocol::FrameType type,
               const uint8_t* data, uint32_t len) {
            onRelayFrame(type, data, len);
        });

    // 注：状态变更日志在 init() 中注册（包含 this 捕获用于日志上下文化）
}

void HostAgentApp::onRelayFrame(
    protocol::FrameType type,
    const uint8_t* data, uint32_t len) {
    switch (type) {
    case protocol::FrameType::REGISTER_ACK: {
        auto payload = protocol::FrameCodec::decodeRegisterAckPayload(
            data, len);
        _assignedPort = payload.assignedPort;

        auto code = (_config.publicIp.find(':') != std::string::npos
            ? "[" + _config.publicIp + "]" : _config.publicIp)
            + ":" + std::to_string(payload.assignedPort);

        // 存储到房间列表（WebUI 用）
        HostRoom hr;
        hr.roomId        = payload.roomId;
        hr.roomName      = _lastRegisterName.empty() ? "未命名房间" : _lastRegisterName;
        hr.assignedPort  = payload.assignedPort;
        hr.localMcPort   = _lastRegisterMcPort;
        hr.connectionCode = code;
        _hostRooms.push_back(hr);
        _lastRegisterName.clear();

        std::cout << "[HostAgent] Room registered! #"
                  << payload.roomId << " port " << payload.assignedPort << std::endl;
        _director->addLog("房间已注册: #" + std::to_string(payload.roomId)
            + " 端口:" + std::to_string(payload.assignedPort));
        break;
    }

    case protocol::FrameType::HEARTBEAT_ACK:
        // 心跳应答，无需处理
        break;

    case protocol::FrameType::OPEN_STREAM: {
        auto payload = protocol::FrameCodec::decodeOpenStreamPayload(data, len);
        uint32_t playerConnId = payload.playerConnId;
        std::cout << "[HostAgent] Player connecting (ID: " << playerConnId
                  << "), opening tunnel..." << std::endl;
        _director->addLog("玩家接入: ID:" + std::to_string(playerConnId));

        // 1. 创建 TLS 隧道连接到中继房间端口
        auto* transport = _director->transport();
        if (!transport || _assignedPort == 0) {
            std::cerr << "[HostAgent] No transport or no assigned port" << std::endl;
            break;
        }

        SOCKET tunnelSock = NetUtil::createSocket(_config.publicIp);
        if (tunnelSock == INVALID_SOCKET) {
            std::cerr << "[HostAgent] Tunnel socket creation failed" << std::endl;
            _director->addLog("隧道创建 socket 失败");
            break;
        }

        uint16_t tunnelPort = payload.tunnelPort;
        _director->addLog("隧道连接 relay:" + std::to_string(tunnelPort));
        sockaddr_storage relayAddr{};
        int relayAddrLen = NetUtil::fillAddr(
            _config.publicIp.c_str(), tunnelPort, relayAddr);
        if (relayAddrLen == 0 ||
            ::connect(tunnelSock, reinterpret_cast<sockaddr*>(&relayAddr),
                      relayAddrLen) == SOCKET_ERROR) {
            std::cerr << "[HostAgent] Tunnel connect to relay port "
                      << tunnelPort << " failed" << std::endl;
            _director->addLog("隧道 TCP 连接失败:" + std::to_string(tunnelPort));
            closesocket(tunnelSock);
            break;
        }
        _director->addLog("隧道 TCP 已连接");
        transport->bindSocket(tunnelSock);

        // 2. TLS 客户端握手
        auto tunnelTls = std::make_unique<TlsPskContext>();
        if (!tunnelTls->init(_config.psk, _config.pskIdentity, TlsRole::Client)) {
            std::cerr << "[HostAgent] Tunnel TLS init failed" << std::endl;
            closesocket(tunnelSock);
            break;
        }

        auto* tunnelSession = new Session(tunnelSock, transport);
        tunnelSession->retain();
        _tunnels[playerConnId] = tunnelSession;  // 跟踪生命周期

        // TLS 握手
        auto handshakeDone = std::make_shared<bool>(false);
        auto connIdSent = std::make_shared<bool>(false);
        auto tlsShared = std::make_shared<std::unique_ptr<TlsPskContext>>(
            std::move(tunnelTls));
        SOCKET mcSock = INVALID_SOCKET;
        auto mcSockShared = std::make_shared<SOCKET>(INVALID_SOCKET);

        // 发起 TLS 握手（客户端先发 ClientHello）
        {
            std::vector<uint8_t> handshakeOutput;
            auto& tls = *tlsShared;
            auto result = tls->doHandshake(nullptr, 0, handshakeOutput);
            if (result == TlsPskContext::HandshakeResult::Failed) {
                std::cerr << "[HostAgent] Tunnel handshake init failed" << std::endl;
                tunnelSession->close();
                _tunnels.erase(playerConnId);
                break;
            }
            if (!handshakeOutput.empty()) {
                tunnelSession->send(
                    reinterpret_cast<const char*>(handshakeOutput.data()),
                    handshakeOutput.size());
            }
            if (result == TlsPskContext::HandshakeResult::Done)
                *handshakeDone = true;
        }

        tunnelSession->setOnData([this, tlsShared, handshakeDone, connIdSent,
                                  playerConnId, mcSockShared, tunnelSession,
                                  transport]
            (SOCKET, const char* data, size_t len) {
            auto& tls = *tlsShared;

            if (!*handshakeDone) {
                // ── 继续 TLS 握手 ──
                std::vector<uint8_t> handshakeOutput;
                auto result = tls->doHandshake(
                    reinterpret_cast<const uint8_t*>(data), len,
                    handshakeOutput);
                if (!handshakeOutput.empty()) {
                    tunnelSession->send(
                        reinterpret_cast<const char*>(handshakeOutput.data()),
                        handshakeOutput.size());
                }
                if (result == TlsPskContext::HandshakeResult::Done) {
                    *handshakeDone = true;
                    std::cout << "[HostAgent] Tunnel TLS handshake done" << std::endl;
                    _director->addLog("隧道 TLS 握手完成");
                    // 握手完成后，发送 playerConnId 标识
                    if (!*connIdSent) {
                        uint8_t idBuf[4];
                        idBuf[0] = static_cast<uint8_t>(playerConnId >> 24);
                        idBuf[1] = static_cast<uint8_t>((playerConnId >> 16) & 0xFF);
                        idBuf[2] = static_cast<uint8_t>((playerConnId >> 8) & 0xFF);
                        idBuf[3] = static_cast<uint8_t>(playerConnId & 0xFF);
                        std::vector<uint8_t> encrypted;
                        if (tls->encrypt(idBuf, 4, encrypted)) {
                            tunnelSession->send(
                                reinterpret_cast<const char*>(encrypted.data()),
                                encrypted.size());
                        }
                        *connIdSent = true;

                        // 连接到本地 MC
                        SOCKET mc = NetUtil::createSocket("127.0.0.1");
                        sockaddr_storage mcAddr{};
                        int mcLen = NetUtil::fillAddr(
                            "127.0.0.1", this->_localMcPort, mcAddr);
                        if (mc != INVALID_SOCKET &&
                            ::connect(mc, reinterpret_cast<sockaddr*>(&mcAddr),
                                      mcLen) != SOCKET_ERROR) {
                            transport->bindSocket(mc);
                            *mcSockShared = mc;
                            auto* mcSession = new Session(mc, transport);
                            mcSession->retain();

                            // MC → 隧道：MC 发来的数据加密后发回中继
                            mcSession->setOnData(
                                [tlsShared, tunnelSession]
                                (SOCKET, const char* d, size_t l) {
                                    std::vector<uint8_t> encrypted;
                                    if ((*tlsShared)->encrypt(
                                            reinterpret_cast<const uint8_t*>(d),
                                            l, encrypted)) {
                                        tunnelSession->send(
                                            reinterpret_cast<const char*>(
                                                encrypted.data()),
                                            encrypted.size());
                                    }
                                });
                            mcSession->setOnClose(
                                [this, mcSession, tunnelSession](SOCKET, int err) {
                                    _director->addLog("MC closed err=" + std::to_string(err) + " → closing tunnel");
                                    tunnelSession->close();
                                });
                            mcSession->start();
                            std::cout << "[HostAgent] Connected to local MC:"
                                      << _localMcPort << std::endl;
                            _director->addLog("隧道已连接 MC:" + std::to_string(_localMcPort));
                        } else {
                            std::cerr << "[HostAgent] Failed to connect MC"
                                      << std::endl;
                            _director->addLog("隧道连接 MC 失败:" + std::to_string(_localMcPort));
                            if (mc != INVALID_SOCKET) closesocket(mc);
                        }
                    }
                } else if (result == TlsPskContext::HandshakeResult::Failed) {
                    std::cerr << "[HostAgent] Tunnel handshake failed" << std::endl;
                    _director->addLog("隧道 TLS 握手失败");
                    tunnelSession->close();
                }
                return;
            }

            // ── 握手完成后: 解密中继转发的数据 → 发往本地 MC ──
            std::vector<uint8_t> plaintext;
            if (!tls->decrypt(
                    reinterpret_cast<const uint8_t*>(data), len, plaintext)) {
                static int decFail = 0;
                if (++decFail <= 3) {
                    _director->addLog("隧道解密失败: len=" + std::to_string(len));
                }
                return;
            }
            if (plaintext.empty()) return;
            static int dataCount = 0;
            if (++dataCount <= 3) {
                _director->addLog("隧道收到数据: " + std::to_string(plaintext.size()) + "B");
            }
            _totalBytesRelayed += plaintext.size();

            SOCKET mc = *mcSockShared;
            if (mc != INVALID_SOCKET) {
                // 通过 TransportCore 异步发送到 MC
                transport->postSend(mc,
                    reinterpret_cast<const char*>(plaintext.data()),
                    plaintext.size(),
                    [](SOCKET, int) {});
            }
        });

        tunnelSession->setOnClose([this, playerConnId, mcSockShared]
            (SOCKET, int cerr) {
            _director->addLog("Tunnel closed player=" + std::to_string(playerConnId) + " err=" + std::to_string(cerr));
            std::cout << "[HostAgent] Tunnel closed for player "
                      << playerConnId << std::endl;
            SOCKET mc = *mcSockShared;
            if (mc != INVALID_SOCKET) closesocket(mc);
            *mcSockShared = INVALID_SOCKET;
            _tunnels.erase(playerConnId);
        });

        tunnelSession->start();

        // 发送 STREAM_BIND 通知中继
        protocol::StreamBindPayload sbp;
        sbp.playerConnId = playerConnId;
        auto bindFrame = protocol::FrameCodec::encodeStreamBind(sbp);
        _controlChannel->sendFrame(bindFrame);

        std::cout << "[HostAgent] Tunnel opened for player "
                  << playerConnId << ", STREAM_BIND sent" << std::endl;
        break;
    }

    case protocol::FrameType::ERROR: {
        auto payload = protocol::FrameCodec::decodeErrorPayload(
            data, len);
        std::cerr << "[HostAgent] Error from relay: "
                  << protocol::errorCodeToString(payload.code)
                  << " - " << payload.message << std::endl;
        break;
    }

    default:
        break;
    }
}

// ============================================================
//  公开 API — connectToRelay / disconnectFromRelay / connectionState
// ============================================================

// SEH 保护包装：ControlChannel::connect 可能触发访问违例
static bool safeControlConnect(ControlChannel* cc,
                                const std::string& relayIp, uint16_t controlPort,
                                const std::string& psk, const std::string& identity,
                                TransportCore* transport) {
    // 原始文件日志追踪
    FILE* fp = nullptr;
    fopen_s(&fp, "connect_trace.log", "a");
    if (fp) {
        fprintf(fp, "safeControlConnect: entry, ip=%s port=%u\n", relayIp.c_str(), controlPort);
        fflush(fp);
    }
#ifdef _WIN32
    __try {
        if (fp) { fprintf(fp, "safeControlConnect: calling cc->connect...\n"); fflush(fp); }
        bool r = cc->connect(relayIp, controlPort, psk, identity, transport);
        if (fp) { fprintf(fp, "safeControlConnect: cc->connect returned %d\n", r); fflush(fp); fclose(fp); }
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (fp) { fprintf(fp, "safeControlConnect: SEH exception caught!\n"); fflush(fp); fclose(fp); }
        return false;
    }
#else
    bool r = cc->connect(relayIp, controlPort, psk, identity, transport);
    if (fp) { fprintf(fp, "safeControlConnect: cc->connect returned %d\n", r); fflush(fp); fclose(fp); }
    return r;
#endif
}

bool HostAgentApp::connectToRelay(const std::string& relayIp,
                                   uint16_t controlPort,
                                   const std::string& psk) {
    if (!_controlChannel) {
        std::cerr << "[HostAgent] No ControlChannel" << std::endl;
        return false;
    }

    auto state = _controlChannel->state();
    if (state == ControlState::Active || state == ControlState::Connecting
        || state == ControlState::Registering) {
        std::cerr << "[HostAgent] Already connected or connecting (state="
                  << static_cast<int>(state) << ")" << std::endl;
        return false;
    }

    // 更新配置
    _config.publicIp    = relayIp;
    _config.controlPort = controlPort;
    if (!psk.empty()) {
        _config.psk = psk;
    }

    std::cout << "[HostAgent] Connecting to relay "
              << relayIp << ":" << controlPort << std::endl;

    if (!safeControlConnect(_controlChannel.get(), relayIp, controlPort,
                             _config.psk, _config.pskIdentity, _director->transport())) {
        std::cerr << "[HostAgent] ControlChannel connect failed" << std::endl;
        _director->addLog("连接中继失败: " + relayIp);
        return false;
    }

    // 等待 TLS 握手完成（connect() 只发送 ClientHello，握手由 IOCP 线程异步完成）
    {
        int waitCount = 0;
        while (_controlChannel->state() != ControlState::Active && waitCount < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waitCount++;
        }
        if (_controlChannel->state() != ControlState::Active) {
            std::cerr << "[HostAgent] TLS handshake timeout" << std::endl;
            _director->addLog("TLS 握手超时");
            return false;
        }
    }

    std::cout << "[HostAgent] Connected to relay, ready to create room"
              << std::endl;
    _director->addLog("已连接中继 " + relayIp + ":" + std::to_string(controlPort));
    return true;
}

void HostAgentApp::disconnectFromRelay() {
    if (!_controlChannel) return;

    auto state = _controlChannel->state();
    if (state == ControlState::Active) {
        _controlChannel->deregister();
    }
    _controlChannel->disconnect();

    // 关闭所有数据隧道
    for (auto& kv : _tunnels) {
        if (kv.second) {
            kv.second->close();
            kv.second->release();
        }
    }
    _tunnels.clear();
    _assignedPort = 0;
    _hostRooms.clear();
    _lastRegisterName.clear();

    _director->addLog("已断开中继连接");
    std::cout << "[HostAgent] Disconnected from relay" << std::endl;
}

ControlState HostAgentApp::connectionState() const {
    if (!_controlChannel) return ControlState::Disconnected;
    return _controlChannel->state();
}

// ============================================================
//  HTTP 服务器（WebUI — 房主端最小实现）
// ============================================================

void HostAgentApp::startHttpServer() {
    _httpServer = std::make_unique<httplib::Server>();
    registerHttpRoutes(*_httpServer);

    uint16_t port = _httpPort;
    _httpThread = std::thread([this, port]() {
        if (!_httpServer->listen("127.0.0.1", port)) {
            std::cerr << "[HostAgent HTTP] Failed to start on port "
                      << port << std::endl;
        }
        std::cout << "[HostAgent HTTP] Server stopped" << std::endl;
    });
    std::cout << "[HostAgent] WebUI: http://127.0.0.1:" << port << std::endl;
}

void HostAgentApp::stopHttpServer() {
    if (_httpServer) {
        _httpServer->stop();
    }
    if (_httpThread.joinable()) {
        _httpThread.join();
    }
}

void HostAgentApp::registerHttpRoutes(httplib::Server& svr) {
    using json = nlohmann::json;

    auto setJsonHeaders = [](httplib::Response& res) {
        res.set_header("Content-Type", "application/json; charset=utf-8");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    };

    // CORS 预检
    svr.Options(R"(/api/v1/.*)", [](const auto&, auto& res) {
        res.set_header("Content-Type", "application/json; charset=utf-8");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // GET /api/v1/ping — 返回角色 + 中继连接状态
    svr.Get("/api/v1/ping", [this, setJsonHeaders](const httplib::Request&,
                                                      httplib::Response& res) {
        setJsonHeaders(res);
        auto state = _controlChannel ? _controlChannel->state() : ControlState::Disconnected;

        json resp = {
            {"status",  "ok"},
            {"role",    "host"},
            {"version", "1.0.1"},
            {"protocol", 1},
            {"relayIp",  _config.publicIp},
            {"controlPort", _config.controlPort},
            {"connectionState", static_cast<int>(state)}
        };
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // GET /api/v1/config — 返回房主端配置
    svr.Get("/api/v1/config", [this, setJsonHeaders](const httplib::Request&,
                                                        httplib::Response& res) {
        setJsonHeaders(res);
        json resp = {
            {"role",         "host"},
            {"publicIp",     _config.publicIp},
            {"controlPort",  _config.controlPort},
            {"httpPort",     _httpPort},
            {"localMcPort",  _localMcPort},
            {"version",      "1.0.1"},
            {"protocolVersion", 1}
        };
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // POST /api/v1/rooms — 创建房间（通过 ControlChannel → REGISTER 帧）
    // 支持可选 relayIp：若提供且未连接，自动先连接中继再创建房间
    svr.Post("/api/v1/rooms", [this, setJsonHeaders](const httplib::Request& req,
                                                        httplib::Response& res) {
        setJsonHeaders(res);

        json body;
        try { body = json::parse(req.body); } catch (...) {
            json err = {{"error", {{"code", "INVALID_PARAMS"}, {"message", "invalid JSON"}}}};
            res.status = 400;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }

        // 若请求带了 relayIp 且未连接 → 自动连接
        std::string relayIp = body.value("relayIp", "");
        uint16_t ctrlPort = body.value("controlPort", 40000);
        if (!relayIp.empty()) {
            auto curState = _controlChannel ? _controlChannel->state() : ControlState::Disconnected;
            bool isSameRelay = (curState == ControlState::Active && _config.publicIp == relayIp);
            if (!isSameRelay) {
                bool connected = connectToRelay(relayIp, ctrlPort, _config.psk);
                if (!connected) {
                    json err = {{"error", {{"code", "CONNECT_FAILED"}, {"message", "连接中继失败: " + relayIp}}}};
                    res.status = 502;
                    res.set_content(err.dump(), "application/json; charset=utf-8");
                    return;
                }
            }
        }

        if (!_controlChannel || _controlChannel->state() != ControlState::Active) {
            json err = {{"error", {{"code", "NOT_CONNECTED"}, {"message", "未连接到中继服务器"}}}};
            res.status = 503;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }

        std::string roomName = body.value("roomName", "未命名房间");
        int mcPortRaw = body.value("localMcPort", 25565);
        if (mcPortRaw < 1 || mcPortRaw > 65535) {
            json err = {{"error", {{"code", "INVALID_PARAMS"}, {"message", "localMcPort must be 1-65535"}}}};
            res.status = 400;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }
        uint16_t mcPort = static_cast<uint16_t>(mcPortRaw);

        // 暂存房间名供 REGISTER_ACK 使用
        _lastRegisterName = roomName;
        _lastRegisterMcPort = mcPort;

        // 使用 promise/future 阻塞等待 REGISTER_ACK
        auto promise = std::make_shared<std::promise<std::tuple<bool, uint32_t, uint16_t, std::string>>>();
        auto future = promise->get_future();

        bool sent = _controlChannel->registerRoomAsync(roomName, mcPort,
            [promise](bool ok, uint32_t roomId, uint16_t port, std::string err) {
                promise->set_value({ok, roomId, port, err});
            });

        if (!sent) {
            json err = {{"error", {{"code", "SEND_FAILED"}, {"message", "REGISTER 帧发送失败"}}}};
            res.status = 500;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }

        // 等待 ACK（最多 6 秒，比内部超时多 1 秒）
        auto status = future.wait_for(std::chrono::seconds(6));
        if (status == std::future_status::timeout) {
            json err = {{"error", {{"code", "TIMEOUT"}, {"message", "等待中继确认超时"}}}};
            res.status = 504;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }

        auto [ok, roomId, assignedPort, errMsg] = future.get();
        if (!ok) {
            json errResp = {{"error", {{"code", "REGISTER_FAILED"}, {"message", errMsg}}}};
            res.status = 502;
            res.set_content(errResp.dump(), "application/json; charset=utf-8");
            return;
        }

        json resp = {
            {"roomId",         roomId},
            {"roomName",       roomName},
            {"connectionCode", (_config.publicIp.find(':') != std::string::npos
                ? "[" + _config.publicIp + "]" : _config.publicIp)
                + ":" + std::to_string(assignedPort)},
            {"assignedPort",   assignedPort},
            {"localMcPort",    mcPort},
            {"status",         "active"},
            {"playerCount",    0}
        };
        res.status = 201;
        res.set_content(resp.dump(), "application/json; charset=utf-8");

        std::cout << "[HostAgent] Room created via WebUI: #" << roomId
                  << " port " << assignedPort << std::endl;
        _director->addLog("房间已创建: #" + std::to_string(roomId)
            + " \"" + roomName + "\" 端口:" + std::to_string(assignedPort));
    });

    // ── Host 控制端点 ──

    // GET /api/v1/host/rooms — Host 创建的房间列表（供 WebUI 显示）
    svr.Get("/api/v1/host/rooms", [this, setJsonHeaders](const httplib::Request&,
                                                           httplib::Response& res) {
        setJsonHeaders(res);
        json arr = json::array();
        uint32_t numTunnels = static_cast<uint32_t>(_tunnels.size());
        for (auto& hr : _hostRooms) {
            arr.push_back({
                {"roomId",         hr.roomId},
                {"roomName",       hr.roomName},
                {"connectionCode", hr.connectionCode},
                {"assignedPort",   hr.assignedPort},
                {"localMcPort",    hr.localMcPort},
                {"status",         "active"},
                {"playerCount",    numTunnels},
                {"tunnelCount",    numTunnels},
                {"bytesRelayed",   _totalBytesRelayed},
                {"hostAddress",    _config.publicIp},
                {"relayIp",        _config.publicIp}
            });
        }
        json resp = {{"rooms", arr}, {"total", _hostRooms.size()}};
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // GET /api/v1/host/status — 查询房主端连接状态
    svr.Get("/api/v1/host/status", [this, setJsonHeaders](const httplib::Request&,
                                                            httplib::Response& res) {
        setJsonHeaders(res);
        auto state = _controlChannel ? _controlChannel->state() : ControlState::Disconnected;
        bool connected = (state == ControlState::Active);
        json resp = {
            {"connected",      connected},
            {"state",          static_cast<int>(state)},
            {"relayIp",        _config.publicIp},
            {"controlPort",    _config.controlPort},
            {"localMcPort",    _localMcPort},
            {"assignedPort",   _assignedPort}
        };
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // POST /api/v1/host/connect — 连接远程中继
    svr.Post("/api/v1/host/connect", [this](const httplib::Request& req,
                                             httplib::Response& res) {
        res.set_header("Content-Type", "application/json; charset=utf-8");
        res.set_header("Access-Control-Allow-Origin", "*");

        json body;
        try { body = json::parse(req.body); } catch (...) {
            json err = {{"error", {{"code", "INVALID_PARAMS"}, {"message", "invalid JSON"}}}};
            res.status = 400;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }

        std::string relayIp = body.value("relayIp", "");
        if (relayIp.empty()) {
            json err = {{"error", {{"code", "INVALID_PARAMS"}, {"message", "relayIp is required"}}}};
            res.status = 400;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }
        uint16_t ctrlPort = body.value("controlPort", 40000);

        // 防止重复连接
        auto curState = _controlChannel->state();
        if (curState != ControlState::Disconnected && curState != ControlState::Closed) {
            json resp = {{"status", "already"}, {"relayIp", _config.publicIp}};
            res.status = 200;
            res.set_content(resp.dump(), "application/json; charset=utf-8");
            return;
        }

        _config.publicIp = relayIp;
        _config.controlPort = ctrlPort;

        // 同步连接
        bool ok = connectToRelay(relayIp, ctrlPort, _config.psk);
        if (!ok) {
            json err = {{"error", {{"code", "CONNECT_FAILED"}, {"message", "连接中继失败"}}}};
            res.status = 502;
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }

        json resp = {{"status", "connected"}, {"relayIp", relayIp}, {"controlPort", ctrlPort}};
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // POST /api/v1/host/disconnect — 断开连接
    svr.Post("/api/v1/host/disconnect", [this, setJsonHeaders](const httplib::Request&,
                                                                  httplib::Response& res) {
        setJsonHeaders(res);
        disconnectFromRelay();
        json resp = {{"status", "disconnected"}};
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // GET /api/v1/logs — 后端日志
    svr.Get("/api/v1/logs", [this, setJsonHeaders](const httplib::Request&,
                                                      httplib::Response& res) {
        setJsonHeaders(res);
        auto logs = _director->getRecentLogs(0);  // 0 = 返回全部
        json arr = json::array();
        for (auto& [t, msg] : logs) {
            arr.push_back({{"time", t}, {"msg", msg}});
        }
        json resp = {{"logs", arr}};
        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // GET / — WebUI 静态页面
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
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
}

} // namespace yunyi
