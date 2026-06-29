#ifndef CONFIG_TYPES_H
#define CONFIG_TYPES_H

#include <Arduino.h>

// 固件版本（/status 与启动日志展示）
#define FW_VERSION "1.0.1"

// 配网热点(STA 连接失败/未配置时广播)
#define AP_SSID_PREFIX "SMS-Forwarder-"
#define WIFI_CONNECT_TIMEOUT_MS 20000UL   // STA 首次连接超时，超时则进配网 AP

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE = 0,      // 未启用
  PUSH_TYPE_POST_JSON = 1, // POST JSON格式 {"sender":"xxx","message":"xxx","timestamp":"xxx"}
  PUSH_TYPE_BARK = 2,      // Bark格式 POST {"title":"xxx","body":"xxx"}
  PUSH_TYPE_GET = 3,       // GET请求，参数放URL中
  PUSH_TYPE_DINGTALK = 4,  // 钉钉机器人
  PUSH_TYPE_PUSHPLUS = 5,  // PushPlus
  PUSH_TYPE_SERVERCHAN = 6,// Server酱
  PUSH_TYPE_CUSTOM = 7,    // 自定义模板
  PUSH_TYPE_FEISHU = 8,    // 飞书机器人
  PUSH_TYPE_GOTIFY = 9,    // Gotify
  PUSH_TYPE_TELEGRAM = 10  // Telegram Bot
};

// 最大推送通道数
#define MAX_PUSH_CHANNELS 5

// 推送通道配置（通用设计，支持多种推送方式）
struct PushChannel {
  bool enabled;           // 是否启用
  PushType type;          // 推送类型
  String name;            // 通道名称（用于显示）
  String url;             // 推送URL（webhook地址）
  String key1;            // 额外参数1（如：钉钉secret、pushplus token等）
  String key2;            // 额外参数2（备用）
  String customBody;      // 自定义请求体模板（使用 {sender} {message} {timestamp} 占位符）
};

// 配置参数结构体
struct Config {
  String wifiSsid;     // WiFi SSID(NVS；为空则回退到 wifi_config.h 宏，再为空则进配网 AP)
  String wifiPass;     // WiFi 密码(NVS)
  String smtpServer;
  int smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  String adminPhone;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];  // 多推送通道
  bool emailEnabled;   // 邮件转发总开关(默认 true；关则不发邮件)
  bool pushEnabled;    // 推送转发总开关(默认 true；关则不发任何推送通道)
  String webUser;      // Web管理账号
  String webPass;      // Web管理密码
  String numberBlackList;  // 号码黑名单（换行符分隔）
  String forwardRules;     // 转发规则链(换行分隔，每行 type\tpattern\taction\tenabled；见 sms_logic.h::evalForwardRules)
  // ---- 保号定时任务（绝对日期，断电不忘）----
  bool kaEnabled;          // 是否启用保号
  int kaIntervalDays;      // 触发周期(天)，如 175(giffgaff 180 天前留余量)
  uint8_t kaAction;        // 动作: 1=蜂窝UDP流量保号 2=发短信 3=USSD查询
  String kaTarget;         // SMS 目标号码 或 USSD 码(UDP保号时忽略)
  uint32_t kaLastTime;     // 上次保号动作的绝对 Unix 时间戳(持久化于 NVS)
  // ---- 时间/NTP ----
  int tzOffsetMin;         // 本地时区相对 UTC 的分钟偏移(默认 +480 = UTC+8)
  String ntpServer;        // 首选 NTP 服务器
  // ---- 定时重启 / 每日心跳(本地小时，绝对时间判定) ----
  bool rebootEnabled;      // 是否启用每日定时重启
  int rebootHour;          // 重启的本地小时(0-23)
  bool hbEnabled;          // 是否启用每日心跳通知
  int hbHour;              // 心跳的本地小时(0-23)
  // ---- SIM / 蜂窝数据(默认不开流量；本机经 WiFi 转发，蜂窝数据仅用于保号与显示) ----
  bool dataEnabled;        // 是否允许蜂窝数据(默认 false=禁用 PDP，零流量)。开启后激活 APN+PDP，供保号/查询并显示蜂窝 IP
  String apn;              // 接入点名称(留空=运营商自动)；仅 dataEnabled 时下发 AT+CGDCONT
  String operatorPlmn;     // 运营商锁定 PLMN(MCC+MNC，空=自动 AT+COPS=0；否则 AT+COPS=1,2,"plmn")
  String phoneNumber;      // 本机号码(AT+CNUM 常为空，可在此手动填写用于显示)
};

