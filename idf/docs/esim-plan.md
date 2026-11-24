# eSIM 管理：开发计划、协议笔记与实现记录

> 状态：已在真实 eUICC（9eSIM + ML307Y）上验证：读 EID、读列表、切卡、下载安装、删除都正常，见 §10 / §12。
> 代码：`idf/components/esim/`、`idf/components/web/web/index.html`、`tools/esim_bridge.py`。

## 1. 先给结论

| 需求 | 可行性 | 做法 |
|---|---|---|
| 读出 eSIM 卡里的配置列表 | ✅ | ES10c `ProfileInfoListRequest`（`BF2D`），单条 APDU，几百字节 |
| 切换启用哪一套配置 | ✅ | ES10c `EnableProfile`（`BF31`），eUICC 自己停用当前那条 |
| **删除**配置 | ✅ | ES10c `DeleteProfile`（`BF33`）；**使用中的配置拒绝删除**，网页还要手输 ICCID 确认 |
| 网页输入 / 解析二维码 | ✅ | 前端内嵌 jsQR 本地解码，不联网、不上传 |
| 网页里「导入」（下载安装新配置） | ⚠️ 设备端不可行 | 交给电脑上的 lpac，通过新增的 APDU 透传接口下发 |

第 5 条是整个需求里唯一需要评审的地方，理由见 §6：**SGP.22 的下载流程（连 SM-DP+、
校验证书、解密几十 KB 的安装包、按段写卡）在 ESP32-C3 上跑不动**，也不是加几行
AT 指令能解决的。所以网页负责「扫码 + 解析 + 生成命令」，真正的下载由 PC 上的
参考实现 lpac 完成，固件只做 APDU 透传。

## 2. 边界

- 只做 eSIM 的 **UICC 侧**操作：EID、profile 列表、启用/切换、删除未启用的配置。
- 删除有硬性保护：**当前使用中的配置不允许删**（固件读列表后直接拒绝），
  网页再要求把 ICCID 手输一遍；没有 `EuiccMemoryReset`（会清空整张卡）。
- 不做：改昵称、通知管理。
- 不做：eSIM 的下载安装（原因见 §6），下载走电脑上的 lpac，见 [esim-import.md](esim-import.md)。
- 切换后**一定会重启模组**：SIM 换了，不重新 `CFUN=1,1` 的话网络注册会停在旧卡上。

## 3. 协议与硬件前提

### 3.1 模组能力

ML307R 支持 ETSI TS 27.007 的 UICC 逻辑通道指令：

| 指令 | 作用 | 备注 |
|---|---|---|
| `AT+CCHO="<AID>"` | 打开逻辑通道 | 返回 `+CCHO: <通道号>`（1..） |
| `AT+CGLA=<ch>,<len>,"<hex>"` | 在通道上发 APDU | `<len>` 是**十六进制字符数**，不是字节数 |
| `AT+CCHC=<ch>` | 关闭逻辑通道 | |

固件启动时不探测，第一次用到 eSIM 功能时才发 `AT+CCHO=?` / `AT+CGLA=?` 判断支持性，
避免每次开机多两条 AT 指令。

### 3.2 ISD-R

eUICC 的卡内管理应用是 ISD-R，AID 固定：

```
A0000005591010FFFFFFFF8900000100
```

### 3.3 ES10x 命令

所有 ES10x 请求都走同一条 APDU：

```
CLA = 0x80 | 逻辑通道号     INS = E2      P1 = 91      P2 = 00
Lc  = DER 长度              Data = DER 请求
```

- 请求超过 255 字节时要分段（P1=0x11 续、0x91 末段，P2=段号）——固件不实现分段，
  因为管理类请求都很短；分段是 lpac 的事（默认 `ES10X_MSS=120`）。
- 应答 `SW1=0x61` 表示还有数据，用 `80 C0 00 00 <SW2>` 继续读。
- 应答数据是 DER，按 `tag + 长度 + 值` 递归解析。

用到的三个请求（与 lpac 的实际字节一致）：

| 操作 | 请求 | 应答 |
|---|---|---|
| 读 EID | `BF3E 03 5C 01 5A` | `BF3E { 5A <16 字节 EID> }` |
| 列配置 | `BF2D 0A 5C 08 5A 4F 9F70 90 91 92 95` | `BF2D { A0 { E3 {...}, E3 {...} } }` |
| 启用 | `BF31 { A0 { 5A 0A <ICCID BCD> 81 01 FF } }` | `BF31 { 80 01 <结果码> }` |

