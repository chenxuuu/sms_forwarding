#include "scheduler.h"
#include "modem.h"
#include "config.h"
#include "push.h"
#include "sms_process.h"
#include "web_handlers.h"
#include <time.h>

struct KeepAliveJob {
  bool pending;
  bool running;
  bool done;
  bool success;
  bool manual;
  unsigned long queuedMs;
  String message;
};

static KeepAliveJob kaJob;

static void schedulerPumpWebDuringWait() { pumpWebDuringWait(); }  // 统一实现见 globals.h

// 保号动作：激活数据连接 + 发送固定字节 UDP 上行，再关闭 PDP 省流量。
// giffgaff 中国漫游约 20p/MB，48KiB 用户数据约 0.98p；固定上行比小 HTTP 首页更可控。
static bool keepAliveDataPing() {
  logCaptureLn("保号: 激活数据连接(CGACT=1)...");
  String r = sendATCommand("AT+CGACT=1,1", 10000);
  bool active = (r.indexOf("OK") >= 0);
  unsigned long stableStart = millis();
  while (millis() - stableStart < 500) schedulerPumpWebDuringWait();
  bool ok = active && consumeCellularDataBytes(CELLULAR_BURN_BYTES, CELLULAR_BURN_DEFAULT_HOST, 53);
  if (ok) logCaptureF("保号: 已发送约 %luKB 蜂窝 UDP 上行流量\n",
                      (unsigned long)(CELLULAR_BURN_BYTES / 1024UL));
  else logCaptureLn("保号: UDP 流量保号失败");
  // 仅在用户未启用蜂窝数据时关闭 PDP 省流量；若用户已在 SIM 页开启数据，保留其连接不误关。
  if (!config.dataEnabled) sendATCommand("AT+CGACT=0,1", 5000);
  return ok;
}

// 保号动作：USSD 查询（如查余额），亦构成一次动账
static bool keepAliveUssd(const String& code) {
  if (code.length() == 0) return false;
  String cmd = "AT+CUSD=1,\"" + code + "\",15";
  String r = sendATCommand(cmd.c_str(), 15000);
  return (r.indexOf("OK") >= 0);
}

bool keepAliveRunNow() {
  logCaptureLn("执行保号动作...");
  bool ok = false;
  switch (config.kaAction) {
    case KA_ACTION_SMS:
      if (isValidPhoneNumber(config.kaTarget)) {
        ok = sendSMS(config.kaTarget.c_str(), "keepalive");
      } else {
        logCaptureLn("保号: 短信目标号码无效，跳过");
      }
      break;
    case KA_ACTION_USSD:
      ok = keepAliveUssd(config.kaTarget);
      break;
    case KA_ACTION_PING:
    default:
      ok = keepAliveDataPing();
      break;
  }
  uint32_t now = (uint32_t)time(nullptr);
  if (ok && epochIsValid(now)) {
    config.kaLastTime = now;
    saveConfig();  // 立即持久化基准 -> 断电不忘日期
    logCaptureLn("保号动作成功，已更新并持久化基准日期");
    if (!enqueueEmailNotification("保号动作已执行", "已执行一次保号动作以维持 SIM 活跃。")) {
      logCaptureLn("保号通知邮件未入队：邮件队列已满");
    }
  } else {
    logCaptureLn("保号动作未成功，将于下次检查重试");
  }
  return ok;
}

bool enqueueKeepAliveRunNow(String& message) {
  if (kaJob.pending || kaJob.running) {
    message = "保号动作已在后台执行中";
    return true;
  }
  kaJob.pending = true;
  kaJob.running = false;
  kaJob.done = false;
  kaJob.success = false;
  kaJob.manual = true;
  kaJob.queuedMs = millis();
  kaJob.message = "保号动作已排队，可继续刷新网页";
  message = kaJob.message;
  logCaptureLn("保号动作已入队");
  return true;
}

static void enqueueScheduledKeepAlive() {
  if (kaJob.pending || kaJob.running) return;
  String ignored;
  enqueueKeepAliveRunNow(ignored);
  kaJob.manual = false;
}

static void processKeepAliveJob() {
  if (!kaJob.pending) return;
  // 网页刚活跃则短暂避让模组AT，给SPA留窗口；但避让有上限——超过 SLOW_WORK_MAX_DEFER_MS 仍强制执行，
  // 防 SPA 持续轮询(每次刷新都刷新 lastWebRequestMs)把保号动作无限期饿死。
  if (lastWebRequestMs != 0 && millis() - lastWebRequestMs < SLOW_WORK_WEB_GRACE_MS &&
      millis() - kaJob.queuedMs < SLOW_WORK_MAX_DEFER_MS) return;
  if (smsUrcReceiving() || smsStoredWorkPending()) return;
  if (!modemReady) {
    kaJob.pending = false;
    kaJob.running = false;
    kaJob.done = true;
    kaJob.success = false;
    kaJob.message = "模组未就绪，保号动作未执行";
    logCaptureLn(kaJob.message);
    return;
  }

  kaJob.pending = false;
  kaJob.running = true;
  kaJob.done = false;
  kaJob.success = false;
  kaJob.message = kaJob.manual ? "手动保号执行中" : "定时保号执行中";

  bool ok = keepAliveRunNow();
  kaJob.running = false;
  kaJob.done = true;
  kaJob.success = ok;
  kaJob.message = ok ? "保号动作已完成" : "保号动作失败，请查看日志";
}

