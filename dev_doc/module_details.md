# 模块详解 — 逐文件源码指南

---

## code.ino — 主入口

**文件**: `code/code.ino` (约 105 行)  
**角色**: Arduino 工程入口点，仅包含 `setup()` 和 `loop()`。

### setup() 初始化顺序

```
1. GPIO 初始化
   ├── pinMode(LED_BUILTIN, OUTPUT); digitalWrite(HIGH)   // LED 灭
   └── (MODEM_EN_PIN 在 modemPowerCycle 内设置)

2. 串口初始化
   ├── Serial.begin(115200); delay(1500)                   // USB CDC 稳定
   └── Serial1.begin(115200, SERIAL_8N1, RXD, TXD)        // 模组 UART

3. 模组上电
   ├── Serial1.read() 清噪声
   ├── modemPowerCycle()                                     // EN 引脚时序
   └── Serial1.read() 清启动噪声

4. 数据加载
   ├── initConcatBuffer()                                    // 长短信缓存清零
   ├── loadConfig()                                          // NVS → config
   └── configValid = isConfigValid()                        // 校验

5. 模组初始化 (AT 指令序列，每步失败重试+LED闪烁)
   ├── sendATandWaitOK("AT", 1000)                          // 握手
   ├── sendATandWaitOK("AT+CGACT=0,1", 5000)               // 禁数据(省流量)
   ├── sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)          // 短信URC上报
   ├── sendATandWaitOK("AT+CMGF=0", 1000)                   // PDU模式
   └── waitCEREG()                                           // 等网络注册

6. WiFi 连接
   ├── WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)             // 扫描全部信道
   └── WiFi.begin(SSID, PASS, 0, nullptr, true)              // 支持隐藏SSID

7. NTP 时间同步
   ├── configTime(0, 0, "ntp.ntsc.ac.cn", ...)              // UTC时区
   └── 等待 time() > 100000 (最多10秒)

8. HTTP 服务器
   ├── server.on("/", handleRoot)
   ├── server.on("/save", HTTP_POST, handleSave)
   ├── server.on("/tools", handleRoot)                       # 兼容旧链接
   ├── server.on("/sms", handleRoot)                         # 兼容旧链接
   ├── server.on("/sendsms", HTTP_POST, handleSendSms)
   ├── server.on("/ping", HTTP_POST, handlePing)
   ├── server.on("/query", handleQuery)
   ├── server.on("/flight", handleFlightMode)
   ├── server.on("/at", handleATCommand)
   ├── server.on("/log", handleLog)                          # 系统日志 JSON
   └── server.begin()

9. 启动通知
   └── if(configValid) sendEmailNotification("短信转发器已启动", ...)
```

### loop() 执行顺序（每帧执行）

```cpp
server.handleClient();        // 1. 处理HTTP请求
if(!configValid) { ... }      // 2. 配置无效时每秒打印IP
checkConcatTimeout();          // 3. 长短信超时合并转发
if(Serial.available())        // 4. USB->模组透传(单字节)
    Serial1.write(Serial.read());
checkSerial1URC();             // 5. 检查模组URC(短信上报)
```

### 修改指南

- **新增 HTTP 路由**: 在 `web_handlers.h/.cpp` 添加处理函数，然后在 `setup()` 中 `server.on("/path", handler)`
- **调整初始化顺序**: 直接修改 `setup()` 中的代码顺序即可，注意模组初始化必须在 WiFi 之前
- **禁用某功能**: 注释掉对应的 `server.on()` 行即可

---

## config_types.h — 类型定义

**依赖**: 仅 `<Arduino.h>`  
**被依赖**: 所有其他模块

### PushType 枚举 (0-10)

表示推送目标平台。值 0 为"未启用"，1-10 对应 10 种推送方式。

**添加新推送类型**:
1. 在 `PushType` 枚举末尾添加新值（如 `PUSH_TYPE_NEW = 11`）
2. 在 `isPushChannelValid()` 添加对应的必填字段校验
3. 在 `sendToChannel()` 添加 `case PUSH_TYPE_NEW:` 实现
4. 在 `handleRoot()` 的 HTML 生成中添加 option
5. 在 HTML JavaScript `updateTypeHint()` 中添加类型提示

