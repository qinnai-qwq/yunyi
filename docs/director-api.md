# 云驿 Director 公开接口手册 v1.2.0

> 本文档定义 `Director` 类的全部公开接口。`Director` 是 NetEngine 的**唯一门面**，
> 所有外部代码（App 层、HTTP 桥接层）**只能通过 Director 调用引擎功能**，
> 不得直接访问引擎内部模块（Session / Scheduler / TunnelManager / TransportCore 等）。

---

## 1. 配置结构体

```cpp
/**
 * @brief 引擎全局配置
 *
 * 在调用 Director::init() 时传入，初始化后不可更改。
 */
struct EngineConfig {
    /** 日志级别: 0=off, 1=error, 2=warn, 3=info, 4=debug, 5=trace */
    int logLevel = 3;

    /** 日志输出路径，空字符串 = stderr */
    std::string logPath;

    /** IO 工作线程数，0 = 自动检测（CPU 核心数） */
    uint32_t ioThreads = 0;
};

/**
 * @brief 端口池配置
 *
 * 中继服务器为每个房间分配一个独立端口，端口从该池中取用。
 */
struct PortPoolConfig {
    /** 端口池起始（含），默认 40000 */
    uint16_t rangeStart = 40000;

    /** 端口池结束（含），默认 41000 */
    uint16_t rangeEnd = 41000;

    /** 单个房间最大玩家数，超出后拒绝新连接 */
    uint32_t maxPlayersPerRoom = 10;
};

/**
 * @brief TLS-PSK 配置
 *
 * 当前仅支持 TLS-PSK，但以结构体封装以便未来扩展其他加密方式。
 */
struct TlsConfig {
    /** PSK 预共享密钥（UTF-8），最少 16 字节 */
    std::string psk;

    /** PSK 身份标识（可选，为空则用默认值 "yunyi"） */
    std::string pskIdentity;

    /** 最低 TLS 版本:
     *  0 = TLS 1.2, 1 = TLS 1.3
     *  默认 1 (TLS 1.3)
     */
    int minTlsVersion = 1;
};

/**
 * @brief 中继服务器地址
 */
struct RelayAddress {
    /** 公网 IPv4 地址 */
    std::string host;

    /** 端口 */
    uint16_t port = 40000;
};
```

---

## 2. 结果类型

```cpp
/**
 * @brief 房间创建结果
 */
struct RoomResult {
    /** 是否成功 */
    bool ok = false;

    /** 房间 ID（成功时有效） */
    uint32_t roomId = 0;

    /** 分配到的公网端口（成功时有效） */
    uint16_t assignedPort = 0;

    /** 连接码字符串，格式 "IP:Port"（成功时有效） */
    std::string connectionCode;

    /** 错误码（失败时有效） */
    std::string errorCode;

    /** 错误描述（失败时有效） */
    std::string errorMessage;
};

/**
 * @brief 引擎运行统计
 */
struct EngineStats {
    uint64_t uptimeSeconds = 0;
    uint32_t activeRooms = 0;
    uint32_t totalRooms = 0;
    uint32_t activeTunnels = 0;
    uint32_t totalConnections = 0;
    uint32_t portPoolUsed = 0;
    uint32_t portPoolTotal = 0;
    uint64_t bytesRelayed = 0;
};

/**
 * @brief 单个房间信息
 */
struct RoomInfo {
    uint32_t roomId = 0;
    std::string roomName;
    std::string connectionCode;
    uint16_t assignedPort = 0;
    uint16_t localMcPort = 0;
    std::string status;          // "waiting" | "active" | "closed"
    uint32_t playerCount = 0;
    std::string createdAt;       // ISO 8601
    std::string hostAddress;     // 房主控制连接远端地址（仅中继端有效）
};

/**
 * @brief 单个玩家连接信息
 */
struct PlayerInfo {
    uint32_t id = 0;
    std::string connectedAt;     // ISO 8601
    uint64_t bytesSent = 0;
    uint64_t bytesRecv = 0;
};
```

---

## 3. Director 类

