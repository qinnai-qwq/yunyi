# 云驿（Yún Yì） v1.2.2 架构设计文档

> 一个让没有公网 IP 的朋友，也能通过你的公网服务器加入 Minecraft 联机房间的中继工具。
> 当前版本：**v1.2.2** — 双端口隧道架构 + 统一事件流 + RoomRegistry 激活 + HostAgent WebUI。

---

## 1. 项目背景与核心场景

**问题**：5 个人一起玩 Minecraft，房主开了"对局域网开放"，但房主没有公网 IP，其他人也没有。只有你有公网 IPv4/IPv6。

**目标**：由你搭一台中继服务器，房主通过它把本地游戏"暴露"出去，其他玩家（包括你自己）直接用一串连接码，在 MC 客户端"直接连接"里粘贴即可进入房间，**不需要安装任何额外软件**。

---

## 2. 三个角色

| 角色 | 运行位置 | 是否需要装软件 | 核心职责 |
|---|---|---|---|
| **中继服务器** | 你的机器（公网IP） | 是（这就是本项目的服务端） | 端口池管理、房间配对、协议边界转换、纯字节转发 |
| **房主端（暴露模式）** | 房主机器（无公网IP，跑MC） | 是 | 常驻控制连接、按信令新开加密数据隧道、把流量转发进本地MC |
| **玩家端** | 其他玩家机器 | **否** | 原生 MC 客户端"直接连接"，粘贴连接码即可 |

---

## 3. 关键技术决策

### 3.1 拓扑与安全

- MC 服务端（房主的"对局域网开放"）只监听 `127.0.0.1:xxxxx`，**永远不直接暴露公网**
- 公网上只暴露中继服务器的隧道端口，别人扫端口只能看到协议握手，看不出这是个 MC 服务器
- 中继模式下：因为中继有公网 IP，房主和玩家都是普通 NAT 出站连接，靠中继转发
- **P2P 模式（v1.2.0 新增）**：双方都运行云驿客户端时，通过云驿后端协调互换公网候选端点，双向 UDP 打洞建立直连（ReliableUdpChannel 承载 MC 流量），绕开中继。打洞失败自动回退中继模式

### 3.2 加密方案

- **房主 ↔ 中继**：TLS-PSK（OpenSSL），预共享密钥握手，成熟库实现，不自研加密算法
- **玩家 ↔ 中继**：**明文原始 MC 协议**（这是为了让玩家能直接用 MC 客户端"直接连接"粘贴连接码进入，免装软件的必然代价）
- 因此中继服务器不再是"纯字节转发不解密"的哑管道，而是**协议边界转换点**：一头收玩家明文，另一头通过 TLS 隧道转给房主

### 3.3 并发模型：每局一条独立隧道

- 朋友本地每一次新的 MC 连接，房主端就**新开一条独立 TCP 隧道**服务它，不做多路复用
- **原因**：多路复用会导致队头阻塞（Head-of-Line Blocking）——一个人的流量会拖慢同隧道里其他人的数据，游戏延迟抖动明显；独立隧道虽然多一次握手开销（约1个RTT，几十毫秒，一次性，可忽略），但杜绝了这个问题
- 由 IOCP 重叠 I/O 处理并发

### 3.4 TLS 库与 IOCP 的结合

- TLS 库使用 **OpenSSL**（生态成熟，便于后续扩展证书验证/更复杂密钥协商）
- 通过 **memory BIO** 对接：网络收到的密文塞进内存 BIO，`SSL_read` 从中解出明文；`SSL_write` 写入明文，从 BIO 取出密文再用 `WSASend` 发送。IOCP 只管字节收发，TLS 库只管内存到内存的加解密转换，两者不冲突

### 3.5 多房间支持：独立端口方案

