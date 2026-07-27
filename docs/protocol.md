# 云驿 控制帧协议规范 v1.0.1

> 本文档定义房主端（HostAgent）与中继服务器（RelayServer）之间控制连接的二进制帧协议。
> 控制连接是房主端启动后与中继建立的**第一条常驻 TLS-PSK 长连接**，仅传输信令，不承载游戏数据。
> 游戏数据走独立的数据隧道（见 Document.md §3.3）。

---

## 1. 帧格式

```
┌────┬────┬────┬─────┬─────┐
│  1 byte  │  1 byte  │  1 byte  │   4 bytes   │   N bytes    │
│  magic   │ version  │  type    │payload len│   payload   │
│  0xC5    │  0x01    │               │  (big-endian)│               │
└────┴────┴────┴─────┴─────┘
```

| 字段 | 大小 | 说明 |
|------|------|------|
| `magic` | 1 byte | 固定值 `0xC5`，用于快速校验帧边界，防止粘包/半包解析错乱 |
| `version` | 1 byte | 协议版本号，当前为 `0x01`。版本不匹配时对方应回复 `ERROR` 帧并关闭连接 |
| `type` | 1 byte | 帧类型，编码空间见 §2 |
| `payload_len` | 4 bytes | payload 字节数，**大端序（network byte order）** |
| `payload` | N bytes | 载荷，结构与各帧类型绑定，见 §3 |

**帧总长度**：`7 + payload_len` 字节。

**解析规则**：
1. 从 TCP 流中读 7 字节头部
2. 校验 `magic == 0xC5`，不匹配则断开连接
3. 校验 `version`，不匹配则发送 `ERROR(ERR_PROTOCOL_VERSION_MISMATCH)` 后断开
4. 读 `type`，确认是否为已知类型
5. 按 `payload_len` 读取 payload
6. 按 type 解析 payload

---

## 2. type 编码空间

```
0x01 ─ 0x0F  │  核心控制帧（当前使用）
0x10 ─ 0x1F  │  【预留】未来扩展（房间迁移、多中继协调、NAT 类型探测…）
0x20 ─ 0xEF  │  未分配
0xF0 ─ 0xFE  │  调试/诊断帧（如 PING、TRACE、DUMP_STATS）
0xFF         │  ERROR（错误响应帧）
```

**约束**：新增帧类型不得占用 `0x10–0x1F` 和 `0xF0–0xFF`，除非是预留类别中的功能。

---

## 3. 帧类型定义

### 3.1 REGISTER（0x01）

| 属性 | 值 |
|------|-----|
| 方向 | 房主 → 中继 |
| 触发 | 控制连接 TLS 握手完成后，房主主动发送 |
| 用途 | 向中继注册一个房间，申请端口分配 |

**payload 结构**：

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 变长 | `room_name` | UTF-8 字符串，长度 = `payload_len - 2` |
| … | 2 bytes | `local_mc_port` | 房主本地 MC 服务端口，大端序 |

---

### 3.2 REGISTER_ACK（0x02）

| 属性 | 值 |
|------|-----|
| 方向 | 中继 → 房主 |
| 触发 | 中继收到 REGISTER 并成功分配端口后 |
| 用途 | 告知房主分配结果 |

**payload 结构**：

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 bytes | `assigned_port` | 分配到的公网端口，大端序 |
| 2 | 4 bytes | `room_id` | 房间唯一 ID（中继分配），大端序 |

---

### 3.3 HEARTBEAT（0x03）

| 属性 | 值 |
|------|-----|
| 方向 | 房主 → 中继 |
| 触发 | 定时发送，间隔 20–30 秒 |
| 用途 | 保活 + 防止 NAT 映射表项被路由器清除 |

**payload**：空（`payload_len = 0`）

---

### 3.4 HEARTBEAT_ACK（0x04）

| 属性 | 值 |
|------|-----|
| 方向 | 中继 → 房主 |
| 触发 | 收到 HEARTBEAT 后立即回复 |
| 用途 | 确认心跳 |

**payload**：空（`payload_len = 0`）

> **超时规则**：房主连续 3 次未收到 HEARTBEAT_ACK（约 60–90 秒）视为连接断开，进入重连流程。

---

### 3.5 OPEN_STREAM（0x05）

| 属性 | 值 |
|------|-----|
| 方向 | 中继 → 房主 |
| 触发 | 玩家通过连接码连接中继后 |
| 用途 | 通知房主有新玩家接入，需新开一条加密数据隧道 |

**payload 结构**：

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 bytes | `player_conn_id` | 玩家连接编号（中继自增 ID），大端序 |
| 4 | 1 byte | `flags` | 标志位（当前保留，填 0） |

> `flags` 预留用于未来区分隧道类型（如 `0x01 = 普通玩家连接`、`0x02 = 观察者模式` 等）。

**房主收到后的响应流程**：
1. 房主新开一条 TCP+TLS-PSK 连接到中继的数据端口
2. 在握手完成后立即发送 `STREAM_BIND`（见 §3.6），携带 `player_conn_id`
3. 中继据此把新隧道和等待中的玩家原始连接配对
4. 之后双向纯字节转发

