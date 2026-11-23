# 固件升级（OTA）开发计划【待实现】

> 状态：**设计已定，尚未实现**。本文是动工前的唯一依据，实现过程中若有偏差，先改这里再改代码。
> 最后更新：2026-11-23

## 0. 决策记录

| 编号 | 决策 | 理由 |
|---|---|---|
| D1 | **只做手动上传升级**，不做自动下载、自动安装 | 升级会重启、会掉线；扣费设备和"半夜自己换固件"不搭 |
| D2 | 设备**每天检查一次**远端版本，只在页面上提示"有新版本，请手动下载升级" | 提示成本低（几百字节），风险为零；是否升级由人决定 |
| D3 | 固件版本号 = **日期 + 时间**（UTC，`YYYYMMDD.HHMM`） | 现有 `git describe` 产物不可比较（tag `latest` 会移动、带 `-dirty`） |
| D4 | **老设备刷新固件必须保留全部配置** | NVS 分区位置与尺寸一个字节都不能动；迁移烧录方式见 §2.3 |
| D5 | 不做 secure boot / 镜像签名、不做差分升级、不做多源 URL | 一期只需要"能安全地换成新固件" |

## 1. 范围

**要做**

- 网页上传 `*-ota.bin` → 校验 → 写入另一个 OTA 分区 → 重启 → 跑新固件
- 失败/断电/上传中断不破坏当前固件；新固件起不来能自动回滚
- 每天一次版本检查，命中新版本时在升级卡片上提示，并给出下载入口
- 升级成功/失败通过现有 `notify_system()` 推送

**不做**

- 自动下载固件、自动重启升级（D1）
- 后台静默升级、灰度、A/B 分流
- 签名校验、安全启动（列为后续可选项，见 §9）
- 升级时迁移配置（配置迁移应该由 app 启动时按 NVS 版本号做，与 OTA 解耦）

## 2. 硬约束：配置必须保留（D4）

### 2.1 现状

`idf/partitions.csv` 现在是单 app 布局：

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x6000
phy_init,   data, phy,     0xf000,   0x1000
factory,    app,  factory, 0x10000,  0x300000
```

配置散在 4 个 NVS namespace：`sms_config`（账号/邮件/推送通道/黑名单/选网/本机号码）、
`sms_task`（定时任务）、`sms_state`（lastIp）、`sms_wifi`（WiFi 账号）。**全部位于 nvs 分区内**。

### 2.2 目标分区表

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x6000     ← 偏移和大小必须保持原样
phy_init,   data, phy,     0xf000,   0x1000
otadata,    data, ota,     0x10000,  0x2000
app0,       app,  ota_0,   0x20000,  0x1F0000
app1,       app,  ota_1,   0x210000, 0x1F0000
```

- 两个槽各 `0x1F0000` = 2,031,616 B（1984 KB），当前固件 1,335,648 B，占 65.7%，余量 680 KB
- `0x12000`–`0x20000` 有 56 KB 空洞，**不要**为了"对齐漂亮"把它消掉
- 结束地址正好 `0x400000`，4 MB flash 用满

**三条红线**

1. 不要改用 `CONFIG_PARTITION_TABLE_TWO_OTA` 内置布局：它把 nvs 缩到 `0x4000` 并把 otadata 挪到 `0xd000`，老配置会掉。
2. nvs 的 offset `0x9000` 与 size `0x6000` 保持不变；`sms_config` 等键名也一个都不改。
3. 分区表必须继续走 `CONFIG_PARTITION_TABLE_CUSTOM=y` + `partitions.csv`，别被 menuconfig 改成内置方案。

### 2.3 迁移烧录：**不要用 merged bin 覆盖 0x0**

这是最容易踩的坑。`esptool merge_bin` 会把 0x0 到末尾用 `0xFF` 填满空隙，
而 nvs（`0x9000`–`0xF000`）正好落在"分区表末尾 ~ app 起始"的空隙里——
**用 merged bin 写 0x0 等于把整个 NVS 抹成 0xFF，配置全丢。**

老设备迁移必须用分开写的方式（`idf.py flash` 内部就是这么干的，也能用）：

```powershell
cd idf
idf.py build
idf.py -p COM3 flash          # 写 bootloader / 分区表 / app，不碰 nvs
# 或者手动：
python -m esptool --chip esp32c3 -p COM3 write_flash `
  0x0     build/bootloader/bootloader.bin `
  0x8000  build/partition_table/partition-table.bin `
  0x20000 build/sms_forwarder.bin
