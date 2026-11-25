# AGENTS.md

低成本短信转发器：ESP32-C3 + ML307R 4G 模组（AT 固件），通过 WiFi 将收到的短信推送到邮件和多种平台。Arduino 框架，代码与文档均为中文。

## 两套实现

本仓库有两个可独立编译的实现：

- `code/` —— Arduino 框架版本（历史实现，下文流程均针对它）
- `idf/` —— ESP-IDF v6.1.1 版本（开发主线，架构/接口/烧录文档在 `idf/docs/`）

### ESP-IDF 版本速查

```powershell
cd idf
idf.py build                            # 目标 esp32c3，默认配置见 sdkconfig.defaults
idf.py -p COM3 flash
python tools/serial_capture.py COM3 60  # 抓日志（直接 monitor/开串口会让芯片复位）
```

- 环境变量与常见坑（`ESP_IDF_VERSION`、`esp-clangd` 缺失、COM 口复位）见 `idf/docs/build-flash.md`。
- 关键约定：日志必须走 `log_write()` / `log_line()`（否则 Web 日志页看不到）；
  配置读写必须 `config_lock()` / `config_unlock()`，改完调用 `config_save_locked()`。
- 新增推送通道：`push_type_t` → `push.c` 的 `build_request()` → `config_push_channel_valid()` → Web 表单选项。
  推送失败的重试只看「可能没送到」（status=0 / 5xx / 429）；2xx 已送达不要重试，否则一条短信推 2~3 条。
  校验响应体要在 `HTTP_EVENT_ON_DATA` 里自己攒——`esp_http_client_perform()` 会把 body 读走。
- **短信 URC 依赖 +CNMI**：ML307Y 在 `AT+CFUN=1` 后约 8 秒（SIM 重新 READY）会把 `+CNMI` 清回
  `0,0,0,0,0`，此后一条 `+CMT` 都不报。`rf_cycle()` 必须等 SIM READY 再多等 3 秒才下发，
  `netmon` 每轮用 `AT+CNMI?` 兜底复核（见 `modem_sms_config_check()`）。
- **Web 接口字段名**：`/config` 里通道模板的 JSON 键是 `custom_body`（同结构体），
  而提交表单用的是 `pushNbody`；两者名字不同，改前端/后端时务必对上（issue #77 就是这么来的）。
- **内存红线**：`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 不能低于 8192——服务器会把整条证书链
  塞进一个 TLS 记录（smtp.qq.com 5489 字节），4096 会握手失败。WiFi 动态收发缓冲 8/8，
  日志环 200 行 × 192 字节，`web_async` 默认栈 6 KB（TLS 类 8 KB、发短信 10 KB）。
- **分区表红线**：`partitions.csv` 里 nvs 的 `0x9000/0x6000` 不能动（老设备配置都在这），
  两个 app 槽各 `0x1F0000`。老设备迁移必须 `idf.py flash` 或分段写 `0x0/0x8000/0x20000`；
  **禁止用 merged bin 覆盖 `0x0`**——它会用 0xFF 填满空隙，把 NVS 抹掉。
- OTA：版本号来自 `PROJECT_VER`（`YYYYMMDD.HHMM`，CI 注入）；上传的镜像必须校验
  `esp_app_desc_t.project_name == "sms_forwarder"`，否则用户会把 merged 包写进 app 槽；
  新固件联网稳定 45 秒后才 `esp_ota_mark_app_valid_cancel_rollback()`，别提前确认。
  版本检查在**浏览器侧**做（页面 fetch `api.github.com` 的 Release `name`），设备不联网、不跑 TLS，
  因此没有 `CONFIG_OTA_MANIFEST_URL` 和 `/ota_check`；CI 必须把 Release 标题写成版本号，否则页面判不了新旧。
- eSIM：组件 `idf/components/esim/` 做 UICC 侧管理：EID / `BF2D` 列表 / `BF31` 启用 /
  `BF33` 删除（仅限未启用），没有 `BF34` 复位。删除必须保留两道闸：固件读列表后拒绝删
  **使用中**的配置，网页还要手输 ICCID 确认。**`BF33` 的 CHOICE 直接放在 BF33 里，不能再套 `A0`**
  （套了 eUICC 回结果码 1「找不到」）；`BF31` 目前带 `A0`，实测可用。
  APDU 走 `AT+CCHO`/`AT+CGLA`/`AT+CCHC`，长应答必须用 `at_command_capture()`（`AT_LINE_MAX` 只有 512）；
  `AT+CCHC` 之后模组会补一条 `+CCHC` 上报，可能撞进下一次 `CCHO`，所以开通道失败要重试一次。
  新配置的下载不在固件里做（BPP 几十 KB + TLS，C3 跑不动），走 `tools/esim_bridge.py` + lpac
  （**Windows 用 lpac 2.2.1**，2.3.0 的 stdio 后端坏；桥接必须用二进制管道），
  固件只暴露 `/esim/apdu` 透传；切卡成功后必须软重启模组（`AT+CFUN=1,1`）重新注册。
  完整导入/删除步骤见 `idf/docs/esim-import.md`。
  C3 堆碎片化严重：`+CGLA` 应答缓冲只能用 1280 字节（512 字节应答转十六进制），
  异步任务栈 4 KB，起不来时必须回显式错误而不是让请求悬着。
  验证 eSIM 是否正常**看能否读到 EID/ICCID**，不要等驻网——外卡漫游附着慢属正常现象。
- NVS 命名空间与键名和 Arduino 版本完全一致，可从 Arduino 固件原地升级。
- WiFi 账号优先级：NVS `sms_wifi`（`ssid`/`pass`，热点配网写入）> menuconfig 的 `CONFIG_WIFI_*`；
  用到编译期账号时**首次启动会写进 NVS**，否则 OTA 到不带 `CONFIG_WIFI_*` 的固件（CI 包）会断网进配网；
  没有账号或连续 3 次连不上时进配网模式（`net/wifi_portal.c`，热点 `SMS-Forwarder-XXXX`，192.168.4.1）。
  配网 httpd 与主 Web 不会同时占用 80 端口。

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

## 仓库结构

```
README.md                     # 门面：原理图、截图、硬件采购、快速开始（功能变了要同步）
AGENTS.md                     # 本文件
assets/
├── photo.png                 # 实物图
├── flow.svg                  # 原理图（SMIL 动画，README 首屏）
├── wiring.svg                # 接线图（README 硬件章节）
├── web-overview.png          # 后台截图：概览页
├── web-esim.png              # 后台截图：eSIM 管理页（demo 假数据，不含真实 EID/ICCID）
├── web-push.png              # 后台截图：推送通道页
└── demo.html                 # 纯前端演示页：注入 fetch 模拟层，所有接口返回假数据，
                              可直接双击打开；不要出现“演示”字样；index.html 改动后要重新生成
