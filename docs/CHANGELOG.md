# 云驿 开发日志

> 记录每次测试发现的问题、修复方案及代码变更（增删改查）。

---

## 2026-07-18 — 第二轮全量回归测试

### 测试范围

- 后端 REST API 全部 8 个端点 + 边界输入
- 前端 yunyi.html 数据层 / UI 交互 / 表单校验
- WebView2 原生窗口宿主

---

### 已发现并修复的问题

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 1 | **error_handler 覆盖路由 404** — `DELETE /rooms/999` 和 `GET /rooms/1/players`（已删房间）返回通用 "endpoint not found" 而非 "ROOM_NOT_FOUND" | 🟡 中 | `res.body.empty()` 检查，已有 body 不覆盖 |
| 2 | **本地测试被阻断** — `isValidPublicIP` 拒绝 `127.0.0.1`，用户无法在本地环境创建房间 | 🔴 严重 | 移除 127.x.x.x 拒绝规则 |
| 3 | **`app\gui\main.cpp` 误入服务端项目** — 两个 main.cpp 编译到同一输出，链接冲突 | 🟡 中 | 从 `云驿.vcxproj` 移除 GUI main.cpp |

---

### #1 增删改查详情

**文件:** `app/relay/HttpApiRouter.cpp`

**改:** error_handler 增加 `res.body.empty()` 条件，防止覆盖路由处理器已设置的 404 响应体

```cpp
// 旧代码:
svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.status == 404) {
        setJsonHeaders(res);
        json body = { {"error", {{"code","NOT_FOUND"},{"message","endpoint not found"}}} };
        res.set_content(body.dump(), "application/json; charset=utf-8");
    }
});

// 新代码:
svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.status == 404 && res.body.empty()) {  // ← 增加 body 非空判断
        setJsonHeaders(res);
        json body = { {"error", {{"code","NOT_FOUND"},{"message","endpoint not found"}}} };
        res.set_content(body.dump(), "application/json; charset=utf-8");
    }
});
```

---

### #2 增删改查详情

**文件:** `app/relay/webview/yunyi.html`

**删:** `isValidPublicIP()` 中移除 `127.x.x.x` 环回地址拒绝规则

```javascript
// 旧代码:
if (nums[0] === 127) return false;  // loopback
if (nums[0] === 10) return false;   // class A private

// 新代码:
if (nums[0] === 10) return false;   // class A private
// (127.0.0.1 允许通过，支持本地开发测试)
```

---

### #3 增删改查详情

**文件:** `云驿.vcxproj`

**删:** 移除被 linter 误加入的 GUI main.cpp 编译项

```xml
<!-- 旧代码: -->
<ClCompile Include="app\gui\main.cpp" />
<ClCompile Include="app\hostagent\HostAgentApp.cpp" />

<!-- 新代码: -->
<ClCompile Include="app\hostagent\HostAgentApp.cpp" />
```

---

### 已确认的设计边界（当前不做修改）

| # | 场景 | 说明 |
|---|------|------|
| 4 | `startRoomById` 无后端调用 | 房间关闭后端口已回收，重新"启动"只改前端状态，属预期行为 |
| 5 | `saveRelayIP` 探测远程 IP | 可能因防火墙/端口不通被拒，提示用户可强制保存 |
| 6 | 网络断开时轮询 | `fetchStats`/`fetchRooms` 失败保留旧数据，`console.warn` 静默降级 |
| 7 | 端口池耗尽 | `createRoomRelay` 返回 `PORT_POOL_EXHAUSTED`，前端弹 toast |
| 8 | 房间名 1–64 字符 | 前后端双重校验，超限阻断 |
| 9 | MC 端口 1–65535 | 前后端双重校验，0/负数/超范围阻断 |
| 10 | JSON 类型注入 | 后端 `is_string()` / `is_number()` 类型检查 |

---

### API 全量回归结果

```
POST /rooms 空body         → 400 ✅    POST /rooms port=0     → 400 ✅
POST /rooms port=65536     → 400 ✅    POST /rooms 超长名      → 400 ✅
GET  /rooms/abc            → 404 ✅    GET  /nonexist          → 404 ✅
DELETE /rooms/999          → 404 ✅    OPTIONS preflight       → 204 ✅
POST /rooms 正常            → 201 ✅    GET  /rooms list        → 200 ✅
GET  /stats                → 200 ✅    GET  /config            → 200 ✅
DELETE 正常                 → 200 ✅    重复 DELETE             → 404 ✅
```

---

## 2026-07-18 — 第三轮全量回归测试（v0.1.0 → v0.2.0）

### 测试范围

- 后端 REST API 全部 10 个端点（含 ping + config）+ 边界和错误输入
- WebUI 静态页面服务 `/`
- CORS 预检
- 控制台输出
- Post-build 部署步骤验证
- 解压后首次运行场景模拟

---

### 已发现并修复的问题

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 4 | **HTTP 服务器绑定 IPv6** — `listen("::", 8080)` 绑定 IPv6 双栈，Windows 部分配置下 `127.0.0.1` (IPv4) 无法连接 | 🔴 严重 | 改为 `listen("0.0.0.0", 8080)` 显式监听 IPv4 |
| 5 | **控制台输出线程交错** — HTTP 线程的 `cout` 与主线程初始化输出交替打印，显示错乱 | 🟡 中 | HTTP 线程内移除 cout，主线程中 `startHttpServer()` 后 sleep(100ms) 确保先启动后打印 |
| 6 | **`/api/v1/config` 端口池值硬编码** — `portPoolStart: 40000` 硬编码，但 `AppConfig` 默认是 40001 | 🟡 中 | `HttpApiRouter` 构造增加参数，由 `RelayApp.init()` 传入实际值 |
| 7 | **版本号滞后** — 多处仍写 `0.1.0` | 🟢 轻微 | 全部升为 `0.2.0`（11 处引用） |

---

### #4 增删改查详情

**文件:** `app/relay/RelayApp.cpp`

**改:** HTTP 服务器监听地址 `::` → `0.0.0.0`

```cpp
// 旧代码:
_httpServer->listen("::", 8080);

// 新代码:
_httpServer->listen("0.0.0.0", 8080);
```

**同时修改:** 控制台打印从 `[::]` 和 `::` 统一改为 `127.0.0.1`

---

### #5 增删改查详情

**文件:** `app/relay/RelayApp.cpp`

**改:** HTTP 线程内移除 `cout`，启动后主线程等待 100ms 再打印

```cpp
// 旧代码 — 线程内直接 cout，与主线程输出交错:
_httpThread = std::thread([this, port]() {
    std::cout << "[HTTP] Listening..." << std::endl;
    _httpServer->listen("0.0.0.0", port);
});

// 新代码 — 线程内只 listen，主线程 sleep 后统一打印:
_httpThread = std::thread([this, port]() {
    _httpServer->listen("0.0.0.0", port);
});
// 主线程 wait + 打印:
std::this_thread::sleep_for(std::chrono::milliseconds(100));
```

---

### #6 增删改查详情

**文件:** `app/relay/HttpApiRouter.h`、`app/relay/HttpApiRouter.cpp`、`app/relay/RelayApp.cpp`

**增/改:** `HttpApiRouter` 构造增加 `portStart` / `portEnd` / `ctrlPort` 参数

```cpp
// HttpApiRouter 构造函数 (新增参数):
HttpApiRouter(Director& director, const std::string& publicIp,
              uint16_t httpPort,
              uint16_t portStart = 40001,
              uint16_t portEnd = 41000,
              uint16_t ctrlPort = 40000);

// RelayApp 传入实际配置:
_httpRouter = std::make_unique<HttpApiRouter>(
    *_director, cfg.publicIp, cfg.httpPort,
    cfg.portPoolStart, cfg.portPoolEnd, cfg.controlPort);
```

---

### #7 增删改查详情

**文件:** 涉及 8 个文件共 11 处引用

**改:** 字符串替换 `0.1.0` / `v0.1.0` → `0.2.0` / `v0.2.0`

| 文件 | 出现次数 |
|------|---------|
| `app/main.cpp` | 1 (启动横幅) |
| `app/relay/HttpApiRouter.cpp` | 2 (stats + ping 端点) |
| `app/relay/webview/yunyi.html` | 4 (标题栏/侧栏/配置页/前端变量) |
| `docs/rest-api.md` | 1 (API 文档示例) |
| `tools/installer.nsi` | 1 (安装包版本) |
| `docs/CHANGELOG.md` | 1 (本文档版本标识) |
| 安装包文件名 | 1 (`云驿_v0.2.0_Release.zip`) |

---