### PushChannel 结构体

通用推送通道配置。字段含义:
- `enabled` — 开关
- `type` — PushType 枚举值
- `name` — WebUI 中显示的名称
- `url` — Webhook URL（GET 类型则为目标 URL）
- `key1` — 通用参数 1（token/secret/chat_id 等）
- `key2` — 通用参数 2（channel/bot_token 等）
- `customBody` — 自定义模板的 HTTP body

### Config 结构体

所有可持久化配置的容器，通过 `saveConfig()/loadConfig()` 与 NVS 同步。

### 常量

| 常量 | 值 | 说明 |
|---|---|---|
| `MAX_PUSH_CHANNELS` | 5 | 推送通道数量上限 |
| `MAX_CONCAT_PARTS` | 10 | 长短信最大分段数 |
| `CONCAT_TIMEOUT_MS` | 30000 | 长短信等待超时(ms) |
| `MAX_CONCAT_MESSAGES` | 5 | 同时缓存的长短信组数 |
| `DEFAULT_WEB_USER` | `"admin"` | 默认 Web 账号 |
| `DEFAULT_WEB_PASS` | `"admin123"` | 默认 Web 密码 |

---

## globals.h / globals.cpp — 全局变量

### 全局变量清单

| 变量 | 类型 | 初始化 | 用途 |
|---|---|---|---|
| `config` | `Config` | 默认构造 | 系统全部配置（运行时+持久化） |
| `preferences` | `Preferences` | 默认构造 | NVS 存储接口 |
| `pdu` | `PDU` | `PDU(4096)` | PDU 编解码器（4KB 缓冲区） |
| `ssl_client` | `WiFiClientSecure` | 默认构造 | TLS 客户端（SMTP/HTTPS） |
| `smtp` | `SMTPClient` | `SMTPClient(ssl_client)` | SMTP 邮件客户端 |
| `server` | `WebServer` | `WebServer(80)` | HTTP 服务器（端口 80） |
| `configValid` | `bool` | `false` | 配置是否有效标志 |
| `timeSynced` | `bool` | `false` | NTP 是否已同步 |
| `lastPrintTime` | `unsigned long` | `0` | 上次打印 IP 的 millis |
| `concatBuffer` | `ConcatSms[5]` | 默认构造 | 长短信合并缓存 |

### 功能说明

- **添加新的全局变量**: 在 `globals.h` 添加 `extern` 声明，在 `globals.cpp` 添加定义
- **添加新的库依赖**: 在 `globals.h` 添加 `#include`，所有模块自动获得该依赖

---

## config.h / config.cpp — 配置管理

### NVS 存储结构

```
Namespace: "sms_config"
├── smtpServer  (String)
├── smtpPort    (Int, 默认 465)
├── smtpUser    (String)
├── smtpPass    (String)
├── smtpSendTo  (String)
├── adminPhone  (String)
├── webUser     (String, 默认 "admin")
├── webPass     (String, 默认 "admin123")
├── numBlkList  (String, 换行分隔)
├── push0en     (Bool)
├── push0type   (UChar)
├── push0url    (String)
├── push0name   (String)
├── push0k1     (String)
├── push0k2     (String)
├── push0body   (String)
├── push1en ... (同上 × 4)
└── ...
```

### 兼容迁移机制

`loadConfig()` 末尾检查旧 key `httpUrl` 和 `barkMode`，若存在则自动迁移到 `pushChannels[0]`。这是旧版单通道配置 → 新版多通道配置的迁移路径。

### 修改指南

- **添加新配置项**: 在 `Config` 结构体加字段 → `loadConfig()` 加读取（设默认值） → `saveConfig()` 加写入 → `handleSave()` 加表单解析
- **修改校验逻辑**: 编辑 `isPushChannelValid()` 和 `isConfigValid()`

---

## modem.h / modem.cpp — 模组控制

### 串口通信模型

```
ESP32-C3                   4G 模组
  Serial1 (UART) ──────────── AT 端口
  GPIO 5     ──────────── EN 引脚
```

### AT 指令函数对比

| 函数 | 返回类型 | 延时处理 | 适用场景 |
|---|---|---|---|
| `sendATCommand()` | String (完整响应) | 收到 OK/ERROR 后 `delay(50)` | 需要解析响应内容 |
| `sendATandWaitOK()` | bool | 无额外 delay | 快速幂等检查 |

