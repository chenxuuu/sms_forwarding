# 系统架构

## 模块依赖关系

```
                    ┌─────────────────────┐
                    │    config_types.h    │  (枚举/结构体/常量，无依赖)
                    └─────────┬───────────┘
                              │
                    ┌─────────▼───────────┐
                    │     globals.h       │  (全局变量声明 + 公共头文件)
                    │     globals.cpp     │  (全局变量定义)
                    └─────────┬───────────┘
                              │
        ┌─────────┬───────────┼───────────┬───────────┐
        │         │           │           │           │
   ┌────▼───┐ ┌───▼────┐ ┌───▼────┐ ┌───▼────┐ ┌───▼────┐
   │config  │ │ modem  │ │ push   │ │sms_prc │ │web_hdl │
   │.h/.cpp │ │.h/.cpp │ │.h/.cpp │ │.h/.cpp │ │.h/.cpp │
   └────────┘ └───┬──┬─┘ └───┬─┬──┘ └───┬────┘ └───┬────┘
                  │  │       │ │        │          │
                  │  └───────┼─┼────────┘    ┌─────┘
                  │          │ └─────────────┘
                  │    ┌─────▼──────┐    ┌───▼────┐
                  │    │sms_process │    │web_html│
                  │    │  .h/.cpp   │    │.h/.cpp │
                  │    └────────────┘    └────────┘
         ┌────────▼─────────┐
         │    code.ino      │  (入口: setup + loop)
         │ (wifi_config.h)  │
         └──────────────────┘
```

箭头方向 = `#include` 依赖方向。`config_types.h` 是最底层，`code.ino` 是最顶层。

## 数据流总览

### 短信接收流程

```
4G模组 UART
    │
    ▼
checkSerial1URC()          [sms_process.cpp]
    │  逐行读取 Serial1
    │  优先识别 "+CMTI:" 存储通知并排队索引
    │  processStoredSmsIndexQueue() → AT+CMGR=<index>
    │  60s watchdog 用 AT+CMGL 兜底补收/清存储
    │  兼容 "+CMT:" 直推 PDU hex 数据
    ▼
pdu.decodePDU()            [pdulib 库]
    │  提取 sender / timestamp / text
    │  提取长短信 concatInfo (refNumber, partNumber, totalParts)
    ▼
┌─[长短信? totalParts > 1]─┐
│                           │
▼ 是                     ▼ 否 (普通短信)
findOrCreateConcatSlot()    │
  → 缓存分段                │
  → 收齐后合并              │
  → assembleConcatSms()     │
         │                  │
         └──────┬───────────┘
                ▼
         processSmsContent()
           ├── 去重(fnv1a)? → 忽略重复
           ├── isInNumberBlackList()? → 忽略
           ├── isAdmin()? → processAdminCommand()
           │                  ├── "SMS:号码:内容" → sendSMS()
           │                  └── "RESET" → resetModule() + ESP.restart()
           ├── inboxAdd()  本地留存
           └── enqueueForward()   [push.cpp]  接收/转发解耦，仅入队
                    │
                    ▼  [loop] processForwardQueue() → forwardNow()
                    │     evalForwardRules() 决定通道掩码/是否邮件/是否丢弃
                    ├── sendSMSToServer()  逐通道 enqueueInitialPush() 入重试队列
                    └── enqueueEmailJob()  入邮件队列
                             │
                             ▼  [后台 worker 任务] 慢速 TLS/SMTP 发送，不阻塞 loop
                       processRetryQueue() → sendToChannel() × N   (失败指数退避重试)
                       processEmailQueue() → sendEmailNotification()
```

### HTTP 请求流程

```
浏览器 (SPA 单页应用)
    │  侧边栏切换面板，所有功能在一个 HTML 页面
    │  JS 控制 panel 可见性，无需页面跳转
    ▼
server.handleClient()     [code.ino loop]
    │
    ▼
checkAuth()               [web_handlers.cpp]
    │  HTTP Basic Auth
    │  账号: config.webUser
    │  密码: config.webPass
    ▼
路由分发:
    GET  /         → handleRoot()        SPA 主页 (HTML 模板变量替换，含 10 个面板)
    GET  /tools    → handleRoot()        兼容旧链接，返回同一 SPA 页面
    GET  /sms      → handleRoot()        兼容旧链接，返回同一 SPA 页面
    POST /save     → handleSave()        保存配置 → saveConfig()（快速返回，不发通知邮件）
    POST /sendsms  → handleSendSms()     网页发送短信 → 入队 → loop 异步 sendSMS()
    POST /ping     → handlePing()        诊断: AT+CGACT=1 → MIPOPEN/MIPSEND UDP 流量 → CGACT=0
    GET  /flight   → handleFlightMode()  AT+CFUN 查询/切换飞行模式
    GET  /at       → handleATCommand()   透传 AT 指令到模组
    GET  /log      → handleLog()         返回环形缓冲区日志 (JSON 数组)
```

### 日志系统架构

```
业务代码调用:
    logCapture("目标号码: ");       → Serial.print() + 追加到 _logLine
    logCaptureLn(String(phone));    → Serial.println() + 提交 _logLine 到环形缓冲区
    logCaptureF("ref=%d\n", ref);    → Serial.print() + 以 \n 结尾时自动提交整行
         │
         ▼
    _logLine (行缓冲区)              // 累积 logCapture 调用直到 logCaptureLn 或 \n
         │
         ▼  logCaptureLn / logCaptureF(\n)
    _logAppend()                    // 写入环形缓冲区
         │
         ▼
    logBuffer[120]                  // 环形缓冲区，循环覆盖
         │
         ▼
    handleLog()                     // GET /log → JSON ["line1","line2",...]
         │
         ▼
    Web 日志面板                   // 每 2 秒自动轮询，终端风格深色主题
```

