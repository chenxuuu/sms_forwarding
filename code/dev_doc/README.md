# 短信转发器 (SMS Forwarding) — 项目总览

## 项目简介

基于 **ESP32-C3 (MakerGO SuperMini)** 的短信转发设备。通过 4G/LTE 模组接收短信，经由 WiFi 网络将短信内容推送到邮件和多种即时通讯平台（微信/钉钉/飞书/Telegram 等）。提供 Web 管理界面进行配置和诊断。

## 硬件平台

| 组件 | 型号 / 规格 |
|---|---|
| MCU | ESP32-C3 (MakerGO SuperMini) |
| 4G 模组 | 通过 UART 连接，AT 指令控制 |
| 模组 EN 引脚 | GPIO 5 |
| 模组 TXD/RXD | GPIO 3 / GPIO 4 |
| LED | GPIO 8（内置 LED） |
| 串口波特率 | USB: 115200, 模组: 115200 |

## 第三方依赖库

| 库 | 版本 | 用途 |
|---|---|---|
| pdulib | 0.5.11 | PDU 格式短信编解码 |
| ReadyMail | 0.4.2 | SMTP 邮件发送 |
| Arduino ESP32 Core | 3.3.10 | WiFi / HTTP / mbedTLS / NTP |

## 核心功能

1. **短信接收** — 4G 模组 PDU 模式自动上报，解析后提取发送者/时间/内容
2. **长短信合并** — 支持多段长短信（最多 10 段）自动拼合，30 秒超时保护
3. **多通道推送** — 同时支持最多 5 个推送通道，可独立配置推送到不同平台
4. **邮件通知** — 通过 SMTP 发送通知邮件（QQ 邮箱等）
5. **Web 管理** — 单页应用 (SPA)，侧边栏导航 + HTTP Basic Auth 保护
6. **系统日志** — Web 端实时查看设备串口日志，自动刷新，最多保留 120 行
7. **管理员命令** — 通过短信远程执行 SMS:发送 和 RESET 命令（仅限管理员号码）
8. **号码黑名单** — 支持按号码过滤骚扰短信

## 支持推送平台

| 类型 | 枚举值 | 说明 |
|---|---|---|
| POST JSON | `PUSH_TYPE_POST_JSON` (1) | 通用 POST JSON，字段: sender / message / timestamp |
| Bark | `PUSH_TYPE_BARK` (2) | iOS 推送，字段: title / body |
| GET 请求 | `PUSH_TYPE_GET` (3) | 参数放在 URL query string |
| 钉钉机器人 | `PUSH_TYPE_DINGTALK` (4) | 支持加签安全验证 |
| PushPlus | `PUSH_TYPE_PUSHPLUS` (5) | 微信公众号推送，支持多渠道 |
| Server酱 | `PUSH_TYPE_SERVERCHAN` (6) | 微信推送 |
| 自定义模板 | `PUSH_TYPE_CUSTOM` (7) | 用户自定义 HTTP 请求体 |
| 飞书机器人 | `PUSH_TYPE_FEISHU` (8) | 支持签名验证 |
| Gotify | `PUSH_TYPE_GOTIFY` (9) | 自建推送服务 |
| Telegram | `PUSH_TYPE_TELEGRAM` (10) | Bot API |

## 工程结构

```
code/
├── code.ino              # 入口: setup() + loop()
├── config_types.h        # 枚举/结构体/常量定义（无依赖）
├── globals.h / .cpp      # 全局变量 + 引脚定义 + 公共头文件
├── config.h / .cpp       # NVS 配置存取、配置校验
├── modem.h / .cpp        # 模组 AT 指令、电源控制、短信发送
├── push.h / .cpp         # 多通道推送、邮件通知、加密工具函数
├── sms_process.h / .cpp  # 短信解析、长短信合并、黑名单、管理员命令
├── web_handlers.h / .cpp # HTTP 请求处理器 + 日志环形缓冲区
├── web_html.h / .cpp     # SPA HTML 页面模板（单页 10 个面板）
└── wifi_config.h         # WiFi SSID/密码（宏定义）
```

## 编译与烧录

```powershell
# 设置环境
$env:Path = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources;$env:Path"
$env:ARDUINO_DIRECTORIES_DATA = "D:\dev\arduino_pack"
$env:ARDUINO_DIRECTORIES_USER = "D:\dev\arduino_pack\user"

# 编译
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini --build-path "D:\dev\arduino_pack\build" "D:\dev\sms_forwarding\code"

# 烧录
arduino-cli upload --fqbn esp32:esp32:makergo_c3_supermini --port COM4 --input-dir "D:\dev\arduino_pack\build" "D:\dev\sms_forwarding\code"

# 串口监视
arduino-cli monitor --port COM4 --config 115200
```

## 启动流程

1. 初始化 GPIO（LED、模组 EN 引脚）
2. 启动 USB Serial (115200) 和模组 Serial1 (115200, GPIO 3/4)
3. 模组断电重启（EN 引脚控制）
4. 加载 NVS 配置，校验有效性
5. 模组初始化：AT 握手 → 禁用数据连接 → 配置短信 URC 上报 → 设置 PDU 模式 → 等待网络注册
6. 连接 WiFi，同步 NTP 时间
7. 启动 HTTP 服务器（端口 80），注册所有路由
8. 若配置有效，发送启动通知邮件

## 主循环

```
loop()
  ├── server.handleClient()     // HTTP 请求处理
  ├── 配置无效时每秒打印 IP
  ├── checkConcatTimeout()      // 长短信超时检查
  ├── Serial → Serial1 透传    // USB → 模组 AT 透传
  └── checkSerial1URC()        // 模组 URC 解析（短信上报）
```

## 关键设计决策

- **PDU 模式** 而非 Text 模式：中文短信必须使用 PDU 编码
- **CGACT 默认关闭**：启动时主动禁用 4G 数据连接，避免意外流量消耗（Ping 功能会临时激活）
- **NVS 持久化**：所有配置存储在 ESP32 非易失存储中，断电不丢失
- **单页应用 (SPA)**：Web UI 采用侧边栏 + 面板切换，所有功能在一个 HTML 页面中，通过 JS 切换可见面板
- **日志行缓冲**：`logCapture()` 将输出追加到行缓冲区，仅 `logCaptureLn()` 提交整行到环形缓冲区，避免非换行输出被误拆成多行
- **日志环形缓冲区**：120 行容量，循环覆盖，通过 `/log` 端点以 JSON 返回，Web 端每 2 秒自动刷新
- **模块化设计**：每个 .h/.cpp 对承担单一职责，便于增减功能和问题定位
- **HTML 模板分离**：HTML 字符串常量单独存于 `web_html.cpp`，便于 UI 修改而不影响逻辑代码