```

- **禁止** `esptool erase_flash`、ESP Launchpad 的 Erase、`write_flash --erase-all`
- merged bin 只给**全新芯片**用（或明确接受配置清空的场景）
- Release 说明、README、升级页面文案都要写清这一点

### 2.4 迁移后第一次启动

otadata 是空的（0xFF），bootloader 会回退到第一个合法 app（`ota_0`），日志里可能有一行
"ota data invalid"，属正常现象，不必处理。之后每次 OTA 都会写 otadata。

### 2.5 与 Arduino 版（`code/`）的关系

迁移方向只有 **Arduino → IDF** 这一种，不需要考虑反向。

Arduino 版默认分区表是 `nvs 0x9000/0x5000` + `otadata 0xe000` + `app0 0x10000`，
nvs 只有 20 KB。IDF 版把同一块 nvs 扩到 `0x9000/0x6000`（24 KB），**起点不变、只往后长**，
已有键值全部落在前 4 个页里，读到的一模一样。

扩容出来的那 4 KB（`0xE000`–`0xF000`）在 Arduino 布局里是 otadata，迁移时 IDF 会把
phy_init 写到 `0xf000`，剩下的半块旧数据会并入 nvs；NVS 扫到无法识别的页会当成空页重新初始化，
不影响已有键值。表现就是：刷完 IDF 固件，后台账号、邮件、推送通道、黑名单、定时任务、WiFi 全在。

OTA 功能只做在 `idf/` 上，`code/` 保持现状。

## 3. 版本号（D3）

### 3.1 格式

```
YYYYMMDD.HHMM     例：20261123.1635
```

- 定宽 13 字符，纯数字加点，**字典序 = 时间序**，直接 `strcmp` 比较，不需要解析
- `esp_app_desc_t.version` 是 `char[32]`，长度足够
- 用 UTC，避免构建机和设备时区不一致

### 3.2 注入方式

```cmake
# idf/CMakeLists.txt，project() 之前
if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")
  string(TIMESTAMP PROJECT_VER "%Y%m%d.%H%M" UTC)
endif()
```

CI 里用环境变量覆盖，保证"一次发布的版本号唯一且等于构建时间"：

```bash
PROJECT_VER="$(date -u +%Y%m%d.%H%M)" idf.py build
```

### 3.3 比较规则

- 两边都匹配 `^[0-9]{8}\.[0-9]{4}$` 才比较：`latest > current` → 提示有新版
- 任一边不匹配（本地改的 dev 版）→ 一律**不提示**，页面显示"开发版"
- 相等 → "已是最新"

## 4. 组件设计：`idf/components/ota/`

```
components/ota/
├── CMakeLists.txt          REQUIRES core app_update bootloader_support esp_app_format
│                           esp_http_client esp-tls mbedtls nvs_flash
├── include/ota/ota.h
└── src/ota.c
```

### 4.1 数据结构

```c
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RECEIVING,   /* 正在接收并写入 */
    OTA_STATE_VERIFYING,   /* esp_ota_end 校验 */
    OTA_STATE_READY,       /* 已写好，等待重启 */
    OTA_STATE_ERROR,
} ota_state_t;

typedef struct {
    char  current[32];     /* esp_app_get_description()->version */
    char  latest[32];      /* 从 manifest 拿到的最新版本，空串表示未知 */
    char  url[192];        /* 下载页/直链，仅用于页面展示 */
    char  notes[160];
    int64_t checked_at;    /* 上次检查成功的 epoch，0 = 从未 */
    bool  has_update;
    ota_state_t state;
    int   progress;        /* 0-100，仅 RECEIVING 时有意义 */
    char  message[96];     /* 最近一次错误/结果，给页面和日志看 */
} ota_info_t;
```

### 4.2 接口

```c
void         ota_init(void);                  /* 读 NVS 缓存 + 记录运行分区 */
const char  *ota_version(void);               /* 当前运行版本 */
void         ota_get_info(ota_info_t *out);   /* 加锁拷贝 */
bool         ota_busy(void);                  /* 上传/写入中，供 /save 等拒绝 */

/* 上传：调用方按块喂，begin 会把 Content-Length 传进来 */
esp_err_t    ota_begin(size_t image_size);
esp_err_t    ota_write(const void *data, size_t len);
esp_err_t    ota_end(void);                   /* 校验 + set_boot_partition */
void         ota_abort(void);                 /* 中途断开必须调，清状态 */

