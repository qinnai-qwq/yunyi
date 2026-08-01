# 云驿 Yún Yì

> Minecraft 联机中继工具 —— 让没有公网 IP 的朋友，通过有公网 IP 的中继服务器加入联机房间。
> 玩家无需安装任何软件，直接把连接码粘贴到 MC 客户端的"直接连接"即可。

**当前版本：v1.2.0**

---

## 快速开始（用户）

### 场景：房主没有公网 IP，朋友有公网 IP

**1. 朋友（中继端）**
- 下载 `云驿-v1.1.1.zip`，解压
- 运行 `云驿GUI.exe`
- 中继面板会显示公网 IPv6 地址（例如 `2408:826c:...`）
- 把这个地址发给房主

**2. 房主（你）**
- 确保本地 MC 服务器在 `127.0.0.1:25565` 运行
- 下载同一 zip，解压，运行 `云驿GUI.exe`
- 点击左侧"房间" → 在创建房间卡片里**填入朋友的中继 IP** → 创建房间
- 创建成功后把连接码（如 `[2408:826c:...]:40001`）发给玩家

**3. 玩家**
- Minecraft 客户端 → 多人游戏 → 直接连接 → 粘贴连接码
- 无需安装云驿

### 环境要求

- Windows 10/11 x64
- 不需要安装 Visual Studio 或 VC++ Redistributable（静态链接）
- 需要 [WebView2 运行时](https://developer.microsoft.com/microsoft-edge/webview2/)（Win11 已内置，Win10 通常已通过 Edge 安装）

---

## 命令行参数

```
云驿.exe --relay --http-port 8080 --psk <密钥> --public-ip <IP>
云驿.exe --host  --http-port 8081 --psk <密钥> --no-auto-connect
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--relay` | 以中继模式运行 | — |
| `--host` | 以房主模式运行 | — |
| `--public-ip <ip>` | 中继公网 IP | 自动检测 |
| `--psk <key>` | TLS-PSK 预共享密钥 | 必填 |
| `--psk-identity <id>` | PSK 身份标识 | `yunyi` |
| `--control-port <port>` | 控制连接端口 | 40000 |
| `--port-start <port>` | 端口池起始 | 40001 |
| `--port-end <port>` | 端口池结束 | 41000 |
| `--http-port <port>` | HTTP / WebUI 端口 | 8080（中继）/ 8081（房主） |
| `--host-http-port <port>` | 房主 HTTP 端口 | 8081 |
| `--no-auto-connect` | 房主启动时不自动连中继 | false |
| `--local-mc-port <port>` | 房主本地 MC 端口 | 25565 |
| `--max-players <n>` | 每房间最大玩家数 | 10 |
| `--heartbeat-interval <ms>` | 心跳间隔 | 25000 |
| `--heartbeat-timeout <ms>` | 心跳超时 | 75000 |
| `--room-grace <ms>` | 房间宽限期 | 45000 |
| `--log-level <0-5>` | 日志级别 | 3 |
| `--log-path <path>` | 日志输出路径 | stderr |

---

## 架构

### 中继模式（默认）

```
玩家 (MC 客户端) ──TCP──▶ 中继服务器 ◀──TLS 控制信道──▶ 房主端 ◀──TCP──▶ 本地 MC 服务
       │                   │    (端口 40000)          │              (127.0.0.1:25565)
       │                   │                          │
       └──端口 X (原始TCP)──▶◀──TLS 数据隧道──▶◀──────────┘
                             (端口 Y)
```

### P2P 直连模式（NAT 打洞，无中继）

```
[房主云驿]                           [玩家云驿]
MC服务器 ◀─TCP─ ReliableUdpChannel ──UDP打洞──▶ ReliableUdpChannel ──TCP──▶ MC客户端
                  ▲                                        ▲
                  └──── 云驿后端协调 (STUN + holepunch) ────┘
                      mc.qinnai.xyz:3478(UD) + :2885(HTTP)
```

双方都运行云驿，通过「P2P 直连」页面互换公网候选端点后双向 UDP 打洞，打通后 MC 流量直连、绕开中继。打洞失败可回退中继模式。

**核心约束：** 外部代码只允许调用 `Director`（引擎唯一门面），不可直接访问引擎内部模块。

### 技术栈

- **语言：** C++17
- **网络：** Windows IOCP（AcceptEx / ConnectEx / WSARecv / WSASend）+ UDP（WSARecvFrom / WSASendTo）
- **打洞：** STUN（RFC 8489 子集）+ ReliableUdpChannel（seq/ack/重传可靠 UDP）
- **加密：** OpenSSL TLS-PSK（memory BIO 模式）
- **HTTP：** cpp-httplib + nlohmann/json（header-only）
- **GUI：** WebView2 原生窗口 + 单文件 SPA
- **分发：** 静态链接 CRT（`/MT`），仅依赖系统 DLL + OpenSSL

---

## 项目结构

```
云驿/
├── NetEngine/                   引擎层（静态库，不含业务逻辑）
│   ├── Director.h/.cpp          引擎唯一门面（全局单例）
│   ├── TransportCore.h/.cpp     Windows IOCP 异步 I/O
│   ├── Session.h/.cpp           会话基类
│   ├── TlsPskContext.h/.cpp     OpenSSL TLS-PSK 封装（memory BIO）
│   ├── FrameCodec.h/.cpp        控制帧编解码
│   ├── FrameDispatcher.h/.cpp   帧分发器
│   ├── TunnelManager.h/.cpp     数据隧道管理
│   ├── PortPool.h/.cpp          端口池分配/回收
│   ├── ReliableUdpChannel.h/.cpp 可靠 UDP 通道（NAT 打洞数据面）
│   ├── Scheduler.h/.cpp         定时器（心跳/超时/重连退避）
│   ├── Ref.h/.cpp               引用计数基类
│   ├── AutoreleasePool.h/.cpp   自动释放池
│   ├── ResourcePool.h           高频对象复用池（模板）
│   └── NetUtil.h/.cpp           网络工具函数
│
├── app/                         业务层
│   ├── common/
│   │   └── Config.h/.cpp        全局配置 + CLI 参数解析
│   ├── relay/
│   │   ├── RelayApp.h/.cpp      中继服务器主程序
│   │   ├── HttpApiRouter.h/.cpp REST API 路由
│   │   └── webview/yunyi.html   WebUI 单文件 SPA
│   ├── hostagent/
│   │   └── HostAgentApp.h/.cpp  房主端主程序（含 P2P 直连端点）
│   ├── component/
│   │   ├── ControlChannel.h/.cpp 控制连接状态机
│   │   ├── RoomRegistry.h/.cpp  房间注册表
│   │   ├── ConnectionCode.h/.cpp 连接码生成/解析
│   │   ├── StunClient.h/.cpp    STUN 客户端（获取公网映射）
│   │   └── P2PTunnel.h/.cpp     P2P 打洞隧道（无中继直连）
│   └── gui/
│       └── main.cpp             WebView2 原生窗口宿主
│
├── docs/
│   ├── protocol.md              控制帧协议规范
│   ├── rest-api.md              REST API 手册
│   ├── director-api.md          Director 公开接口手册
│   └── CHANGELOG.md             开发日志
│
├── tools/                       测试工具
├── dist/                        分发目录
│   ├── pkg/                     可部署文件
│   └── 云驿-v1.1.1.zip  发布包
│
├── 云驿.vcxproj                 后端项目文件
└── 云驿GUI.vcxproj              GUI 项目文件
```

---

## REST API

> 中继监听 `8080`，房主监听 `8081`。所有 API 前缀 `/api/v1`。

### 中继（8080）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | WebUI 页面 |
| GET | `/TROUBLESHOOTING.md` | 问题解决文档（markdown 预览） |
| GET | `/api/v1/ping` | 健康检查，返回 `{"role":"relay"}` |
| GET | `/api/v1/stats` | 引擎统计（房间数、隧道数、端口池使用率） |
| GET | `/api/v1/rooms` | 活跃房间列表 |
| POST | `/api/v1/rooms` | 创建房间 `{"roomName":"mc","localMcPort":25565}` |
| GET | `/api/v1/rooms/:id` | 单个房间详情 |
| GET | `/api/v1/rooms/:id/players` | 房间内玩家列表 |
| DELETE | `/api/v1/rooms/:id` | 强制关闭房间 |
| GET | `/api/v1/config` | 当前配置 |
| POST | `/api/v1/config` | 更新公网 IP `{"publicIp":"1.2.3.4"}` |
| GET | `/api/v1/logs` | 后端日志（轮询） |

### 房主（8081）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/ping` | 健康检查，返回 `{"role":"host"}` |
| GET | `/api/v1/host/status` | 连接状态 |
| POST | `/api/v1/host/connect` | 连接远程中继 `{"relayIp":"...","controlPort":40000}` |
| POST | `/api/v1/host/disconnect` | 断开中继 |
| POST | `/api/v1/rooms` | 在中继上创建房间 `{"roomName":"mc","localMcPort":25565}` |
| GET | `/api/v1/rooms` | 本地房间列表 |
| GET | `/api/v1/config` | 当前配置 |
| POST | `/api/v1/p2p/start` | 启动 P2P 打洞直连 `{"roomId":"abc","isHost":true,"localMcPort":25565}` |
| POST | `/api/v1/p2p/stop` | 停止 P2P 直连 |
| GET | `/api/v1/p2p/status` | P2P 状态（idle/gathering/registering/punching/connected/failed） |

---

## 控制帧协议

房主端与中继之间通过 TLS-PSK 加密的控制信道（端口 40000）通信，帧格式：

```
[1B magic] [1B type] [1B version] [1B reserved] [4B payload_len] [N bytes payload]
```

帧类型：`REGISTER` / `REGISTER_ACK` / `HEARTBEAT` / `HEARTBEAT_ACK` / `DEREGISTER` / `OPEN_STREAM` / `STREAM_BIND` / `ERROR`

详见 `docs/protocol.md`。

---

## 编译

```powershell
# 中继 + 房主后端
MSBuild 云驿.vcxproj /p:Configuration=Release /p:Platform=x64

# GUI 桌面客户端
MSBuild 云驿GUI.vcxproj /p:Configuration=Release /p:Platform=x64
```

要求：Visual Studio 2022（v143），Windows SDK 10.0，C++17，OpenSSL 头文件和库放在 `third_party/openssl/`。

Release 配置使用 `/MT`（静态链接 CRT），分发的 exe 仅依赖：
- `libssl-4-x64.dll`、`libcrypto-4-x64.dll`（OpenSSL，在发布包中）
- `WebView2Loader.dll`（GUI 需要，在发布包中）
- `KERNEL32.dll`、`WS2_32.dll`（Windows 系统自带）

---

## 开发路线

- [x] 项目结构 + 架构设计
- [x] NetEngine 全部模块
- [x] TLS-PSK 加密控制信道
- [x] 控制帧协议（REGISTER / HEARTBEAT / OPEN_STREAM 等）
- [x] 双端口隧道架构（玩家 → 中继 → TLS 隧道 → 房主 → MC）
- [x] WebUI 单文件 SPA
- [x] WebView2 桌面 GUI
- [x] 中继 + 房主双模式 GUI
- [x] 房间心跳 + 超时清理
- [x] 连接码生成（IPv6 地址格式）
- [x] 流量统计 + WebUI 实时展示（转发流量、端口池占用）
- [x] 多玩家同房间支持（上限 10 人）
- [x] 主页设备信息卡片（硬件配置 + 实时占用率）
- [x] 问题解决文档 + WebUI 预览（内置 markdown 渲染 + 命令复制）
- [x] 主题自定义（透明度 / 模糊 / 背景图 / 背景色，持久化保存）
- [x] NAT 打洞 P2P 直连（UDP 打洞 + 可靠传输，无中继）
- [x] P2P 协调服务（云驿后端 STUN + holepunch）
- [ ] 玩家断线检测 + 自动清理
- [ ] Linux epoll 后端（预留）

---

## 命名

"驿"取古代驿站中转传递之意，"云"点出网络属性，呼应中继服务器"帮两边本来连不上的人搭一座桥"的核心功能。