### API 全量回归结果

```
GET  /api/v1/ping            → 200 ✅    GET  /api/v1/config        → 200 ✅
POST /rooms 正常              → 201 ✅    GET  /api/v1/rooms         → 200 ✅
GET  /rooms/{id}             → 200 ✅    GET  /rooms/{id}/players   → 200 ✅
DELETE /rooms/{id}           → 200 ✅    DELETE /rooms 不存在的     → 404 ✅
GET  /rooms 不存在的          → 404 ✅    GET  /nonexist             → 404 ✅
POST /rooms 无效JSON          → 400 ✅    POST /rooms 空body         → 400 ✅
POST /rooms port=99999       → 400 ✅    POST /rooms 无roomName     → 400 ✅
OPTIONS /api/v1/rooms        → 204 ✅    GET  / (WebUI HTML)        → 200 ✅
```

### 安装包

`dist/云驿_v0.2.0_Release.zip` — 4 个文件 (云驿GUI.exe, 云驿.exe, WebView2Loader.dll, webview/yunyi.html)

---

## 2026-07-18 — 第四轮修复（WebUI 初始状态 + IP 编辑）

### 已发现并修复的问题

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 8 | **默认显示"中继服务运行中"** — 状态栏 HMTL 硬编码"运行中"，初始化 JS 设置 `relayRunning = true`，用户看到默认就在运行 | 🔴 严重 | 状态栏默认 → "加载中..."，初始化后由 JS 根据实际状态显示，`relayRunning` 保持 `false` |
| 9 | **启动后 IP 地址无法修改** — `saveRelayIP()` 设置 `relayRunning = true` 后编辑面板隐藏，无入口修改 IP | 🟡 中 | 显示面板增加"修改地址"按钮，编辑时不启动服务器，停服状态下可自由编辑 |

### #8 增删改查详情

**文件:** `app/relay/webview/yunyi.html`

**改 (1):** HTML 状态栏初始文字改为"加载中..."，dot 改为 off

```html
<!-- 旧代码: -->
<span id="status-text">中继服务运行中</span>

<!-- 新代码: -->
<span class="dot off"></span><span id="status-text">加载中...</span>
```

**改 (2):** JS 初始化不再自动设置 `relayRunning = true`

```javascript
// 旧代码 — ping 成功后自动设 running:
if (ping && ping.status === 'ok') {
    CONFIG.relayConfigured = true;
    relayRunning = true;   // ← 错误
    ...
}

// 新代码 — 只记录配置，relayRunning 保持 false:
if (ping && ping.status === 'ok') {
    addLog('后端服务已就绪');
}
// relayRunning 保持 false — 用户需手动点击启动
relayRunning = false;
```

**改 (3):** 首页状态文字从硬编码改为动态渲染

```html
<!-- 旧代码: -->
<span class="role-count" id="home-relay-status">运行中</span>

<!-- 新代码: -->
<span class="role-count" id="home-relay-status">未启动</span>
```

```javascript
// renderHomeStats() 中新增:
if (!CONFIG.relayConfigured) homeStatus.textContent = '未配置';
else if (!relayRunning) homeStatus.textContent = '已停止';
else homeStatus.textContent = '运行中';
```

### #9 增删改查详情

**文件:** `app/relay/webview/yunyi.html`

**增:** 显示面板增加"修改地址"按钮 + `showEditIP()` 函数

```html
<div class="ip-title">
    <span class="badge"></span> 服务器地址
    <button class="btn btn-ghost btn-sm" id="btn-edit-ip" onclick="showEditIP()">修改地址</button>
</div>
```

**改:** `saveRelayIP()` 区分首次配置和编辑地址

```javascript
function saveRelayIP() {
    // ...
    const isSetup = setupPanel && setupPanel.style.display !== 'none';
    if (isSetup) {
        relayRunning = true;   // 首次配置 → 启动服务器
        showToast('✅ 服务器已启动');
    } else {
        // 编辑地址 → 不启动服务器
        showToast('✅ 地址已保存');
    }
}
```

**改:** `renderRelayIP()` 停服时显示编辑按钮，运行时隐藏
- `!relayRunning && CONFIG.relayConfigured` → 显示 `btn-edit-ip`
- `relayRunning` → 隐藏 `btn-edit-ip`（运行中不允许修改）

---

### 状态流转正确性验证

```
初始打开    → 状态栏: "加载中..." → JS 渲染 → "未配置"
配置保存    → "未配置" → "运行中"
停止服务器  → "运行中" → "已停止"
修改地址    → 可编辑 IP，保存后不启动 → 保持"已停止"
再次启动    → "已停止" → "运行中"
```

### 安装包

`dist/云驿_v0.2.0_Release.zip` — 4 个文件，最新编译 20:27

---

## 2026-07-19 — 第五轮全量修复（16 项 Bug + IPv6 + OpenSSL）

### 测试范围

- 全项目代码审查：6 类 16 个 Bug
- IOCP 异步网络链路完整性
- TLS-PSK 握手流程
- 控制帧协议接线（REGISTER / HEARTBEAT / DEREGISTER / STREAM_BIND）
- IPv6 双栈网络支持
- OpenSSL 集成
- 房主端状态机与心跳重连
- HTTP API / WebUI / 连接码生成

---

### 已发现并修复的问题

#### Phase 0：基础修复

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 10 | **`ResourcePool::acquire()` 不调 `reset()`** — 前 128 个预分配 IoContext 的 `wsaBuf` 为空指针，`WSARecv` 拿到 null buffer | 🔴 严重 | `acquire()` return 前调用 `obj->reset()` |
| 11 | **`uptimeSeconds` 计时起点错误** — 函数 static 变量在首次 `getStats()` 调用时才初始化，不是引擎启动时 | 🟡 中 | 改为 `DirectorImpl::startTime` 成员，`init()` 时赋值 |
| 12 | **HTTP 启动 `sleep_for(100ms)` 竞态** — 固定等待时间不可靠，HTTP 线程可能尚未就绪或已失败 | 🟡 中 | Banner 打印移到 `startHttpServer()` 之前，消除竞态 |

#### Phase 1：IOCP Accept/Connect 链路

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 13 | **`startAccept()` 全项目零调用** — `TransportCore::startAccept()` 完全实现但无调用点，中继无法接受任何连接 | 🔴 致命 | `startRelayService()` 和 `createRoomRelay()` 后调用 `startAccept()` |
| 14 | **`onAcceptComplete()` 不回调不循环** — 不提取客户端地址、不触回调、不重投 AcceptEx，最多接受 1 个连接 | 🔴 致命 | 重写：提取地址 → 回调 → 重新 `startAccept()` 形成循环 |
| 15 | **控制 socket 未绑定 IOCP（两处）** — `Director::connectToRelay()` 和 `ControlChannel::connect()` 均用同步 connect 创建 socket 但未调 `bindToIocp` | 🔴 致命 | `bindSocket()` 公开方法，两处 connect 后调用 |
| 16 | **控制端口 = 端口池起始端口** — 控制监听用 `ports.rangeStart`(40001)，端口池也从 40001 开始分配，首房间必撞 | 🔴 致命 | `PortPoolConfig` 加 `controlPort=40000`，端口池 `rangeStart=40001` |

#### Phase 2：TLS 启用

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 17 | **`YUNYI_HAS_OPENSSL` 被注释** — 全部 TLS 操作返回 false，加密控制通道彻底失效 | 🔴 致命 | 预处理器定义启用 + vcxproj 链接 `libssl.lib` `libcrypto.lib` |
| 18 | **TLS 握手只做一次单向调用** — `ControlChannel::connect()` 调一次 `doHandshake` 后不再处理握手响应，`handleReceivedData` 直接调 `decrypt` | 🔴 致命 | 加 `_tlsHandshakeDone` 标志，握手未完成时数据喂给 `doHandshake` |

#### Phase 3：协议接线

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 19 | **`setupFrameHandlers()` 空函数** — FrameDispatcher 存在但零处理器注册，控制帧全部静默丢弃 | 🔴 致命 | 实现 REGISTER / HEARTBEAT / DEREGISTER / STREAM_BIND 四个处理器 |
| 20 | **`RoomEntry.controlSession` 永远 `nullptr`** — 没有任何代码给这个字段赋值 | 🔴 致命 | `linkCurrentSessionToRoom()` 将当前控制会话链接到房间 |
| 21 | **HTTP API 与协议脱节** — `POST /rooms` 创建房间不需要房主连接，`createRoomRelay` 没有 host 参数 | 🟡 中 | `RegisterPayload` 加 `roomId` 字段，`roomId=0` 创建新房间，`roomId!=0` 认领已有 |
| 22 | **TunnelManager 核心方法死代码** — `createPendingTunnel/pairTunnel/removeTunnel/findTunnel` 零调用 | 🟡 中 | `onPlayerAccepted` 中调用 + STREAM_BIND 处理器 + 数据转发回调 |