/* 每天一次的版本检查（同步，只拉几百字节 manifest） */
esp_err_t    ota_check_latest(void);

/* 回滚确认：启动稳定后调用一次 */
void         ota_confirm_running_image(void);
```

### 4.3 写入流程

1. `ota_begin(size)`
   - 拒绝：已有任务在跑、`size` 超出 `esp_ota_get_next_update_partition()->size`、`size < 0x10000`
   - 调 `esp_ota_begin(part, size, &h)`：**这一步会擦整块分区**，2 MB 约 2–4 秒，
     期间 httpd 任务被占住属正常；擦的是非活动槽，失败也不影响当前固件
2. 前 4 KB 先缓存在 RAM，做镜像头校验（见 4.4），通过后再往下写
3. 之后每块直接 `esp_ota_write(h, buf, len)`，同步更新 `progress`
4. `ota_end()` → `esp_ota_end`（校验镜像结构/校验和）→ `esp_ota_set_boot_partition`
5. 状态置 `READY`，`notify_system("固件升级完成", "旧版本 x → 新版本 y，即将重启")`，
   `notify_drain(15000)`（照抄 `forwarder.c` 里 RESETPASS 的写法）→ `vTaskDelay(2s)` → `esp_restart()`

任一环节失败：`ota_abort()`，状态 `ERROR`，把 err 名字写进 `message` 和日志。
**不要**在任何失败路径上重启设备。

### 4.4 镜像头校验（防止用户传 merged bin）

两者开头都是 `0xE9`，只看魔数分不出来。必须解析 `esp_app_desc_t`（镜像偏移 `0x20`）：

```c
esp_app_desc_t desc;
memcpy(&desc, buf + 0x20, sizeof(desc));
if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD)          return 拒绝;
if (strcmp(desc.project_name, "sms_forwarder") != 0)      return 拒绝;
if (!version_is_valid(desc.version))                     return 拒绝;
```

返回给前端的具体文案：

- 魔数不对 → `"这不是固件文件"`
- project_name 不对 → `"这是整包固件（含 bootloader），请上传 *-ota.bin"`
- 版本号格式不对 → `"固件版本号异常，请重新下载"`

### 4.5 回滚（新固件起不来时自动退回）

开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（`sdkconfig.defaults`）。
新镜像以 `PENDING_VERIFY` 启动，**只有调用 `esp_ota_mark_app_valid_cancel_rollback()` 才算数**，
否则下次复位 bootloader 直接回滚到旧分区。

确认时机（`app_main.c`，`web_server_start()` 之后起一个 45 秒的 task）：

- WiFi 连上 + 主 Web 服务已启动 → 45 秒后确认
- 进入配网热点（`192.168.4.1` 可达）→ 同样确认

第二条是有意的取舍：新固件如果只是"连不上 WiFi"，设备还能通过配网页自救，
此时若判为无效，用户换路由器就会把刚升级的固件反复回滚。代价是"WiFi 功能性 bug"不会被自动回滚，
需要人判断——这个代价可以接受，写进文档即可。

> 注意：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 是**编译进 bootloader 的**，
> 所以迁移那一次必须连 bootloader 一起刷（`idf.py flash` 已包含）。

## 5. 网页端

### 5.1 位置

放**「设备」页**（和飞行模式、重启、模组信息这些维护操作在一起），卡片标题「固件升级」，
放在维护卡片上面。不放「后台管理」，那里是账号相关。

### 5.2 卡片内容

```
固件升级
├─ 当前版本  20261123.1635          运行分区 ota_0
├─ 最新版本  20261124.0900  [有新版本]   ← 黄色提示条 + 「去下载」链接
│  上次检查 2026-11-23 16:40        [立即检查]
├─ 上传固件  [选择文件] firmware-ota.bin   [上传并升级]
│  ▓▓▓▓▓▓▓▓░░░░ 62%  正在写入…
└─ 提示：只能上传 *-ota.bin；升级中不要断电；升级后设备会重启约 20 秒
```

- 无新版：最新版本行显示"已是最新"（灰色）
- 从未检查成功：显示"尚未检查" + 「立即检查」
- 状态 `READY`：显示"固件已就绪，正在重启…"，禁用按钮
- 状态 `ERROR`：红色一行显示 `message`，保留「重新上传」

### 5.3 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/ota` | 返回 `ota_info_t` 的 JSON：current / latest / has_update / url / notes / checked_at / state / progress / message |
| POST | `/ota` | 原始字节流上传，`Content-Length` 必填，`Content-Type: application/octet-stream` |
| POST | `/ota_check` | 立即检查版本。走 `web_spawn_async`（TLS 要 3–10 秒，别阻塞 httpd） |