```cpp
/**
 * @class Director
 * @brief 引擎唯一门面，全局单例
 *
 * 生命周期：
 *   1. Director::instance() 获取单例
 *   2. init(cfg)            初始化引擎
 *   3. startRelayService()  或 connectToRelay()  选择角色
 *   4. 运行期间通过公开方法操作房间/隧道
 *   5. shutdown()           关闭引擎
 *
 * @note 外部代码严禁直接引用 Session、Scheduler、TunnelManager、
 *       TransportCore 等内部头文件。所有操作必须通过 Director。
 */
class Director {
public:
    // ======================== 生命周期 ========================

    /**
     * @brief 获取全局单例
     * @return Director 引用
     *
     * 线程安全，可在 init() 之前调用（此时引擎未初始化）。
     */
    static Director& instance();

    /**
     * @brief 初始化引擎
     * @param cfg 引擎全局配置
     * @return true 成功，false 失败（可调用 getLastError() 获取原因）
     *
     * @pre 未调用过 init()，或上次调用后已 shutdown()
     * @post 引擎进入 "已初始化" 状态，可以调用角色选择方法
     *
     * 必须在使用任何其他方法前调用。
     */
    bool init(const EngineConfig& cfg);

    /**
     * @brief 关闭引擎，释放所有资源
     *
     * @pre 已调用 init()
     * @post 引擎回到 "未初始化" 状态，所有连接关闭、端口回收
     *
     * 阻塞直到所有 IO 操作完成。
     */
    void shutdown();

    /**
     * @brief 引擎是否已初始化
     */
    bool isInitialized() const;

    // ======================== 中继角色 ========================

    /**
     * @brief 启动中继服务（本机作为公网中继服务器）
     * @param ports 端口池配置
     * @param tls TLS-PSK 配置
     * @return true 成功，false 失败
     *
     * @pre 已 init()
     * @pre 未调用 connectToRelay()（两个角色互斥）
     * @post 中继服务开始监听，等待房主连接
     *
     * 调用后本机进入"中继服务器"角色：
     *   - 在端口池范围内准备 accept 玩家连接
     *   - 接受房主控制连接（TLS-PSK）
     *   - 接受房主数据隧道（TLS-PSK）
     */
    bool startRelayService(const PortPoolConfig& ports,
                           const TlsConfig& tls);

    // ======================== 房主角色 ========================

    /**
     * @brief 连接到中继服务器（本机作为房主端）
     * @param addr 中继服务器地址
     * @param tls TLS-PSK 配置（必须与中继服务器的 PSK 一致）
     * @return true 成功（控制连接建立 + TLS 握手完成），false 失败
     *
     * @pre 已 init()
     * @pre 未调用 startRelayService()（两个角色互斥）
     * @post 控制连接建立，可以调用 createRoom()
     *
     * 调用后本机进入"房主端"角色。
     */
    bool connectToRelay(const RelayAddress& addr,
                        const TlsConfig& tls);

    /**
     * @brief 创建房间（房主端调用）
     * @param roomName 房间名称，1–64 字符 UTF-8
     * @param localMcPort 本机 MC 服务端口（127.0.0.1 上监听的）
     * @return 创建结果，含连接码
     *
     * @pre 已 connectToRelay() 且控制连接处于 ACTIVE 状态
     *
     * 通过控制连接向中继发送 REGISTER 帧，等待 REGISTER_ACK。
     * 成功后 roomResult.connectionCode 可直接分享给朋友。
     */
    RoomResult createRoom(std::string_view roomName,
                          uint16_t localMcPort);

    /**
     * @brief 关闭房间（房主端调用）
     * @param roomId 房间 ID
     * @return true 成功
     *
     * @pre 已 createRoom() 且房间未关闭
     *
     * 向中继发送 DEREGISTER 帧，中继回收端口。
     */
    bool closeRoom(uint32_t roomId);

    // ======================== 房间查询（中继角色） ========================

    /**
     * @brief 获取指定房间信息
     * @param roomId 房间 ID
     * @return 房间信息；若不存在，RoomInfo::status 为 "" 且 roomId 为 0
     *
     * @pre 已 startRelayService()
     */
    RoomInfo getRoomInfo(uint32_t roomId) const;

    /**
     * @brief 获取所有活跃房间列表
     * @pre 已 startRelayService()
     */
    std::vector<RoomInfo> listRooms() const;

    /**
     * @brief 获取指定房间的玩家列表
     * @param roomId 房间 ID
     * @pre 已 startRelayService()
     */
    std::vector<PlayerInfo> getRoomPlayers(uint32_t roomId) const;

    /**
     * @brief 强制关闭房间（中继端调用）
     * @param roomId 房间 ID
     * @return true 成功
     *
     * @pre 已 startRelayService()
     *
     * 关闭后端口回收，向对应房主发送 ERROR 帧通知。
     */
    bool forceCloseRoom(uint32_t roomId);

    // ======================== 会话与心跳 ========================

    /**
     * @brief 更新当前控制会话所在房间的心跳时间
     * @return 房间 ID，0 表示当前控制会话未关联任何房间
     * @pre 在 FrameDispatcher handler 回调中调用（依赖 currentReplySession）
     */
    uint32_t touchRoomHeartbeat();

    /**
     * @brief 按控制会话查找房间 ID
     * @param session 控制会话指针
     * @return 房间 ID，0 表示未找到
     */
    uint32_t findRoomByControlSession(Session* session) const;

    // ======================== 统计与诊断 ========================

    /**
     * @brief 获取引擎运行统计
     *
     * 可在任意角色下调用（init() 之后）。
     */
    EngineStats getStats() const;

    /**
     * @brief 获取最后一次错误描述
     * @return 错误信息字符串，无错误时为空
     */
    std::string getLastError() const;

    // ======================== 禁止 ========================

    Director(const Director&) = delete;
    Director& operator=(const Director&) = delete;
    Director(Director&&) = delete;
    Director& operator=(Director&&) = delete;

private:
    Director() = default;
    ~Director() = default;
};
```