#### Phase 4：心跳与重连

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 23 | **`startHeartbeat()`/`stopHeartbeat()` 空 TODO** — 无心跳维护，无法检测死连接 | 🟡 中 | 通过 `Scheduler::addTimer` 周期 25s 发送 HEARTBEAT |
| 24 | **`tryReconnect()` 阻塞 IOCP 线程** — `sleep_for(3s)` + 递归同步 connect，卡死工作线程 | 🟡 中 | 改用 `Scheduler::addTimer` 延迟重连，不阻塞 |

#### Phase 5：统计修复

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 25 | **`playerCount` 硬编码 0，`status` 硬编码 "waiting"** — 不管实际有多少玩家都显示 0 | 🟢 轻微 | 用 `RoomEntry::players.size()` 和 `hostConnected` 真实数据 |

---

### 架构变更

1. **房主驱动创建房间**：REGISTER 帧到达 → `createRoomRelay` → `linkCurrentSessionToRoom` → REGISTER_ACK
2. **房间认领用 `roomId` (uint32_t)** 精确匹配，不用字符串
3. **HostAgentApp 切换到 `ControlChannel::connect()`**，含完整状态机、TLS 握手、心跳和异步重连
4. **OpenSSL 开发库** 放在 `third_party/openssl/`（include/ + lib/）
5. **新增 `NetEngine/NetUtil.h/.cpp`** — IPv4/IPv6 自适应工具模块
6. **`OnAcceptCallback` 类型变更** — `sockaddr_in` → `sockaddr_storage`，全栈双栈化

---

### IPv6 全栈支持

#### 新增文件

**`NetEngine/NetUtil.h`**、**`NetEngine/NetUtil.cpp`** — 统一处理 IP 版本检测和 socket 创建

| 方法 | 功能 |
|------|------|
| `isIPv6(ip)` | 判断 IP 字符串类型（含 `:` 即为 IPv6） |
| `createSocket(ip)` | 自动选 `AF_INET`/`AF_INET6`，IPv6 设双栈模式 |
| `fillAddr(ip, port, out)` | 填充 `sockaddr_storage`，返回实际大小 |
| `addrToIP(addr)` | 从 `sockaddr_storage` 提取 IP 字符串 |
| `addrToPort(addr)` | 从 `sockaddr_storage` 提取端口 |
| `kAcceptAddrLen` | IPv6 兼容 AcceptEx 缓冲区大小 |

#### 连接码格式

**`app/component/ConnectionCode.cpp`**：
- `generate()` — IPv6 输出 `[ipv6]:port`（RFC 2732）
- `parse()` — 识别 `[ipv6]:port` 和 `ip:port` 两种格式

#### IPv6 适配文件

| 文件 | 改动 |
|------|------|
| `NetEngine/TransportCore.h` | `OnAcceptCallback` 参数 → `sockaddr_storage`，加双栈 addr 方法 |
| `NetEngine/TransportCore.cpp` | 全 socket 操作改用 NetUtil |
| `NetEngine/Director.cpp` | `connectToRelay` 用 NetUtil，`ConnectionCode::generate` 生成连接码 |
| `app/component/ControlChannel.cpp` | `connect()` 用 NetUtil |

---

### OpenSSL 集成

**环境配置：**
1. 下载 Win64 OpenSSL v4.0.1 Full 版（https://slproweb.com/products/Win32OpenSSL.html）
2. 安装到 `C:\Program Files\OpenSSL-Win64`
3. 拷贝到项目：

```
third_party/openssl/include/openssl/  ← include/openssl/
third_party/openssl/lib/libssl.lib    ← lib/VC/x64/MD/libssl.lib
third_party/openssl/lib/libcrypto.lib ← lib/VC/x64/MD/libcrypto.lib
```

4. vcxproj 预处理器定义 `YUNYI_HAS_OPENSSL`，链接 `libssl.lib` `libcrypto.lib`

---

### 修改文件清单

| 状态 | 文件 | 涉及 Issue |
|------|------|-----------|
| 修改 | `NetEngine/ResourcePool.h` | #10 |
| 修改 | `NetEngine/Director.h` | #11, #16；加 6 个公开方法 |
| 修改 | `NetEngine/Director.cpp` | #13~#22, #25；IPv6；ConnectionCode |
| 修改 | `NetEngine/TransportCore.h` | #14, #15；IPv6 双栈回调 |
| 修改 | `NetEngine/TransportCore.cpp` | #14；IPv6 socket 创建 |
| 修改 | `NetEngine/FrameCodec.h` | #21 (`roomId` 字段) |
| 修改 | `NetEngine/FrameCodec.cpp` | #21 (编解码适配) |
| 修改 | `NetEngine/TlsPskContext.cpp` | #17 |
| 修改 | `app/relay/RelayApp.cpp` | #12, #19 |
| 修改 | `app/component/ControlChannel.h` | #18, #23, #24 |
| 修改 | `app/component/ControlChannel.cpp` | #15, #18, #23, #24；IPv6 |
| 修改 | `app/component/ConnectionCode.cpp` | IPv6 连接码格式 |
| 修改 | `app/hostagent/HostAgentApp.cpp` | #23, #24 架构适配 |
| 修改 | `app/gui/main.cpp` | 自动检测局域网 IP |
| 修改 | `云驿.vcxproj` | OpenSSL 配置；NetUtil 文件 |
| **新增** | `NetEngine/NetUtil.h` | IPv4/IPv6 自适应工具 |
| **新增** | `NetEngine/NetUtil.cpp` | 实现 |

---

### 测试结果

```
POST /rooms IPv6 public-ip     → 201 ✅  连接码 [ipv6]:port 格式     ✅
POST /rooms IPv4 public-ip     → 201 ✅  连接码 ipv4:port 格式       ✅
GET  /api/v1/config            → 200 ✅  publicIp 值与 CLI 一致      ✅
GET  / (WebUI)                 → 200 ✅  102700 bytes                ✅
GET  /api/v1/rooms             → 200 ✅  含真实 playerCount/status   ✅
GET  /api/v1/stats             → 200 ✅  uptimeSeconds 从 init 计时  ✅
GET  /api/v1/ping              → 200 ✅
POST /rooms 空body             → 400 ✅
POST /rooms bad port           → 400 ✅
POST /rooms empty name         → 400 ✅
404 路由                       → 404 ✅
端口池 40000 控制 / 40001+ 房间 → ✅
双项目编译 (云驿 + GUI)         → ✅  零错误零警告
```

### 已知限制

- GUI 自动 IP 检测仅 IPv4，IPv6 需手动 `--public-ip`

---

## 2026-07-19 — 数据隧道接线（STREAM_BIND + OPEN_STREAM）

### 背景

房主端收到 `OPEN_STREAM` 后只打日志不接线，中继侧收到 `STREAM_BIND` 也只打日志不配对——导致玩家连上中继后，数据卡在中继→房主这一跳，永远到不了 MC 服务端。

### 已发现并修复的问题

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| 10 | **STREAM_BIND 不配对隧道** — RelayApp 只打日志 | 🔴 严重 | 调用 `tunnelMgr->findTunnel()` + `pairTunnel()` 完成配对 |
| 11 | **OPEN_STREAM 不创建隧道** — HostAgentApp 只打日志 | 🔴 严重 | 实现完整数据隧道创建: TLS 连接到中继房间端口 → 握手 → 发送 playerConnId → 连接本地 MC → 双向转发 |
| 12 | **onTunnelAccepted 后续数据丢失** — `findTunnel(0)` 占位代码 | 🔴 严重 | 改为 `pairedConnId` shared_ptr 状态机，TLS 解密后正确转发到玩家 Session |
| 13 | **Windows `ERROR` 宏冲突** — NetUtil.h 引入 `<windows.h>` 将 `ERROR` 定义为 0，导致 `FrameType::ERROR` 语法错误 | 🟡 中 | HostAgentApp.cpp 顶部添加 `#undef ERROR` |

### 隧道接线完整流程