`/status` 增加三个扁平字段，方便前端轮询判断是否重启成功：
`"version"`、`"ota_state"`、`"has_update"`。

### 5.4 上传协议为什么不是 multipart

浏览器 `FormData` 是 multipart，服务端要解析 boundary、处理分块边界，纯 C 写容易出 bug。
前端直接 `xhr.send(file)`（`File` 对象），浏览器按二进制原样发出，服务端只需读 `Content-Length`：

```js
var xhr = new XMLHttpRequest();
xhr.open('POST', '/ota');
xhr.setRequestHeader('Content-Type', 'application/octet-stream');
xhr.setRequestHeader('X-File-Name', encodeURIComponent(file.name));
xhr.upload.onprogress = function (e) { setProgress(e.loaded / e.total * 100); };
xhr.onload = function () { /* 200 → 展示重启倒计时；4xx → 显示 message */ };
xhr.send(file);          // 不要 FileReader.readAsArrayBuffer，省一份 1.3 MB 内存
```

`fetch` 不支持上传进度，这里必须用 `XMLHttpRequest`。

### 5.5 上传处理细节

- 认证：复用现有 basic auth 检查（`web_server.c:30` 那段），未登录 401
- `Content-Length` 缺失 → 411；超过目标分区大小 → 413，且**在 `ota_begin` 之前**判断
- 读循环：`httpd_req_recv` 4 KB 一块，返回 0 或负数 → `ota_abort()` 并 500
- `recv_wait_timeout` 现在是 10 秒，够用；上传 1.3 MB 在 WiFi 下约 5–20 秒
- 上传期间单任务 httpd 被占住，`/status` 等会排队——前端已经显示进度条，可以接受；
  想改善再用 async，但 async 的参数缓冲只有 512+1024 字节，**不能**放文件流，别走这条
- 并发保护：`ota_busy()` 为真时，`/save`、`/sendsms`、`/tasks` POST 一律返回
  `{"success":false,"message":"固件升级进行中，请稍后"}`

### 5.6 升级后的前端交互

1. POST 返回 200 `{"success":true,"version":"20261124.0900"}`
2. 前端显示"设备正在重启，约 20 秒后自动刷新"，禁用所有按钮
3. 每 2 秒拉一次 `/status`，比较 `version`：
   - 变成新版本 → 绿色"升级成功"，刷新页面
   - 90 秒还没变 → "设备可能未重启成功，请手动刷新或断电重启"

## 6. 每日版本检查（D2）

### 6.1 触发

- 启动后 60 秒执行第一次（等 WiFi 连上 + NTP 同步）
- 之后每 24 小时一次，由 `ota` 自己的 `esp_timer` 驱动
- 重启不重复检查：NVS 记 `last`（epoch），距上次成功 < 20 小时就跳过

### 6.2 前提条件（缺一不可，否则跳过并记日志）

- `wifi` 已连接
- `time(NULL) > 1600000000`（没对时，TLS 证书会报 not yet valid）
- `!ota_busy()`

### 6.3 请求

```
GET https://github.com/chenxuuu/sms_forwarding/releases/latest/download/latest.json
```

- `esp_http_client` + `crt_bundle_attach = esp_crt_bundle_attach`（**不允许**跳过证书校验）
- 允许 302 跳转（GitHub Releases 会跳到 objects.githubusercontent.com），超时 15 秒，响应上限 4 KB
- 地址写死在 `ota.h` 的宏里；fork 的人改宏即可，不做成可配置项（少一个字段少一堆坑）

### 6.4 manifest 格式

```json
{
  "version": "20261124.0900",
  "url": "https://github.com/chenxuuu/sms_forwarding/releases/latest/download/sms_forwarder-ota.bin",
  "size": 1362000,
  "sha256": "…",
  "notes": "修复 XX；新增 YY"
}
```

- 解析：手写字段提取（`strstr` + 引号切片），不引 cJSON，字段固定不需要通用解析器
- `sha256` 一期只做展示（用户下载后人工核对），设备端不做校验——传上来的镜像由 `esp_ota_end` 校验结构，够了
- 解析失败 / 版本号格式非法 → 记日志，视为"检查失败"，不动 `latest`