### 模组电源控制

```
modemPowerCycle():
  EN=HIGH (默认)  模组运行
      ↓
  EN=LOW (1200ms) 模组断电
      ↓
  EN=HIGH (6000ms) 模组上电启动
```

6000ms 是关键时间，太短模组初始化未完成会导致 AT 无响应。

### sendSMS() 时序图

```
ESP32                         模组
  │                            │
  ├─ AT+CMGS=<len>\r\n ──────►│
  │                            │
  │◄───────────────────── >  │  (提示符)
  │                            │
  ├─ PDU hex data + 0x1A ────►│
  │                            │
  │◄──────────────── +CMGS:   │  (成功响应)
  │◄──────────────── OK       │
```

### 修改指南

- **更换模组型号**: 修改 `modemPowerCycle()` 的时序参数，调整 AT 指令序列
- **添加新 AT 功能**: 实现新函数，使用 `sendATCommand()` 或直接操作 `Serial1`
- **调波特率**: 修改 `Serial1.begin()` 参数 + 模组 AT 配置

---

## push.h / push.cpp — 推送服务

### 推送通道执行流程

```
sendSMSToServer(sender, message, timestamp)
  │
  ├─ 检查 WiFi 连接
  ├─ 检查是否有启用的有效通道
  │
  └─ for each valid channel:
       └─ sendToChannel(channel, sender, message, timestamp)
            │
            ├─ jsonEscape() 转义 sender/message/timestamp
            │
            └─ switch(channel.type):
                 ├─ POST_JSON  → POST {sender, message, timestamp}
                 ├─ BARK       → POST {title, body}
                 ├─ GET        → GET ?sender=&message=&timestamp=
                 ├─ DINGTALK   → POST {msgtype:"text", text:{content:"..."}}
                 │               ├─ 有secret → dingtalkSign() 追加URL参数
                 ├─ PUSHPLUS   → POST {token, title, content, channel}
                 │               └─ 默认URL: http://www.pushplus.plus/send
                 ├─ SERVERCHAN → POST title=&desp= (form-urlencoded)
                 │               └─ 默认URL: https://sctapi.ftqq.com/{key1}.send
                 ├─ CUSTOM     → POST (使用 customBody 模板替换)
                 ├─ FEISHU     → POST {timestamp?, sign?, msg_type, content}
                 │               └─ 有secret → HMAC-SHA256签名
                 ├─ GOTIFY     → POST {title, message, priority}
                 │               └─ URL: {url}/message?token={key1}
                 └─ TELEGRAM   → POST {chat_id, text}
                     └─ 默认URL: https://api.telegram.org/bot{key2}/sendMessage
```

### HMAC 签名实现

钉钉和飞书都使用 `mbedtls_md` 库（ESP32 内置）:
```cpp
mbedtls_md_context_t ctx;
mbedtls_md_init(&ctx);
mbedtls_md_setup(&ctx, MBEDTLS_MD_SHA256, 1);
mbedtls_md_hmac_starts(&ctx, key, keyLen);
mbedtls_md_hmac_update(&ctx, data, dataLen);
mbedtls_md_hmac_finish(&ctx, hmacResult);
mbedtls_md_free(&ctx);
// hmacResult → base64::encode() → urlEncode()
```

### 邮件发送

使用 ReadyMail 库的 SMTP 客户端:
```cpp
smtp.connect(server, port, callback);
smtp.authenticate(user, pass, readymail_auth_password);
SMTPMessage msg;
msg.headers.add(rfc822_from, from);
msg.headers.add(rfc822_to, to);
msg.headers.add(rfc822_subject, subject);
msg.text.body(body);
msg.timestamp = time(nullptr);
smtp.send(msg);
```

### 修改指南

- **添加新推送通道**: 在 `PushType` 加枚举 → `isPushChannelValid()` 加校验 → `sendToChannel()` 加 case → Web UI 加选项
- **修改钉钉/飞书签名逻辑**: 编辑 `dingtalkSign()` 或 `sendToChannel()` 中 FEISHU case
- **更换 SMTP 库**: 只需修改 `sendEmailNotification()` 函数

