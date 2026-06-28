# API 参考手册 — 完整函数索引

---

## 模块: config.cpp — 配置管理

### `void saveConfig()`
**用途**: 将 `config` 全局结构体所有字段写入 NVS。

**NVS Namespace**: `"sms_config"`  
**写入模式**: `false`（读写模式）

**写入的 Key 列表**:
| NVS Key | 类型 | 对应字段 |
|---|---|---|
| `smtpServer` | String | `config.smtpServer` |
| `smtpPort` | Int | `config.smtpPort` |
| `smtpUser` | String | `config.smtpUser` |
| `smtpPass` | String | `config.smtpPass` |
| `smtpSendTo` | String | `config.smtpSendTo` |
| `adminPhone` | String | `config.adminPhone` |
| `webUser` | String | `config.webUser` |
| `webPass` | String | `config.webPass` |
| `numBlkList` | String | `config.numberBlackList` |
| `push{i}en` | Bool | 通道 i 是否启用 |
| `push{i}type` | UChar | 通道 i 推送类型 |
| `push{i}url` | String | 通道 i URL |
| `push{i}name` | String | 通道 i 名称 |
| `push{i}k1` | String | 通道 i 参数1 |
| `push{i}k2` | String | 通道 i 参数2 |
| `push{i}body` | String | 通道 i 自定义模板 |

---

### `void loadConfig()`
**用途**: 从 NVS 加载所有配置到 `config` 结构体。包含旧版本迁移逻辑。

**兼容迁移**: 如果存在旧 key `httpUrl` 且第一个推送通道未启用，自动迁移到通道 1。

**默认值**:
- `smtpPort`: 465
- `webUser`: `"admin"` (`DEFAULT_WEB_USER`)
- `webPass`: `"admin123"` (`DEFAULT_WEB_PASS`)
- 通道名称: `"通道1"` ~ `"通道5"`
- 通道类型: `PUSH_TYPE_POST_JSON` (1)

---

### `bool isPushChannelValid(const PushChannel& ch)`
**用途**: 检查单个推送通道配置是否完整可用。

**校验规则**:

| 推送类型 | 必填字段 |
|---|---|
| POST_JSON / BARK / GET / DINGTALK / FEISHU / CUSTOM | `url` 非空 |
| PUSHPLUS / SERVERCHAN | `key1` 非空 |
| GOTIFY | `url` 非空 **且** `key1` 非空 |
| TELEGRAM | `key1` 非空 **且** `key2` 非空 |

**前提**: `ch.enabled == true`，否则直接返回 false。

---

### `bool isConfigValid()`
**用途**: 检查系统是否有至少一种可用的通知方式。

**返回 true 条件**: 邮件配置完整（4 个 SMTP 字段均非空）**或** 至少一个推送通道通过 `isPushChannelValid()` 校验。

---

### `String getDeviceUrl()`
**返回**: `"http://{WiFi.localIP}/"`

---

## 模块: modem.cpp — 模组控制

### `String sendATCommand(const char* cmd, unsigned long timeout)`
**参数**:
- `cmd`: AT 指令字符串（不含 `\r\n`，函数自动追加）
- `timeout`: 等待超时（毫秒）

**行为**:
1. 清空 Serial1 缓冲区
2. 发送 `cmd\r\n`
3. 等待直到收到 "OK" 或 "ERROR" 或超时
4. 收到 OK/ERROR 后额外 `delay(50)` 读取剩余数据

**返回**: 模组完整响应字符串（含 OK/ERROR）

---

### `void modemPowerCycle()`
**行为**:
1. 配置 `MODEM_EN_PIN` 为输出
2. 拉低 1200ms → 关闭模组
3. 拉高 6000ms → 开启模组并等待启动

**注意**: 调用后需清空 Serial1 缓冲区（该函数不自动清空）

---

### `void resetModule()`
**行为**:
1. 调用 `modemPowerCycle()`
2. 清空 Serial1
3. 循环 10 次尝试 `sendATandWaitOK("AT", 1000)`
4. 打印恢复结果

---

### `bool sendATandWaitOK(const char* cmd, unsigned long timeout)`
**与 sendATCommand 区别**: 仅返回 true/false，不做额外 delay。用于初始化流程中的幂等检查。

---

### `bool waitCEREG()`
**用途**: 轮询 `AT+CEREG?` 直到网络注册成功。

**注册状态码**:
- `1`: 已注册本地网络 → true
- `5`: 已注册漫游 → true
- `0,2,3,4`: 未注册 → false

**超时**: 单次轮询 2000ms（调用方在外层循环调用）