### 6.5 结果处理

- 更新 NVS `sms_ota`：`last`(i64) / `latest`(str32) / `url`(str192) / `notes`(str160) / `size`(u32)
- `has_update = version_compare(latest, current) > 0`
- **不下载固件、不重启、不打扰用户**；只在升级卡片和概览页显示一个小提示
- 失败重试：1 小时后一次，当天最多 3 次；都失败就等明天。失败只写日志

### 6.6 内存

版本检查和邮件一样要 TLS 握手，30 KB 量级，而空闲堆只有 38 KB（实测）。
检查前判断 `esp_get_free_heap_size() < 28 * 1024` 就跳过本次，日志写"堆不足，跳过版本检查"。
这比 OOM 崩在 TLS 里好。

## 7. CI 与发布

`firmware.yml` 改动：

1. 注入版本号：`PROJECT_VER="$(date -u +%Y%m%d.%H%M)"`
2. 产出三个文件：

   | 文件 | 用途 | 烧录/上传方式 |
   |---|---|---|
   | `sms_forwarder-merged.bin` | 全新芯片 / 明确要清空配置 | `write_flash 0x0`、ESP Launchpad |
   | `sms_forwarder-ota.bin` | 网页升级 | 只在后台「固件升级」上传 |
   | `latest.json` | 设备每天检查 | 设备自己 GET |

3. `merge_bin` 的 app 偏移从 `0x10000` 改成 **`0x20000`**（分区表变了，容易漏）
4. `latest.json` 里的 `url` 用固定地址
   `https://github.com/chenxuuu/sms_forwarding/releases/latest/download/sms_forwarder-ota.bin`
5. Release 正文分两段写清楚"整包 vs ota 包"，并提示老设备迁移要用 `idf.py flash`（§2.3）

## 8. 注意点清单

| # | 风险 | 触发条件 | 处理 |
|---|---|---|---|
| 1 | **合并固件被当成 OTA 包上传，写坏分区** | 用户从 Release 随手抓第一个文件 | §4.4 镜像头校验（项目名比对）+ UI 明写"只能传 ota 包" |
| 2 | **merged bin 抹掉 NVS** | 迁移时 `write_flash 0x0 merged.bin` | §2.3 用 `idf.py flash` 或分段写；文档三处强调 |
| 3 | **38 KB 堆做 TLS 握手失败** | 版本检查 / 上传无关，检查时峰值可达 30–45 KB | 检查前判堆；实测记录 `heap_caps_get_minimum_free_size()`；必要时降 `MBEDTLS_SSL_IN_CONTENT_LEN` |
| 4 | 断电 / 断网导致写一半 | 上传过程中 | 写的是非活动槽，当前固件完好；上电后照常启动旧版，用户重传即可 |
| 5 | 新固件能启动但功能坏 | 新版本有 bug | rollback（§4.5）；确认窗口 45 秒，别一启动就标有效 |
| 6 | 用 `erase_flash` 后配置全丢 | 用户在 ESP Launchpad 点了 Erase | 文档写明；升级页面下方也提示"不要擦除整片 flash" |
| 7 | 证书校验因未对时而失败 | 断电重启后立刻检查 | 强制 `time > 1600000000` 才检查 |
| 8 | GitHub 302 跳转带来二次握手 | 每次检查 | 允许跳转但限制次数；失败按退避重试，不刷日志 |
| 9 | 后台密码 = 固件写入权 | basic auth 明文 | 一期：文档强调改默认密码；二期再考虑 HTTPS 反代 / 签名 |
| 10 | httpd 单任务被上传占住 | 上传 5–20 秒 | 前端进度条 + `ota_busy` 拒绝并发写操作 |
| 11 | 路由数不够 | 现在 29/32，本次 +3 | `max_uri_handlers` 直接提到 40（`web_server.c:700`） |
| 12 | 版本号格式跑偏 | 有人手动改了 `PROJECT_VER` | 非规范格式一律按"开发版"处理：不比较、不提示 |
| 13 | flash 擦写寿命 | 只有手动上传才写 | 无需优化；但不要做"每天重刷"（已由 D1 排除） |
| 14 | 升级撞上定时任务 / 收短信 | 用户恰好在任务执行时上传 | 升级前检查 `ota_busy` 反向：定时任务执行中拒绝上传，提示"任务执行中，请稍后" |
| 15 | 升级期间 4G 掉线 | 重启后模组重新注册 10–30 秒 | 正常现象，页面提示"重启约 20 秒"；升级完成后推一条通知 |

