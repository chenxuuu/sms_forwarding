<div align="center">

<img src="assets/photo.png" width="168" alt="短信转发器实物">

# 低成本短信转发器

**一张 4G 卡 + 一块 ESP32-C3，把收到的短信在几秒内推到邮件、钉钉、飞书、微信、Telegram**

[![License](https://img.shields.io/github/license/chenxuuu/sms_forwarding?style=flat-square&color=4f7cff)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1.1-e7352c?style=flat-square)](idf/)
[![MCU](https://img.shields.io/badge/MCU-ESP32--C3-5b8cff?style=flat-square)](#硬件)
[![Firmware](https://img.shields.io/github/actions/workflow/status/chenxuuu/sms_forwarding/firmware.yml?style=flat-square&label=%E5%9B%BA%E4%BB%B6%E6%9E%84%E5%BB%BA)](../../actions/workflows/firmware.yml)
[![Release](https://img.shields.io/github/v/release/chenxuuu/sms_forwarding?style=flat-square&label=%E6%9C%80%E6%96%B0%E5%9B%BA%E4%BB%B6&color=22b8a6)](../../releases/latest)
[![Demo](https://img.shields.io/badge/%E5%9C%A8%E7%BA%BF%E7%8E%A9%E4%B8%80%E4%B8%8B-8b5cf6?style=flat-square)](https://sms.j2.cx/)

[在线演示](https://sms.j2.cx/) · [硬件采购](#硬件) · [烧录](#烧录) · [视频教程](https://www.bilibili.com/video/BV1cSmABYEiX) · [常见问题](#常见问题)

</div>

<img src="assets/flow.svg" width="100%" alt="原理：SIM 卡收到短信，4G 模组经 UART 交给 ESP32-C3，再通过 WiFi 推送到邮件与各种平台">

> **老方案在哪**：[`old-luatos` 分支](https://github.com/chenxuuu/sms_forwarding/tree/old-luatos)（2022 年的 LuatOS 版）、
> [`code/`](code/)（Arduino 框架版，文档见 [code/README.md](code/README.md)）。当前主线是 [`idf/`](idf/)。
>
> **免责声明**：本项目仅供个人学习与自用。请遵守所在地法律法规、运营商条款与短信服务协议；
> 转发内容可能涉及他人隐私，请自行确认你有权处理。因使用本项目产生的费用、封号或法律风险由使用者自行承担。
>
> **功能边界**：只做短信接收、转发与保号。多卡控制、通话、拨号、开放接口、自动化（自动回复 / 自动操作）
> **永远不会支持**，请勿提出 issue 或 PR。

## 它解决什么问题

给设备插一张手机卡（Nano SIM），开机后连上 WiFi 并盯着 4G 模组。
任何人给这张卡发短信，设备会解析内容、合并长短信、过滤黑名单，然后**同时**转发到你配置的渠道。

- **不需要服务器、不需要公网 IP**：设备直连邮件服务器和推送平台，短信内容不经过任何中转
- **硬件约 28 元**，插一根 USB 线就能 7×24 挂着，长期挂着建议配个 5V 电源
- **专为"养卡"设计**：低流量保号操作 + 定时任务，让卡保持活跃又不烧钱

## 界面

固件内置完整网页（不到 100 KB，直接嵌在 Flash 里），手机上也能用。
主题支持深色 / 浅色 / 跟随系统。

| 概览 | 推送通道 |
|---|---|
| <img src="assets/web-overview.png" alt="概览页"> | <img src="assets/web-push.png" alt="推送通道页"> |

## 功能一览

| 能力 | 说明 |
|---|---|
| **10 种推送通道** | 邮件 + POST JSON / GET / Bark / 钉钉 / 飞书 / PushPlus / Server酱 / Gotify / Telegram / 自定义模板；默认 5 个槽位、最多 50 个，可随时启用停用，每个都能单独点「测试」 |
| **长短信合并** | PDU 模式解码，多段长短信 30 秒内自动拼合，中文不乱码 |
| **黑名单** | 按号码过滤验证码轰炸和骚扰短信 |
| **远程命令** | 管理员号码发 `SMS:号码:内容` 远程发短信、`RESET` 重启、`RESETPASS` 重置后台密码并推送新密码 |
| **保号** | 模组 Ping / 访问指定网址（默认只下 10 字节），按需消耗余额 |
| **定时任务** | 指定时间自动发短信 / Ping / 访问网址；错过时间会补执行，失败只重试 1 次；可设「每 N 天」循环 |
| **自动恢复** | WiFi 掉线重连；4G 掉网按「飞行模式（10 秒起，翻倍至 5 分钟）→ 软重启 → 断电重启」分级自愈，连续 5 轮失败才提醒 |
| **配置兼容** | NVS 命名空间与键名和 Arduino 版一致，可从老固件直接升级，配置不丢 |

## 硬件

**不想焊接**：直接买成品套件，成本和自己攒一样（支持移动 / 联通 / 电信卡）

- [小蓝鲸 WIFI 短信宝](https://item.taobao.com/item.htm?id=1003711355912)（找客服问）
- [4G FPC 天线](https://item.taobao.com/item.htm?id=1003711355912&skuId=6162872574943)（与开发板同购）

**自己接**：总成本约 ¥27.8（价格会浮动；DIY 方案目前实测支持移动 / 联通卡）

- ESP32-C3 开发板：[SuperMini](https://item.taobao.com/item.htm?id=852057780489&skuId=5813710390565)，¥9.5 包邮
- 4G 模组：[小蓝鲸 ML307R-DC 核心板](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥16.3 包邮
- [4G FPC 天线](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥2，与核心板同购
- Nano SIM 卡一张 + 5 根杜邦线

<p align="center"><img src="assets/wiring.svg" width="780" alt="接线：GPIO5→EN、GPIO3→RX、GPIO4→TX、GND 共地、5V→VCC"></p>

> 模组跑的是出厂 AT 固件，**不用刷机、不用改**，只当"能收短信的串口猫"用。

## 烧录

两种方式，选一种即可。

### 方式一：烧现成固件（不用装编译环境）

1. 从 [Releases](../../releases/latest) 下载 `sms_forwarder-merged.bin`
2. 用 [ESP Launchpad](https://espressif.github.io/esp-launchpad/)（Chrome / Edge）选 ESP32-C3、烧录地址填 `0x0`；
   或命令行：`esptool.py --chip esp32c3 -p COM3 write_flash 0x0 sms_forwarder-merged.bin`
3. 配网：连热点 `SMS-Forwarder-XXXX`（无密码）→ 打开 <http://192.168.4.1/> → 选 WiFi、输密码 → 设备自动重启入网

固件在每次推送后由 GitHub Actions 自动构建，功能和源码编译的完全一致。

### 方式二：自己编译

需要 **ESP-IDF v6.1.1**（[官方安装器](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/get-started/index.html)）和一根 USB-C 数据线。

```bash
git clone https://github.com/chenxuuu/sms_forwarding.git
cd sms_forwarding/idf
idf.py build            # 首次约 3~5 分钟
idf.py -p COM3 flash    # 串口按实际改：Windows 是 COMx，Linux 是 /dev/ttyACM0
```

烧完同样按方式一的第 3 步配网。要批量烧多台，可以 `idf.py menuconfig → 短信转发器配置` 预填 WiFi，跳过配网。

### 首次配置

1. 浏览器打开 `http://<设备IP>/`，或 mDNS 域名 `http://sms-xxxx.local/`（后缀见串口日志）
2. 默认账号 `admin` / `admin123`，**登录后立刻到「后台管理」改掉**
3. 「推送通道」填邮箱或任一通道，点「测试」确认能收到
4. 「后台管理」填管理员手机号（之后可用短信远程操作）和本机号码（推送里会带上）

## 常见问题

| 现象 | 原因与处理 |
|---|---|
| 一直显示"重连中"、收不到短信 | 4G 模组没注册上。先看日志里的 `+CEREG`，模组偶发假死时**断一次电**最快；长期挂机建议用独立 5V 电源，别用电脑 USB |
| 设备 IP 老在变 | 路由器 DHCP 分配的。设备地址变化时会推通知；想固定就在路由器给它的 MAC 做静态绑定，或直接用 mDNS 域名 |
| 换了路由器 / 改了 WiFi 密码 | 设备连续 3 次启动连不上会自动开热点 `SMS-Forwarder-XXXX`，连上去重新填一次即可；也可以提前在 `menuconfig` 里改好再烧录 |
| 想换运营商 / 锁网 | 「设备 → 选网配置」选手动，填 PLMN（移动 `46000`、联通 `46001`、电信 `46011`），保存后重启 |
| 推送失败但网页正常 | 先点该通道的「测试」看具体报错；多数是 token / webhook 填错或平台侧限制 |

更多踩坑记录见 [idf/docs/build-flash.md](idf/docs/build-flash.md)。

## 项目结构

```
sms_forwarding/
├── idf/                    # 主线：ESP-IDF v6.1.1 实现
│   ├── components/         # core / modem / net / notify / forwarder / sched / web
│   ├── docs/               # 架构、模块、接口、编译烧录文档
│   └── tools/              # 串口抓日志脚本
├── code/                   # 历史实现：Arduino 框架
│   ├── dev_doc/            # Arduino 版的架构与接口文档
│   └── README.md
├── assets/                 # 图片、原理图、纯前端演示页
├── README.md
└── .github/workflows/      # 固件构建、页面部署、Arduino 构建
```

想改代码？先读 [idf/docs/architecture.md](idf/docs/architecture.md) 和 [idf/docs/modules.md](idf/docs/modules.md)，
项目约定写在 [AGENTS.md](AGENTS.md)（日志、配置锁、新增推送通道的步骤都在里面）。

## License

[MIT](LICENSE)
