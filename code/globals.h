#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <pdulib.h>
#define ENABLE_SMTP
#include <ReadyMail.h>
#include "config_types.h"
#include "sms_logic.h"  // 纯逻辑工具（json/url/html转义、脱敏、黑名单、去重、退避、模板扫描器）

// 串口映射
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5

// LED引脚定义（用于通过CI验证，给个假的）
#ifndef LED_BUILTIN
#define LED_BUILTIN 8
#endif

#define SERIAL_BUFFER_SIZE 500
#define MAX_PDU_LENGTH 300

// 全局变量声明
extern Config config;
extern Preferences preferences;
extern PDU pdu;
extern WiFiClientSecure ssl_client;
extern SMTPClient smtp;
extern WebServer server;
extern bool configValid;
extern bool timeSynced;
extern bool modemReady;
extern bool apMode;                   // 是否处于配网热点(SoftAP)模式
extern volatile bool gInWebRequest;   // true=正在HTTP请求处理栈内；此时AT函数不得再泵server.handleClient()(防WebServer重入崩溃)
extern volatile unsigned long smsRecvGuardUntil;  // >0且未到期=正在接收短信(+CMT后窗口)；loop内信号采样AT暂停，防清空Serial1冲掉PDU
extern unsigned long lastWebRequestMs; // 最近一次进入HTTP handler的时间；慢任务据此避让，保持网页可响应
extern unsigned long lastPrintTime;
extern unsigned long lastModemOkMs;   // 最近一次模组健康探测/AT成功的时间
extern unsigned long smsTotalCount;   // 累计已处理短信数
extern time_t lastSmsEpoch;           // 最近一条短信的时间(Unix秒)
extern int modemCsq;                  // 缓存的 4G 信号 CSQ(0-31，99=未知)，周期采样(AT+CSQ)
extern int modemBer;                  // AT+CSQ 误码率(0-7，99=未知)
extern int modemRsrp;                 // LTE RSRP(实际 dBm，如 -92；>=0 视为未知)，AT+MUESTATS 采样
extern String modemOperator;          // 缓存的运营商名(开机采样一次)
extern String modemImei;              // IMEI(开机采样一次)
extern String modemImsi;              // SIM IMSI(AT+CIMI，开机采样一次)
extern String modemApn;               // SIM 默认/已配置 APN(AT+CGDCONT?)
extern String modemMfr;               // 模组制造商(ATI)
extern String modemModel;             // 模组型号(ATI)
extern String modemFwVer;             // 模组固件版本(ATI)
extern String modemIccid;             // SIM ICCID(开机采样一次)
extern String modemPhone;             // 本机号码(AT+CNUM，常为空)
extern String modemCellIp;            // 蜂窝 PDP IP(仅 dataEnabled 时有效，否则空)
// 详细 LTE 服务小区信息(由 AT+MUESTATS="cell" 采样；未取得为未知值，前端显示 --)
extern int modemRsrq;                 // RSRQ(实际 dB，如 -11；999=未知)
extern int modemSinr;                 // SINR(实际 dB，如 14；999=未知)
extern int modemPci;                  // 物理小区 PCI(-1=未知)
extern String modemPlmn;              // PLMN(MCC+MNC，如 "46001")
extern String modemTac;               // 跟踪区 TAC(AT+CEREG? 十六进制)
extern ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

// ---- 并发原语：推送/邮件后台 worker 线程与 loop/web 线程之间的最小加锁面 ----
// gLogMux  : 保护日志环形缓冲(logCapture*/handleLog)；worker 大量写日志，web 读取，防 String 重分配崩溃。
// gWorkMux : 保护推送/邮件/测试三类队列槽位 + worker 对 config 的快照读(pushChannels/smtp*)。
//            约定：绝不在持有 gWorkMux 时做网络调用；持锁顺序只允许 workMux→logMux，不可反向。
// gSlowWorkBusy: worker 正在发送(邮件出队后在途窗口)期间置位，供 heapGuard/定时重启避让，避免在途时误判空闲。
extern SemaphoreHandle_t gLogMux;
extern SemaphoreHandle_t gWorkMux;
extern volatile bool gSlowWorkBusy;
void initConcurrency();      // 创建互斥量；须在启动 worker 前(setup 早期)调用一次
void startPushWorker();      // 启动推送/邮件后台 worker 任务

// NULL 安全加锁(互斥量在 setup 中创建前，早期 logCapture 已可能被调用 —— 此时单线程，跳过加锁即可)
static inline void muxLock(SemaphoreHandle_t m)   { if (m) xSemaphoreTake(m, portMAX_DELAY); }
static inline void muxUnlock(SemaphoreHandle_t m) { if (m) xSemaphoreGive(m); }

// loop 线程的阻塞等待(AT/保号/诊断)期间泵一次网页，保持网页可响应。
// gInWebRequest 重入保护：已在 web handler 栈内则不再重入 WebServer，仅让出 CPU。
// (modem/scheduler/web_handlers 三处此前各有一份同样实现，统一到此免漂移。)
static inline void pumpWebDuringWait() {
  if (gInWebRequest) { yield(); return; }
  gInWebRequest = true;
  server.handleClient();
  gInWebRequest = false;
  yield();
}

#endif