- 长期要支持多个房主/多个房间共用一台中继
- 采用**独立端口方案**：每个房间自动从一个可配置端口池（如 40000–41000）里分配一个当前空闲的端口
- 端口自动分配，用户不用手动指定；UI 需提示端口池配置和"当前池已满"等异常情况
- 房间关闭/房主掉线后，端口释放回池子供复用

### 3.6 连接码

- 格式：`公网IP:自动分配端口`（如 `1.2.3.4:40012`），符合 MC "直接连接"框的输入格式
- 放弃了域名方案，也放弃了"用户自定义标识符"方案（如 `qinnai:端口`，这种字符串必须能被真实 DNS 解析才能连通，否则会连接失败，增加不必要的运维负担）
- **生成流程**：房主点击"创建房间" → 中继自动分配端口并登记房间表 → 拼接 `公网IP:端口` → 点击"复制连接码"写入剪贴板 → 分享给朋友 → 朋友直接粘贴进 MC"直接连接"

### 3.7 控制连接（常驻长连接）

- 房主端软件一启动就与中继建立一条**常驻不断开**的 TCP+TLS 连接，专门用于收发调度指令（不传游戏数据）
- **心跳**：每 20~30 秒发一次心跳帧，除了确认存活，更重要的隐藏作用是**防止家用路由器把长时间无数据往来的 NAT 映射表项悄悄清掉**
- **断线重连**：检测到连接真的断了（心跳连续失败/socket报错），自动带着原房间号重新拨号注册
- **宽限期**：中继服务器给断线房间保留一段宽限期（数十秒）再回收端口，避免正常网络抖动导致房间被误杀

#### 控制连接状态机

```
DISCONNECTED --启动/创建房间--> CONNECTING
CONNECTING --握手成功--> REGISTERING
CONNECTING --握手失败/超时--> DISCONNECTED
REGISTERING --收到REGISTER_ACK--> ACTIVE
REGISTERING --超时无响应--> DISCONNECTED
ACTIVE --定时心跳(自环)--> ACTIVE
ACTIVE --心跳失败/socket错误--> RECONNECTING
RECONNECTING --重连成功(宽限期内)--> ACTIVE
RECONNECTING --重连成功但宽限期已过--> REGISTERING（相当于开新房间，连接码会变）
RECONNECTING --重试N次仍失败--> DISCONNECTED
任意状态 --用户主动关闭房间--> CLOSED --> DISCONNECTED
```

### 3.8 控制帧协议

帧结构（都在 TLS 隧道内，不需要额外加密）：

```
┌──────────┬──────────┬──────────┬─────────────┬──────────────┐
│  1 byte  │  1 byte  │  1 byte  │   4 bytes   │   N bytes    │
│  magic   │ version  │  type    │ payload len │   payload    │
└──────────┴──────────┴──────────┴─────────────┴──────────────┘
```

- `magic`：固定 `0xC5`，基本校验，防止粘包解析错乱
- `version`：协议版本号，当前 `0x01`。房主端和中继服务器版本不一致时，靠这个字节在解析前就能判断兼容性，而不是等解出乱码才发现问题
- `payload len`：大端序

| type | 名称 | 方向 | payload 内容 |
|---|---|---|---|
| 0x01 | REGISTER | 房主→中继 | 房间名(变长字符串) + 本地MC端口(2字节) + 房间ID(4字节,0=新建) |
| 0x02 | REGISTER_ACK | 中继→房主 | 分配到的公网端口(2字节) + 房间ID(4字节) |
| 0x03 | HEARTBEAT | 房主→中继 | 空 |
| 0x04 | HEARTBEAT_ACK | 中继→房主 | 空 |
| 0x05 | OPEN_STREAM | 中继→房主 | 玩家连接编号(4字节) + 隧道端口(2字节) + 标志位(1字节) |
| 0x06 | STREAM_BIND | 房主→中继 | 玩家连接编号(4字节) |
| 0x07 | DEREGISTER | 房主→中继 | 空 |
| 0xFF | ERROR | 中继→房主 | 错误码(1字节) + 错误信息(变长字符串) |

