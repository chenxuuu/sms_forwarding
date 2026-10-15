# AGENTS.md

低成本短信转发器：ESP32-C3 + ML307R 4G 模组（AT 固件），通过 WiFi 将收到的短信推送到邮件和多种平台。Arduino 框架，代码与文档均为中文。

## 构建与烧录

使用 `arduino-cli`（路径随机器不同，需自行确认），板型 FQBN: `esp32:esp32:makergo_c3_supermini`，串口按真实情况来。

```powershell
$env:Path = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources;$env:Path"
$env:ARDUINO_DIRECTORIES_DATA = "D:\dev\arduino_pack"
$env:ARDUINO_DIRECTORIES_USER = "D:\dev\arduino_pack\user"

# 编译（首次约 3-5 分钟，不要提前中断）
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini --build-path "D:\dev\arduino_pack\build" "<项目路径>\code"

# 烧录
arduino-cli upload --fqbn esp32:esp32:makergo_c3_supermini --port COM4 --input-dir "D:\dev\arduino_pack\build" "<项目路径>\code"

# 串口日志（115200）
arduino-cli monitor --port COM4 --config 115200
```

注意：
- 所有 arduino-cli 相关目录**不能含中文路径**，否则编译失败。
- 依赖库：pdulib 0.5.11、ReadyMail 0.4.2、ESP32 Core 3.3.10。
- `.github/agent/编译烧录实测.md` 是已验证的可靠流程，优先参考。
- 本机实际项目路径为 `C:\Users\chenx\Desktop\git-repos\sms_forwarding`（非 ASCII 路径仅限源码位置；build 目录必须放在纯英文路径）。

## 工程结构

```
code/
├── code.ino              # 入口: setup() + loop()
├── config_types.h        # 枚举/结构体/常量（最底层，无依赖）
├── globals.h/.cpp        # 全局变量 + 引脚定义 + 公共头文件
├── config.h/.cpp         # NVS 配置存取、有效性校验
├── modem.h/.cpp          # AT 指令、电源控制、短信发送
├── push.h/.cpp           # 多通道推送、邮件通知、加密/编码工具
├── sms_process.h/.cpp    # URC 解析、长短信合并、黑名单、管理员命令
├── web_handlers.h/.cpp   # HTTP 处理器 + 日志环形缓冲区
├── web_html.h/.cpp       # SPA HTML 模板（模板占位符 %KEY% 替换）
├── task_types.h          # 任务/结果结构体定义
└── wifi_config.h         # WiFi SSID/密码宏定义
dev_doc/                  # 架构/API/模块详细文档（改代码前先读）
.github/agent/            # 编译烧录流程文档
```

依赖方向：config_types.h → globals.h → 各模块 → code.ino。每个 .h/.cpp 对单一职责。

## 关键约定

- **日志输出必须用 `logCapture()` / `logCaptureLn()` / `logCaptureF()`**（web_handlers.cpp），不要直接用 Serial.print——这些函数同时写入串口和 Web 端可见的 120 行环形缓冲区。`logCapture` 不换行，`logCaptureLn` 提交整行。
- **配置持久化走 NVS**（Preferences，namespace `"sms_config"`）：新增配置字段需同步修改 config_types.h、saveConfig/loadConfig、handleSave 和 web_html.cpp 表单。
- **新增推送通道**：在 PushType 枚举加类型 → push.cpp 的 sendToChannel() 加 case → config.cpp 的 isPushChannelValid() 加校验 → web_html.cpp 加 UI 选项。
- 短信用 **PDU 模式**（中文短信必需），解析用 pdulib。
- 主循环是单线程 Arduino 模型，HTTP 与 URC 处理均为非阻塞；避免在 loop 路径中引入长阻塞。**推送/邮件是异步的**：loop 只把 NotifyJob 入队，由 push.cpp 中的 `notifyTask`（FreeRTOS 任务，core 0）执行 `sendSMSToServer`/`sendEmailNotification`；入队接口为 `queueSmsPush`/`queueEmailNotification`，重启前可用 `waitNotifyQueueEmpty` 等待发完。日志函数 `logCapture*` 已加互斥锁，可在任意任务中调用。
- 启动时主动 `AT+CGACT=0,1` 禁用 4G 数据连接以省流量，勿移除。
- Web UI 是单页应用（web_html.cpp 中字符串常量 + `%KEY%` 占位符），修改界面只动 web_html.cpp。

## 其他说明

- 默认 Web 账号 admin/admin123；管理员短信命令支持 `SMS:号码:内容` 和 `RESET`。
- 项目明确不做：多卡控制、通话、拨号、开放接口——勿实现此类需求。