// 保号动作类型
#define KA_ACTION_PING 1
#define KA_ACTION_SMS  2
#define KA_ACTION_USSD 3

// 默认Web管理账号密码
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

// 日志脱敏：默认不把完整短信正文/号码写入网页可见的日志环形缓冲(亦镜像到串口)。
// 仅记录长度与脱敏号码；置 1 可输出完整内容用于本地调试。
#ifndef SMS_LOG_VERBOSE
#define SMS_LOG_VERBOSE 0
#endif

// 长短信合并相关定义
#define MAX_CONCAT_PARTS 10       // 最大支持的长短信分段数
#define CONCAT_TIMEOUT_MS 30000   // 长短信等待超时时间(毫秒)
#define MAX_CONCAT_MESSAGES 5     // 最多同时缓存的长短信组数

// ---- 可调运行参数（低配保守默认；集中于此便于按硬件调整） ----
#define MODEM_HEALTH_INTERVAL_MS 60000UL  // 模组健康探测周期(ms)
#define SMS_POLL_INTERVAL_MS     60000UL  // 轮询 SIM 暂存短信周期(ms)：URC(+CMTI)失效时的兜底接收
#define SMS_CNMI_REASSERT_EVERY  5        // 每 N 次轮询重申一次 CNMI/CMGF(防长时间运行后 URC 直传被重置)
#define SMS_STORED_INDEX_QUEUE_MAX 8      // +CMTI 索引队列容量；满时退回 CMGL 全量兜底
#define SMS_STORED_INDEX_BATCH_MAX 1      // 每帧最多按索引读取/删除几条短信，降低网页无响应窗口
#define SMS_STORED_BATCH_GAP_MS  120UL    // 连续处理多条暂存短信之间给 WebServer 的喘息间隔
#define SMS_CMGR_TIMEOUT_MS      2500UL   // 按索引读取单条短信超时
#define SMS_CMGL_TIMEOUT_MS      3000UL   // 全量兜底扫描超时
#define SMS_CMGD_TIMEOUT_MS      1500UL   // 删除单条暂存短信超时
#define SIGNAL_FAST_INTERVAL_MS  30000UL  // 4G 信号条(CSQ)采样周期；避免频繁抢模组串口
#define SIGNAL_DETAIL_INTERVAL_MS 120000UL // 详细信号(RSRP/RSRQ/SINR/PCI/TAC)低频采样，降低后台 AT 占用
#define MODEM_HEALTH_FAIL_LIMIT  3        // 连续探测失败多少次触发自动断电恢复
#define WIFI_CHECK_INTERVAL_MS   15000UL  // WiFi 掉线兜底检查周期(ms)
#define REBOOT_MIN_UPTIME_MS     7200000UL // 每日定时重启所需的最小运行时长(2h>重启窗口1h)：防止重启后在同一小时内反复重启
#define MODEM_POWERDOWN_MS       1200     // EN 拉低关机保持时间(ML307R-DC 调优；换 ML307A 等可改)
#define MODEM_POWERUP_MS         6000     // EN 拉高后等待模组启动的硬上限(关键，按型号调)
#define MODEM_POWERUP_MIN_MS     1500     // 上电最小安定延时；之后轮询 AT 探活，应答即提前结束(省 3-4s/开机)
#define MODEM_INIT_AT_RETRIES    10       // modemInit AT 握手单轮重试次数
#define MODEM_INIT_CMD_RETRIES   5        // modemInit 其它配置命令重试次数
#define CELLULAR_BURN_BYTES      (48UL * 1024UL)  // giffgaff 中国约 20p/MB，48KiB≈0.98p；小于50KB仍能建立计费数据连接
#define CELLULAR_BURN_MAX_BYTES  (48UL * 1024UL)  // 硬上限：避免误把流量打得更大
#define CELLULAR_BURN_MIPSEND_BYTES (48UL * 1024UL)  // 单次 MIPSEND 发送大小，低于 UDP 65535 字节上限
#define CELLULAR_BURN_DEFAULT_HOST "223.5.5.5"    // 默认 UDP 目标：阿里公共 DNS
#define HTTP_CONNECT_TIMEOUT_MS  1500     // 推送 HTTP 连接超时：坏通道快速释放 worker，降低网页卡顿
#define HTTP_READ_TIMEOUT_MS     2500     // 推送 HTTP 读超时，避免慢 Webhook 长时间占住 worker
// 推送/邮件/测试三类慢任务已移到后台 worker(见 push.cpp::pushWorkerTask)，loop 不再被其阻塞，
// 故原 FORWARD_WEB_GRACE_MS / PUSH_JOB_GAP_MS 人为节流已删除(worker 单任务串行天然限速)。
// SLOW_WORK_WEB_GRACE_MS 仍保留：供 loop 线程上的保号/网页发短信/诊断UDP 在网页活跃后短暂避让模组AT。
#define SLOW_WORK_WEB_GRACE_MS   1500UL   // 刚处理过网页请求后，loop 上的模组类慢任务暂缓，给SPA留窗口
#define SLOW_WORK_MAX_DEFER_MS   10000UL  // 但单个慢任务被避让的上限：超过即强制执行，防 SPA 持续轮询饿死保号/网页发短信
#define TLS_MIN_FREE_HEAP        50000UL  // 发起 TLS(SMTP/HTTPS) 前要求的最小可分配堆(worker与网页并发，留余量)
// 后台 worker 任务参数：栈给足(TLS/SMTP 握手较吃栈；现有代码在 8KB 的 loopTask 上已能跑通，10KB 留余量)。
#define PUSH_WORKER_STACK        10240    // worker 任务栈字节数(按需调；可观察 uxTaskGetStackHighWaterMark)
#define PUSH_WORKER_PRIO         1        // 与 Arduino loopTask 同优先级，时间片公平，互不饿死
#define LOW_HEAP_RESTART_BYTES   20000UL  // 空闲堆低于此值且空闲时有序自重启
#define PUSH_QUEUE_MAX           10        // 离线/失败推送重试队列容量上限(有界，控 RAM)
#define EMAIL_QUEUE_MAX          3         // 短信转发邮件队列容量(有界，避免SMTP阻塞接收/网页)
#define PUSH_RETRY_MAX           4         // 单条消息最大重试次数
#define PUSH_RETRY_BASE_SEC      20        // 重试退避基数(秒)，失败通道少打扰系统
#define PUSH_RETRY_MAX_SEC       3600      // 重试退避封顶(秒)
#define DEDUP_WINDOW             16        // 短信去重最近哈希环大小
#define FWD_QUEUE_MAX            8         // 接收/转发解耦队列容量(有界)
#define OUT_SMS_QUEUE_MAX        3         // 网页端待发短信队列容量(有界，避免HTTP请求阻塞等待模组)
#define INBOX_MAX                50        // 本地收件箱留存最近短信条数(有界，控 RAM)
#define INBOX_BODY_MAX           320       // 收件箱单条正文最大留存字符(超出截断，转发不受影响)
#define SENT_MAX                 10        // 本地已发送短信留存条数(有界，控 RAM)

// 长短信分段结构
struct SmsPart {
  bool valid;           // 该分段是否有效
  String text;          // 分段内容
};

// 长短信缓存结构
struct ConcatSms {
  bool inUse;                           // 是否正在使用
  int refNumber;                        // 参考号
  String sender;                        // 发送者
  String timestamp;                     // 时间戳（使用第一个收到的分段的时间戳）
  int totalParts;                       // 总分段数
  int receivedParts;                    // 已收到的分段数
  unsigned long firstPartTime;          // 收到第一个分段的时间
  unsigned long lastPartTime;           // 收到最近一个分段的时间，按最近分段计算超时
  SmsPart parts[MAX_CONCAT_PARTS];      // 各分段内容
};

#endif
