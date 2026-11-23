# HTTP 接口

- 所有接口都需要 **HTTP Basic 认证**（账号密码即 Web 管理账号，默认 `admin/admin123`）。
- 未认证返回 `401` + `WWW-Authenticate`。
- JSON 响应统一为 `{"success": bool, "message": string}`，字符集 UTF-8。

## 异步与阻塞

`esp_http_server` 只有一个任务，所以耗时接口都改成"先把请求转异步、立即返回"：

| 接口 | 处理方式 |
|---|---|
| `/sendsms`、`/ping`、`/keepalive_http`、`/query`、`/flight`、`/at`、`/modem?action=signal\|operator\|imei`、`/decode`、`/selftest`、`/push_test`、`/mail_test` | 异步任务执行后回包，httpd 任务立即释放 |
| `/modem?action=restart\|hardreset`、`/wifi?action=restart`、`/system?action=restart` | 先回包，后台任务执行动作 |
| `/`、`/log`、`/save` | 同步，但不碰 AT，毫秒级完成 |

需要注意：**AT 串口仍然是独占资源**（模组作业队列深度 4）。例如一次 35 秒的 `/ping`
或最长 45 秒的 `/keepalive_http` 会让之后提交的 AT 作业排队，若排队等待超时则返回"超时或无响应"；
HTTP 服务本身不受影响，页面还能正常打开和刷新。

## 页面

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/tools`、`/sms` | 管理界面（单页，标签页切换） |
| GET | `/favicon.ico` | 空响应，避免日志噪声 |

单页内含 8 个标签页：概览、推送通道、设备、固件升级、保号发送（发短信 + Ping + HTTP 访问）、
后台管理、日志（含 AT 指令）、关于项目。关于页为纯前端页面，
展示项目开源地址（https://github.com/chenxuuu/sms_forwarding）、固件版本、设备型号与设备名，
不新增后端接口。

## 配置

### POST /save

`application/x-www-form-urlencoded`，只更新出现的字段（对应页面上的分表单提交）：

| 字段 | 说明 |
|---|---|
| `webUser` / `webPass` | 管理账号（留空恢复默认 `admin/admin123`） |
| `mailEnabled` | 邮件通道开关：`on`/`off`（不传则保持原值） |
| `smtpServer` / `smtpPort` / `smtpUser` / `smtpPass` / `smtpSendTo` | 邮件通知 |
| `adminPhone` | 管理员号码（可下发 `SMS:`/`RESET`/`RESETPASS` 命令） |
| `selfPhone` | 本机（SIM 卡）号码，用于通知文案与 `{self_phone}` 模版变量；留空则每次启动尝试 `AT+CNUM` 自动获取 |
| `numberBlackList` | 号码黑名单，换行分隔 |
| `operatorMode` | 选网模式：0=自动（默认）/ 1=手动 |
| `operatorCode` | 手动模式的 PLMN，5~6 位数字（如 46000） |
| `operatorAct` | 手动模式网络类型：0=2G(GSM) / 2=3G(UTRAN) / 7=LTE（默认） |
| `pushNen` / `pushNtype` / `pushNname` / `pushNurl` / `pushNkey1` / `pushNkey2` / `pushNbody` | 第 N 个推送通道（N=0..4） |

返回 3 秒后跳回首页的提示页。手动选网若 PLMN 非 5~6 位数字、或网络类型非法，
接口直接返回 JSON 错误且不落盘。保存选网配置后需重启设备，在模组初始化时通过 `AT+COPS` 生效。

### GET /status

前端轮询用的轻量状态（不碰 AT，毫秒级）：`ip/ssid/rssi/heap_kb/uptime_s/modem_ready/email_ok/mail_enabled/self_phone/push_count/device_name/model`，
外加固件相关的 `version`（当前版本，`YYYYMMDD.HHMM`）、`ota_state`（0 空闲/1 接收中/2 校验中/3 等待重启/4 失败）。
有没有新版本由浏览器比对（见「固件升级」），设备端不再保存远端版本。
`mail_enabled` 是邮件通道开关；`email_ok` 表示「开关打开且 SMTP 四项配置齐全」（即 config_mail_active），
关闭后即便 SMTP 填好也不再发邮件。

模组字段来自网络守护的最近一次检测（每 60 秒）：
`net_stat`（0 未注册未搜索 / 1 已注册本地 / 2 搜索中 / 3 被拒绝 / 4 未知 / 5 漫游 / -1 查询失败）、
`net_csq`（0..31，99 无信号，-1 未知）、`net_sim`（CPIN 是否 READY）。
`modem_ready` 已是「初始化完成 + 当前注册正常」，掉线自愈期间为 `false`。

### GET /config

返回完整配置快照（含 5 个推送通道），前端据此渲染表单，避免服务端拼 HTML。
另含本机号码 `self_phone`（`AT+CNUM` 自动获取或后台手填，可能为空）、
邮件开关 `mail_enabled`（布尔，缺省视为开启），以及选网字段 `operator_mode`（0 自动/1 手动）、`operator_code`（PLMN）、`operator_act`（0=2G/2=3G/7=LTE）。
密码等敏感字段也在其中，接口本身受 Basic 认证保护。

### POST /save?json=1

与 `/save` 相同，但返回 JSON 而不是跳转页，供前端 AJAX 使用（只更新请求里出现的字段）。

## 短信

### POST /sendsms

| 参数 | 说明 |
|---|---|
| `phone` | 目标号码 |
| `content` | 内容（UTF-8） |

加 `?json=1` 时返回 JSON（`{"success":..,"message":..}`），否则返回 HTML 结果页。

返回 HTML 结果页（3 秒后跳回 `/sms`）。单条上限 70 个汉字 / 160 个英文字符。

### GET /query?type=…

| type | 返回内容 |
|---|---|
| `ati` | 制造商 / 型号 / 固件版本 |
| `signal` | RSRP/RSRQ 与强度评价（AT+CESQ） |
| `siminfo` | IMSI / ICCID（自动兼容 `AT+ICCID`、`AT+QCCID`）/ 本机号码 |
| `network` | 注册状态 / 运营商 / 数据连接 / APN |
| `wifi` | SSID / RSSI / IP / 网关 / 掩码 / DNS / MAC / BSSID / 信道 |

`message` 字段为可直接插入页面的 HTML 表格片段。

## 模组控制

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/at?cmd=…` | 执行任意 AT 指令，返回原始应答（最长 4KB） |
| GET | `/flight?action=query|toggle|on|off` | 飞行模式（AT+CFUN） |
| GET | `/modem?action=signal|operator|imei` | 信号/运营商/序列号 |
| GET | `/modem?action=restart|hardreset` | 软重启（CFUN=1,1）/ 硬重启（EN 断电），立即返回，后台执行 |

