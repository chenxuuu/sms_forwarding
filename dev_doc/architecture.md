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
    │  识别 "+CMT:" 头
    │  接收 PDU hex 数据
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
           ├── isInNumberBlackList()? → 忽略
           ├── isAdmin()? → processAdminCommand()
           │                  ├── "SMS:号码:内容" → sendSMS()
           │                  └── "RESET" → resetModule() + ESP.restart()
           ├── sendSMSToServer()     [push.cpp]
           │     └── sendToChannel() × N
           └── sendEmailNotification()  [push.cpp]
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
    POST /save     → handleSave()        保存配置 → saveConfig() → 发邮件
    POST /sendsms  → handleSendSms()     网页发送短信 → sendSMS()
    POST /ping     → handlePing()        AT+CGACT=1 → MPING → AT+CGACT=0
    GET  /query    → handleQuery()       查询 ATI/CESQ/ICCID/CEREG 等
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
sendEmailNotification()   发送 "配置已更新" 邮件
```

## 线程/任务模型

单线程事件循环（标准 Arduino 模型），所有操作在主 loop 中串行执行：

| 操作 | 频率 | 超时保护 |
|---|---|---|
| HTTP 请求处理 | 每帧 | 无（非阻塞） |
| 长短信超时检查 | 每帧 | 30s 超时自动转发不完整消息 |
| Serial→Serial1 透传 | 每帧 | 无（单字节读） |
| Serial1 URC 检查 | 每帧 | 逐行读取，无行则立即返回 |
| 配置无效告警 | 每秒 | 仅 configValid=false 时 |

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
- **HTTP 推送失败**：记录错误码和响应内容，不重试（避免卡顿）
- **PDU 解析失败**：记录日志，丢弃该条短信
- **长短信超时**：强制转发已收到的分段（标记缺失段）
- **配置无效**：每秒打印设备 IP 提示用户配置