`5C` 是「我只要这些字段」的 tag 列表：`5A` ICCID、`4F` AID、`9F70` 状态、`90` 昵称、
`91` 运营商名、`92` 配置名、`95` 类型。`81 01 FF` 是 refreshFlag（`00` 表示不主动刷新）。

结果码（SGP.22 §5.7.5）：0 成功、1 找不到、2 状态不对、3 策略禁止、4 错误的重新启用、
5 终端忙（CAT busy）、6 内部错误。`5` 在实际卡上很常见，代码会自动降级成
`refresh=00` 再试一次；还失败就提示手动重启模组。

## 4. 设备端实现

### 4.1 组件

```
components/esim/
├── include/esim/esim.h
└── src/esim.c          # AT 通道 + DER 编解码 + ES10b/ES10c
```

对外接口（`esim.h`）：

```c
esp_err_t esim_init(void);                 // 建互斥锁
esim_err_t esim_get_eid(char *out, size_t cap);
int        esim_list_profiles(esim_profile_t *out, int max);   // 返回条数，<0 为错误
esim_err_t esim_enable_profile(const char *iccid_or_aid, bool refresh, int *ecc);
esim_err_t esim_channel_open(const char *aid_hex, int *channel);
void       esim_channel_close(int channel);
esim_err_t esim_transmit(int ch, const char *tx_hex, char *rx_hex, size_t cap, int *sw);
```

### 4.2 AT 层新增 `at_command_capture()`

`+CGLA` 的应答是**一行**，列表有 10 条配置时可能上千字节，原来的 `AT_LINE_MAX=512`
行缓冲放不下。为此在 `at_client` 里新增：

- 按字节直接写调用方缓冲区（不进定长行缓冲），上限由调用方给（现在 4096）；
- 事务期间夹进来的 URC（`+CMT`）照常入队，不丢短信；
- 长指令的日志只打前 60 个字符，避免一条 APDU 刷满 120 行日志。

### 4.3 并发与内存

- 组件内一把互斥锁，`list`/`enable` 整段持锁，`transmit` 单条持锁；超时 20 秒。
- 每次事务的临时堆：捕获缓冲 4 KB + 应答 0.3 KB + 列表解析 4 KB + 结果数组 2.6 KB，
  峰值约 11 KB。切卡时如果正好在发邮件（TLS 占用大），可能返回「内存不足」，重试即可。
- 所有大缓冲都走 heap，不放在 httpd 任务的 4 KB 栈上。

### 4.4 切换流程

1. `GET /esim` 先读 EID，再读列表；
2. 用户点「切换到此配置」→ `POST /esim`（表单 `id=<ICCID 或 AID>`）；
3. 固件按 `refresh=FF` 发 `EnableProfile`；返回 5（CAT busy）就按 `refresh=00` 重试；
4. 成功后**延迟 2 秒**软重启模组（`AT+CFUN=1,1`），等重新注册；
5. 前端每 2 秒查 `/status`，看到 `modem_ready` 再刷新列表。

## 5. 网页端

- 导航顺序：概览 / 推送通道 / 设备 / **eSIM 管理** / 保号发送 / 后台管理 / 日志 / 固件升级 / 关于项目。
- 「eSIM 配置」卡片：EID + 每条配置的名称、运营商、ICCID、AID、状态；
  生效中的打「当前生效」，其它给「切换到此配置」。
- 「导入新配置」卡片：
  - 选二维码图片（或直接 `Ctrl+V` 粘贴截图）→ 浏览器本地解码 → 解析 `LPA:1$...`；
  - 也可以直接把激活码粘进输入框；
  - 解析成功显示 SM-DP+、匹配 ID、OID，并生成一条电脑上执行的命令。
- 二维码解码用 **jsQR 1.4.0**（Apache-2.0，内嵌在页面里，不请求外网）。
  注意：`getUserMedia` 在 `http://192.168.x.x` 下不是安全上下文，摄像头会被浏览器禁用，
  所以只做「图片/截图」解码，不做实时扫码。

## 6. 导入（下载安装）的评审

SGP.22 的下载不是「发一条 APDU」，而是 LPA 与 SM-DP+ 的双向流程：

