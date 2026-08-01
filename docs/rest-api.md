# 云驿 REST API 手册 v1.2.0

> 本文档定义中继服务器（RelayServer）对 webview 前端暴露的 HTTP/JSON 接口。
> 所有接口均以 `/api/v1/` 为前缀，便于未来版本并存过渡。

---

## 1. 通用约定

### 1.1 Base URL

```
http://{中继服务器IP}:{HTTP端口}/api/v1
```

HTTP 端口由配置决定（默认建议 `8080`），中继服务器监听 `0.0.0.0`（接受本机和外网请求），房主端仅监听 `127.0.0.1`。

### 1.2 双角色 API

云驿提供两套 HTTP API，由 `role` 字段区分：

| role | 角色 | 监听地址 | 可用端点 |
|------|------|---------|---------|
| `"relay"` | 中继服务器 | `0.0.0.0:8080` | 全部端点（rooms/stats/config/logs/ping） |
| `"host"` | 房主端 | `127.0.0.1:8080` | ping/config/rooms（通过 ControlChannel 代理） |

房主端的 `POST /api/v1/rooms` 通过 ControlChannel 发送 REGISTER 帧到中继，**等待 REGISTER_ACK 后才返回**，确保返回时房间已完整就绪。

### 1.2 请求格式

- `Content-Type: application/json`
- 请求体为 JSON（除 `GET` 请求外）
- 字符编码：UTF-8

### 1.3 成功响应

HTTP 状态码在 2xx 范围内，响应体为 JSON。

### 1.4 错误响应

统一格式：

```json
{
  "error": {
    "code": "ROOM_NOT_FOUND",
    "message": "可读的错误描述"
  }
}
```

HTTP 状态码规则：

| 状态码 | 场景 |
|--------|------|
| `200` | 成功 |
| `201` | 创建成功 |
| `400` | 请求参数不合法 |
| `404` | 资源不存在 |
| `409` | 资源冲突（如端口池已满） |
| `500` | 服务器内部错误 |

---

## 2. 端点清单

---

### 2.1 创建房间

```
POST /api/v1/rooms
```

**请求体**：

```json
{
  "roomName": " string, 1–64 字符，可选，不传则自动生成 "
}
```

**成功响应** `201 Created`：