**数据隧道配对 (v1.2.0 双端口方案)**：每房间分配 **两个端口**——玩家端口（明文，连接码）和隧道端口（TLS-PSK）。房主收到含 `tunnelPort` 的 `OPEN_STREAM` 后，新开一条 TLS-PSK 连接连到隧道端口，握手后在隧道上发送 4 字节玩家连接编号，中继据此调用 `pairTunnel()` 配对。配对完成后双向纯字节转发。

---

## 4. 软件架构：Core 平台层 + Framework 引擎层 + App 应用层

### 4.1 分层原则

参考自研游戏引擎（Director/Ref/Scheduler/ActionManager 那套）的分层思路：**引擎不知道任何业务概念，业务逻辑不碰引擎内部**。

```
┌─────────────────────────────────────────────┐
│           WebUI (yunyi.html)                │
│    双角色自适应：relay / host               │
└──────────┬──────────────────────────────────┘
           │ role=relay              role=host │
           ▼                                   ▼
┌──────────────────────┐    ┌──────────────────────────┐
│ HttpApiRouter (relay)│    │ HostAgent HTTP (host)    │
│ • 全部 REST 端点     │    │ • GET /ping, /config     │
│ • RoomRegistry 同步  │    │ • POST /rooms → REGISTER │
└──────────┬───────────┘    └──────────┬───────────────┘
           │                           │
           ▼                           ▼
┌─────────────────────────────────────────────────┐
│  应用层：RelayApp / HostAgentApp / 业务组件      │
│  RoomRegistry(超时清理) ControlChannel(状态机)   │
│  ConnectionCode(连接码)                          │
└──────────────────────┬──────────────────────────┘
                       │  只能调用 Director
                       ▼
┌─────────────────────────────────────────────────┐
│  Director —— 引擎唯一门面(全局单例)              │
│  createRoomRelay() / linkRoomToHost() /          │
│  touchRoomHeartbeat() / findRoomByControlSession()│
└──────────────────────┬──────────────────────────┘
                       │ 内部调度
                       ▼
┌─────────────────────────────────────────────────┐
│ Session / Scheduler / TunnelManager / PortPool   │
│ FrameCodec / FrameDispatcher / AutoreleasePool   │
└──────────────────────┬──────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────┐
│ TransportCore(IOCP+Keepalive) / TlsPskContext    │
│ configureKeepalive(30s idle, 10s interval)       │
└──────────────────────┬──────────────────────────┘
                       ▼
      Windows(IOCP,已实现) / Linux(epoll,预留) / macOS(kqueue,预留)
```

### 4.2 核心约束

**外部代码（App层、HTTP层）只允许调用 `Director`，不可直接访问 `Session`/`Scheduler`/`TunnelManager`/`TransportCore` 等内部模块。**

### 4.3 关键基类对照表（沿用自研引擎命名习惯）

| GameEngine 中的类 | Framework 中对应 | 作用 |
|---|---|---|
| Director | Director | 全局单例，引擎唯一门面 |
| Ref | Ref | 引用计数基类，管理 socket/session 生命周期 |
| Node | Session | 网络会话基类（控制连接、数据隧道的父类） |
| Scheduler | Scheduler | 定时器：心跳、超时检测、重连退避 |
| ActionManager | TunnelManager | 管理当前所有活跃数据隧道 |
| KeyDispatcher | FrameDispatcher | 按帧类型分发处理 |
| AutoreleasePool | AutoreleasePool | 延迟释放 Ref 对象（管理**释放时机**） |
| （新增） | ResourcePool\<T\> | 高频对象复用池：OVERLAPPED上下文/缓冲区/Session对象（管理**是否复用**，避免高频 new/delete） |

> `AutoreleasePool` 与 `ResourcePool` 职责不同、不可互相替代：前者管"什么时候真正销毁"，后者管"要不要重新分配内存"。

---

## 5. 项目目录结构