---

## sms_process.h / sms_process.cpp — 短信处理

### 长短信合并状态机

```
┌─────────────────────────────────────────────────────┐
│                  concatBuffer[0..4]                  │
│  ┌─────────┐  ┌─────────┐      ┌─────────┐         │
│  │ Slot 0  │  │ Slot 1  │ ...  │ Slot 4  │         │
│  │ inUse   │  │ inUse   │      │ inUse   │         │
│  │ refNum  │  │ refNum  │      │ refNum  │         │
│  │ total   │  │ total   │      │ total   │         │
│  │ recv'd  │  │ recv'd  │      │ recv'd  │         │
│  │ time    │  │ time    │      │ time    │         │
│  │ parts[] │  │ parts[] │      │ parts[] │         │
│  └─────────┘  └─────────┘      └─────────┘         │
└─────────────────────────────────────────────────────┘
```

### PDU 解析流程

```
checkSerial1URC() 循环:
  ┌─ IDLE 状态 ──────────────────────────────────────┐
  │  逐行读取 Serial1                                 │
  │  检测 "+CMT:" → 转入 WAIT_PDU                    │
  └──────────────────────────────────────────────────┘
                       │
  ┌─ WAIT_PDU 状态 ──────────────────────────────────┐
  │  读取下一行                                       │
  │  isHexString()?                                   │
  │    ├─ 是 → pdu.decodePDU(line)                   │
  │    │       ├─ 失败 → 打印错误, 回 IDLE            │
  │    │       └─ 成功 → 提取 sender/text/timestamp   │
  │    │                → 检查 concatInfo              │
  │    │                  ├─ 长短信 → 缓存分段         │
  │    │                  │   └─ 收齐? → 合并→process │
  │    │                  └─ 普通短信 → process        │
  │    └─ 否 → 回 IDLE                               │
  └──────────────────────────────────────────────────┘
```

### 黑名单匹配算法

```
原始发送者号码: "+8613800138000"
  │
  ├─ 提取纯号码: "13800138000" (去+86)
  │
  └─ 逐行对比黑名单:
       黑名单行 "13800138000" → 匹配 ✓
       黑名单行 "13900139000" → 不匹配
       黑名单行 "+8613800138000" → 匹配 ✓ (用原始号码对比)
```

### 管理员命令解析

```
短信内容: "SMS:13800138000:你好世界"
  │
  ├─ 第一个 ':'  → "SMS"
  ├─ 第二个 ':'  → "13800138000"  (目标号码)
  └─ 剩余内容    → "你好世界"      (短信内容)
  │
  └─ sendSMS("13800138000", "你好世界")

短信内容: "RESET"
  └─ resetModule() + ESP.restart()
```

### 修改指南

- **调整长短信超时**: 修改 `CONCAT_TIMEOUT_MS` 宏（config_types.h）
- **修改黑名单匹配逻辑**: 编辑 `isInNumberBlackList()`
- **添加新管理员命令**: 在 `processAdminCommand()` 中添加 `else if` 分支
- **更换短信库**: 修改 `checkSerial1URC()` 中的 `pdu.decodePDU()` 调用

---

## web_handlers.h / web_handlers.cpp — HTTP 处理 + 日志系统

### 路由表

| 方法 | 路径 | 处理函数 | Auth | 说明 |
|---|---|---|---|---|
| GET | `/` | `handleRoot()` | ✓ | SPA 主页（含 10 个面板） |
| GET | `/tools` | `handleRoot()` | ✓ | 旧链接兼容，返回同一 SPA 页面 |
| GET | `/sms` | `handleRoot()` | ✓ | 旧链接兼容，返回同一 SPA 页面 |
| POST | `/save` | `handleSave()` | ✓ | 保存配置 |
| POST | `/sendsms` | `handleSendSms()` | ✓ | 网页发送短信 |
| POST | `/ping` | `handlePing()` | ✓ | Ping 测试 |
| GET | `/query` | `handleQuery()` | ✓ | 模组/WiFi 信息查询 |
| GET | `/flight` | `handleFlightMode()` | ✓ | 飞行模式控制 |
| GET | `/at` | `handleATCommand()` | ✓ | AT 指令调试 |
| GET | `/log` | `handleLog()` | ✓ | 系统日志（JSON 数组） |