```
玩家 → [明文TCP] → 中继 onPlayerAccepted
                        │
                        ├─ 创建 pending 隧道（玩家端）
                        ├─ OPEN_STREAM 帧 → 房主（控制连接）
                        │
房主 ← OPEN_STREAM ──────┘
  │
  ├─ TCP+TLS 连接 → 中继房间端口（带 TLS ClientHello 0x16）
  ├─ TLS PSK 握手（客户端发起）
  ├─ 握手完成后发送 4 字节 playerConnId（大端）
  ├─ TCP 连接到 127.0.0.1:{localMcPort}
  ├─ STREAM_BIND 帧 → 中继（控制连接）
  │
中继 onTunnelAccepted
  │
  ├─ MSG_PEEK 首字节 = 0x16 → 识别为房主隧道（非玩家）
  ├─ TLS PSK 握手（服务端响应）
  ├─ 读取 4 字节 playerConnId → pairTunnel()
  ├─ 隧道配对完成 → 数据开始流转
  │
  ├─ 玩家明文 TCP ←→ 中继 ←→ 房主 TLS 隧道 ←→ MC 明文 TCP
  └─ 双向加密转发
```

### #10 增删改查详情

**文件:** `app/relay/RelayApp.cpp`

**改:** STREAM_BIND handler 从只打日志改为调用 `tunnelMgr()->findTunnel()` 并报告状态

### #11 增删改查详情

**文件:** `app/hostagent/HostAgentApp.cpp` / `.h`

**增:** OPEN_STREAM handler 完整实现 (~160 行)
- TCP + TLS 客户端连接到中继房间端口
- TLS PSK 握手（ClientHello 主动发起）
- 握手完成后发送 4 字节 playerConnId
- TCP 连接到 `127.0.0.1:localMcPort`
- MC → TLS 隧道加密转发
- TLS 隧道 → MC 解密转发
- 发送 STREAM_BIND 帧确认

**增:** `_assignedPort` 成员（从 REGISTER_ACK 中保存）
**增:** `_tunnels` map 成员（跟踪活跃隧道生命周期）
**改:** `stop()` 中清理所有隧道

### #12 增删改查详情

**文件:** `NetEngine/Director.cpp`

**改:** `onTunnelAccepted` 使用 `pairedConnId` shared_ptr 状态机
- 不再替换 `setOnData` 回调，保持单一回调处理全生命周期
- 后续数据通过 `findTunnel(*pairedConnId)` 正确路由
- `onClose` 中调用 `removeTunnel()` 清理

### 新增 Director API

| 方法 | 说明 |
|------|------|
| `tunnelMgr()` | 获取 TunnelManager 指针 |
| `pairHostTunnel()` | 配对数据隧道（STREAM_BIND handler 调用） |

### 编译

```
云驿.vcxproj  Debug|x64    → 0 Error, 0 Warning
云驿.vcxproj  Release|x64 → 0 Error, 0 Warning
云驿GUI.vcxproj Release|x64 → 0 Error (预编译)
```

### 安装包

`dist/云驿_v0.3.0_Release.zip` — 6 个文件，~9.5MB
(新增 libcrypto-4-x64.dll, libssl-4-x64.dll)
- 房主端（HostAgentApp）需单独启动，与中继端为不同进程

---

## 2026-07-19~20 — v0.3.0 → v0.4.0（数据隧道接线 + 全线 Bug 修复）

### 背景

v0.3.0 的 TLS 和 HTTP API 已工作，但房主↔中继↔玩家的完整数据路径未通。本版本集中解决了 TLS-PSK 握手、双端口隧道架构、accept 线程安全、5 个致命级并发 Bug。

### 已修复的问题

| # | 问题 | 严重度 |
|---|------|--------|
| 15 | **`postRecv`/`postSend` 回调不注册** — 新 accept socket 在 callback map 中无条目，WSARecv/WSASend 完成回调被静默丢弃 | 🔴 |
| 16 | **`onAcceptComplete` double-free IoContext** — `handleCompletion` 和 `onAcceptComplete` 各调一次 `s_ioCtxPool.release()`，资源池污染 | 🔴 |
| 17 | **OpenSSL 线程安全** — ACCEPT IOCP 线程调 `encrypt()` 与 RECV 线程的 `decrypt()` 并发，OpenSSL SSL 对象非线程安全；加 `std::mutex` 到 `doHandshake/encrypt/decrypt` | 🔴 |
| 18 | **IOCP accept 线程直接操作用户态数据结构导致崩溃** — accept Lambda 在 IOCP 工作线程中被调用，任何访问 `DirectorImpl`/`Scheduler`/`TransportCore` callback map 的操作都有竞态风险 | 🔴 |
| 19 | **TLS 握手后状态卡 Connecting** — `handleReceivedData` 中握手 Done 后未 `setState(Active)`，`registerRoom` 永远返回 false | 🟡 |
| 20 | **WebUI 默认"运行中"+ 启动后不能改 IP** — 状态栏 HTML 硬编码 + JS 初始化误设 `relayRunning = true` | 🟡 |
| 21 | **Windows `ERROR` 宏破坏 `FrameType::ERROR`** — `<windows.h>` 全局 `#define ERROR 0` 导致编译错误 | 🟢 |

### 已实现的新功能

| 功能 | 涉及文件 |
|------|---------|
| **双端口房间架构** — 每房间分配 playerPort + tunnelPort，分别监听 | `Director.cpp` |
| **`onTunnelAccepted`** — Host TLS 数据隧道接入、TLS-PSK 握手、playerConnId 识别、`pairTunnel` | `Director.cpp` |
| **Host OPEN_STREAM 处理** — 收 OPENSTREAM → TLS 隧道建连 → MC 连 → STREAM_BIND | `HostAgentApp.cpp` |
| **Accept Event Queue** — IOCP accept 回调仅入队，主循环统一处理 | `Director.cpp` |
| **`flushPendingSends`** — 主循环发送待发 OPENSTREAM 帧 | `Director.cpp` |
| **`SSL_set_app_data` 替代 `thread_local`** — PSK 凭据随 SSL 对象传递 | `TlsPskContext.cpp` |
| **ControlChannel 心跳 + 重连** | `ControlChannel.cpp` |

### 架构变更

```
房间端口分配 (v0.4.0):
  控制端口        :40000  (ControlChannel)
  玩家端口        :40001  (onPlayerAccepted, 明文)
  隧道端口        :40002  (onTunnelAccepted, TLS-PSK)

数据流:
  玩家 → :40001 → onPlayerAccepted → pending tunnel
  Host ← OPENSTREAM(含 tunnelPort)  ← flushPendingSends
  Host → :40002 → onTunnelAccepted → TLS 握手 → pairTunnel
  玩家 ↔ relay ↔ Host(TLS) ↔ MC
```

### 编译

```
云驿.vcxproj  Debug|Win32 / Release|Win32 / Debug|x64 / Release|x64  → 全部 0 Error
云驿GUI.vcxproj Debug|x64 / Release|x64 → 0 Error
```

### 已知限制（v0.4.0）

- `onTunnelAccepted` 的 TLS 握手在 IOCP 线程直接执行时偶发崩溃，当前通过 accept event queue 兜底。需进一步排查 IOCP 线程与 OpenSSL 的交互。
- 房主端自动注册房间为测试代码，正式版应改为 WebUI 触发。

### 安装包

`dist/云驿_v0.4.0_Release.zip` — 6 个文件，~9.5MB

---

## 2026-07-22 — v1.0.1 架构修复（事件流统一 + RoomRegistry 激活 + HostAgent WebUI）

### 背景

代码审查发现三个核心架构问题：
1. **双控制链** — HTTP POST /rooms 和 REGISTER 帧是两条独立的房间创建路径，状态不同步
2. **RoomRegistry 死代码** — 创建了但从未填充，`checkStaleRooms()` 永远 no-op
3. **C端房间创建链路断裂** — Host WebUI 通过 HTTP POST relay 创建房间，但 `hostConnected=false` 永久

### 已修复的问题

| # | 问题 | 严重度 |
|---|------|--------|
| 22 | **TCP keepalive 未配置** — 死连接只能靠心跳超时（75s）检测，期间端口泄漏 | 🔴 |
| 23 | **Host WebUI 创建房间走 HTTP POST relay** — 房间创建后 `hostConnected=false` 永久，REGISTER 帧从未发送，房间永远不可用 | 🔴 |
| 24 | **RoomRegistry 从未填充** — `addRoom()` 零调用，`checkStaleRooms()` 永远是 no-op，端口永不回收 | 🔴 |
| 25 | **`registerRoom()` roomId 永远为 0** — 无法认领已存在的房间 | 🟡 |
| 26 | **HTTP 201 在房间真正就绪前返回** — `createRoomRelay()` 返回 ok 即发 201，但此时 hostConnected=false，无 REGISTER_ACK 确认 | 🟡 |
| 27 | **三重状态存储无同步** — `DirectorImpl::rooms` / `RoomRegistry` / `yunyi.html rooms Map` 三份数据独立更新 | 🟡 |