```
云驿/
├─ core/                             Core.lib — 平台/协议层，纯技术无业务 (可开源)
│  ├─ include/yunyi/core/            头文件 (命名空间 yunyi/core)
│  │  ├─ eventloop/                 INetEventLoop.h, TransportCore.h (IOCP 双栈)
│  │  ├─ tls/                       TlsPskContext.h (OpenSSL TLS-PSK)
│  │  ├─ protocol/                  FrameCodec.h (控制帧编解码)
│  │  ├─ net/                       NetUtil.h (双栈网络工具)
│  │  ├─ pool/                      PortPool.h, ResourcePool.h (端口池/对象池)
│  │  ├─ log/                       Logger.h (日志单例)
│  │  └─ util/                      ConnectionCode.h, StunClient.h
│  └─ src/                          实现 (与 include 同功能域)
│
├─ framework/                       Framework.lib — 引擎风格编程模型 (可开源)
│  ├─ include/yunyi/framework/      头文件 (命名空间 yunyi/framework)
│  │  ├─ engine/                    Director.h, Scheduler.h, FrameDispatcher.h
│  │  ├─ session/                   Session.h, TunnelManager.h
│  │  ├─ channel/                   ControlChannel.h (六状态控制连接状态机)
│  │  ├─ p2p/                       P2PTunnel.h, P2PCoordinator.h
│  │  ├─ lifecycle/                 Ref.h, AutoreleasePool.h
│  │  └─ udp/                       ReliableUdpChannel.h (可靠 UDP 打洞数据面)
│  └─ src/                          实现 (与 include 同功能域)
│
├─ Core.vcxproj / Framework.vcxproj 静态库工程
├─ 云驿.vcxproj                     主 exe (链接 Framework.lib + Core.lib)
│
├─ app/                             业务逻辑层 — 角色逻辑，闭源
│  ├─ common/
│  │  └─ Config.h/.cpp               全局配置 — 公网IP/端口范围/心跳参数
│  ├─ component/
│  │  ├─ RoomRegistry.h/.cpp         房间注册表 + 超时检测 (中继用)
│  │  └─ P2PCoordinatorWinHttp.h/.cpp P2P 协调服务器客户端 (winhttp)
│  ├─ gui/
│  │  └─ main.cpp                    Win32 + WebView2 无边框窗口宿主
│  ├─ relay/
│  │  ├─ RelayApp.h/.cpp             中继服务器主程序 (Core+Framework + HTTP 线程)
│  │  ├─ HttpApiRouter.h/.cpp        REST API 路由 (cpp-httplib)
│  │  └─ webview/
│  │     └─ yunyi.html               WebUI 单文件 SPA
│  ├─ hostagent/
│  │  └─ HostAgentApp.h/.cpp         房主端主程序 (含数据隧道 + P2P 接线)
│  └─ main.cpp                       CLI 入口 (--relay / --host)
│
├─ third_party/                      已集成第三方库
│  ├─ httplib/                       cpp-httplib HTTP 服务器
│  ├─ nlohmann_json/                 nlohmann/json 序列化
│  └─ openssl/                       OpenSSL 3.x (include + lib)
│
├─ webview2-sdk/                     Microsoft Edge WebView2 SDK
│
├─ docs/                             设计文档
│  ├─ Document.md                    架构设计文档 (当前文件)
│  ├─ protocol.md                    控制帧协议规范
│  ├─ rest-api.md                    REST API 手册 (前后端通信契约)
│  ├─ director-api.md                Director 公开接口手册
│  └─ CHANGELOG.md                   开发日志 + 测试报告
│
└─ SVG/                              蓝图 / 架构图
   ├─ 引擎架构图.html                项目结构骨架
   ├─ 引擎接口蓝图.html              模块接口 + 调用关系
   └─ 前端接口蓝图.html              WebUI ↔ REST API
```

---

## 6. 数据流（v1.2.0 双端口方案）