## 9. 后续可选（不在本次）

- **镜像签名 / secure boot v2**：能防"拿到后台密码就能刷任意固件"，但 eFuse 熔断不可逆、
  CI 要加签名步骤、且与 Arduino 版的迁移冲突。等真有需求再说。
- 上传前的 SHA-256 校验（需要设备端算 1.3 MB 哈希，约 1 秒，收益只是防传输位翻转）。
- 配置版本迁移框架（NVS 里记 `cfgVer`，启动时按版本升级字段）。
- 网页端 HTTPS（反向代理或自签证书）。

## 10. 测试计划（实机必做）

| # | 用例 | 预期 |
|---|---|---|
| 1 | 记录当前 `/config` 全部字段 → `idf.py flash` 刷带 OTA 的新固件 → 再读 `/config` | 账号、邮件、推送通道、黑名单、选机号码、定时任务、WiFi 全部不变 |
| 2 | 网页上传正确 `-ota.bin` | 进度走完 → 重启 → `/status.version` 变成新版本 |
| 3 | 上传 `-merged.bin` | 400 + "请上传 *-ota.bin"，设备不重启，版本不变 |
| 4 | 上传到 50% 拔网线 | 请求失败，设备仍跑旧固件，重启后正常 |
| 5 | 上传到 90% 直接断电（USB 拔掉） | 上电后仍跑旧固件（旧槽未动） |
| 6 | 上传一个"能启动但故意不确认"的固件（临时把确认 task 注释掉） | 复位后回滚到旧版本，日志有 rollback 记录 |
| 7 | 后台把 `latest.json` 的版本改大 → 点「立即检查」 | 卡片出现"有新版本"，链接正确，设备没有下载行为（抓日志确认） |
| 8 | 断网状态下点「立即检查」 | 15 秒超时，提示失败，主功能不受影响 |
| 9 | 升级全过程抓 `heap_caps_get_minimum_free_size()` | 记录峰值，低于 15 KB 要回头调 TLS 缓冲 |
| 10 | 升级完成后等 5 分钟 | 收短信、推送、定时任务均正常（回归） |

## 11. 任务拆分

- [ ] **T1** 分区表 + sdkconfig：`partitions.csv` 改两槽、`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`、`idf/CMakeLists.txt` 注入 `PROJECT_VER`
- [ ] **T2** `components/ota/` 骨架：状态机、NVS 缓存、`ota_version()`、`ota_busy()`
- [ ] **T3** 写入路径：`ota_begin/write/end/abort` + 镜像头校验 + 进度
- [ ] **T4** `ota_confirm_running_image()`：`app_main.c` 45 秒确认 task，配网分支也确认
- [ ] **T5** Web 上传：`POST /ota`（流式 + Content-Length 校验 + `ota_busy` 并发保护）
- [ ] **T6** Web 状态与检查：`GET /ota`、`POST /ota_check`、`/status` 三个新字段
- [ ] **T7** 每日检查：`esp_timer` + 前提判断 + manifest 解析 + NVS 缓存 + 退避
- [ ] **T8** 前端：设备页「固件升级」卡片、上传进度、重启轮询、概览页小提示
- [ ] **T9** CI：`PROJECT_VER` 注入、`-ota.bin`、`latest.json`、app 偏移改 `0x20000`、Release 文案
- [ ] **T10** 文档：`README.md` 升级章节、`idf/docs/api.md`（3 个接口）、`modules.md`（ota 组件）、
      `architecture.md`（分区表 + 组件表一行）、`AGENTS.md`（分区表红线）
- [ ] **T11** 实机跑完 §10 的 10 个用例

## 12. 参考

- 现有异步 handler 写法：`web_api.c` 的 `mail_test_async` / `push_test_async`
- 通知 + 等待发完再重启的写法：`forwarder.c` 的 `RESETPASS` 分支
- 时间有效性判断：`sched.c` 里 `now < 1600000000` 的守卫
- ESP-IDF：`esp_ota_ops.h`、`esp_app_format.h`（`esp_app_desc_t`）、`esp_https_ota`（本次只用
  `esp_http_client` 拉 manifest，不用它下载固件）