### 日志环形缓冲区

```
容量: LOG_BUF_SIZE = 120 行
结构: String logBuffer[120] + 行缓冲 _logLine
写入:
  logCapture(msg)      → Serial.print() + 追加到 _logLine
  logCaptureLn(msg)    → Serial.println() + 提交 _logLine 到环形缓冲区
  logCaptureF(fmt,...) → Serial.print() + 追加到 _logLine (fmt 以 \n 结尾时自动提交)
读取:
  handlerLog()         → JSON ["行1","行2",...] (按插入顺序)
```

**行缓冲设计目的**: 避免 `logCapture("A:"); logCaptureLn(B);` 在环形缓冲区中产生两个独立条目。实际只产生一行 `"A: B"`。

### 模板变量替换

SPA 页面中的 `%PLACEHOLDER%` 在 `handleRoot()` 中通过 `html.replace()` 替换:

| 占位符 | 数据来源 |
|---|---|
| `%IP%` | `WiFi.localIP().toString()` |
| `%WEB_USER%` / `%WEB_PASS%` | `config.webUser` / `config.webPass` |
| `%SMTP_SERVER%` ~ `%SMTP_SEND_TO%` | `config.smtp*` |
| `%ADMIN_PHONE%` | `config.adminPhone` |
| `%NUMBER_BLACK_LIST%` | `config.numberBlackList` |
| `%PUSH_CHANNELS%` | 循环生成 5 个通道的 HTML 表单 |

### 响应格式

| 端点 | Content-Type | 返回格式 |
|---|---|---|
| `/` `/tools` `/sms` `/save` | `text/html` | HTML 页面 |
| `/query` `/flight` `/at` `/ping` | `application/json` | `{"success":bool, "message":"..."}` |
| `/log` | `application/json` | `["行1", "行2", ...]` |

### 修改指南

- **添加新页面**: 在 `web_handlers.cpp` 添加处理函数 → `setup()` 注册路由
- **修改页面样式**: 编辑 `web_html.cpp` 中的 HTML 模板
- **添加查询类型**: 在 `handleQuery()` 的 if-else 链中添加新 type

---

## web_html.h / web_html.cpp — HTML 模板

### 内容说明

**单页应用 (SPA)**：整个项目仅一个 HTML 常量 `htmlPage`（约 500 行），包含 10 个面板：

| 面板 ID | 名称 | 功能 |
|---|---|---|
| `panel-overview` | 系统概览 | 显示 IP/信号/配置状态 |
| `panel-account` | 账号管理 | 修改 Web 登录密码 |
| `panel-email` | 邮件通知 | SMTP 邮件配置 |
| `panel-push` | 推送通道 | 5 个推送通道配置 |
| `panel-admin` | 管理员 & 黑名单 | 管理员号码 + 号码黑名单 |
| `panel-sendsms` | 发送短信 | Web 端发送短信 |
| `panel-diagnose` | 模组诊断 | ATI/信号/SIM/网络查询 |
| `panel-network` | 网络测试 | Ping 测试 |
| `panel-modem` | 模组控制 | 飞行模式开关/模组重启 |
| `panel-atterm` | AT 终端 | AT 指令交互调试 |
| `panel-log` | 系统日志 | 实时日志查看（终端风格） |

**侧边栏**分为"配置"和"工具"两个分组，底部有"修改密码"快捷按钮。JS 通过 `switchPanel(name)` 控制面板显示/隐藏，日志面板激活时自动开始轮询，切换离开后停止。

### 修改指南

- 直接编辑 `web_html.cpp` 中的 `R"rawliteral(...)rawliteral"` 块
- HTML 中的 `"` 在 `R"rawliteral"` 中不需要转义
- 需要动态内容的地方使用 `%PLACEHOLDER%`，在对应 handler 中 replace

---

## wifi_config.h — WiFi 凭据

```cpp
#define WIFI_SSID "liuwifi"
#define WIFI_PASS "Bairuiqin"
```

**修改**: 直接编辑此文件填入实际 WiFi 信息。  
**注意**: 此文件包含明文密码，请勿提交到公开仓库。