```
LPA → eUICC : ES10b.GetEUICCChallenge / GetEUICCInfo1
LPA → SM-DP+: ES9+.InitiateAuthentication（HTTPS，双向证书）
LPA → eUICC : ES10b.AuthenticateServer
LPA → SM-DP+: ES9+.AuthenticateClient
LPA → eUICC : ES10b.PrepareDownload
LPA → SM-DP+: ES9+.AuthenticateClient（拿 Bound Profile Package）
LPA → eUICC : ES10b.LoadBoundProfilePackage（分段写入，几十~几百条 APDU）
LPA → SM-DP+: ES9+.HandleNotification
```

其中 BPP（加密后的 profile 安装包）常见 30–100 KB，必须整体拿到、按 120 字节一段
写进 eUICC；还要校验 SM-DP+ 证书链、算 ECKA/SCP03t。

四条路线的对比：

| 路线 | 内存 | 可行性 | 结论 |
|---|---|---|---|
| A. 固件自己跑完整 RSP | 需要 >100 KB 连续堆 + mbedtls + HTTP 客户端 | ❌ | C3 只有 ~40 KB 空闲、最大连续块 7–12 KB，BPP 都放不下，更别说 TLS |
| B. 浏览器直连 SM-DP+ | 浏览器端无压力 | ⚠️ | SM-DP+ 是给终端/PC 客户端设计的，不保证 `Access-Control-Allow-Origin`；被 CORS 拦掉就完全没法用 |
| C. 浏览器 + 设备做 HTTPS 中转 | 设备要流式转发 100 KB 响应 | ⚠️ | 能省掉 CORS，但等于在设备上再实现一层 TLS 代理 + 浏览器侧完整 RSP（ASN.1/分段/错误处理），没有实卡根本没法验证，失败点太多 |
| D. PC 上跑 lpac，设备只透传 APDU | 设备每条 APDU 几百字节 | ✅ | 采用：lpac 是久经使用的参考实现，固件只加一个 `/esim/apdu` |

路线 D 的具体形态：

```
网页：二维码 → LPA → 生成命令
电脑：python tools/esim_bridge.py --host <设备IP> -- lpac profile download -a "LPA:1$..."
        └─ lpac（LPAC_APDU=stdio）把 APDU 写成 JSON 给脚本
        └─ 脚本 POST /esim/apdu → 固件 AT+CCHO/CGLA → eUICC
```

代价：导入这一步必须有一台电脑（手机浏览器做不到）。收益：下载流程的正确性由 lpac 保证，
固件侧只有一条几十行的透传路径，出错也容易定位。

## 7. 接口

见 [api.md](api.md)。三个接口：

- `GET /esim`：EID + 配置列表（异步，1–3 秒）
- `POST /esim`：`id=<ICCID|AID>` 切换
- `POST /esim/apdu`：`func=connect|logic_channel_open|logic_channel_close|transmit|disconnect` + `param=<hex>`，给 `tools/esim_bridge.py` 用

`/esim/apdu` 需要后台账号密码（Basic Auth），和设备上其它接口一致。

## 8. 安全与限制

- 只能切卡、不能删卡：固件里没有 `DeleteProfile`、没有 `EuiccMemoryReset`。
- `/esim/apdu` 相当于把 SIM 卡的命令通道暴露给局域网内知道后台密码的人。
  它能做的事：切卡、读信息、写新配置；不能读短信内容（那走模组接口），
  但足以装一张新卡。默认密码 `admin/admin123` 请务必改掉。
- 切换过程中断电：eUICC 的 profile 状态是原子写，一般不会坏卡，但可能停在
  「已启用但网络没注册」的状态，重启一次即可。
- 模组侧：逻辑通道一次只开一条，列表操作期间不要同时用别的工具连卡。

## 9. 测试计划

### 9.1 没有实卡时已经验证的

- 全量编译通过（`idf.py build`）。
- 网页端二维码链路：造一张写着 `LPA:1$rsp.truphone.com$QR-G-5C-1LS-1W1Z9P7` 的二维码，
  用无头浏览器走真实 DOM 流程（设置 `input.files` → change 事件）验证：
  解码成功、字段解析正确、命令字符串正确。
- `tools/esim_bridge.py`：用假设备 + 假 lpac 跑通 5 条 APDU 的转发与 JSON 编解码。
- 错误路径：`AT+CCHO` 失败、APDU 状态字非 9000、响应过长，都会落到具体错误文案。

### 9.2 拿到设备和卡之后要跑的（**全程不允许删卡**）