---

### `void blink_short(unsigned long gap_time = 500)`
**行为**: LED 亮 50ms → 灭 → 等待 `gap_time` ms。

---

### `bool sendSMS(const char* phoneNumber, const char* message)`
**流程**:
1. `pdu.setSCAnumber()` 使用默认短信中心
2. `pdu.encodePDU(phoneNumber, message)` 编码
3. 发送 `AT+CMGS=<pduLen>`
4. 等待 `>` 提示符（5 秒超时）
5. 发送 PDU 数据 + `Ctrl+Z` (0x1A)
6. 等待 OK/ERROR（30 秒超时）

**返回**: true=成功, false=PDU编码失败/无提示符/ERROR/超时

---

## 模块: push.cpp — 推送与邮件

### `void sendEmailNotification(const char* subject, const char* body)`
**前提检查**: SMTP 四个字段均非空，否则打印跳过日志。

**实现**:
1. 创建 `smtp.connect(server, port, callback)`
2. `smtp.authenticate(user, pass, readymail_auth_password)`
3. 构造 `SMTPMessage`，设置 from/to/subject/body/timestamp
4. `smtp.send(msg)`

**from 格式**: `"sms notify <user@example.com>"`  
**to 格式**: `"your_email <receiver@example.com>"`  
**timestamp**: 使用 `time(nullptr)`（需 NTP 已同步）

---

### `void sendSMSToServer(const char* sender, const char* message, const char* timestamp)`
**行为**:
1. 检查 WiFi 连接
2. 检查是否有启用的有效通道
3. 遍历所有通道，对每个有效通道调用 `sendToChannel()`
4. 通道间 delay(100ms)

---

### `void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp)`
**核心推送函数**。根据 `channel.type` 分发到 10 种推送方式之一。每个 case 构建对应的 HTTP 请求（URL/Header/Body），使用 `HTTPClient` 发送，打印响应码和内容。

**签名相关**:
- 钉钉: `HMAC-SHA256(timestamp+"\n"+secret)` → Base64 → URLEncode → 追加到 URL
- 飞书: `HMAC-SHA256(timestamp+"\n"+secret)` → Base64 → 放入 JSON body

**占位符**: 自定义模板（PUSH_TYPE_CUSTOM）支持 `{sender}` `{message}` `{timestamp}` 占位符替换。

---

### `String urlEncode(const String& str)`
标准 URL 编码（空格 → `+`, 非字母数字 → `%XX`）

---