void keepAliveResetBaseline() {
  uint32_t now = (uint32_t)time(nullptr);
  if (epochIsValid(now)) {
    config.kaLastTime = now;
    saveConfig();
    logCaptureLn("保号基准日已重置为当前时间");
  } else {
    logCaptureLn("时间未同步，无法重置保号基准");
  }
}

// loop() 周期调用。时间无效(NTP 未同步)时不计时、下个 loop 再试；有效后每小时检查一次。
void keepAliveTick() {
  static unsigned long lastCheck = 0;
  processKeepAliveJob();
  if (!config.kaEnabled) return;
  if (lastCheck != 0 && millis() - lastCheck < 3600000UL) return;  // 每小时
  uint32_t now = (uint32_t)time(nullptr);
  if (!epochIsValid(now)) return;  // 时间无效，先不计时
  lastCheck = millis();
  if (keepAliveDue(config.kaLastTime, now, (uint32_t)config.kaIntervalDays)) {
    logCaptureLn("保号到期，触发动作");
    enqueueScheduledKeepAlive();
  }
}

// 每日定时任务(定时重启 / 每日心跳)：按本地小时触发，每天各一次。绝对时间判定，断电不误触发。
void dailyTasksTick() {
  static unsigned long lastCheck = 0;
  static long hbLastDay = -1, rbLastDay = -1;
  if (millis() - lastCheck < 60000UL) return;  // 每分钟检查一次
  lastCheck = millis();
  uint32_t now = (uint32_t)time(nullptr);
  if (!epochIsValid(now)) return;
  int64_t local = (int64_t)now + (int64_t)config.tzOffsetMin * 60;  // 本地时间(有符号，负时区也正确)
  int hour = (int)((local / 3600) % 24);
  long day = (long)(local / 86400);

  if (config.hbEnabled && hour == config.hbHour && day != hbLastDay) {
    hbLastDay = day;
    String body = "设备运行正常。\n累计转发: " + String(smsTotalCount) +
                  " 条\n空闲堆: " + String(ESP.getFreeHeap() / 1024) + " KB";
    if (enqueueEmailNotification("设备每日心跳", body.c_str())) logCaptureLn("每日心跳通知已入队");
    else logCaptureLn("每日心跳通知未入队：邮件队列已满");
  }
  // 定时重启：①运行时长须超过重启窗口(>1h)才允许，否则重启后会在同一重启小时内反复重启
  // (rbLastDay 是普通 static，软复位后丢失；故不能只靠它防循环——用 uptime 兜底，对掉电也安全)；
  // ②rbLastDay 仅在“真正重启前”置位，繁忙时不置位、下一分钟重试，避免漏掉当天重启。
  if (config.rebootEnabled && hour == config.rebootHour && day != rbLastDay &&
      millis() >= REBOOT_MIN_UPTIME_MS) {
    if (concatBufferIdle() && !gSlowWorkBusy && retryQueueDepth() == 0 && forwardQueueDepth() == 0 &&
        outgoingSmsQueueDepth() == 0 && emailQueueDepth() == 0) {
      rbLastDay = day;
      logCaptureLn("每日定时重启...");
      delay(300);
      ESP.restart();
    }
  }
}

String keepAliveStatusJson() {
  uint32_t now = (uint32_t)time(nullptr);
  String j;
  j.reserve(360);
  j += "{";
  j += "\"enabled\":"; j += (config.kaEnabled ? "true" : "false"); j += ",";
  j += "\"intervalDays\":"; j += String(config.kaIntervalDays); j += ",";
  j += "\"action\":"; j += String((int)config.kaAction); j += ",";
  j += "\"target\":\""; j += jsonEscape(config.kaTarget); j += "\",";
  j += "\"lastTime\":"; j += String(config.kaLastTime); j += ",";
  long daysLeft = -1;
  if (epochIsValid(now) && config.kaLastTime > 0) {
    long nextDue = (long)config.kaLastTime + (long)config.kaIntervalDays * 86400L;
    daysLeft = (nextDue - (long)now) / 86400L;
  }
  j += "\"daysLeft\":"; j += String(daysLeft); j += ",";
  j += "\"timeValid\":"; j += (epochIsValid(now) ? "true" : "false"); j += ",";
  j += "\"jobQueued\":"; j += (kaJob.pending ? "true" : "false"); j += ",";
  j += "\"jobRunning\":"; j += (kaJob.running ? "true" : "false"); j += ",";
  j += "\"jobDone\":"; j += (kaJob.done ? "true" : "false"); j += ",";
  j += "\"jobSuccess\":"; j += (kaJob.success ? "true" : "false"); j += ",";
  j += "\"jobMessage\":\""; j += jsonEscape(kaJob.message.length() ? kaJob.message : ""); j += "\"";
  j += "}";
  return j;
}