---

### 3.6 STREAM_BIND（0x06）

| 属性 | 值 |
|------|-----|
| 方向 | 房主 → 中继 |
| 触发 | 房主收到 OPEN_STREAM 后新开数据隧道时 |
| 用途 | 告知中继这条新隧道对应哪个玩家连接 |

**payload 结构**：

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 bytes | `player_conn_id` | 与 OPEN_STREAM 中的 ID 对应，大端序 |

> 此帧在**数据隧道**上发送（非控制连接），是该隧道上的第一条帧。

---

### 3.7 DEREGISTER（0x07）

| 属性 | 值 |
|------|-----|
| 方向 | 房主 → 中继 |
| 触发 | 房主主动关闭房间 |
| 用途 | 通知中继回收端口、清理房间 |

**payload**：空（`payload_len = 0`）

> DEREGISTER 必须在控制连接上发送。中继收到后立即回收端口，此后该房间的连接码失效。

---

### 3.8 ERROR（0xFF）

| 属性 | 值 |
|------|-----|
| 方向 | 双向 |
| 触发 | 任何一方检测到协议错误或无法处理的请求 |
| 用途 | 告知对方错误原因，随后关闭对应连接 |

**payload 结构**：

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 byte | `error_code` | 错误码，见 §4 |
| 1 | 变长 | `message` | UTF-8 错误描述（长度 = `payload_len - 1`） |

---

## 4. 错误码枚举

| 码值 | 符号名 | 含义 | 发送方 |
|------|--------|------|--------|
| `0x01` | `ERR_PROTOCOL_VERSION_MISMATCH` | 帧 version 字节与己方不匹配 | 双向 |
| `0x02` | `ERR_UNKNOWN_FRAME_TYPE` | 收到了未定义的 type 值 | 双向 |
| `0x03` | `ERR_ROOM_NOT_FOUND` | 房间不存在（房间 ID 无效或已过期） | 中继 |
| `0x04` | `ERR_PORT_POOL_EXHAUSTED` | 端口池已满，无法分配新端口 | 中继 |
| `0x05` | `ERR_TUNNEL_SETUP_FAILED` | 数据隧道建立失败（TLS 握手失败等） | 中继 |
| `0x06` | `ERR_AUTH_FAILED` | TLS-PSK 认证失败 | 中继 |
| `0x07` | `ERR_TIMEOUT` | 操作超时 | 双向 |
| `0x08` | `ERR_INVALID_PAYLOAD` | payload 格式/长度不合法 | 双向 |
| `0x09` | `ERR_ROOM_FULL` | 房间玩家数已达上限 | 中继 |
| `0x0A` | `ERR_INTERNAL` | 内部错误（不可恢复） | 双向 |
| `0x0B` | `ERR_ALREADY_REGISTERED` | 该控制连接已注册过房间 | 中继 |
| `0x0C` | `ERR_STREAM_BIND_FAILED` | 数据隧道绑定失败（player_conn_id 无效或已超时） | 中继 |

> 收到 `ERROR` 帧后，接收方应关闭该连接。若在控制连接上收到不可恢复的错误（如 `ERR_INTERNAL`、`ERR_AUTH_FAILED`），不应自动重连。

---

## 5. 版本协商

**当前版本**：`0x01`

**版本不匹配处理流程**：

```
A (version=0x02)  ──连接并发送帧──▶  B (version=0x01)
                                      │
                                      │ B 检测 version != 0x01
                                      │ B 回复 ERROR(ERR_PROTOCOL_VERSION_MISMATCH)
                                      │   payload = [0x01] + "protocol version: local=0x01 peer=0x02"
                                      │ B 关闭连接
                                      │
A 收到 ERROR ◀────────────────────────┘
│ A 解析 error_code == ERR_PROTOCOL_VERSION_MISMATCH
│ A 从 message 中提取对方版本号
│ A 决定：降级到 0x01 重连，或提示用户升级
```

> 当前阶段不要求版本协商自动降级，只需能**检测并报错**。自动降级可在未来版本中实现。

---

## 6. 帧类型速查表

| type | 名称 | 方向 | 连接类型 | 触发条件 |
|------|------|------|----------|----------|
| `0x01` | REGISTER | 房主→中继 | 控制连接 | TLS 握手完成后 |
| `0x02` | REGISTER_ACK | 中继→房主 | 控制连接 | 端口分配成功 |
| `0x03` | HEARTBEAT | 房主→中继 | 控制连接 | 定时 20–30s |
| `0x04` | HEARTBEAT_ACK | 中继→房主 | 控制连接 | 收到 HEARTBEAT |
| `0x05` | OPEN_STREAM | 中继→房主 | 控制连接 | 玩家连入 |
| `0x06` | STREAM_BIND | 房主→中继 | **数据隧道** | 新隧道建立后 |
| `0x07` | DEREGISTER | 房主→中继 | 控制连接 | 主动关闭房间 |
| `0x10–0x1F` | — | — | — | **预留** |
| `0xF0–0xFE` | — | — | — | **调试/诊断预留** |
| `0xFF` | ERROR | 双向 | 任意 | 协议错误 |