idf/                          # 主线实现（ESP-IDF v6.1.1），文档在 idf/docs/
code/                         # Arduino 框架实现（历史版本，功能与 idf 对齐）
├── README.md                 # Arduino 版说明（硬件/接线/Arduino IDE 编译）
├── dev_doc/                  # Arduino 版架构/API/模块文档
├── code.ino                  # 入口: setup() + loop()
├── config_types.h            # 枚举/结构体/常量（最底层，无依赖）
├── globals.h/.cpp            # 全局变量 + 引脚定义 + 公共头文件
├── config.h/.cpp             # NVS 配置存取、有效性校验
├── modem.h/.cpp              # AT 指令、电源控制、短信发送
├── push.h/.cpp               # 多通道推送、邮件通知、加密/编码工具
├── sms_process.h/.cpp        # URC 解析、长短信合并、黑名单、管理员命令
├── web_handlers.h/.cpp       # HTTP 处理器 + 日志环形缓冲区
├── web_html.h/.cpp           # SPA HTML 模板（模板占位符 %KEY% 替换）
├── task_types.h              # 任务/结果结构体定义
└── wifi_config.h             # WiFi SSID/密码宏定义
.github/workflows/
├── firmware.yml              # 推送 master 后编译 idf，合并单文件固件发到 latest Release
├── build.yml                 # 校验 Arduino 版可编译
└── pages.yml                 # 把 assets/demo.html 作为 index.html 发布到 GitHub Pages
.github/agent/                # 编译烧录流程文档
```

依赖方向（code/）：config_types.h → globals.h → 各模块 → code.ino。每个 .h/.cpp 对单一职责。

## 关键约定

- **日志输出必须用 `logCapture()` / `logCaptureLn()` / `logCaptureF()`**（web_handlers.cpp），不要直接用 Serial.print——这些函数同时写入串口和 Web 端可见的 120 行环形缓冲区。`logCapture` 不换行，`logCaptureLn` 提交整行。
- **配置持久化走 NVS**（Preferences，namespace `"sms_config"`）：新增配置字段需同步修改 config_types.h、saveConfig/loadConfig、handleSave 和 web_html.cpp 表单。
- **新增推送通道**：在 PushType 枚举加类型 → push.cpp 的 sendToChannel() 加 case → config.cpp 的 isPushChannelValid() 加校验 → web_html.cpp 加 UI 选项。
- 短信用 **PDU 模式**（中文短信必需），解析用 pdulib。
- 主循环是单线程 Arduino 模型，HTTP 与 URC 处理均为非阻塞；避免在 loop 路径中引入长阻塞（模组初始化等慢操作已有异步化结构，见 task_types.h）。
- 启动时主动 `AT+CGACT=0,1` 禁用 4G 数据连接以省流量，勿移除。
- Web UI 是单页应用（web_html.cpp 中字符串常量 + `%KEY%` 占位符），修改界面只动 web_html.cpp。

## 其他说明

- 默认 Web 账号 admin/admin123；管理员短信命令支持 `SMS:号码:内容`、`RESET` 和 `RESETPASS`（重置后台密码并推送默认账号密码）。
- 项目明确不做：多卡控制、通话、拨号、开放接口——勿实现此类需求。