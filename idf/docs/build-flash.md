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
- 首次构建会从组件仓库拉取 `espressif/mdns`（版本记录在 `dependencies.lock`），
  需要能访问 `components.espressif.com`；离线环境会构建失败。

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

设备不可能在没网的时候打开 Web 页面，所以有两种给账号的方式，**优先用热点配网**：

### 5.1 热点配网（推荐，不需要编译环境）

| 步骤 | 说明 |
|---|---|
| 触发条件 | 启动时没有任何账号，或连续 3 次连不上已保存的账号（路由器改密码） |
| 热点 | `SMS-Forwarder-XXXX`，XXXX = MAC 后两字节，开放网络（无密码） |
| 页面 | <http://192.168.4.1/>，扫描附近 2.4G 网络并列出（按信号排序），也可手输 SSID |
| 保存后 | 写入 NVS namespace `sms_wifi`（键 `ssid` / `pass`），1.5 秒后自动重启入网 |
| 手机探测 | `/*` 通配把 `/generate_204`、`/hotspot-detect.html` 等 302 回首页，多数手机会自动弹出页面 |

实现：`components/net/src/wifi_portal.c`（`wifi_portal_start()`），AP+STA 模式，DHCP 服务由 esp_netif 提供。

### 5.2 编译期预置（适合批量烧录）

```powershell
idf.py menuconfig      # 短信转发器配置 → WiFi SSID / WiFi 密码
idf.py build flash
```

| 项 | 说明 |
|---|---|
| 配置符号 | `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD`（`main/Kconfig.projbuild`，默认空） |
| 读取优先级 | `wifi.c: load_credentials()` 先读 NVS，读不到才用 Kconfig |
| 持久化 | Kconfig 值在 `sdkconfig`（gitignore）；配网写入的值在 NVS，重启不丢 |
| 未配置时 | 不重启循环，直接进配网热点；串口同时打印热点名与 192.168.4.1 |

> 之所以把 Kconfig 默认值留空：真实 WiFi 密码不应该跟着仓库走。**删掉 sdkconfig 后要重新填**
> （或者干脆走热点配网，完全不用碰 sdkconfig）。

## 6. 内存与 TLS 调优（sdkconfig.defaults）

| 配置 | 作用 |
|---|---|
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` | TLS 缓冲按需分配，握手后释放 |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192` | 容纳较长的服务器证书链 |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048` | 出方向缓冲减小 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16` 等 | 降低 WiFi 缓冲占用（省约 15KB） |

实测：空闲可用堆从 43KB 提升到 **55~68KB**，HTTPS 推送可正常握手。

## 6.1 mDNS 解析不到怎么办

1. 确认手机/电脑连的是**同一个 WiFi（同一二层网段）**——mDNS 是链路本地组播，不能跨路由；
2. 名字是 `sms-<MAC 后四位的十六进制>`，以串口日志 `设备名: sms-xxxx` 为准；
3. Windows 10 1703+ 自带 mDNS 解析，失败时先 `ipconfig /flushdns` 再试；
4. 仍不行就直接用 IP（启动邮件 / `/log` 里都有 `设备地址`）；
5. 想固定名字：`menuconfig` → `mDNS 设备名`，多设备时每台填不同的。

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
| 系统通知 | 启动/心跳事件走「邮件 + 所有推送通道」，日志为 `开始多通道推送（N 个，系统通知）` |
| mDNS | 发布 `http://sms-9e3c.local/`；跨网段（本机在 192.168.31.x，设备在 192.168.1.x）无法解析，属链路本地限制 |
| HTTPS 自检 | `https://www.baidu.com` 200、`https://p.papapoi.com` 200、`https://api.telegram.org` 连接被重置（该网络不可达） |
| 配置保存 / 重启 | `/save` 落盘并回显；`/system?action=restart` 重启后自动重连、模组重新就绪 |
| 热点配网 | 清空 WiFi 账号后启动 → 热点 `SMS-Forwarder-9E3C` 起来、192.168.4.1 出页面、扫到 6 个邻居网络；POST `/save` 后重启，用 NVS 里的账号（含 `#` 的 SSID）连上并拿到原 IP |

## 8. 一个容易踩的 TLS 坑

早期实现照搬 Arduino 版本的“不校验证书”，在 `esp_http_client` 里用了
`.skip_cert_common_name_check = true`。这个选项在 esp-tls 中会**连带关闭 SNI**，
于是 Cloudflare 这类共享 IP 的站点直接握手失败（`ESP_ERR_HTTP_CONNECT`），
而普通站点（如百度）看起来一切正常。

结论：**用 `crt_bundle_attach = esp_crt_bundle_attach` 做正常校验，不要设置
`skip_cert_common_name_check`**，SNI 才会正常发送。若确实需要连自签名内网服务，
再单独评估（会牺牲 SNI）。

## 9. 其它实测与坑

| 场景 | 结论 |
|---|---|
| 动态通道 | 加到 6 个 -> /config 返回 6；缩回 5 后多余 NVS 键被删除 |
| 路由数量 | 注册了 24 个 URI，max_uri_handlers 曾为 20，导致 /wifi、/system 404；现为 32 且注册失败会打日志 |
| 界面交互 | agent-browser 实测：查询信号结果落面板、飞行模式开关回显 已开启/已关闭、主题三态切换正确 |
| 保存表单 | 50 个通道的表单可能超过 16KB，请求体上限已放宽到 40KB |
| 长短信乱码 | 7bit+UDH 的分段消息解出 `¡Ñ§ΩΛ` 乱码：UDH 结束后要从第 **49** bit（7 个 septet）开始解，按 48 bit 会整体错位 1 bit。已修，用 4 段真实 PDU + 构造的 4 段 GSM7 / 3 段 UCS2 用例回归 |
| 短信被应答打断 | `+CMT` 与 PDU 之间夹了 `+CPIN: READY` 时旧逻辑会丢掉整条短信（长短信缺第 1 段）。现在等待期间不中断，5 秒 / 16 行才放弃 |

## 10. 调试期关掉掉电检测（Brownout）

现象：串口反复打印 `Brownout detector was triggered` 然后重启，调试被打断。

4G 模组发射瞬间电流能冲到 2 A，USB 供电被拉低到 2.5 V 附近就会触发。
C3 默认阈值是 level 7（2.51 V，已是最低档），再往下没有可选项，只能在调试时关掉数字 BOD：

```
CONFIG_ESP_BROWNOUT_DET=n
```

已加在 `sdkconfig.defaults` 里（**发布固件前删掉**）。注意已有的 `sdkconfig` 不会自动跟随
defaults 变化，要么删掉 `sdkconfig` 重新生成，要么直接把里面的
`CONFIG_ESP_BROWNOUT_DET` 改成 `# CONFIG_ESP_BROWNOUT_DET is not set`。

两点限制：

- bootloader 还会打开**模拟 BOD**（`brownout_ll_ana_reset_enable(true)`，C3 ECO2 及以上固定行为），
  这个关不掉。真掉到 2.5 V 以下仍会**静默复位**，只是不再打印那条日志。
- 关掉后写 flash / OTA 期间掉压没有保护，可能写坏分区。调试完记得恢复。

顺带一提，频繁触发说明供电确实不够：换短一点的 USB 线、插到能给 1 A 以上的口，
或在模组供电脚并 1000 µF 以上的电容，比关检测更治本。