### 6.1 完整流程（v1.2.0 统一事件流）

**C端（房主通过 WebUI 创建房间）— 统一路径：**

1. 房主运行 HostAgentApp → 通过 `ControlChannel` TLS-PSK 连接中继控制端口
2. 房主在 WebUI 点击"创建房间" → **POST 本地 127.0.0.1/api/v1/rooms**
3. HostAgent HTTP 调用 `ControlChannel::registerRoomAsync()` → 发送 REGISTER 帧
4. 中继收到 REGISTER → `createRoomRelay()` 分配 playerPort + tunnelPort → `linkCurrentSessionToHost()` 绑定控制会话 → `RoomRegistry.addRoom()` 登记超时追踪
5. 中继发送 REGISTER_ACK {roomId, assignedPort}
6. HostAgent 收到 ACK → promise resolved → **HTTP 201 返回**（此时房间已完整就绪）
7. 房主 WebUI 显示连接码 `公网IP:playerPort`

**S端（中继操作员手动创建房间）：**
1. 中继操作员在 WebUI 点击"创建房间" → POST 本地 127.0.0.1/api/v1/rooms
2. 中继 HttpApiRouter 调用 `createRoomRelay()` 分配端口 → `RoomRegistry.addRoom()`
3. 返回 201（房间创建但 hostConnected=false，等待房主后续通过 REGISTER 认领）

**玩家接入流程（不变）：**
1. 玩家 MC 客户端连接 `公网IP:playerPort`（明文）
2. 中继 `onPlayerAccepted`：创建 player Session → pending tunnel → 发送 `OPEN_STREAM`（含 tunnelPort）
3. 房主收到 `OPEN_STREAM` → TLS-PSK 连接 `公网IP:tunnelPort` → 隧道握手 → 发送 playerConnId → 连接本地 MC
4. 中继 `onTunnelAccepted`：TLS 握手 → 识别 playerConnId → `pairTunnel()` → 双向转发开始
5. 玩家 ↔ 中继(明文) ↔ 房主(TLS) ↔ MC(明文)

### 6.2 端口分配

| 端口 | 协议 | 用途 |
|------|------|------|
| `controlPort` (40000) | TLS-PSK | 房主控制连接（常驻） |
| `playerPort` (40001+) | 明文 TCP | 玩家 MC 连接（连接码端口） |
| `tunnelPort` (40002+) | TLS-PSK | 房主数据隧道（每玩家一条） |
| `httpPort` (8080) | HTTP | WebUI + REST API |

### 6.3 accept 线程安全（event queue）

v0.3.x 的 IOCP accept 回调直接操作 `DirectorImpl`/`TransportCore` 导致 segfault。v1.2.0 采用 **accept event queue** 架构：IOCP 回调仅将 accept 事件入队（mutex 保护），主循环 `onTick() → flushPendingSends() → processAcceptEvents()` 统一处理。

### 6.4 TCP Keepalive（死连接检测）

所有 IOCP 绑定 socket 自动配置 TCP keepalive：

| 参数 | 值 | 说明 |
|------|-----|------|
| `SO_KEEPALIVE` | 1 | 启用 keepalive 探测 |
| `keepalivetime` | 30,000ms | 空闲 30 秒后开始探测 |
| `keepaliveinterval` | 10,000ms | 探测间隔 10 秒 |

在 `TransportCore::bindToIocp()` 中调用 `configureKeepalive()`，覆盖所有 IOCP socket（listen/accept/connect）。与心跳（25s 间隔 + 75s 超时）互补形成双层死连接检测。

### 6.5 RoomRegistry 超时清理

房间创建时同时登记到 `DirectorImpl::rooms`（引擎层）和 `RoomRegistry`（应用层）：