1. `GET /esim` 返回 EID 与至少一条配置，名称/运营商/ICCID 与卡商后台一致。
2. 点「切换到此配置」，模组重启后 `+CEREG` 能注册上，`/status` 的 `self_phone` 变成新卡的号码。
3. 再切回原配置，确认原配置仍在、仍可用（这一步验证「只切不删」）。
4. 切到一张**已停用**的配置时，eUICC 自己停用当前那张；列表刷新后只有一个 `state=1`。
5. 卡片不支持 eSIM（普通 SIM）：页面提示「打不开 eUICC 的逻辑通道」，不影响其它功能。
6. 并发：切卡时点「重新读取」，第二次请求返回「eSIM 正忙」。
7. `tools/esim_bridge.py -- lpac chip info` 能读到 EID（验证透传通道）。
8. 用一张测试卡 `lpac profile download`：下载完列表多一条，再切过去能注册。
9. 断电恢复：切换过程中拔电，重新上电后列表仍完整。

## 10. 实机验证记录（2026-11-24，9eSIM + ML307Y）

设备 `sms-7668`（ML307Y）插的是 **9eSIM** 卡，实测数据：

```
EID   8904…（已脱敏）
配置1  Helium   8901…（已脱敏）  AID …1100  未启用
配置2  WEBBING  8985…（已脱敏）  AID …1200  使用中（运营商 Saily）
```

验证过的操作：读 EID、读列表、`Saily → Helium → Saily` 往返切换（两条配置都还在，没有删除任何东西），
切换后模组软重启并重新注册。**过程中踩到三个模组/卡的行为差异**，代码已按下面处理：

### 10.1 `AT+CCHO` 的应答不带前缀

ETSI 标准应答是 `+CCHO: 1`，但 ML307Y 只回一个光秃秃的 `1`（前面有空行）。
按前缀匹配会「已捕获应答」但拿不到通道号，通道从此不再关闭，几次之后 CCHO 直接 `+CME ERROR: 4`（通道被占用）。

处理：`at_command_capture_ex(cmd, "+CCHO:", numeric_line=true, …)`，两种格式都接受。
顺带补了容错——打不开通道时提示「逻辑通道被占用，请软重启模组后重试」。

### 10.2 模组先回 OK，过一会儿才吐数据

偶发 `AT+CGLA=…` 先返回 `OK`，`+CGLA: <len>,"<hex>"` 几百毫秒后才到。
原来的实现看到 OK 就结束，数据行被当成 URC 丢掉（日志里表现为「无目标行」+ 下一条命令拿到上一条的应答）。

处理：`at_command_capture` 在 `expect_payload` 时收到 OK 不退出，再等 3 秒（`AT_PAYLOAD_GRACE_MS`）；
超时仍没数据才判定失败。日志会打一行「AT 先返回 OK，继续等待数据行」。

### 10.3 切换成功但状态字是 910B

`EnableProfile` 带 `refreshFlag=FF` 时，eUICC 会挂一条 **REFRESH 主动命令**，应答是：

```
BF31 03 80 01 00   +   SW = 91 0B
```

业务结果码是 0（成功），但状态字不是 9000。旧实现直接报错，而且那条主动命令一直挂着——
之后所有 APDU（包括读列表）全返回 `6985`（conditions of use not satisfied），模组也读不到 SIM。

处理：状态字 `91 XX` 时用 `GET RESPONSE` 把主动命令取走（数据不并入应答），
业务应答已经拿到就按成功处理；随后本来就会软重启模组，REFRESH 的效果由重启覆盖。

### 10.4 其它

- 19 位 ICCID 的 BCD 末位补 `F`，解码时跳过 `F`（Helium 那条就是 19 位）。
- 读列表用 `esim_read_all()` **只开一次通道**：这个模组每次开关通道都要多等几秒，
  分两次读（EID、列表）容易踩到上一条命令的残留应答。
- `/esim` 的异步任务栈改成 4 KB：C3 的最大连续堆经常只有 6–8 KB，`web_async` 默认的 10 KB
  会直接 `xTaskCreate` 失败（表现为请求无响应）。异步任务起不来时现在会直接回
  `{"success":false,"message":"设备内存紧张…"}`，前端把原因显示出来并自动重试一次。
- `+CGLA` 的应答缓冲从 4 KB 降到 1280 字节：单次应答最多 512 字节，转十六进制 1024 字符，
  4 KB 的 `malloc` 在碎片化时容易失败（页面报「内存不足，无法发送 APDU」），1280 就够了。

