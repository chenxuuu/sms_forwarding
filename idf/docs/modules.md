# 模块说明

## core

### log（`core/log.h`）

串口控制台 + Web 环形缓冲（200 行 × 256 字节）双写。

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
- 选网字段：`operator_mode`（0 自动/1 手动）、`operator_code`（PLMN）、`operator_act`（0=2G/2=3G/7=LTE）；
  NVS 键 `operatorMode`/`operatorCode`/`operatorAct`，前两个与 Arduino 版同名，升级可直接沿用；
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
- 事务期间的非应答行入 8 条深的 URC 队列，由 `at_reader` 统一解析（单一消费者模型）；
- **每条 AT 交互压成一行日志**：`AT> 命令 → 应答摘要 [OK/ERROR/超时]`。
  这样 Web 日志页能看到完整 AT 往来（含飞行模式、ping、HTTP），又不会因逐行应答刷爆环形缓冲。
  URC 仍由 at_reader 单独记录；日志页有「AT指令」过滤 chip 只看交互。

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

- 作业队列（初始化 / 发送短信 / 原始 AT / ping / HTTP 访问 / 软重启 / 硬重启），作业对象在堆上传递；
- 初始化流程：EN 断电重启 → `AT` 握手 → `ATI` 识别型号 → `AT+CGACT=0,1`（ML307Y 跳过）
  → `AT+CNMI=2,2,0,0,0` → `AT+CMGF=0` → `AT+COPS`（按配置选网）→ `AT+CEREG?` 等注册；
- `modem_ping_ip(ip)`：`AT+MPING="<ip>",30,1` → 解析延迟/TTL；默认目标 `qq.com`，
  `modem_ping()` 是其兼容包装。**不执行 CGACT**（见下方说明）；
- `modem_http_get(url)`：保号流量操作——`AT+MHTTPDEL=0` → `AT+MHTTPCREATE="<scheme>://<host>"`
  → HTTPS 时 `AT+MHTTPCFG="ssl",0,1,0` → `AT+MHTTPCFG="encoding",0,1,1`（路径用 hex）
  → `AT+MHTTPREQUEST=0,1,0,<path hex>` → 等 `+MHTTPURC: "content"/"err"` → 删除实例；
  返回收到的 body 字节数。公网可达性取决于 SIM 运营商（默认 cloudflare 网址在国内蜂窝网络下常不可达）。
- **ML307Y 禁止 CGACT**：这颗模组执行 `AT+CGACT=1,1` / `CGACT=0,1` 会触发重启，
  重启后可能长时间搜不到网。保号操作直接依赖注册后可用的数据连接，不手动开关。
- 初始化时打开 `AT+CREG=2` / `AT+CEREG=2`，注册状态变化会以 URC 出现在日志里（相同状态去重）；
- 初始化末尾 `apply_operator_selection()`：自动模式 `AT+COPS=0`；手动模式
  `AT+COPS=1,2,"<PLMN>",<AcT>`（Act：0=2G / 2=3G / 7=LTE），最多等 60 秒；失败只记日志、继续等注册。

### netmon（蜂窝网络守护）

- 每 60 秒用 `AT`、`AT+CPIN?`、`AT+CREG?`、`AT+CEREG?`、`AT+CSQ`、`AT+CFUN?` 检查一次，
  全部是**信令查询，不发短信、不激活数据连接，零资费**；
- 连续 2 次确认未注册才判定掉线；已注册但连续 3 次 `CSQ=99`（读不到信号）同样视为射频卡死；
- 自愈阶梯：`AT+CFUN=4 → AT+CFUN=1 → AT+COPS`（射频重启 + 按配置重新选网；
  自动模式 `AT+COPS=0`，手动模式重发 `AT+COPS=1,2,"<PLMN>",<AcT>`，不会把手动配置覆盖成自动）
  → `AT+CFUN=1,1`（软重启，不再断电）→ EN 引脚断电（硬重启）；
- 三级都失败算一轮，冷却 5 分钟后从第 1 级重来，避免无 SIM 卡时无休止断电；
- 日常掉线自动重连**只写日志**；只有整轮（射频+软+硬重启）都失败时才发一次通知，
  内容包含掉线起始时间；之后若恢复，再发一条恢复通知；
- 用户手动开飞行模式（`CFUN=4`）时暂停检测，不会擅自把射频打开。

### modem_hw

EN 引脚（GPIO5，低电平断电）与板载 LED（GPIO8，低电平点亮）。

## net

- `wifi_start()`：STA 连接，20s 超时；断线后 3s 退避重连；
- `wifi_get_info()`：SSID/IP/网关/掩码/DNS/MAC/BSSID/信道/RSSI；
- `wifi_reconnect()`：供 Web “重启 WiFi” 使用；
- `wifi_kick()`：非阻塞踢一次重连，供心跳使用；
- `time_sync_start()`：SNTP（ntp.ntsc.ac.cn / ntp.aliyun.com / pool.ntp.org）；
  同时设置时区 `CST-8`（否则日志/通知里的时间会是 UTC，差 8 小时）；
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
- 邮件是否发送由 `config_mail_active()` 判定：邮件开关 `mail_enabled` 打开 **且** SMTP 四项齐全；
  关闭开关后短信照常转发到推送通道，只是不再发邮件；
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
- 推送通道改动即进入「未保存」状态：底部悬浮保存条 + 卡片内提示，离开页面/刷新会二次确认；
  保存邮件、账号等其它配置不会冲掉正在编辑的通道
- 确认框按钮必须写具体动作（留在此页 / 放弃修改并离开 / 确定重启），主色高亮安全选项，
  危险动作使用红色描边，禁止只写“确定/取消”
- 日志：只追加新行；用户上翻时不自动滚动，改为底部「N 条新日志」气泡；
  过滤 chip「全部/错误/短信/网络/通知/AT指令」，原始 AT 交互以 `AT>` 开头统一弱显示
- 开/关飞行模式后立即把模组就绪状态同步为 false，首页不再误报「已就绪」；
  关飞行模式后由 netmon 检测到重新注册再置 true
- 侧栏固定 7 个页面：概览 / 推送通道（邮件、推送通道、号码黑名单）/ 设备（选网配置、模组信息查询、维护）/
  保号发送（发短信、Ping、HTTP 访问）/ 后台管理（账号、管理员号码）/ 日志（日志 + AT 指令）/ 关于项目；
  「发短信」并入保号页，「AT 指令」并入日志页，避免功能分散
- 邮件通道可关闭：邮件卡片右上角「启用」开关，关闭后 notify_sms/notify_system 只发推送通道，
  状态标签显示「已关闭」；NVS 键 `mailEnable` 缺失时按开启处理，老配置升级后行为不变
- 主题：深色 / 浅色 / 跟随系统（默认跟随），偏好存 localStorage
- 「关于」页展示项目开源地址（https://github.com/chenxuuu/sms_forwarding）、固件版本与设备信息