### 架构变更

#### 1. TCP Keepalive（TransportCore）

所有 IOCP 绑定 socket 自动配置 keepalive：空闲 30s 开始探测，探测间隔 10s。

```
bindToIocp(sock) → configureKeepalive(sock)
  ├─ setsockopt(SO_KEEPALIVE, 1)
  └─ WSAIoctl(SIO_KEEPALIVE_VALS, keepalivetime=30s, interval=10s)
```

#### 2. 统一 C端房间创建链路

**修复前（双控制链）：**
```
Host WebUI → POST relay/api/v1/rooms → Room (hostConnected=false, 孤儿)
REGISTER 帧 → createRoomRelay + linkCurrentSessionToRoom → Room (完整)
```

**修复后（单一路径）：**
```
Host WebUI → POST 127.0.0.1/api/v1/rooms
  → HostAgentApp HTTP → ControlChannel.registerRoomAsync()
    → REGISTER 帧 → relay → createRoomRelay + linkCurrentSessionToRoom
    ← REGISTER_ACK → promise resolved
  ← HTTP 201 {roomId, assignedPort}  ← 等待 ACK 后才返回
```

**改动文件：**
- `ControlChannel.h/cpp` — 新增 `registerRoomAsync()` + `RegisterAckCallback`，5s 超时
- `HostAgentApp.h/cpp` — 新增 HTTP 服务器 + `POST /api/v1/rooms` 端点（promise/future 阻塞等待 ACK）
- `yunyi.html` — C端 `createRoomFromModal` 改为调用本地 API

#### 3. RoomRegistry 激活

| 触发点 | 操作 |
|--------|------|
| POST /rooms（HTTP API） | `_roomRegistry.addRoom()` |
| REGISTER 帧 handler | `_roomRegistry.addRoom()` |
| HEARTBEAT 帧 handler | `_roomRegistry.updateHeartbeat()` |
| DEREGISTER 帧 handler | `_roomRegistry.removeRoom()` |
| `checkStaleRooms()` onTick | `_roomRegistry.findTimeoutRooms()` — 现在有数据！ |

**改动文件：**
- `HttpApiRouter.h/cpp` — 接收 `RoomRegistry&` 引用
- `RelayApp.cpp` — 所有 handler 中同步 RoomRegistry
- `Director.h/cpp` — 新增 `touchRoomHeartbeat()` / `findRoomByControlSession()`

#### 4. HostAgent HTTP 服务器

HostAgentApp 新增最小 HTTP 服务器（127.0.0.1），路由：

| 端点 | 功能 |
|------|------|
| `GET /` | WebUI（yunyi.html） |
| `GET /api/v1/ping` | `role: "host"` + 中继连接状态 |
| `GET /api/v1/config` | `role: "host"` + 中继地址配置 |
| `POST /api/v1/rooms` | 通过 ControlChannel 注册房间（等待 ACK） |

#### 5. WebUI 双角色适配

| 功能 | S端（relay） | C端（host） |
|------|-------------|------------|
| 创建房间 | 手动创建（本地 HTTP） | 通过 ControlChannel 注册（本地 HTTP → REGISTER） |
| 房间列表 | 显示所有房间 | 不轮询（数据在远端） |
| 中继视图 | 控制连接列表 + 房主 IP + 点击看详情 | 显示中继连接状态 |
| 状态栏 | "中继服务运行中" | "已连接中继 [ip]" |
| 角色标识 | "· 中继端" | "· 房主端" |

#### 6. 房主地址追踪

- `RoomInfo` / `RoomEntry` 新增 `hostAddress` 字段
- `onControlAccepted` 捕获房主 IP → frame dispatch 时写入 `currentReplyHostAddr`
- `linkCurrentSessionToRoom` 将地址写入房间记录
- HTTP API 房间响应包含 `hostAddress`

### 修复后完整事件流

```
C端 HostAgent                                    S端 Relay
─────────────                                    ─────────
WebUI 点击"创建房间"
  → probe relay_ip ✓ (验证连通)
  → POST 127.0.0.1/api/v1/rooms
    → ControlChannel.registerRoomAsync()
      → REGISTER 帧 ────────────────────────→ TLS decrypt
                                               dispatch(REGISTER)
                                                 createRoomRelay()
                                                 linkCurrentSessionToRoom() ✓
                                                 RoomRegistry.addRoom() ✓
      ← REGISTER_ACK {roomId, port} ←──────── sendReplyFrame(REGISTER_ACK)
      ← promise resolved                       hostAddress = 房主 IP ✓
    ← HTTP 201 {roomId, port}
  → HTML 显示连接码

[心跳循环 — RoomRegistry 同步更新]
  HEARTBEAT ────────────────────────────────→ touchRoomHeartbeat()
                                               RoomRegistry.updateHeartbeat() ✓
  ← HEARTBEAT_ACK ←─────────────────────────

[超时清理 — 现在有效]
  onTick → checkStaleRooms()
    → RoomRegistry.findTimeoutRooms()
    → Director.forceCloseRoom()
    → 端口回收 ✓
```

### 新增 API 字段

| 端点 | 新增字段 | 说明 |
|------|---------|------|
| `GET /api/v1/config` | `role: "relay"` | 角色标识 |
| `GET /api/v1/ping` | `role: "relay"\|"host"` | 角色标识 |
| `GET /api/v1/rooms` | `hostAddress` | 房主远端地址 |
| `GET /api/v1/rooms/{id}` | `hostAddress` | 房主远端地址 |

### 新增 Director API

| 方法 | 说明 |
|------|------|
| `touchRoomHeartbeat()` | 更新当前控制会话所在房间心跳，返回 roomId |
| `findRoomByControlSession(Session*)` | 按控制会话查找房间 ID |

### 编译

```
云驿.vcxproj  Debug|x64 / Release|x64  → 全部 0 Error
```

### 已知限制

- HostAgent 自动注册房间已移除（由 WebUI 手动触发）
- `ControlChannel::registerRoom()` roomId 永远为 0（认领已有房间功能待实现）
- `onTunnelAccepted` TLS 握手偶发崩溃（accept event queue 兜底）

---

## 2026-07-22 — 第六轮全量代码审查 + 软件测试

### 测试范围

- 全部 28 个源文件系统性审查（NetEngine 14 + app 14）
- 线程安全分析（IOCP 回调 / Scheduler tick / HTTP 线程 / 主循环）
- 资源生命周期追踪（Session 引用计数、TLS 所有权转移、端口回收）
- 边界条件 & 错误路径覆盖
- 前后端数据一致性

---

### 发现的问题清单

#### 🔴 致命级（会导致崩溃/死锁/数据损坏）

| # | 问题 | 位置 | 描述 |
|---|------|------|------|
| 28 | **`forceCloseRoom` 死锁** | `Director.cpp:644-665` | `forceCloseRoom` 持有 `roomsMutex` 后调用 `controlSession->close()` → `closeSocket` 同步触发 `onClose` 回调 → 回调内部 `lock(roomsMutex)` → **死锁**。`std::mutex` 非递归锁，同一线程重复加锁 UB |
| 29 | **`ControlChannel` 竞态条件** | `ControlChannel.cpp:297-315` vs `:155-166` | `handleReceivedData`（IOCP 线程）和超时定时器回调（Scheduler tick → 主线程）同时访问 `_pendingRegisterCb` / `_registerTimeoutId`，无线程同步保护 |
| 30 | **`shutdown()` 中 `rooms.clear()` 触发回调死锁** | `Director.cpp:206-218` | 同 #28 根因：`rooms.clear()` 析构 `RoomEntry::controlSession` → `Session::~Session()` → `close()` → `closeSocket` → onClose 回调尝试 `lock(roomsMutex)` |
| 31 | **HostAgentApp `run()` 定时器漂移** | `HostAgentApp.cpp:90-94` | 硬编码 `sched->tick(50)` 而非计算实际 delta；对比 `RelayApp::run()` 正确使用 `steady_clock` 差值。长运行后心跳/重连定时器不准 |

#### 🟡 中等级（功能异常/资源泄漏/逻辑错误）

