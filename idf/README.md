# 短信转发器（ESP-IDF 版）

ESP32-C3 SuperMini + ML307R（ML307Y）4G 模组的低成本短信转发器：
收到短信后通过 **邮件 + 多种 HTTP 推送通道** 通知，并提供内嵌 Web 管理界面。

本目录是从 Arduino 版本（仓库根目录 `code/`）移植到 **ESP-IDF v6.1.1** 的完整实现，
使用官方组件模型、FreeRTOS 任务与 `esp_http_server` 重写。

## 快速开始

```bash
# 1. 编译
idf.py -C idf build          # 或在 idf/ 目录下直接 idf.py build

# 2. 烧录（按实际串口修改）
idf.py -C idf -p COM3 flash

# 3. 查看日志（USB-Serial/JTAG 控制台）
python idf/tools/serial_capture.py COM3 60
```

首次启动后访问 `http://<设备IP>/`（默认账号 `admin` / `admin123`）配置邮件与推送通道。

## 配网（WiFi）

WiFi 账号是**编译期配置**，没有运行时配网页面——设备要先联网才能打开 Web 页面，是个鸡生蛋问题，
所以沿用旧固件的做法（旧版在 `code/wifi_config.h`）。

```powershell
cd idf
idf.py menuconfig      # 短信转发器配置 → WiFi SSID / WiFi 密码
idf.py build flash
```

- 配置项：`CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD`（定义在 `main/Kconfig.projbuild`）。
- 实际值保存在 `sdkconfig` 里，该文件已被 `.gitignore` 忽略，**不会把家里的 WiFi 密码提交到仓库**；
  Kconfig 的默认值是空的，换机器或删掉 sdkconfig 后必须重新填写。
- 只支持 2.4G；注意 `SSID` 里含 `#`、空格等字符时要在 sdkconfig 里用引号包起来（Kconfig 生成的格式已带引号）。
- Web 页面上的「重启 WiFi」只是断开重连，**不能改账号**；改账号只能重新编译烧录。
- 忘了配置时不会反复重启：串口会打印「设备未配置 WiFi…」，方便你 menuconfig 后重新烧录。

## 目录结构

```
idf/
├── CMakeLists.txt          # 顶层工程
├── sdkconfig.defaults      # 默认配置（含内存/TLS 调优）
├── partitions.csv          # 4MB Flash 分区表
├── main/                   # 启动流程与业务装配
├── components/
│   ├── core/               # 日志、NVS 配置、字符串工具
│   ├── modem/              # AT 通道、PDU 编解码、长短信合并、模组作业队列
│   ├── net/                # WiFi 管理、SNTP 时间同步
│   ├── notify/             # 邮件（SMTP over TLS）、HTTP 推送通道
│   ├── forwarder/          # 转发策略：黑名单、管理员命令、通知分发
│   └── web/                # esp_http_server + 内嵌前端页面
├── tools/serial_capture.py # 串口抓取脚本（不会复位设备）
└── docs/                   # 开发文档
```

## 开发文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 分层架构、任务模型、关键数据流 |
| [docs/modules.md](docs/modules.md) | 各组件职责与接口 |
| [docs/api.md](docs/api.md) | HTTP 接口说明 |
| [docs/build-flash.md](docs/build-flash.md) | 编译、烧录、调试与实测记录 |

## 与 Arduino 版本的差异

- **配置兼容**：NVS 命名空间 `sms_config` 与键名完全一致，从 Arduino 固件升级后无需重新配置。
- **自实现 PDU 编解码**：替代 pdulib，支持 GSM 7bit / UCS2、长短信 UDH、字母数字发送方。
- **自实现 SMTP 客户端**：替代 ReadyMail，基于 `esp_tls`（465 端口隐式 TLS）。
- **Web 界面按块输出**：不申请 40KB 连续内存，避免 C3 上大块分配失败。
- **长耗时接口异步化**：发送短信、ping、模组重启等通过 `httpd_req_async_handler_begin` 处理，不阻塞 HTTP 服务。
- **限制**：单条短信发送最大 70 个汉字 / 160 个英文字符（与旧版能力一致，超长会明确报错而不是静默失败）。
