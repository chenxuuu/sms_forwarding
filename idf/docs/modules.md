# 模块说明

## core

### log（`core/log.h`）

串口控制台 + Web 环形缓冲（120 行 × 256 字节）双写。

| 接口 | 说明 |
|---|---|
| `log_init()` | 创建互斥锁 |
| `log_write(fmt, ...)` | 追加内容，不提交行（等价旧版 `logCapture`） |
| `log_line(fmt, ...)` | 提交整行（等价 `logCaptureLn`） |
| `log_append_json(sbuf_t*)` | 导出为 JSON 字符串数组，供 `/log` 使用 |

行内容按 UTF-8 字符边界截断，保证导出的 JSON 始终是合法 UTF-8。

### config（`core/config.h`）

- NVS 命名空间 `sms_config`，键名与 Arduino 版本逐一对应（升级不掉配置）；
- 全部访问都必须 `config_lock()` / `config_unlock()`（递归锁）；
- 修改后调用 `config_save_locked()` 落盘；
- 提供 `config_push_channel_valid()`、`config_sender_is_admin()`、`config_sender_blocked()` 等语义接口。

### sbuf

动态字符串构建器，替代 Arduino `String` 拼接。OOM 时置 `oom` 标志并停止写入，不会崩溃。

### util

JSON 转义、URL 编码、Base64、HMAC-SHA256→Base64（钉钉/飞书签名）、UTC 毫秒时间戳。

## modem

### at_client

| 接口 | 说明 |
|---|---|
| `at_client_init()` | 安装 UART1（TX=3/RX=4/115200）+ 启动读任务 |
| `at_command()` | 发指令，收集应答直到 OK/ERROR/超时；期间 URC 转交回调 |
| `at_command_wait_for()` | 等到包含关键字的行（用于 `+MPING:` 这类上报） |
| `at_send_pdu()` | `AT+CMGS=<len>` → 等 `>` → PDU+0x1A → 等 OK |
| `at_set_urc_handler()` | 注册 URC 回调 |

注意：
- 开始新事务时**不重置**行缓冲，避免丢弃只收到一半的 URC；
- 事务期间的非应答行入 8 条深的 URC 队列，由 `at_reader` 统一解析（单一消费者模型）。

### pdu

自实现 GSM 03.40：

- `pdu_encode_submit()`：纯 ASCII → GSM 7bit；含中文 → UCS2；号码自动补 `86` 并置国际 TON；
  返回 TPDU 字节数（`AT+CMGS` 参数），`-1` 参数非法，`-2` 超出单条容量。
- `pdu_decode_deliver()`：解析 SMSC/发送方（含字母数字型）/DCS/SCTS/UDL/UDH，
  输出 UTF-8 文本与长短信 `ref/part/total`。
- 解码校验（防止畸形 PDU 产生乱码通知）：时间戳范围（月 1-12、日 1-31、时分秒范围）、
  DCS 必须是文本编码（GSM7/UCS2）、UDL 与实际数据长度一致、UDH 长度自洽。任一不满足即返回失败。

### sms_assembler

- 5 组 × 10 段缓存，按 `(参考号, 发送方)` 聚合；
- 收齐立即合并；30 秒未收齐则强制合并并标注缺失分段；
- 缓存满时覆盖最旧的一组；
- 合并缓冲在堆上（5KB），因为该函数可能在栈很小的 `esp_timer` 任务中运行。

### modem（服务层）

- 作业队列（初始化 / 发送短信 / 原始 AT / ping / 软重启 / 硬重启），作业对象在堆上传递；
- 初始化流程：EN 断电重启 → `AT` 握手 → `ATI` 识别型号 → `AT+CGACT=0,1`（ML307Y 跳过）
  → `AT+CNMI=2,2,0,0,0` → `AT+CMGF=0` → `AT+CEREG?` 等注册；
- `modem_ping()`：临时 `AT+CGACT=1,1` → `AT+MPING="8.8.8.8",30,1` → 解析 → 关闭数据连接。

### modem_hw

EN 引脚（GPIO5，低电平断电）与板载 LED（GPIO8，低电平点亮）。

## net

- `wifi_start()`：STA 连接，20s 超时；断线后 3s 退避重连；
- `wifi_get_info()`：SSID/IP/网关/掩码/DNS/MAC/BSSID/信道/RSSI；
- `wifi_reconnect()`：供 Web “重启 WiFi” 使用；
- `wifi_kick()`：非阻塞踢一次重连，供心跳使用；
- `time_sync_start()`：SNTP（ntp.ntsc.ac.cn / ntp.aliyun.com / pool.ntp.org）；
- `heartbeat_start(cb, current_ip)`：周期自检（本机 Web + 外网），失败时重连/重启，并通过回调
  通知「IP 变化」「网络恢复」；配置项 `CONFIG_HEARTBEAT_HOST/PORT/INTERVAL_S`。