### 10.5 判断"卡能识别"看 ICCID，不要等驻网

外卡（尤其是漫游的 eSIM）附着本来就很慢，`+CREG: 2,4` / `+CEREG: 4`（searching）能持续好几分钟，
这与 eSIM 功能无关；其中 Helium 那条还是无余额的卡，本来也注册不上。

判断标准：**能读到 EID 和 ICCID 就说明卡片识别正常**，`/esim` 返回列表即视为通过；
驻网慢等模组自己附着，实在不放心再到「设备」页软重启模组（本次实测复位后 8 秒内恢复 `net_stat=5`）。


## 11. 已知限制 / 后续

- 只支持短 APDU 请求（≤255 字节）。管理类请求够用；RSP 的分段由 lpac 负责。
- 列表最多 10 条（`ESIM_MAX_PROFILES`），一般 eUICC 也就几条到十几条。
- 没有通知（Notification）管理：下载完成后 lpac 会自己发通知，固件不做。
- 摄像头扫码不可用（HTTP 非安全上下文），只能上传图片/粘贴截图。
- 外卡附着慢（几分钟）属于正常现象，与 eSIM 功能无关，见 10.5。

## 12. 写卡与删卡实测（2026-11-24）

设备同上（9eSIM + ML307Y），用公网免费测试配置验证「下载 → 读 ICCID → 删除」整条链路。

### 12.1 下载（写卡）

- 电脑端：lpac **v2.2.1**（Windows mingw 包）+ `tools/esim_bridge.py`，
  激活码 `LPA:1$rsp.truphone.com$QRF-SPEEDTEST`。
- 过程：`es10b_get_euicc_challenge_and_info → es9p_initiate_authentication → es10b_authenticate_server
  → es9p_authenticate_client → … → es10b_load_bound_profile_package`，共 **199 条 APDU**，耗时约 2 分钟。
- 结果：`{"code":0,"message":"success"}`；网页重新读取后列表多出一条
  ICCID `8944…（已脱敏，19 位）`、名字 `BetterRoaming`、运营商 `Speedtest Travel`，**ICCID 读取正确**。
- 另两个免费码（`QR-G-5C-1LS-1W1Z9P7`、`QR-G-5C-KR-1PCDWP9`）在 `es9p_authenticate_client` 返回
  `Refused`，是服务端策略，换码即可，与设备无关。

### 12.2 删除（删卡）

顺序（也是推荐的用法）：

1. 直接删当前使用中的配置 → 固件拒绝：`「Helium」正在使用中，不能删除；请先切换到其它配置`。
2. 切到 Saily，等模组重启、状态确认 `state=1`。
3. 删 Helium（未启用）→ 成功；再删刚下载的测试配置 → 成功。
4. 最终卡里只剩 Saily 一条且仍是 `state=1`，**没有动到能用的卡**。

### 12.3 新增的坑

- **`BF33` 不能套 `A0`**：
  - 我们最初按「`[51] { A0 { profileIdentifier } }`」拼，eUICC 返回结果码 1（找不到该 ICCID / AID）；
  - 抓 lpac 的 APDU 发现它最后成功的一帧是 `BF33 0C 5A 0A <BCD>`，**CHOICE 直接放在 BF33 里**；
  - 改成不包 `A0` 后，网页删除一次成功。`BF31`（启用）仍保留 `A0`，因为实测可用。
- **`AT+CCHC` 会补一条 `+CCHC` 主动上报**：它偶尔正好落进下一次 `AT+CCHO` 的应答窗口，
  导致拿不到通道号（日志「无目标行」）。现在 `esim_channel_open()` 失败会隔 300 ms 重试一次。
- **lpac v2.3.0 的 Windows 包 stdio 后端是坏的**：收到第一条 APDU 应答就退出（`euicc_init`）。
  同样的输入 v2.2.1 正常。导入指南里已固定推荐 2.2.1。
- **`esim_bridge.py` 必须用二进制管道**：Windows 文本模式会把换行改写成 CRLF，
  lpac 的 JSON 解析器会因此判错；改成二进制读写后稳定。

### 12.4 页面

- 配置卡片改成两行 `ICCID` / `AID`（等宽、按标签对齐），不再挤成一行；
- 未启用的配置有「切换」「删除」两个按钮，使用中的显示「不可删除」；
- 删除弹窗要求把 ICCID 原样输入一遍才能点确认；
- 导入卡片底部给出完整步骤链接（`idf/docs/esim-import.md`）。