> AT 指令里的 `+` 在 URL 中必须写成 `%2B`（或使用 `encodeURIComponent`）。

## 系统与诊断

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/log` | 最近 200 行日志（JSON 字符串数组）；含 `AT> 命令 → 应答 [结果]` 格式的原始 AT 交互，前端可按「AT指令」过滤 |
| GET | `/decode?pdu=<hex>` | 解析一条 SMS-DELIVER PDU，返回发送者/时间/文本/长短信信息 |
| GET | `/selftest?url=<https地址>` | HTTPS 连通性自检（CA bundle / TLS / DNS），默认测试 baidu |
| POST | `/push_test` | 用请求里的通道参数发一条测试消息，用于验证新通道是否可用 |
| POST | `/mail_test` | 用请求里的 SMTP 参数发一封测试邮件（不读已保存配置） |
| GET | `/wifi?action=restart` | 断开并重连 WiFi（立即返回，后台执行） |
| GET | `/system?action=restart` | 重启整机（立即返回） |
| POST | `/ping?ip=<目标>` | 保号 Ping：ping 指定 IP/域名（默认 `qq.com`），不碰 CGACT（约 40s，异步） |
| GET | `/keepalive_http?url=<网址>` | 保号访问：通过模组 MHTTP 主动 GET 指定 http/https 网址，返回收到的 body 字节数（异步）；会产生流量 |

### /decode 响应示例

```json
{
  "success": true,
  "sender": "10010",
  "timestamp": "26/09/07 15:55:44",
  "text": "【河南省民政厅】...",
  "dcs": 8,
  "concat": { "ref": 1, "part": 1, "total": 2 }
}
```

### GET /flight?action=state

返回飞行模式的真实状态，供网页开关回显：

    {"success":true,"cfun":1,"flight":false}

cfun=1 全功能、cfun=4 飞行模式（射频关闭）。

开/关飞行模式后立即把模组就绪状态同步为未就绪（首页不再误报「已就绪」），
关闭后由 netmon 检测到重新注册成功再恢复为就绪。

## 配置变更通知

/save 保存前对 6 个分区做 FNV-1a 哈希：账号密码 / 邮件设置 / 管理员号码 / 号码黑名单 / 推送通道 / 选网配置，
保存后逐项比对：

| 结果 | 行为 |
|---|---|
| 什么都没变 | 不发通知；JSON 返回「配置未变化」 |
| 有分区变化 | 发送「短信转发器配置已更新」，正文列出变更项，如「· 推送通道（当前 6 个）」 |
| 账号或密码变化 | 正文追加「管理账号已修改，请使用新账号密码登录。」 |
| 选网配置变化 | 正文追加「选网配置已保存，重启设备后生效。」 |

通知通过 notify_system() 发送到邮件 + 所有已启用推送通道。

## 动态推送通道

模板与载荷里的本机号码：自定义模板（type=7）支持 `{sender}` `{message}` `{timestamp}` `{self_phone}`；
POST JSON 载荷多一个 `self_phone` 字段，GET 多一个 `self_phone` 查询参数，
Bark 的 body 与钉钉/飞书/PushPlus/Server酱/Gotify/Telegram 的正文末尾会自动追加「本机号码: …」（未配置时省略）。

### POST /push_test

用请求体里的通道参数**直接**发一条测试消息，不用先保存，也不受「启用」开关影响——
方便添加新通道后立刻验证 URL/密钥是否可用。请求体为 `application/x-www-form-urlencoded`：

| 字段 | 说明 |
|---|---|
| `type` | 推送类型 1..10 |
| `name` | 通道名称，仅用于日志和返回文案 |
| `url` / `key1` / `key2` / `body` | 该类型对应的 URL、密钥与自定义模板 |

参数校验复用 `config_push_channel_valid()`，缺必填项时返回「通道参数不完整」；
推送失败（含平台返回业务失败）返回 `success:false` 与原因，方便直接定位问题。

### POST /mail_test

用请求体里的 SMTP 参数直接发一封测试邮件，同样不需要先保存（字段名与 /save 一致：
`smtpServer` / `smtpPort` / `smtpUser` / `smtpPass` / `smtpTo`），端口缺省按 465（SSL）处理。
四项任一为空返回「邮件配置不完整」；发送失败时日志里能看到具体阶段（连接 / 认证 / DATA / 服务端拒收）。

## 固件升级

### GET /ota

返回升级状态，前端进入「固件升级」页时拉一次：

```json
{"success":true,"current":"20261123.1033","target":"","state":0,"progress":0,"message":""}
```

- `current`：当前运行版本，浏览器用它和 GitHub 上的最新版本比较
- `target`：已写入、等待重启后生效的版本
- `state` / `progress` / `message`：升级进度与最近一次结果（失败原因给页面直接显示）

远端版本信息**不在设备端**：页面直接请求
`https://api.github.com/repos/chenxuuu/sms_forwarding/releases/latest`，读 `name`（Release 标题就是版本号）
和 `assets` 里的下载地址，结果缓存在浏览器 `localStorage`（12 小时）。

