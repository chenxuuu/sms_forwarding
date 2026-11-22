# HTTP 接口

- 所有接口都需要 **HTTP Basic 认证**（账号密码即 Web 管理账号，默认 `admin/admin123`）。
- 未认证返回 `401` + `WWW-Authenticate`。
- JSON 响应统一为 `{"success": bool, "message": string}`，字符集 UTF-8。

## 异步与阻塞

`esp_http_server` 只有一个任务，所以耗时接口都改成"先把请求转异步、立即返回"：

| 接口 | 处理方式 |
|---|---|
| `/sendsms`、`/ping`、`/query`、`/flight`、`/at`、`/modem?action=signal\|operator\|imei`、`/decode`、`/selftest` | 异步任务执行后回包，httpd 任务立即释放 |
| `/modem?action=restart\|hardreset`、`/wifi?action=restart`、`/system?action=restart` | 先回包，后台任务执行动作 |
| `/`、`/log`、`/save` | 同步，但不碰 AT，毫秒级完成 |

需要注意：**AT 串口仍然是独占资源**（模组作业队列深度 4）。例如一次 35 秒的 `/ping`
会让之后提交的 AT 作业排队，若排队等待超时则返回"超时或无响应"；HTTP 服务本身不受影响，
页面还能正常打开和刷新。

## 页面

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/tools`、`/sms` | 管理界面（单页，标签页切换） |
| GET | `/favicon.ico` | 空响应，避免日志噪声 |

## 配置

### POST /save

`application/x-www-form-urlencoded`，只更新出现的字段（对应页面上的分表单提交）：

| 字段 | 说明 |
|---|---|
| `webUser` / `webPass` | 管理账号（留空恢复默认 `admin/admin123`） |
| `smtpServer` / `smtpPort` / `smtpUser` / `smtpPass` / `smtpSendTo` | 邮件通知 |
| `adminPhone` | 管理员号码（可下发 `SMS:`/`RESET` 命令） |
| `numberBlackList` | 号码黑名单，换行分隔 |
| `pushNen` / `pushNtype` / `pushNname` / `pushNurl` / `pushNkey1` / `pushNkey2` / `pushNbody` | 第 N 个推送通道（N=0..4） |

返回 3 秒后跳回首页的提示页。

## 短信

### POST /sendsms

| 参数 | 说明 |
|---|---|
| `phone` | 目标号码 |
| `content` | 内容（UTF-8） |

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
| GET | `/log` | 最近 120 行日志（JSON 字符串数组） |
| GET | `/decode?pdu=<hex>` | 解析一条 SMS-DELIVER PDU，返回发送者/时间/文本/长短信信息 |
| GET | `/selftest?url=<https地址>` | HTTPS 连通性自检（CA bundle / TLS / DNS），默认测试 baidu |
| GET | `/wifi?action=restart` | 断开并重连 WiFi（立即返回，后台执行） |
| GET | `/system?action=restart` | 重启整机（立即返回） |
| POST | `/ping` | 激活数据连接 → ping 8.8.8.8 → 关闭数据连接（约 40s，异步） |

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