**mDNS（`net/mdns.h`）**

| 接口 / 配置 | 说明 |
|---|---|
| `net_device_name(out, cap)` | 设备名：优先 `CONFIG_MDNS_NAME`，否则 `sms-<MAC 后两字节>` |
| `net_mdns_start()` | `mdns_init` + 主机名 + 实例名 `SMS Forwarder` + `_http._tcp`/80；失败仅记日志 |
| `CONFIG_MDNS_NAME` | 留空按 MAC 自动生成（多台同固件也唯一）；填写时各设备必须不同 |
| 依赖 | 组件仓库 `espressif/mdns`（版本锁定在 `dependencies.lock`） |

## notify

- 队列 + 工作任务的异步通知模型，调用方只投递；
- `notify_sms()`：一条短信 → 邮件 + 所有有效推送通道（文案"📱短信通知"）；
- `notify_system()`：系统事件 → 邮件 + 所有有效推送通道（文案"🔔 标题"）；
  用于启动、IP 变化、网络恢复、配置更新；
- `notify_email()`：仅邮件（管理员命令结果）；
- `notify_drain()`：重启前等待队列清空。

### email（SMTP）

- 465 端口隐式 TLS（`esp_tls` + CA bundle）；其它端口按明文处理（无 STARTTLS）；
- `AUTH LOGIN` 认证；主题非 ASCII 时按 RFC 2047 Base64 编码；正文 Base64 折行；
- 3 次尝试，认证失败立即停止。

### push（推送通道）

| 类型 | 值 | 关键字段 |
|---|---|---|
| POST JSON | 1 | url |
| Bark | 2 | url |
| GET | 3 | url |
| 钉钉机器人 | 4 | url + key1(secret，HMAC 签名) |
| PushPlus | 5 | key1(token) + key2(channel) |
| Server酱 | 6 | key1(sendkey) |
| 自定义模板 | 7 | url + 模板（`{sender}{message}{timestamp}`） |
| 飞书机器人 | 8 | url + key1(secret) |
| Gotify | 9 | url + key1(token) |
| Telegram | 10 | key1(chat_id) + key2(bot token)，url 可填反代前缀 |

响应体业务码校验（钉钉/飞书/PushPlus/Server酱/Telegram）与 4xx 不重试策略与旧版一致。

## forwarder

短信策略层，唯一知道“业务规则”的地方：

1. 黑名单命中 → 丢弃（支持 `+86` 前缀差异）；
2. 管理员号码 + `SMS:号码:内容` → 发短信并回执邮件；
3. 管理员号码 + `RESET` → 邮件通知 → 等待通知队列 → 硬重启模组 → 整机重启；
4. 其它 → `notify_sms()`。

## web

- `web_server_start()`：80 端口，Basic 认证，5 个 socket，LRU 清理；
- 首页：读取 `EMBED_TXTFILES` 内嵌的 `web/index.html`，替换 17 个 `%KEY%` 占位符，
  推送通道表单片段动态生成，按 2KB 分块发送；
- 长耗时接口用 `httpd_req_async_handler_begin()` 转独立任务，响应完成后
  `httpd_req_async_handler_complete()`；
- 请求体解析：`application/x-www-form-urlencoded`，字段按需更新（兼容旧版多表单提交）。

### 界面结构（web/index.html）

单文件内联 CSS/JS，通过 EMBED_TXTFILES 固化进固件；服务端只负责分块输出，不再做占位符替换：

| 数据 | 来源 |
|---|---|
| 表单初值 | GET /config |
| 状态卡片 | GET /status（8 秒轮询，页面隐藏时暂停） |
| 日志 | GET /log（3 秒轮询，内容未变不重建 DOM） |

交互约定（避免按钮/标签/开关混用）：

- 侧栏只做页面切换，顺序固定，不随使用频率重排
- 按钮只表示动作，文字以动词开头（查询信号 / 保存账号 / 执行指令）
- 开关只用于持久化布尔值，旁边必须显示 开/关 或 已启用/已停用
- 查询结果固定写入当前页的「查询结果」面板，失败也显示原因
- 日志：只追加新行；用户上翻时不自动滚动，改为底部「N 条新日志」气泡
- 主题：深色 / 浅色 / 跟随系统（默认跟随），偏好存 localStorage