```
createRoomRelay() → DirectorImpl::rooms[id]
                  → RoomRegistry.addRoom()   ← v1.2.0 激活

HEARTBEAT 帧    → Director.touchRoomHeartbeat()
                  → RoomRegistry.updateHeartbeat()  ← v1.2.0 激活

onTick()        → checkStaleRooms()
                  → RoomRegistry.findTimeoutRooms()
                  → Director.forceCloseRoom()  ← 现在有数据！
```

心跳超时阈值 = `roomGracePeriodMs`（默认 45s），超时后端口自动回收。

### 6.6 P2P 直连（NAT 打洞，v1.2.0）

两个无公网用户通过云驿后端协调互换候选端点后双向 UDP 打洞，打通后 MC 流量直连：

```
[房主云驿]                           [玩家云驿]
MC服务器 ◀─TCP─ ReliableUdpChannel ──UDP打洞──▶ ReliableUdpChannel ──TCP──▶ MC客户端
                  ▲                                       ▲
                  └───── 云驿后端协调 ──────────────────────┘
                  STUN(3478) 回显公网映射 + holepunch(2885) 互换候选
```

**关键组件**：
- `TransportCore`：UDP socket（WSARecvFrom/WSASendTo + IOCP）
- `ReliableUdpChannel`：可靠字节流（分片/seq/ack/重传），对上层伪装成 Session
- `StunClient`：RFC 8489 子集，获取公网映射端点
- `P2PTunnel`：状态机 `Idle → Gathering → Registering → Punching → Connected/Failed`，打通后转发本地 MC TCP
- `HostAgentApp`：HTTP 端点 `/api/v1/p2p/{start,stop,status}`

**协调**：云驿后端（mc.qinnai.xyz）STUN 回显 + `POST /api/v1/holepunch/register` 上报候选 + `GET /api/v1/holepunch/peers/{roomId}` 互换。

**降级**：打洞失败（对称 NAT 等）自动回退中继 TCP 转发路径，不影响现有功能。

---

## 7. 命名

项目定名 **云驿（Yún Yì）**——"驿"取古代驿站中转传递之意，"云"点出网络属性，呼应中继服务器"帮两边本来连不上的人搭一座桥"的核心功能。

---

## 8. 待细化事项（TODO）

- [x] `Director` 具体对外暴露的公开方法签名 → 见 `docs/director-api.md`
- [x] webview 与后端 REST API 的完整契约 → 见 `docs/rest-api.md`
- [x] 控制帧协议版本字节 + 错误码枚举 → 见 `docs/protocol.md`
- [x] `Session`/`Ref` 生命周期实现 → Doxygen 注释已完成
- [x] `ResourcePool<T>` 的取用/归还/池满处理 → Doxygen 注释已完成
- [x] TLS-PSK 加密 → OpenSSL 3.x 已集成，TLS 1.3 握手已验证
- [x] 双端口隧道架构 → v1.2.0 每房间 playerPort + tunnelPort
- [x] 心跳 + 断线重连 → ControlChannel 状态机已实现
- [x] GUI 桌面客户端 → WebView2 无边框窗口
- [x] 统一房间创建入口 → HostAgent WebUI 通过 ControlChannel REGISTER 创建
- [x] RoomRegistry 超时清理 → 心跳同步 + checkStaleRooms 激活
- [x] TCP Keepalive → TransportCore::configureKeepalive 30s/10s
- [x] 房主地址追踪 → RoomInfo::hostAddress
- [x] 双角色 WebUI → relay/host 自适应
- [x] 房间宽限期具体时长（45s）、触发条件细节
- [x] 中继服务器公网 IP 探测/配置方式（自动检测 + WebUI POST /config 手动修改）
- [x] 日志系统（分级、输出目标、HTTP API 暴露）
- [x] 端口池耗尽 UI 提示和重试引导
- [x] NAT 打洞 P2P 直连 → UDP 打洞 + ReliableUdpChannel + 云驿后端协调
- [ ] tunnel accept TLS 握手偶发崩溃（当前 accept event queue 兜底）
- [ ] 房主认领已有房间（ControlChannel registerRoom roomId 支持）