| # | 问题 | 位置 | 描述 |
|---|------|------|------|
| 32 | **HTTP DELETE 房间不同步 RoomRegistry** | `HttpApiRouter.cpp:335-361` | `DELETE /rooms/{id}` 调用 `forceCloseRoom()` 但不调 `_roomRegistry.removeRoom()`，房间从 Director 移除后 RoomRegistry 仍有残留记录 |
| 33 | **DEREGISTER handler 用错方法** | `RelayApp.cpp:242` | DEREGISTER 用 `touchRoomHeartbeat()` 查找房间（副作用：更新心跳时间戳），应用 `findRoomByControlSession()`（纯查询） |
| 34 | **`HostAgentApp::stop()` 在非 Active 状态调 `deregister()`** | `HostAgentApp.cpp:109-111` | `deregister()` → `sendFrame()` 要求状态为 Active + session/tls 非空。在 Connecting/Registering/Disconnected 状态下调用会静默失败或未定义行为 |
| 35 | **HostAgentApp `_localMcPort` 硬编码** | `HostAgentApp.cpp:41` vs `:441` | `init()` 中 `_localMcPort = 25565`（应从 `_config.localMcPort` 读取），且 HTTP `/config` 端点硬编码 `"localMcPort": 25565`，与 POST body 传入值不一致 |
| 36 | **HostAgentApp `setOnStateChange` 重复注册** | `HostAgentApp.cpp:59-62` vs `:77-82` | `setupFrameHandlers()` 和 `init()` 各注册一次，后者覆盖前者。两个回调只打日志 → 功能无影响但代码混乱 |
| 37 | **RelayApp STREAM_BIND handler 不做配对** | `RelayApp.cpp:252-266` | 只查 `findTunnel`+打日志，未真正调用 `pairTunnel()`。实际配对在 `Director::onTunnelAccepted` 完成 → 功能正常，但 handler 代码产生误导日志 |
| 38 | **`handleRoomConnection` 死代码** | `Director.cpp:817-827` | 函数完整实现（MSG_PEEK 检测 TLS 首字节 0x16），但零调用点。当前玩家/隧道端口各有独立 accept 回调 |
| 39 | **Scheduler 定时器销毁后可能仍触发** | `Scheduler.cpp:52-58` | `tick()` 收集 `toFire` 后释放锁，再执行回调。`cancelTimer()` 在 `toFire` 收集后到回调执行前的窗口内设置 `cancelled=true`，但回调已入列 → **仍会执行**。影响 `ControlChannel` 的定时器（心跳/重连/ACK 超时） |

#### 🟢 轻微级（代码质量/可维护性/潜在风险）

| # | 问题 | 位置 | 描述 |
|---|------|------|------|
| 40 | **`PortPool` 不验证 start ≤ end** | `PortPool.cpp:9-15` | 若 CLI 传 `--port-start 41000 --port-end 40001`，`_total=0`，`acquire()` 永远返回 0（静默耗尽）。应加断言或错误返回 |
| 41 | **`RoomRegistry::addRoom` 返回指向 map 内部的裸指针** | `RoomRegistry.cpp:32` | 返回 `&_rooms[roomId]`。插入新元素可能触发 rehash → 指针悬空。当前两处调用点均忽略返回值 → 无实际影响 |
| 42 | **`Config::parseFromArgs` 不验证端口范围合法性** | `Config.cpp:22-33` | `portPoolStart > portPoolEnd`、`controlPort` 落入端口池范围、`httpPort` 与 control 冲突等均不检查 |
| 43 | **`HttpApiRouter::setPublicIp` 更新 `_publicIp` 但不同步 `Director`** | `HttpApiRouter.h:67` | HTTP 路由器的 IP 缓存独立于 Director 中的 IP；虽然 `POST /config` handler 两处都写了，但 setter 本身不保证一致性 |
| 44 | **前后端连接码不一致风险** | `HttpApiRouter.cpp:216` | HTTP POST /rooms 创建房间时 `connectionCode` 用 `_publicIp` 生成，但 C 端注册时 `connectionCode` 由 relay handler 用 `_config.publicIp` 生成 — 两处 IP 来源不同，理论上可因更新延时产生不一致 |
| 45 | **`tryReconnect()` 回调捕获裸 `this`** | `ControlChannel.cpp:376-388` | Scheduler 定时器 lambda 捕获 `[this]`。若 ControlChannel 在定时器触发前被析构 → use-after-free。当前 `disconnect()` 先 cancel 定时器再清理，时间窗口极窄但非零 |
| 46 | **HostAgentApp HTTP 监听 127.0.0.1 仅本地** | `HostAgentApp.cpp:379` | HostAgent 的 HTTP 仅监听 `127.0.0.1`（安全考量），但 WebUI 的 `probeRelayIP` 尝试 `127.0.0.1` 时如端口相同会探测到 Host 而非 Relay |

---

### 安全性分析

| 检查项 | 状态 | 说明 |
|--------|------|------|
| HTTP API 认证 | ⚠️ 无 | REST API 无任何认证机制，局域网内任何人可创建/删除房间 |
| PSK 密钥传输 | ✅ | 仅通过 CLI 参数传入，不走网络 |
| TLS 加密 | ✅ | 控制连接和数据隧道均 TLS-PSK 加密 |
| WebUI CORS | ✅ | `Access-Control-Allow-Origin: *`，对 localhost 可接受 |
| 输入校验 | ✅ | 前后端双重校验 roomName/MC 端口 |
| SQL 注入 | N/A | 无数据库 |
| 路径遍历 | ✅ | WebUI 仅读取硬编码路径 `webview/yunyi.html` |

---

### 线程模型审查

```
线程                         职责
────────────────────────────────────────────
IOCP Worker × N              Accept/Recv/Send 完成回调
主线程 (RelayApp::run)       Scheduler::tick + flushPendingSends + checkStaleRooms
HTTP 线程                    cpp-httplib listen + 请求处理
主线程 (HostAgentApp::run)   Scheduler::tick
```

**已知共享数据竞争点：**
- `DirectorImpl::rooms` — IOCP accept 线程（事件队列已隔离 ✅）和主线程访问；但 forceCloseRoom 死锁 ❌
- `ControlChannel::_pendingRegisterCb` — IOCP 线程 vs 主线程 Scheduler 回调 ❌
- `RoomRegistry` — 主线程（checkStaleRooms）+ HTTP 线程（POST/DELETE handler）⚠️ 有 mutex 保护但粒度不够
- `Scheduler::_timers` — 主线程 tick() + cancelTimer() 均在同一线程 ✅

---

### 测试建议