### POST /ota

上传固件。**必须**带 `Content-Length`，请求体是 `sms_forwarder-ota.bin` 的原始字节，
`Content-Type` 建议 `application/octet-stream`（不接受 multipart 表单）。

| 情况 | 响应 |
|---|---|
| 成功 | 200 `{"success":true,"version":"20261124.1200","message":"固件已写入，设备即将重启"}`，约 2 秒后重启 |
| 缺 Content-Length | 411 |
| 超过分区容量 | 413，且不会擦写 flash |
| 不是 app 镜像（如 merged 包） | 400 `这不是可用的 ota 固件…`，当前固件不受影响 |
| 传输中断 / 校验失败 | 400，另一个分区仍保留原固件 |
| 正在升级 | 409 |

写入的是非活动分区，失败不影响当前固件；写完后 `esp_ota_set_boot_partition` 并等待重启。

### 版本检查（不在设备端）

设备**不联网检查版本**，也没有 `/ota_check` 接口（旧固件有，现已删除，请求返回 404）。
页面打开「固件升级」页时直接 fetch GitHub API，用 `name` 和 `GET /ota` 的 `current` 比新旧：

- 有新版本：显示最新版本、下载链接、侧边栏红点
- 12 小时内不重复请求（避开 GitHub API 的 60 次/小时/IP 限流）
- 请求失败（离线、被墙、限流）只在页面提示，不影响上传升级

好处是设备端省掉一次 TLS 握手：`ota` 组件的 8 KB 工作任务和 HTTP 客户端都已删除，
实测空闲堆从 24 KB 回到 40 KB。

## 定时任务

### GET /tasks

返回全部任务（最多 8 个）：

```json
{"success":true,"tasks":[{"index":0,"enabled":true,"type":2,"phone":"","text":"",
  "host":"qq.com","url":"","next":1795417327,"repeat_days":3,"last":0}]}
```

`type`：1=发送短信，2=Ping，3=访问网址；`next`/`last` 为 epoch 秒；`repeat_days` 为 0 表示单次。

### POST /tasks

`application/x-www-form-urlencoded`，`action=save|delete`：

| 字段 | 说明 |
|---|---|
| `action` | `save`（默认）/ `delete` |
| `index` | 任务下标；`-1` 表示新增 |
| `type` | 1=短信 / 2=Ping / 3=HTTP |
| `next` | 执行时间（epoch 秒） |
| `repeat` | 循环天数，0=只执行一次，上限 365 |
| `enabled` | `on`/`off` |
| `phone` / `text` / `host` / `url` | 对应该类型的参数 |

校验：类型非法、未选时间、短信缺号码/内容、网址不以 http(s) 开头都会拒绝；
时间已过时仍接受，返回「任务已保存：时间已过，会在下一轮检查时立即补执行」。

/config 返回的 channels 数组长度即当前通道数（1..50，默认 5）。
POST /save 通过 pushCount 字段调整数量：增加时新通道名为「未命名通道」，
减少时设备会删除多余通道在 NVS 中的键。表单字段仍为 pushN + en/type/name/url/key1/key2/body。