### 配置持久化流程

```
Web 表单提交 /save
    │
    ▼
handleSave()
    │  解析 POST 参数
    │  赋值到 config 全局结构体
    ▼
saveConfig()              [config.cpp]
    │  Preferences(NVS) 写入
    │  namespace: "sms_config"
    ▼
configValid = isConfigValid()  重新校验
    │
    ▼
快速返回响应（不发送通知邮件，避免 SMTP 阻塞前端；连通性请用各通道“测试推送”验证）
    注：handleSave/handleImport 的 config 写入区在 gWorkMux 内，与 worker 的快照读互斥
```

## 线程/任务模型

**双任务**：Arduino **loop 任务** + 一个**推送/邮件后台 worker 任务**（`pushWorkerTask`，`push.cpp`，由 `setup()` 中 `startPushWorker()` 启动）。

绝大多数工作仍在 loop 上串行执行（网页服务、模组 AT/Serial1、短信收发与转发判定、保号、定时任务）；**只有慢速的 WiFi/TLS 发送**——`processRetryQueue()`（HTTP 推送）、`processEmailQueue()`（SMTP）、`processTestPushQueue()`——被移到 worker。这样一次阻塞数秒的 TLS/SMTP **既不卡收短信、也不卡网页刷新**。worker 只碰 WiFi（全局 `smtp`/`ssl_client` 为其独占），**绝不触碰** Serial1/模组、`inbox`、`concatBuffer`、`pdu`、`smsTotalCount`。单 worker 串行 ⇒ 任意时刻仅一个 TLS 会话（堆峰值与改造前持平，仍由 `TLS_MIN_FREE_HEAP` 预检兜底）。

loop 任务每帧（节选）：

| 操作 | 频率 | 说明 |
|---|---|---|
| `server.handleClient()` | 每帧 | HTTP 请求处理（非阻塞） |
| `checkConcatTimeout()` | 每帧 | 长短信 30s 超时强制转发 |
| Serial→Serial1 透传 | 每帧 | USB↔模组 AT 透传（单字节） |
| `checkSerial1URC()` | 每帧 | 逐行读 `+CMTI/+CMT`，无行立即返回 |
| `processForwardQueue()` | 每帧 | 取一条短信做规则判定 + 入 worker 队列（无网络，开销极小） |
| `processOutgoingSmsQueue()` | 每帧 | 网页待发短信异步出队（AT+CMGS，loop 上） |
| `wifiEnsureConnected` / `modemHealthTick` / `signalSampleTick` / `smsReceiveWatchdogTick` / `heapGuardTick` / `keepAliveTick` / `dailyTasksTick` | millis 门控 | 各自周期任务 |

### 并发与加锁（最小面）

worker 与 loop 之间仅有少量共享可变状态，用两把 FreeRTOS 互斥量保护（`globals.h`）：

- **`gWorkMux`**：推送/邮件/测试三类任务队列 + worker 对 `config.pushChannels[]`/`config.smtp*` 的**快照读**（写方为 `handleSave`/`handleImport`）。
- **`gLogMux`**：日志环形缓冲（`logCapture*` ↔ `handleLog`/`handleLogDownload`）。

**铁律**：绝不在持锁期间发起网络/AT 调用（只在“取槽/快照/释放”时短暂持锁，解锁后再发送）；持锁顺序只允许 `gWorkMux→gLogMux`（`logCapture` 是叶子锁，从不取 `gWorkMux`）。`muxLock`/`muxUnlock` 对 `initConcurrency()` 之前的早期调用 NULL 安全。`gSlowWorkBusy`（volatile）标记邮件“已出队、SMTP 在途”窗口，供 `heapGuardTick`/定时重启避让。网页 handler 跑在 loop 任务的 `server.handleClient()` 内，故 loop-vs-loop 访问（如 `forwardNow` 读 config 与 `handleSave` 写 config）**不构成竞态**，只有 worker-vs-loop 才需加锁。

## 内存管理

- ESP32-C3 可用内存：~327KB
- 全局变量占用：~43KB
- PDU 缓冲区：4096 字节（`pdu = PDU(4096)`）
- 长短信缓存：5 组 × 10 段，每段内容动态分配（`String`）
- 日志环形缓冲区：120 行 × String，约 12KB
- 日志行缓冲区：1 个 String（`_logLine`），用于合并 logCapture 输出
- 串口行缓冲：500 字节（`SERIAL_BUFFER_SIZE`）
- HTTP 响应在函数内栈分配，调用结束自动释放

## 关键硬件引脚

| 定义 | GPIO | 功能 |
|---|---|---|
| `TXD` | 3 | 模组 UART TX |
| `RXD` | 4 | 模组 UART RX |
| `MODEM_EN_PIN` | 5 | 模组 EN 使能（LOW=关机, HIGH=开机） |
| `LED_BUILTIN` | 8 | 板载 LED（LOW=亮） |

## 错误处理策略

- **模组 AT 无响应**：重试（最多 10 次），LED 闪烁指示
- **WiFi 连接失败**：阻塞等待，LED 闪烁
- **NTP 同步失败**：记录日志，后续推送使用设备时间
- **HTTP 推送失败 / 断网**：进入有界重试队列（`PUSH_QUEUE_MAX`），指数退避重试（`PUSH_RETRY_MAX` 次封顶），由后台 worker 处理，不阻塞 loop
- **PDU 解析失败**：记录日志，丢弃该条短信
- **长短信超时**：强制转发已收到的分段（标记缺失段）
- **配置无效**：每秒打印设备 IP 提示用户配置