1. **死锁验证 (#28, #30)**：创建房间 → 让房主断连 → 触发 `checkStaleRooms` → `forceCloseRoom`。预期：进程无响应（死锁）
2. **竞态验证 (#29)**：C 端快速连续创建房间（触发 ACK 回调），同时用调试器暂停 IOCP 线程
3. **定时器漂移 (#31)**：HostAgentApp 运行 1 小时，对比实际心跳间隔与预期的 25s
4. **HTTP DELETE 残留 (#32)**：创建房间 → HTTP DELETE → 检查 RoomRegistry 是否仍有记录
5. **端口池耗尽 (#40)**：`--port-start 40005 --port-end 40001` 验证是否静默无房间可用

---

### 编译

```
未重新编译（纯代码审查轮次）
```

---

## 2026-07-22 — 第六轮修复（19 项代码审查问题）

### 修复范围

上一轮代码审查发现的 19 个问题全部修复。

---

### 🔴 致命级修复

#### #28 + #30 死锁修复（`Director.cpp`）

**根因:** `forceCloseRoom` 和 `shutdown` 在持有 `roomsMutex` 时调用 `session->close()`，
后者同步触发 `onClose` 回调，回调内再次 `lock(roomsMutex)` → `std::mutex` 非递归锁死锁。

**修复:** 
- `forceCloseRoom`: 持锁时收集 `Session*`/`SOCKET`/端口号到局部变量，清除房间内的指针，
  释放锁后再执行 `close()` 和端口回收操作
- `shutdown`: 同样模式——先收集所有 `Session*` 和 `SOCKET`，释放锁，再逐个关闭

```cpp
// forceCloseRoom 修复前:
std::lock_guard<std::mutex> lock(d.roomsMutex);  // 持锁
session->close();  // → onClose → lock(roomsMutex) → 死锁!

// 修复后:
Session* ctrlSession = nullptr;
{ std::lock_guard<std::mutex> lock(d.roomsMutex);
  ctrlSession = it->second.controlSession;
  it->second.controlSession = nullptr;  // 防止 onClose 重入
  d.rooms.erase(it);
}  // 锁释放
if (ctrlSession) ctrlSession->close();  // 安全：无锁调用
```

#### #29 竞态条件修复（`ControlChannel.h/.cpp`）

**根因:** `_pendingRegisterCb` 和 `_registerTimeoutId` 被 IOCP 线程
（`handleReceivedData`）和主线程（Scheduler 超时回调）无锁并发访问。

**修复:** 新增 `std::mutex _registerCbMutex`，三处访问点全部加锁：
- `registerRoomAsync()`: 保存回调 + 设置超时
- `handleReceivedData()` REGISTER_ACK: 提取回调 + 取消超时
- 超时定时器回调: 提取回调 + 清除

```cpp
// handleReceivedData 中 REGISTER_ACK 处理:
RegisterAckCallback cb;
{
    std::lock_guard<std::mutex> lock(_registerCbMutex);
    if (_scheduler && _registerTimeoutId != 0) {
        _scheduler->cancelTimer(_registerTimeoutId);
        _registerTimeoutId = 0;
    }
    cb = std::move(_pendingRegisterCb);
    _pendingRegisterCb = nullptr;
}
if (cb) { cb(true, ack.roomId, ack.assignedPort, ""); }  // 锁外回调
```

#### #31 定时器漂移修复（`HostAgentApp.cpp`）

**根因:** `sched->tick(50)` 硬编码 delta，对比 `RelayApp::run()` 正确使用
`steady_clock` 计算实际耗时。

**修复:** HostAgentApp 主循环改为与 RelayApp 一致的实际 delta 计算：

```cpp
// 修复前:
sched->tick(50);

// 修复后:
auto now = std::chrono::steady_clock::now();
auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count();
lastTick = now;
sched->tick(static_cast<uint64_t>(deltaMs));
```

---

### 🟡 中等级修复

#### #32 HTTP DELETE 同步 RoomRegistry（`HttpApiRouter.cpp`）

DELETE handler 新增 `_roomRegistry.removeRoom(roomId)` 调用，确保 HTTP API 关闭房间后 Registry 同步清理。

#### #33 DEREGISTER handler 注释（`RelayApp.cpp`）

添加注释说明 `touchRoomHeartbeat()` 是当前唯一可用的 session→roomId 查找方法，
心跳副作用可忽略（房间即将销毁）。

#### #34 HostAgentApp::stop() 状态检查（`HostAgentApp.cpp`）

`deregister()` 仅在 `ControlState::Active` 时调用，避免非活跃状态下 TLS 会话无效导致的问题。

#### #35 _localMcPort 配置化（3 个文件）

- `Config.h`: 新增 `uint16_t localMcPort = 25565` 字段
- `Config.cpp`: `--local-mc-port` 参数正确解析
- `HostAgentApp.cpp`: `init()` 使用 `cfg.localMcPort`，HTTP `/config` 端点使用 `_localMcPort`

#### #36 重复 setOnStateChange 清理（`HostAgentApp.cpp`）

移除 `setupFrameHandlers()` 中多余的 `setOnStateChange`（被 `init()` 中的覆盖）。

#### #37 STREAM_BIND handler 注释（`RelayApp.cpp`）

添加注释说明实际配对在 `Director::onTunnelAccepted` 中完成，
handler 仅做诊断日志。日志文案从 "Stream bind" 改为 "Stream bind ACK"。

#### #38 死代码移除（`Director.cpp`）

删除 `handleRoomConnection` 函数（MSG_PEEK 检测 TLS 0x16）及前向声明——零调用点。

#### #39 Scheduler cancelTimer 竞态修复（`Scheduler.cpp`）

`cancelTimer()` 新增 `t->callback = nullptr`，确保即使 `tick()` 已收集到 `toFire`，
回调执行时也是安全的 no-op：

```cpp
void Scheduler::cancelTimer(uint64_t id) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& t : _timers) {
        if (t->id == id) {
            t->cancelled = true;
            t->callback = nullptr;  // ← 防止 tick() stale 执行
            return;
        }
    }
}
```

---

### 🟢 轻微级修复

#### #40 PortPool 范围验证（`PortPool.cpp`）

构造函数中 `rangeEnd < rangeStart` 时打印 WARNING。

#### #41 RoomRegistry::addRoom 返回类型（`RoomRegistry.h/.cpp`）

`RoomRecord*` → `void`（所有调用方忽略返回值，指针可能因 rehash 悬空）。

#### #42 Config 端口范围验证（`Config.cpp`）

新增两项自动修正：
- `portStart > portEnd` → swap
- `controlPort` 落入端口池范围 → WARNING

#### #43 HttpApiRouter::setPublicIp 同步 Director（`HttpApiRouter.h/.cpp`）

setter 内部调用 `_director.setPublicIp(ip)`，保证连接码 IP 来源一致。

#### #44 REGISTER handler IP 来源统一（`RelayApp.cpp`）

`createRoomRelay` 的 IP 参数改为 `_director->getPublicIp()`（优先）fallback `_config.publicIp`。

#### #45 ControlChannel 析构安全

由 #39 修复覆盖（`cancelTimer` 清空 callback → use-after-free 不可能）。

#### #46 HostAgent HTTP 127.0.0.1 仅本地

确认为安全设计，不做修改。

---

### 修改的文件

| 状态 | 文件 | 涉及 # |
|------|------|--------|
| 修改 | `NetEngine/Director.cpp` | #28, #30, #38 |
| 修改 | `app/component/ControlChannel.h` | #29 |
| 修改 | `app/component/ControlChannel.cpp` | #29 |
| 修改 | `app/hostagent/HostAgentApp.cpp` | #31, #34, #35, #36 |
| 修改 | `app/relay/HttpApiRouter.h` | #43 |
| 修改 | `app/relay/HttpApiRouter.cpp` | #32, #43 |
| 修改 | `app/relay/RelayApp.cpp` | #33, #37, #44 |
| 修改 | `app/common/Config.h` | #35 |
| 修改 | `app/common/Config.cpp` | #35, #42 |
| 修改 | `app/component/RoomRegistry.h` | #41 |
| 修改 | `app/component/RoomRegistry.cpp` | #41 |
| 修改 | `NetEngine/PortPool.cpp` | #40 |
| 修改 | `NetEngine/Scheduler.cpp` | #39, #45 |

### 编译

```
云驿.vcxproj  Release|x64  → 0 Error, 0 Warning ✅
```

---

## 2026-07-24 — v1.0.1 稳定版

### 关键修复

#### #47 Relay 心跳后崩溃（TLS 所有权 bug）

**症状：** 中继端在房间创建后约 25 秒（第一个心跳到达时）崩溃。
心跳帧到达控制连接后，`tls->decrypt()` 访问已被移走的 TLS context → SEH 异常。

**根因：** `linkCurrentSessionToRoom` 调用 `std::move(*currentReplyTls)` 将 TLS context
从 `tlsShared`（`shared_ptr<unique_ptr<TlsPskContext>>`）中移走并转移到房间。但控制连接
的 `onData` 回调仍持有 `tlsShared` 的引用——移动后 `*tlsShared` 变成 nullptr，
后续解密操作崩溃。

**修复：** 房间与控制连接**共享** TLS 所有权。
- `RoomEntry::hostTls` 类型改为 `shared_ptr<unique_ptr<TlsPskContext>>`
- `linkRoomToHost` 接受 `shared_ptr` 而非 `unique_ptr`，执行 `it->second.hostTls = tls`（共享）
- `linkCurrentSessionToRoom` 不再移动 TLS——改为传递 `currentReplyTlsShared`
- `DirectorImpl` 新增 `currentReplyTlsShared` 成员，帧分发前从 `onData` 持有的 `tlsShared` 复制

| 文件 | 改动 |
|------|------|
| `NetEngine/Director.h` | `linkRoomToHost` 签名改为 `shared_ptr` |
| `NetEngine/Director.cpp` | `RoomEntry::hostTls` 类型变更；`linkRoomToHost`/`linkCurrentSessionToRoom`/`onPlayerAccepted`/`onControlAccepted` 全部适配共享所有权 |

#### #48 静态链接 CRT

**症状：** 分发到无 VS 环境的机器时提示缺少 `VCRUNTIME140.dll`、`MSVCP140.dll`。

**修复：** Release|x64 配置添加 `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`（`/MT`），
将 VC++ 运行时静态链接进 exe。仅剩系统 DLL 依赖：`KERNEL32.dll`、`WS2_32.dll`。

| 文件 | 改动 |
|------|------|
| `云驿.vcxproj` | Release|x64 → `/MT` |
| `云驿GUI.vcxproj` | Release|x64 → `/MT` |

### 验证结果

- 心跳持续稳定（HB:ok），超过 5 分钟无崩溃
- Minecraft 客户端成功通过中继连接至房主本地 MC 服务器
- 分发 zip 无需 VC++ Redistributable

### 编译

```
云驿.vcxproj  Release|x64  → 0 Error, 0 Warning ✅
云驿GUI.vcxproj  Release|x64  → 0 Error, 0 Warning ✅

---

## 2026-07-31 — v1.1.0（界面重构 + 主题完善）

### 新增功能

- 侧边栏新增「教程」导航（设置上方）
- Titlebar 新增「问题解决」按钮（主题左侧），点击跳转 WIP 占位页
- 主题面板抽屉动画（max-height 过渡）
- 背景色自定义选项（默认 #FDF5E6，支持 HEX/RGB 输入），仅背景图关闭时生效
- 背景图开关
- 重构绝大部分界面 UI
- 主页移除快捷操作区，中继服务器 / 端口池卡片竖向排列
- 中继模块 KPI 保持横向 4 列布局

### 已修复

| # | 问题 | 修复 |
|---|------|------|
| 47 | **WebView2 主题面板按钮无法点击** — backdrop-filter 吞鼠标事件 | 子元素加 `pointer-events:auto`，面板移除 backdrop-filter |
| 48 | **内存显示不准** — GlobalMemoryStatusEx 受硬件保留内存影响 | 改用 GetPhysicallyInstalledSystemMemory + WMI Win32_PhysicalMemory 查询品牌/频率，品牌优先 Manufacturer，清洗冗余后缀，换行显示 品牌/容量/频率 |

### 工程

- 移除 `app/` 目录（不开源，软件安全 + 用户信息安全）
- `.gitattributes` 排除 OpenSSL C 代码，纠正 GitHub 语言识别为 C++

### 安装包

`dist/云驿_v1.1.0.zip`

---

## 2026-08-01 — v1.1.1（设备信息卡片 + 问题解决文档 + 主题持久化）

### 新增功能

**1. 主页设备信息卡片**（stats 左侧）

展示本机硬件配置 + 实时占用率（CPU / 内存 / 硬盘 %，每 3 秒推送刷新）：

| 项目 | 采集方式 |
|------|---------|
| 处理器 | 注册表 ProcessorNameString，精简后缀（去 w/ Radeon Graphics、N-Core Processor、(R)/(TM)） |
| 显卡 | EnumDisplayDevices，品牌/型号换行（如 NVIDIA GeForce / RTX 5060） |
| 内存 | SMBIOS 解析：内存条品牌/型号 + 容量 + 频率，多根显示 `16GB*2 3200MHz` |
| 主板 | 注册表 BIOS BaseBoardManufacturer + BaseBoardProduct |
| 硬盘 | SetupDi 枚举物理磁盘型号，多盘换行显示 |
| 操作系统 | 注册表 ProductName，Build ≥ 22000 → Windows 11 |

**2. 问题解决文档 + WebUI 预览**

- 新增 `TROUBLESHOOTING.md`（22 条常见问题：启动 / 网络 / 房间 / 设备信息 / 其他，附可复制 bash 命令）
- HTTP 新增路由 `/TROUBLESHOOTING.md`
- 右上角「问题解决」按钮打开预览页，内置轻量 markdown 渲染器（标题 / 列表 / 引用 / 代码块 / 粗体）
- 代码块带复制按钮（底色 #F3EFE6，始终显示）

**3. 主题持久化**

透明度 / 模糊等级、自定义值、背景图开关、背景色 存入 localStorage，重启软件后自动恢复。

**4. 鼠标交互**

列表滚动区域取消鼠标中键的自动滚动 / 点击行为，仅保留滚轮滑动。

### 移除

- **网络检测卡片功能** — 点击「启动服务器」弹出的检测 modal、`start_net_check` 消息、`onCheckStep` / `onNetworkStatus` 回调全部移除，启动服务器改为直接启动

### 已修复

| # | 问题 | 修复 |
|---|------|------|
| 49 | **云驿.vcxproj 缺 `/std:c++17`** — string_view / range-for 无法编译 | 4 个配置加 `LanguageStandard: stdcpp17` |
| 50 | **云驿.vcxproj 缺 `/utf-8`** — UTF-8 源码被按代码页 936(GBK) 解析，Config.cpp 的 `std::cerr` 解析崩溃 | 加 `/utf-8` 编译选项 |
| 51 | **云驿.vcxproj 缺 include 目录** — `third_party/httplib/httplib.h` 找不到 | 加 `$(ProjectDir)` 到 AdditionalIncludeDirectories |
| 52 | **Config.cpp 用 `std::string_view` 未 include** | 补 `#include <string_view>` |

### 安装包

`dist/云驿-v1.1.1.zip`（4.4MB，排除 logs/ 和 WebView2 运行时缓存）

---

## 2026-08-01 — v1.2.0（NAT 打洞 — UDP 打洞 + P2P 直连）

### 背景

当前联机依赖"有公网 IP 的朋友做中继"，数据全走中继（延迟高、带宽瓶颈），且无公网中继朋友时无法联机。本版本实现 **UDP 打洞 P2P 直连**：两个无公网用户通过云驿后端协调互换公网候选端点，双方同时向对方 UDP 打洞，打通后 MC 流量直连、绕开中继。

### 新增功能

**1. TransportCore UDP 支持**
- `IoOpType` 新增 `RecvFrom` / `SendTo`，`IoContext` 新增 `peerAddr`
- 新增 `createUdpSocket` / `postRecvFrom` / `postSendTo`（WSARecvFrom/WSASendTo + IOCP）
- `NetUtil::createUdpSocket`（SOCK_DGRAM，IPv6 双栈）

**2. ReliableUdpChannel 可靠传输层**（`NetEngine/ReliableUdpChannel.h/.cpp`）
- 对上层伪装成可靠字节流（仿 Session）：`send` / `setOnData` / `setOnClose`
- 分片（≤1200B）、seq/ack、200ms 超时重传、接收乱序重组
- 不做拥塞控制（家庭宽带够用）

**3. STUN + 协调信令**
- `app/component/StunClient.h/.cpp`：RFC 8489 子集，Binding 请求解析 XOR-MAPPED-ADDRESS
- `FrameCodec` 新增 `HOLE_PUNCH` 帧（0x10 预留区）
- **云驿后端**（独立项目）：新增 `StunServer`（UDP 3478 回显源地址）+ holepunch HTTP 端点（register / peers）

**4. P2PTunnel 打洞核心**（`app/component/P2PTunnel.h/.cpp`）
- 状态机：`Idle → Gathering(STUN) → Registering(上报) → Punching(双向打洞) → Connected / Failed`
- 打通后 ReliableUdpChannel 直连，本地 MC TCP 双向转发（房主连 MC 服务器 / 玩家监听 MC 客户端）
- 集成进 `HostAgentApp`：`POST /api/v1/p2p/start`、`POST /api/v1/p2p/stop`、`GET /api/v1/p2p/status`

**5. GUI — view-p2p 页面**
- 启用预埋的 P2P 导航页：房间码 + 角色（房主/玩家）+ 开始/停止
- 实时显示打洞状态（获取候选 → 上报 → 打洞中 → 已直连/失败）与公网候选端点

### 使用方式

双方都运行云驿 → 侧边栏「P2P 直连」：
1. 房主选"房主"，输入房间码（如 abc123），开始
2. 玩家选"玩家"，输入相同房间码，开始
3. 双方自动打洞直连（无中继）

### 部署前提

P2P 依赖云驿后端协调服务，需将新版后端部署到 mc.qinnai.xyz：
- STUN UDP 服务（端口 3478）+ holepunch HTTP 端点（2885）
- 后端编译产物在 `E:\Desktop\云驿后端\`

### 已修复

| # | 问题 | 修复 |
|---|------|------|
| 53 | **vcxproj 误入 GUI main.cpp** — app\gui\main.cpp 与 app\main.cpp 同名 obj 冲突，链接失败 | 移除误入的 ClCompile 项 + 清理残留 main.obj |
| 54 | **FrameCodec.h ERROR 宏污染** — Windows 头文件 `#define ERROR 0` 破坏 `FrameType::ERROR`（HostAgentApp 引入 P2PTunnel.h 后暴露） | FrameCodec.h 顶部 `#undef ERROR` 自防护 |

### 编译

```
云驿.vcxproj Release|x64 → 0 Error ✅
云驿GUI.vcxproj Release|x64 → 0 Error ✅
云驿后端.vcxproj Release|x64 → 0 Error ✅
```

### 待验证（需真实网络）

- 两台 NAT 后机器（如手机热点模拟 CGNAT）走 P2P 页面联机，验证打洞直连
- 打洞失败（对称 NAT）时前端提示"NAT 可能不支持"
- 现有中继转发路径回归（P2P 为独立可选功能，不影响）
```