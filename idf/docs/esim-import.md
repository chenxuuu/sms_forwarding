# eSIM 导入指南（下载 / 切换 / 删除）

网页上的「eSIM 管理」页负责读卡和切换，**下载新配置必须借助电脑**：
GSMA SGP.22 要用 TLS 连 SM-DP+、处理几十 KB 的加密安装包（BPP），
ESP32-C3 的 RAM 和算力都扛不住。仓库里的 `tools/esim_bridge.py` 把 lpac 的 APDU
转发到设备的 `/esim/apdu`，效果和在网页上点一下完全一样（都是往 eUICC 发 ES10x 指令）。

## 0. 前提

- 设备已联网，网页能打开「eSIM 管理」，能读到 EID 和配置列表。
- 卡是 eUICC（9eSIM、5ber、ESTK 等）；普通 SIM 会提示「打不开 eUICC 的逻辑通道」。
- 电脑和设备（例如 192.168.1.9）在同一网络。默认账号 `admin / admin123`。

## 1. 准备 lpac（电脑端）

1. 打开 <https://github.com/estkme-group/lpac/releases>。
2. **Windows 请下 v2.2.1 的 `lpac-windows-x86_64-mingw.zip`**，解压到例如 `C:\lpac`。
   - v2.3.0 的 Windows 版本在 stdio APDU 后端上有问题：收到第一条应答就退出，
     报 `euicc_init`，本项目的桥接用不了（见第 7 节）。
3. macOS / Linux 下对应 release 包即可，不需要读卡器和 pcscd。

不需要设置 `LPAC_APDU`：`esim_bridge.py` 会强制设成 `stdio`。

## 2. 先验证 APDU 通路

在仓库根目录执行（路径按实际改）：

```bash
python tools/esim_bridge.py --host 192.168.1.9 -- "C:\lpac\lpac.exe" chip info
```

看到 `"eidValue":"…"` 和 `返回码 0` 就说明通道正常。这一步只读，不会改动卡里的任何东西。

参数：`--host` 设备地址、`--port` 默认 80、`--user/--password` 默认 admin/admin123、
`--timeout` 单条 APDU 的 HTTP 超时（秒）。

## 3. 在网页上取激活码

「eSIM 管理 → 导入新配置」：

1. 选运营商给的二维码图片（浏览器本地解码，不上传），或者直接粘贴 `LPA:1$…`。
2. 卡商要求确认码时填「确认码」。
3. 点「生成导入命令」，页面会给出带 `--lpac` 的完整命令，复制即可。

## 4. 下载配置

```bash
python tools/esim_bridge.py --host 192.168.1.9 -- "C:\lpac\lpac.exe" \
  profile download -a 'LPA:1$rsp.truphone.com$QRF-SPEEDTEST'
```

激活码**必须用单引号包住**：里面有美元符号，用双引号会被 bash 或 PowerShell 展开成空字符串。

成功时最后一行是：

```json
{"type":"lpa","payload":{"code":0,"message":"success","data":null}}
```

下载过程会转发上百条 APDU，耗时 1–3 分钟，中途不要关页面或重启设备。
完成后回网页点「重新读取」，列表里出现新的 ICCID 就说明写卡成功。

> **判断成败只看 ICCID / EID**，不要等驻网。外卡漫游附着慢是正常现象，
> 有的测试卡本身没有余额，永远也注册不上。

## 5. 切换

网页上点「切换」，或：

```bash
python tools/esim_bridge.py --host 192.168.1.9 -- "C:\lpac\lpac.exe" profile enable <ICCID>
```

切换成功后固件会在 2 秒后软重启模组（`AT+CFUN=1,1`）重新注册。
网页会自动轮询，约 20 秒后恢复。

## 6. 删除

网页上点「删除」，按提示把 ICCID 原样输入一遍确认；或者：

```bash
python tools/esim_bridge.py --host 192.168.1.9 -- "C:\lpac\lpac.exe" profile delete <ICCID>
```

规则：

- **正在使用中的配置不允许删除**，固件会直接拒绝（要删先切到别的配置），
  避免把唯一能用的卡删掉导致失联。
- 删除不可撤销，只能拿运营商的激活码重新下载。
- 删除后 lpac 会向 SM-DP+ 发通知，它自己会处理。

## 7. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `euicc_init` 且 lpac 立刻退出（Windows） | 用了 v2.3.0，换 v2.2.1 的 Windows 包 |
| `打不开 eUICC 的逻辑通道` | 上一次操作没关干净通道；到「设备」页软重启模组后重试 |
| `内存不足，无法发送 APDU` | 设备堆碎片化；稍后重试，反复出现请反馈 |
| `es9p_authenticate_client: Refused` | 激活码失效、被卡商限制，或这张卡不被该 SM-DP+ 接受 |
| 返回码 5（CAT busy） | 终端忙，稍等几秒重试；启用时固件会自动改用「不刷新」再试一次 |
| 删除返回「找不到该 ICCID / AID」 | 抄错了 ICCID，或这条已经删掉了 |
| 列表里 ICCID 是 19 位 | 正常，BCD 末位补 `F` |

## 8. 公开测试配置

<https://euicc-manual.osmocom.org/docs/rsp/known-test-profile/> 列了一批可反复下载的免费 profile。
2026-11 实测这张卡（GSMA Live CI）可以用：

```
LPA:1$rsp.truphone.com$QRF-SPEEDTEST
```

其它几个地址（`QR-G-5C-1LS-1W1Z9P7` 等）当时返回 `Refused`，属于服务端策略，不是设备问题。
测试完可以在网页上删掉。

## 9. 安全提醒

- 删除前确认 ICCID：删错只能用激活码重新下载，有的卡商不给补。
- 不要删除当前使用中的配置（固件也会拒绝）。
- 桥接只在局域网内用，不要把 `/esim/apdu` 暴露到公网。
