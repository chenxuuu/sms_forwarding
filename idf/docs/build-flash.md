# 编译、烧录与调试

## 1. 环境

- ESP-IDF **v6.1.1**（`C:\esp\v6.1.1\esp-idf`）
- 工具链目录 `C:\Espressif\tools`，Python 虚拟环境 `C:\Espressif\tools\python\v6.1.1\venv`
- 目标芯片 **ESP32-C3**（SuperMini 板，4MB Flash，USB-Serial/JTAG 控制台）

若 `export.ps1` 因缺少 `esp-clangd` 之类可选工具而激活失败，可直接设置环境变量后调用 `idf.py`：

```powershell
chcp 65001 > $null
$env:PYTHONUTF8='1'
$env:IDF_PATH='C:\esp\v6.1.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1.1\venv'
$env:ESP_IDF_VERSION='6.1.1'            # 缺这个 idf_component_manager 会报错
$env:ESP_ROM_ELF_DIR='C:\Espressif\tools\esp-rom-elfs\20260528\'
$env:PATH='C:\Espressif\tools\python\v6.1.1\venv\Scripts;C:\Espressif\tools\ninja\1.12.1;' +
          'C:\Espressif\tools\cmake\4.0.3\bin;' +
          'C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;' +
          'C:\Espressif\tools\idf-exe\1.0.3;' + $env:PATH
idf.py --version                        # 期望输出 ESP-IDF v6.1.1
```

## 2. 编译

```powershell
cd idf
idf.py build                 # 首次会自动按 sdkconfig.defaults 生成 sdkconfig（esp32c3）
```

- 修改 `sdkconfig.defaults` 后必须删除 `sdkconfig` 再编译，否则新默认值不会生效。
- 产物：`idf/build/sms_forwarder.bin`（约 1.2MB，app 分区 3MB）。
- 工程路径不能含中文（Arduino 版本同样的限制）；本仓库路径为纯 ASCII。

## 3. 烧录

```powershell
idf.py -p COM3 flash         # 按实际串口修改
```

分区表 `partitions.csv`：

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24KB |
| phy_init | data/phy | 0xF000 | 4KB |
| factory | app/factory | 0x10000 | 3MB |

## 4. 查看日志

```powershell
idf.py -p COM3 monitor
# 或使用仓库自带脚本（不会复位设备）
python tools/serial_capture.py COM3 60
```

> **供电**：长期挂机建议用独立 5V 电源。USB 供电时 PC 睡眠/重启会导致 USB 重新枚举，
> 芯片跟着复位（`复位原因: 11` 即 `ESP_RST_USB`），IP 也可能被 DHCP 重新分配。
>
> **重要坑位**：这块板子的控制台是芯片内置 USB-Serial/JTAG。用 pyserial/esptool 打开
> `COM3` 时 DTR/RTS 电平变化会让芯片复位（日志里 `复位原因: 11` 即 `ESP_RST_USB`）。
> 想观察设备“当前状态”或崩溃现场，请用 `tools/serial_capture.py`（打开后立即把 DTR/RTS 拉低），
> 或直接通过 `/log` 接口读取环形缓冲。

## 5. WiFi 配置（配网）

**没有运行时配网**：设备必须先联网才能提供 Web 配置页面，所以 WiFi 账号是编译期常量，
沿用旧固件的做法（旧版在 `code/wifi_config.h`）。

```powershell
idf.py menuconfig      # 短信转发器配置 → WiFi SSID / WiFi 密码
idf.py build flash
```

| 项 | 说明 |
|---|---|
| 配置符号 | `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` |
| 定义位置 | `main/Kconfig.projbuild`（默认值为空） |
| 生效位置 | `components/net/src/wifi.c` 的 `load_credentials()`，仅在启动连接时读取 |
| 持久化 | 值存在 `sdkconfig`（gitignore），NVS 不参与 |
| 修改方式 | 只能重新 menuconfig + 编译 + 烧录；Web 的「重启 WiFi」仅重连 |
| 未配置时 | 串口提示「设备未配置 WiFi…」，不进入重启循环，便于重新烧录 |

> 之所以把 Kconfig 默认值留空：真实 WiFi 密码不应该跟着仓库走。当前这台机器的
> `sdkconfig` 里已经有可用值，直接 `idf.py build` 仍然正常；**删掉 sdkconfig 后要重新填**。

## 6. 内存与 TLS 调优（sdkconfig.defaults）

| 配置 | 作用 |
|---|---|
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` | TLS 缓冲按需分配，握手后释放 |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192` | 容纳较长的服务器证书链 |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048` | 出方向缓冲减小 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16` 等 | 降低 WiFi 缓冲占用（省约 15KB） |

实测：空闲可用堆从 43KB 提升到 **55~68KB**，HTTPS 推送可正常握手。

## 7. 本机实测记录（ESP32-C3 + ML307Y + 联通卡）

| 项目 | 结果 |
|---|---|
| 启动 / WiFi / NTP | 正常，IP 192.168.1.8，SNTP 同步成功 |
| 模组初始化 | `ML307Y`，`ML307Y-DL-MBRH0S01`，注册成功（`+CEREG: 0,1`） |
| 网页 | 48KB 页面完整返回，占位符全部替换 |
| 发送短信（中文 UCS2） | `+CMGS: N` 成功，约 3~5 秒 |
| 接收短信（自发自收） | 网络侧延迟不定（实测 30 秒~5 分钟），到达后 `+CMT` 解码出中文与时间戳 |
| 邮件通知 | SMTP 465 发送成功 |
| 推送通道 | TLS 握手成功、返回 HTTP 200；平台侧返回业务失败（反代/机器人 token 属外部配置问题） |
| PDU 解码 | 8 条真实 PDU（含 2 段长短信、DCS=0x18）全部正确 |
| 长短信合并 | 开发期用临时注入接口喂入两段 PDU，按顺序合并为完整文本并进入推送（该注入接口已移除，避免误触发真实通知） |
| 畸形 PDU | 校验时间戳/DCS/UDL 一致性后拒绝，不会再解出乱码并推送 |
| 事务期间收短信 | 在 `AT+CMGL` 事务进行中到达的短信，经 URC 队列仍被正确解析并转发 |
| 过夜运行 | 连续运行 10 小时，WiFi 掉线 9 次均自动重连；期间 DHCP 把 IP 从 .8 换成 .6 |
| 心跳（外网不可达） | 3 次告警后重连 WiFi、6 次后仅告警不重启，130 秒观察期内 0 次重启 |
| HTTPS 自检 | `https://www.baidu.com` 200、`https://p.papapoi.com` 200、`https://api.telegram.org` 连接被重置（该网络不可达） |
| 配置保存 / 重启 | `/save` 落盘并回显；`/system?action=restart` 重启后自动重连、模组重新就绪 |

## 8. 一个容易踩的 TLS 坑

早期实现照搬 Arduino 版本的“不校验证书”，在 `esp_http_client` 里用了
`.skip_cert_common_name_check = true`。这个选项在 esp-tls 中会**连带关闭 SNI**，
于是 Cloudflare 这类共享 IP 的站点直接握手失败（`ESP_ERR_HTTP_CONNECT`），
而普通站点（如百度）看起来一切正常。

结论：**用 `crt_bundle_attach = esp_crt_bundle_attach` 做正常校验，不要设置
`skip_cert_common_name_check`**，SNI 才会正常发送。若确实需要连自签名内网服务，
再单独评估（会牺牲 SNI）。