### `String jsonEscape(const String& str)`
JSON 字符串转义：`"` → `\"`, `\` → `\\`, `\n` → `\\n`, `\r` → `\\r`, `\t` → `\\t`

---

### `String dingtalkSign(const String& secret, int64_t timestamp)`
HMAC-SHA256(timestamp + "\n" + secret, secret) → Base64 → URLEncode

---

### `int64_t getUtcMillis()`
通过 `gettimeofday()` 获取 UTC 毫秒级时间戳，失败则回退 `time(nullptr) * 1000`

---

## 模块: sms_process.cpp — 短信处理

### `void initConcatBuffer()`
将 `concatBuffer[MAX_CONCAT_MESSAGES]` 全部标记为未使用，清空所有分段。

---

### `int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts)`
**查找逻辑**:
1. 遍历缓冲区，找已用且 refNumber+sender 匹配的槽位 → 直接返回
2. 遍历缓冲区，找未使用槽位 → 初始化并返回
3. 无空闲槽位 → 覆盖最老的槽位（按 firstPartTime 比较）

---

### `String assembleConcatSms(int slot)`
按顺序拼接 `concatBuffer[slot].parts[0..totalParts-1]`，缺失分段标记 `[缺失分段N]`

---

### `void clearConcatSlot(int slot)`
将该槽位所有字段复位。

---

### `void checkConcatTimeout()`
遍历所有使用中的槽位，若 `millis() - firstPartTime >= 30000ms`，强制合并转发不完整消息，并清空槽位。

---

### `String readSerialLine(HardwareSerial& port)`
逐字节读取串口，遇 `\n` 返回行，`\r` 跳过，超长行保护（超过 SERIAL_BUFFER_SIZE 归零），无完整行返回空串。

**注意**: 使用 `static` 缓冲区，仅适合单线程调用。

---

### `bool isHexString(const String& str)`
检查字符串是否全为十六进制字符 `[0-9A-Fa-f]`。

---

### `bool isInNumberBlackList(const char* sender)`
**匹配逻辑**: 逐行读取 `config.numberBlackList`（换行符分隔），支持 `+86` 前缀的模糊匹配：
- `+8613800138000` 匹配黑名单中的 `13800138000`
- `13800138000` 匹配黑名单中的 `+8613800138000`

---

### `bool isAdmin(const char* sender)`
比较发送者与 `config.adminPhone`，忽略 `+86` 前缀差异。

---

### `void processAdminCommand(const char* sender, const char* text)`
**支持命令**:
| 命令格式 | 行为 |
|---|---|
| `SMS:号码:内容` | 调用 `sendSMS()` 代发短信，邮件通知结果 |
| `RESET` | 发送通知邮件 → `resetModule()` → `ESP.restart()` |

---

### `void processSmsContent(const char* sender, const char* text, const char* timestamp)`
**处理顺序**:
1. `isInNumberBlackList()` → 忽略
2. `isAdmin()` + 命令格式检测 → `processAdminCommand()` → 不再发普通通知
3. `sendSMSToServer()` — 推送所有启用的通道
4. `sendEmailNotification()` — 邮件通知

---

### `void checkSerial1URC()`
**状态机**: `IDLE` ↔ `WAIT_PDU`

IDLE 状态检测 `+CMT:` 行 → 转入 WAIT_PDU。
WAIT_PDU 状态读取 PDU hex 数据 → `pdu.decodePDU()` 解析 → 根据 `concatInfo` 判断长短信/普通短信 → 恢复 IDLE。

---

## 模块: web_handlers.cpp — HTTP 处理

### 日志系统 API

### `void logCapture(const String& msg)`
### `void logCapture(const char* msg)`
**用途**: 输出日志，但不换行。内容追加到行缓冲区 `_logLine`，不会立即写入环形缓冲区。同时输出到 `Serial`。

**使用场景**: 与 `logCaptureLn()` 配合，构建一行完整日志：
```cpp
logCapture("目标号码: ");
logCaptureLn(String(phoneNumber));
// 环形缓冲区中只有一行: "目标号码: 13800138000"
```

---

### `void logCaptureLn(const String& msg)`
### `void logCaptureLn(const char* msg)`
**用途**: 输出日志并换行。将 `_logLine` + msg 提交到环形缓冲区，然后清空行缓冲区。同时输出到 `Serial.println`。

**注意**: 环形缓冲区每调用一次 `logCaptureLn` 即可产生一行日志，因此 `logCapture()` 和 `logCaptureLn()` 的日志会在环形缓冲区中合并为同一行。

---

### `void logCaptureF(const char* fmt, ...)`
**用途**: 格式化日志输出（类似 `printf`）。支持 `%d`/`%s`/`%f` 等格式。如果格式化字符串以 `\n` 结尾，自动提交整行到缓冲区。

---

### `void handleLog()`
**用途**: HTTP 端点 `GET /log`，返回环形缓冲区中所有日志行。

**返回格式**: `application/json`
```json
["行1", "行2", "行3", ...]
```
**鉴权**: 需要 HTTP Basic Auth。最多返回 120 行（环形缓冲区容量）。

---

### `bool checkAuth()`
HTTP Basic Authentication，账号密码来自 `config.webUser` / `config.webPass`。

---

### `void handleRoot()`
返回 SPA 主页 HTML。通过 `html.replace("%KEY%", value)` 替换模板占位符，动态生成 5 个推送通道的表单。页面包含 10 个可切换面板（系统概览/账号管理/邮件通知/推送通道/管理员黑名单/发送短信/模组诊断/网络测试/模组控制/AT终端/系统日志），通过侧边栏 JS 切换显示。

**模板占位符**:

| 占位符 | 数据来源 |
|---|---|
| `%IP%` | `WiFi.localIP().toString()` |
| `%WEB_USER%` / `%WEB_PASS%` | `config.webUser` / `config.webPass` |
| `%SMTP_SERVER%` ~ `%SMTP_SEND_TO%` | `config.smtp*` |
| `%ADMIN_PHONE%` | `config.adminPhone` |
| `%NUMBER_BLACK_LIST%` | `config.numberBlackList` |
| `%SMTP_CHECK%` | 邮件配置是否完整 |
| `%PUSH_COUNT%` | 已启用的有效推送通道数 |
| `%PUSH_CHANNELS%` | 循环生成 5 个通道的 HTML 表单 |

---

### `void handleToolsPage()`
兼容旧链接，直接委托给 `handleRoot()` 返回同一 SPA 页面。

---

### `void handleSave()`
解析 POST 表单 → 写入 `config`（写入区在 `gWorkMux` 内，与 worker 的快照读互斥）→ `saveConfig()` → 重新校验 → **快速返回**成功页面（3 秒跳转）。**不发送通知邮件**（避免 SMTP 阻塞前端；连通性请用各通道“测试推送”验证）。

---

### `void handleQuery()`
根据 `?type=` 参数执行不同查询：

| type | AT 指令 | 返回内容 |
|---|---|---|
| `ati` | `ATI` | 制造商/型号/固件版本 |
| `signal` | `AT+CESQ` | RSRP/RSRQ 信号强度 |
| `siminfo` | `AT+CIMI` `AT+ICCID` `AT+CNUM` | IMSI/ICCID/本机号码 |
| `network` | `AT+CEREG?` `AT+COPS?` `AT+CGACT?` `AT+CGDCONT?` | 注册/运营商/数据/APN |
| `wifi` | (WiFi 对象) | SSID/RSSI/IP/网关/DNS/MAC/BSSID/信道 |

---

### `void handleFlightMode()`
根据 `?action=` 控制飞行模式：

| action | AT 指令 | 说明 |
|---|---|---|
| `query` | `AT+CFUN?` | 查询当前 CFUN 值 |
| `toggle` | 查询 + `AT+CFUN=1/4` | 1↔4 切换 |
| `on` | `AT+CFUN=4` | 强制开启 |
| `off` | `AT+CFUN=1` | 强制关闭 |

---

### `void handleATCommand()`
通过 `?cmd=` 透传 AT 指令到 `sendATCommand()`，返回 JSON `{success, message}`。

---

### `void handleSendSms()`
解析 POST 表单 phone/content → `enqueueOutgoingSms()` 入队（HTTP 立即返回）→ 真正的 `AT+CMGS` 由 loop 的 `processOutgoingSmsQueue()` 异步执行，避免浏览器长等。

---

### `void handlePing()` / `processPingJob()`
网页“蜂窝 UDP 流量”诊断（与保号不同）。`handlePing()` 只启动/查询后台任务并立即返回；真正流量在 loop 的 `processPingJob()` 执行：
1. `AT+CGACT=1,1` 激活数据连接
2. `consumeCellularDataBytes()`：`AT+MIPOPEN` UDP socket → `AT+MIPSEND` 发送约 45KB 上行 → `AT+MIPCLOSE`
3. `AT+CGACT=0,1` 关闭数据连接

`?action=status` 轮询进度。注意：**保号**动作（默认 HTTP GET baidu）在 `/keepalive`，不是这里。

---

### 其它路由（补充）

下列路由同样注册于 `setup()`（均经 `checkAuth()`）：

| 路由 | 处理函数 | 说明 |
|---|---|---|
| `GET /status` | `handleStatus()` | 健康状态 JSON（版本/信号/堆水位/各队列深度/统计/复位原因/时间） |
| `GET /messages` | `handleMessages()` | 收件箱/已发送 JSON（`?box=sent`），流式分块输出 |
| `POST /resend` `POST /delete` | `handleResend()` / `handleDeleteMsg()` | 重发/删除收件箱某条（`?id=`） |
| `GET /keepalive` | `handleKeepAlive()` | 保号：`?action=status\|run\|reset`；`run` 入队由 loop 执行（默认动作 = HTTP GET baidu 消耗流量） |
| `GET /testpush` | `handleTestPush()` | 通道测试：`?ch=` 入队，`?action=status` 轮询；真实发送在 worker |
| `GET /ussd` | `handleUssd()` | USSD 查询（`AT+CUSD`，同步最长 ~20s） |
| `GET /modem` `GET /wifi` `GET /wifiscan` `POST /wificonfig` | — | 模组信息 / WiFi 状态 / 扫描 / 保存并重启接入 |
| `POST /reboot` `POST /factory` | `handleReboot()` / `handleFactory()` | 重启 / 恢复出厂（清 NVS） |
| `GET /export` `POST /import` | `handleExport()` / `handleImport()` | 配置导出 / 导入（含凭据，注意保管） |
| `GET /logdownload` | `handleLogDownload()` | 纯文本下载全部日志 |
| `POST /update` | `handleOtaUpload()` / `handleOtaFinish()` | 网页 OTA 固件升级 |

并发说明：`/testpush` 真正的推送发送在后台 `pushWorkerTask`；`/keepalive run`、`/sendsms`、`/ping` 的模组/网络动作仍在 loop 任务（经队列异步）。日志相关读取（`/log`、`/logdownload`）与写入用 `gLogMux` 保护。