```json
{
  "roomId": 1,
  "roomName": "我的世界",
  "connectionCode": "1.2.3.4:40012",
  "assignedPort": 40012,
  "localMcPort": 25565,
  "status": "waiting",
  "playerCount": 0,
  "hostAddress": "203.0.113.5:54321",
  "createdAt": "2026-07-17T12:00:00Z"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `roomId` | uint32 | 房间唯一 ID |
| `roomName` | string | 房间名 |
| `connectionCode` | string | 连接码，可直接粘贴到 MC"直接连接"框 |
| `assignedPort` | uint16 | 中继分配的公网端口 |
| `localMcPort` | uint16 | 房主本地 MC 端口 |
| `status` | string | `"waiting"` 等待房主 / `"active"` 有房主在线 / `"closed"` 已关闭 |
| `hostAddress` | string | 房主控制连接的远端地址（仅中继端有效，手动创建为空） |
| `createdAt` | string | ISO 8601 创建时间 |

**错误响应**：

| code | HTTP | 场景 |
|------|------|------|
| `PORT_POOL_EXHAUSTED` | `409` | 端口池已满 |
| `ALREADY_REGISTERED` | `409` | 该控制连接已注册房间 |
| `INTERNAL_ERROR` | `500` | 内部错误 |

---

### 2.2 查询房间状态

```
GET /api/v1/rooms/{roomId}
```

**路径参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `roomId` | uint32 | 房间 ID |

**成功响应** `200 OK`：

```json
{
  "roomId": 1,
  "roomName": "我的世界",
  "connectionCode": "1.2.3.4:40012",
  "assignedPort": 40012,
  "localMcPort": 25565,
  "status": "active",
  "playerCount": 3,
  "createdAt": "2026-07-17T12:00:00Z",
  "players": [
    {
      "id": 1,
      "connectedAt": "2026-07-17T12:05:00Z",
      "bytesSent": 123456,
      "bytesRecv": 654321
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `playerCount` | uint32 | 当前玩家数 |
| `players` | array | 玩家列表（含流量统计） |

**错误响应**：

| code | HTTP | 场景 |
|------|------|------|
| `ROOM_NOT_FOUND` | `404` | 房间 ID 不存在 |

---

---
### 2.3 查询房间玩家列表

```
GET /api/v1/rooms/{roomId}/players
```

**路径参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `roomId` | uint32 | 房间 ID |

**成功响应** `200 OK`：

```json
{
  "roomId": 1,
  "players": [
    {
      "id": 1,
      "connectedAt": "2026-07-17T12:05:00Z",
      "bytesSent": 123456,
      "bytesRecv": 654321
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `roomId` | uint32 | 所属房间 ID |
| `players` | array | 当前在线玩家列表 |
| `players[].id` | uint32 | 玩家连接编号（中继自增） |
| `players[].connectedAt` | string | ISO 8601 连接时间 |
| `players[].bytesSent` | uint64 | 该玩家已发送字节数 |
| `players[].bytesRecv` | uint64 | 该玩家已接收字节数 |

**错误响应**：

| code | HTTP | 场景 |
|------|------|------|
| `ROOM_NOT_FOUND` | `404` | 房间 ID 不存在 |

> **前端用途**：打开房间详情页时调用此接口获取实时玩家列表和流量统计。
> 每个玩家对应一条数据隧道（`id` → `T{id}`），隧道方向为"玩家 → 中继 ↔ 房主 → MC"。

---

### 2.4 关闭房间

```
DELETE /api/v1/rooms/{roomId}
```

**成功响应** `200 OK`：

```json
{
  "roomId": 1,
  "status": "closed",
  "closedAt": "2026-07-17T13:00:00Z"
}
```

关闭后端口立即回收，该房间的连接码失效。

**错误响应**：

| code | HTTP | 场景 |
|------|------|------|
| `ROOM_NOT_FOUND` | `404` | 房间 ID 不存在或已关闭 |

---

### 2.5 列出所有房间

```
GET /api/v1/rooms
```

**成功响应** `200 OK`：

```json
{
  "rooms": [
    {
      "roomId": 1,
      "roomName": "我的世界",
      "connectionCode": "1.2.3.4:40012",
      "status": "active",
      "playerCount": 3,
      "createdAt": "2026-07-17T12:00:00Z"
    }
  ],
  "total": 1
}
```

> 已关闭的房间不在列表中。

---

### 2.6 服务器统计

```
GET /api/v1/stats
```

**成功响应** `200 OK`：

```json
{
  "uptimeSeconds": 3600,
  "activeRooms": 3,
  "totalRooms": 10,
  "activeTunnels": 12,
  "totalConnections": 25,
  "portPoolUsed": 3,
  "portPoolTotal": 1001,
  "bytesRelayed": 1234567890,
  "version": "v1.2.0",
  "protocolVersion": 1
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `uptimeSeconds` | uint64 | 服务运行秒数 |
| `activeRooms` | uint32 | 当前活跃房间数 |
| `totalRooms` | uint32 | 历史累计房间数（含已关闭） |
| `activeTunnels` | uint32 | 当前活跃数据隧道数 |
| `totalConnections` | uint32 | 历史累计连接数 |
| `portPoolUsed` | uint32 | 端口池已用数量 |
| `portPoolTotal` | uint32 | 端口池总容量 |
| `bytesRelayed` | uint64 | 累计转发字节数 |
| `version` | string | 中继服务器软件版本 |
| `protocolVersion` | uint32 | 控制帧协议版本 |

---

### 2.7 查看配置

```
GET /api/v1/config
```

**成功响应** `200 OK`：

```json
{
  "role": "relay",
  "publicIp": "1.2.3.4",
  "portPoolStart": 40000,
  "portPoolEnd": 41000,
  "controlPort": 40000,
  "httpPort": 8080,
  "heartbeatIntervalMs": 25000,
  "roomGracePeriodMs": 45000,
  "maxPlayersPerRoom": 10
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `role` | string | `"relay"`（中继端）或 `"host"`（房主端），前端据此切换 UI 模式 |
| `controlPort` | uint16 | 控制连接端口（房主 TLS-PSK 连接端口） |

> 该接口为只读；公网 IP 可通过下述 `POST /api/v1/config` 动态更新。

**更新公网 IP** `POST /api/v1/config`：

```json
// 请求体
{ "publicIp": "2408:826c:xxxx:xxxx::1" }
```

**成功响应** `200 OK`：

```json
{ "publicIp": "2408:826c:xxxx:xxxx::1", "status": "ok" }
```

---

### 2.8 服务器握手

```
GET /api/v1/ping
```

**成功响应** `200 OK`：

```json
{
  "status": "ok",
  "role": "relay",
  "version": "v1.2.0",
  "protocol": 1,
  "serverIp": "1.2.3.4",
  "serverTime": "2026-07-17T12:00:00Z"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `role` | string | `"relay"` 或 `"host"` |
| `connectionState` | int | **仅 host 角色**：ControlChannel 连接状态码 |

> 前端初始化时通过 `role` 字段确定当前角色，切换 UI 布局和 API 调用策略。

---

### 2.9 P2P 直连端点（房主端 8081）

无中继 NAT 打洞直连（双方都运行云驿，云驿后端协调）。房主端仅监听 `127.0.0.1`。

**启动打洞直连** `POST /api/v1/p2p/start`：

```json
// 请求体
{ "roomId": "abc123", "isHost": true, "localIp": "127.0.0.1", "localMcPort": 25565 }
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `roomId` | string | 房间码，双方需一致 |
| `isHost` | bool | true = 房主（转发到本地 MC 服务器），false = 玩家 |
| `localIp` | string | 本地 MC 地址（默认 127.0.0.1） |
| `localMcPort` | uint16 | 本地 MC 端口（默认 25565） |

**成功响应** `200 OK`：
```json
{ "success": true, "candidateIp": "2408:abcd::1", "candidatePort": 34782 }
```

**停止直连** `POST /api/v1/p2p/stop`：
```json
{ "success": true }
```

**查询状态** `GET /api/v1/p2p/status`：
```json
{ "state": "punching", "candidateIp": "2408:abcd::1", "candidatePort": 34782 }
```

| `state` 值 | 说明 |
|-----------|------|
| `idle` | 未开始 |
| `gathering` | STUN 获取公网候选端点 |
| `registering` | 上报候选到云驿后端 |
| `punching` | 双向 UDP 打洞中 |
| `connected` | 直连建立，数据流转 |
| `failed` | 打洞失败（NAT 不支持或对端未加入） |

---

### 2.8 错误码速查

| code | HTTP | 说明 |
|------|------|------|
| `ROOM_NOT_FOUND` | 404 | 房间不存在 |
| `PORT_POOL_EXHAUSTED` | 409 | 端口池已满 |
| `ALREADY_REGISTERED` | 409 | 已注册房间 |
| `INVALID_PARAMS` | 400 | 请求参数不合法 |
| `INTERNAL_ERROR` | 500 | 服务器内部错误 |

---

## 3. 前端轮询策略

> 前端 yunyi.html 通过定时轮询与后端保持数据同步，不依赖 WebSocket。

### 3.1 轮询配置

| 参数 | 值 | 说明 |
|------|-----|------|
| 轮询间隔 | **5 秒** | `setInterval(5000)` |
| 上行时间更新 | **1 秒** | 仅客户端本地 `getUptimeSeconds()` 计算，不请求后端 |
| 初始加载 | 页面打开时立即执行一次 `syncAll()` | 并行调用 stats + rooms + config |
| 视图切换 | 切换到房间管理/中继视图时触发 `syncAll()` | 确保数据最新 |

### 3.2 syncAll() 并行请求

```
syncAll():
  ├─ GET /api/v1/stats   → 更新 engineStats
  ├─ GET /api/v1/rooms   → 更新 rooms Map
  └─ GET /api/v1/config  → 更新 CONFIG + 设置页
```

每次 5s 轮询同时发出 3 个 GET 请求（`Promise.all`），任一失败不影响其余。

### 3.3 房间详情按需加载

打开房间详情时额外调用 `GET /api/v1/rooms/{id}/players`，获取实时玩家列表。
5s 轮询时如果详情页可见，同步刷新玩家数据。

---

## 4. 前端数据层映射

> 以下定义 yunyi.html 前端 JavaScript 变量与后端 JSON 字段的一一对应关系。

### 4.1 engineStats（来源：GET /api/v1/stats）

| 前端变量 | JSON 字段 | 类型 | 默认值 |
|----------|-----------|------|--------|
| `engineStats.uptimeSeconds` | `uptimeSeconds` | uint64 | `0` |
| `engineStats.activeRooms` | `activeRooms` | uint32 | `0` |
| `engineStats.totalRooms` | `totalRooms` | uint32 | `0` |
| `engineStats.activeTunnels` | `activeTunnels` | uint32 | `0` |
| `engineStats.totalConnections` | `totalConnections` | uint32 | `0` |
| `engineStats.portPoolUsed` | `portPoolUsed` | uint32 | `0` |
| `engineStats.portPoolTotal` | `portPoolTotal` | uint32 | `1001` |
| `engineStats.bytesRelayed` | `bytesRelayed` | uint64 | `0` |

### 4.2 rooms Map（来源：GET /api/v1/rooms）

| 前端字段 | JSON 字段 | 类型 | 说明 |
|----------|-----------|------|------|
| `room.roomId` | `roomId` | uint32 | 房间唯一 ID |
| `room.roomName` | `roomName` | string | 房间名称 |
| `room.connectionCode` | `connectionCode` | string | `IP:端口` 格式 |
| `room.assignedPort` | `assignedPort` | uint16 | 中继分配端口 |
| `room.localMcPort` | `localMcPort` | uint16 | MC 本地端口 |
| `room.status` | `status` | string | `"waiting"` / `"active"` / `"closed"` |
| `room.playerCount` | `playerCount` | uint32 | 当前玩家数 |
| `room.createdAt` | `createdAt` | string | ISO 8601 |
| `room.hostAddress` | `hostAddress` | string | 房主远端地址 |
| `room.relayIp` | *(客户端计算)* | string | 中继 IP，来自 CONFIG 或用户输入 |

### 4.3 房间玩家（来源：GET /api/v1/rooms/{id}/players）

| 前端字段 | JSON 字段 | 类型 |
|----------|-----------|------|
| `player.id` | `id` | uint32 |
| `player.connectedAt` | `connectedAt` | string |
| `player.bytesSent` | `bytesSent` | uint64 |
| `player.bytesRecv` | `bytesRecv` | uint64 |

> 前端为每个玩家生成对应的隧道对象：`{ id: "T{playerId}", playerId, direction, bytesForwarded }`

### 4.4 CONFIG（来源：GET /api/v1/config）

| 前端字段 | JSON 字段 | 类型 | 默认值 |
|----------|-----------|------|--------|
| `CONFIG.role` | `role` | string | `""` |
| `CONFIG.publicIp` | `publicIp` | string | `""` |
| `CONFIG.portPoolStart` | `portPoolStart` | uint16 | `40000` |
| `CONFIG.portPoolEnd` | `portPoolEnd` | uint16 | `41000` |
| `CONFIG.controlPort` | `controlPort` | uint16 | `40000` |
| `CONFIG.httpPort` | `httpPort` | uint16 | `8080` |
| `CONFIG.heartbeatIntervalMs` | `heartbeatIntervalMs` | uint32 | `25000` |
| `CONFIG.roomGracePeriodMs` | `roomGracePeriodMs` | uint32 | `45000` |
| `CONFIG.maxPlayersPerRoom` | `maxPlayersPerRoom` | uint32 | `10` |

### 4.5 房间创建请求体（POST /api/v1/rooms）

```json
{
  "roomName": "string, 1-64 字符",
  "localMcPort": 25565
}
```

> 前端额外字段 `roomIp`（中继服务器 IP）为客户端本地计算，不传给后端。

---

## 5. 前后端契约检查清单

> 开发联调前逐项确认，防止字段名不一致导致数据断裂。

### 后端必须提供

- [ ] `GET /api/v1/stats` 返回全部 8 个字段（见 4.1）
- [ ] `GET /api/v1/rooms` 返回 `{ rooms: [...], total: N }`，每个 room 含全部 7 个字段（见 4.2）
- [ ] `GET /api/v1/rooms/{id}` 返回单个房间完整信息（含 players）
- [ ] `GET /api/v1/rooms/{id}/players` 返回 `{ roomId, players: [...] }`（见 4.3）
- [ ] `POST /api/v1/rooms` 接受 `{ roomName, localMcPort }`，返回 `RoomResult`（含 connectionCode）
- [ ] `DELETE /api/v1/rooms/{id}` 返回 `{ roomId, status: "closed" }`
- [ ] `GET /api/v1/config` 返回全部配置字段（见 4.4）
- [ ] 所有响应 `Content-Type: application/json; charset=utf-8`
- [ ] 错误响应统一格式 `{ error: { code, message } }`（见 1.4）

### 前端必须遵循

- [ ] 所有数据初始值为 `0` / `""` / 空集合，不硬编码 mock 数据
- [ ] `syncAll()` 并行请求，单一失败不阻塞其余
- [ ] 房间操作（创建/关闭）后不等待轮询，立即更新本地状态并 re-render
- [ ] 连接码格式 `{ip}:{port}`，由前端拼接（ip 来自用户输入或 CONFIG）
- [ ] 运行时长 = `engineStats.uptimeSeconds + 本地增量`（1s 定时器本地自增）