---

## 4. 使用示例

### 4.1 中继服务器

```cpp
#include "NetEngine/NetEngine_H.h"

int main() {
    auto& d = Director::instance();

    EngineConfig engineCfg;
    engineCfg.logLevel = 4;

    if (!d.init(engineCfg)) {
        std::cerr << "init failed: " << d.getLastError() << "\n";
        return 1;
    }

    PortPoolConfig ports;
    ports.rangeStart = 40000;
    ports.rangeEnd   = 41000;

    TlsConfig tls;
    tls.psk = "my-secure-pre-shared-key-min-16bytes";
    tls.minTlsVersion = 1;  // TLS 1.3

    if (!d.startRelayService(ports, tls)) {
        std::cerr << "startRelayService failed: " << d.getLastError() << "\n";
        return 1;
    }

    std::cout << "Relay server running...\n";

    // 在另一个线程或通过 HTTP API 查询
    auto stats = d.getStats();
    auto rooms = d.listRooms();

    // ...

    d.shutdown();
    return 0;
}
```

### 4.2 房主端

```cpp
#include "NetEngine/NetEngine_H.h"

int main() {
    auto& d = Director::instance();

    EngineConfig engineCfg;
    if (!d.init(engineCfg)) {
        return 1;
    }

    RelayAddress addr;
    addr.host = "1.2.3.4";
    addr.port = 40000;

    TlsConfig tls;
    tls.psk = "my-secure-pre-shared-key-min-16bytes";

    if (!d.connectToRelay(addr, tls)) {
        std::cerr << "connect failed: " << d.getLastError() << "\n";
        return 1;
    }

    auto result = d.createRoom("我的世界", 25565);
    if (!result.ok) {
        std::cerr << "createRoom failed: " << result.errorMessage << "\n";
        return 1;
    }

    std::cout << "连接码: " << result.connectionCode << "\n";
    std::cout << "分享给朋友，直接在 MC 里粘贴即可！\n";

    // ... 运行，等待玩家 ...

    d.closeRoom(result.roomId);
    d.shutdown();
    return 0;
}
```

---

## 5. 调用前置条件速查表

| 方法 | 前置条件 |
|------|----------|
| `init(cfg)` | 未初始化，或已 shutdown() |
| `shutdown()` | 已 init() |
| `startRelayService(ports, tls)` | 已 init()，未调用 connectToRelay() |
| `connectToRelay(addr, tls)` | 已 init()，未调用 startRelayService() |
| `createRoom(name, port)` | 已 connectToRelay()，控制连接 ACTIVE |
| `closeRoom(id)` | 已 createRoom()，房间未关闭 |
| `getRoomInfo(id)` | 已 startRelayService() |
| `listRooms()` | 已 startRelayService() |
| `forceCloseRoom(id)` | 已 startRelayService() |
| `getStats()` | 已 init() |

---

## 6. 线程安全

- `Director::instance()` — 线程安全
- `init()` / `shutdown()` — 不可并发，须在单一线程调用
- `createRoom()` / `closeRoom()` — 线程安全（内部加锁）
- `getRoomInfo()` / `listRooms()` / `getStats()` — 线程安全（只读或内部加锁）
- `getLastError()` — 线程安全（thread_local）
