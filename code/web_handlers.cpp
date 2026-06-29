#include "web_handlers.h"
#include "web_html.h"
#include "config.h"
#include "modem.h"
#include "push.h"
#include "scheduler.h"
#include "inbox.h"
#include "sms_process.h"
#include "wifi_config.h"
#include <esp_system.h>
#include <Update.h>

// ---- 日志环形缓冲区 ----
String logBuffer[LOG_BUF_SIZE];
int logBufIdx = 0;
int logBufCount = 0;
unsigned long logSeq = 0;  // 单调递增的累计行号(/log since 游标)
static String _logLine;    // 行缓冲：logCapture 写入这里，logCaptureLn 提交整行

// 首页是大块 HTML/JS/CSS，允许浏览器缓存；配置变更后递增版本让缓存自动失效。
static uint32_t webPageRev = 1;
static uint32_t webPageBootNonce = 0;

static uint32_t getWebPageBootNonce() {
  if (webPageBootNonce == 0) {
    webPageBootNonce = esp_random();
    if (webPageBootNonce == 0) webPageBootNonce = (uint32_t)millis() | 1u;
  }
  return webPageBootNonce;
}

static void bumpWebPageCacheRev() {
  webPageRev++;
  if (webPageRev == 0) webPageRev = 1;
}

static String makeWebPageEtag() {
  String tag;
  tag.reserve(48);
  tag += "\"sms-";
  tag += FW_VERSION;
  tag += "-";
  tag += String(getWebPageBootNonce(), HEX);
  tag += "-";
  tag += String(webPageRev, HEX);
  tag += "\"";
  return tag;
}

static void _logAppend(const String& line) {
  logBuffer[logBufIdx] = line;
  logBufIdx = (logBufIdx + 1) % LOG_BUF_SIZE;
  if (logBufCount < LOG_BUF_SIZE) logBufCount++;
  logSeq++;
}

static void _logCommit() {
  if (_logLine.length() > 0) {
    _logAppend(_logLine);
    _logLine = "";
  }
}

// 以下 logCapture* 由 loop/URC/web/后台 worker 多线程并发调用 —— 整段在 gLogMux 内执行，
// 杜绝 worker 写入与 web 读取(/log)同时操作 logBuffer/_logLine 导致的 String 重分配崩溃。
void logCapture(const String& msg) {
  Serial.print(msg);
  muxLock(gLogMux);
  _logLine += msg;
  muxUnlock(gLogMux);
}

void logCapture(const char* msg) {
  Serial.print(msg);
  muxLock(gLogMux);
  _logLine += msg;
  muxUnlock(gLogMux);
}

void logCaptureF(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  muxLock(gLogMux);
  _logLine += buf;
  // 如果格式化字符串以 \n 结尾，则提交此行
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') {
    _logLine.trim();  // 去掉尾部空格和可能多余的 \n
    _logCommit();
  }
  muxUnlock(gLogMux);
}

void logCaptureLn(const String& msg) {
  Serial.println(msg);
  muxLock(gLogMux);
  _logLine += msg;
  _logCommit();
  muxUnlock(gLogMux);
}

void logCaptureLn(const char* msg) {
  Serial.println(msg);
  muxLock(gLogMux);
  _logLine += msg;
  _logCommit();
  muxUnlock(gLogMux);
}

// 检查HTTP Basic认证
bool checkAuth() {
  lastWebRequestMs = millis();
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

// 处理配置页面请求
//
// 关键优化：不再 `String html = String(htmlPage)`(整页 ~37KB 拷进堆) + ~20 次
// replace()(每次可重分配) + send()(再缓冲)。改为分块流式：静态片段用 sendContent_P 直接
// 从 flash 流出，仅把小动态值/通道块写出。占位符识别走 sms_logic.h::streamTemplate
// (已主机测试，安全跳过 CSS 中的裸 %)。峰值堆从 ~80KB+ 降到 <10KB。
void handleRoot() {
  if (!checkAuth()) return;
  String etag = makeWebPageEtag();
  String inm = server.header("If-None-Match");
  if (inm.indexOf(etag) >= 0) {
    server.sendHeader("Cache-Control", "private, no-cache, must-revalidate");
    server.sendHeader("ETag", etag);
    server.sendHeader("Vary", "Authorization");
    server.send(304, "text/plain", "");
    return;
  }

  auto escHtml = [](const String& v) -> String { return htmlEscape(v); };

  // 唯一较大的动态块：推送通道 HTML，预留容量一次成型，避免多次重分配
  String channelsHtml;
  channelsHtml.reserve(6144);
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enabledClass = config.pushChannels[i].enabled ? " enabled" : "";
    String checked = config.pushChannels[i].enabled ? " checked" : "";
    
    channelsHtml += "<div class=\"push-channel" + enabledClass + "\" id=\"channel" + idx + "\">";
    channelsHtml += "<div class=\"push-channel-header\">";
    channelsHtml += "<input type=\"checkbox\" name=\"push" + idx + "en\" id=\"push" + idx + "en\" onchange=\"toggleChannel(" + idx + ")\"" + checked + ">";
    channelsHtml += "<label for=\"push" + idx + "en\" class=\"label-inline\">启用推送通道 " + String(i + 1) + "</label>";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"push-channel-body\">";
    
    // 通道名称
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>通道名称</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "name\" value=\"" + escHtml(config.pushChannels[i].name) + "\" placeholder=\"自定义名称\">";
    channelsHtml += "</div>";
    
    // 推送类型
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送方式</label>";
    channelsHtml += "<select name=\"push" + idx + "type\" id=\"push" + idx + "type\" onchange=\"updateTypeHint(" + idx + ")\">";
    channelsHtml += "<option value=\"1\"" + String(config.pushChannels[i].type == PUSH_TYPE_POST_JSON ? " selected" : "") + ">POST JSON（通用格式）</option>";
    channelsHtml += "<option value=\"2\"" + String(config.pushChannels[i].type == PUSH_TYPE_BARK ? " selected" : "") + ">Bark（iOS推送）</option>";
    channelsHtml += "<option value=\"3\"" + String(config.pushChannels[i].type == PUSH_TYPE_GET ? " selected" : "") + ">GET请求（参数在URL中）</option>";
    channelsHtml += "<option value=\"4\"" + String(config.pushChannels[i].type == PUSH_TYPE_DINGTALK ? " selected" : "") + ">钉钉机器人</option>";
    channelsHtml += "<option value=\"5\"" + String(config.pushChannels[i].type == PUSH_TYPE_PUSHPLUS ? " selected" : "") + ">PushPlus</option>";
    channelsHtml += "<option value=\"6\"" + String(config.pushChannels[i].type == PUSH_TYPE_SERVERCHAN ? " selected" : "") + ">Server酱</option>";
    channelsHtml += "<option value=\"7\"" + String(config.pushChannels[i].type == PUSH_TYPE_CUSTOM ? " selected" : "") + ">自定义模板</option>";
    channelsHtml += "<option value=\"8\"" + String(config.pushChannels[i].type == PUSH_TYPE_FEISHU ? " selected" : "") + ">飞书机器人</option>";
    channelsHtml += "<option value=\"9\"" + String(config.pushChannels[i].type == PUSH_TYPE_GOTIFY ? " selected" : "") + ">Gotify</option>";
    channelsHtml += "<option value=\"10\"" + String(config.pushChannels[i].type == PUSH_TYPE_TELEGRAM ? " selected" : "") + ">Telegram Bot</option>";
    channelsHtml += "</select>";
    channelsHtml += "<div class=\"push-type-hint\" id=\"hint" + idx + "\"></div>";
    channelsHtml += "</div>";
    
    // URL
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送URL/Webhook</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "url\" value=\"" + escHtml(config.pushChannels[i].url) + "\" placeholder=\"http://your-server.com/api 或 webhook地址\">";
    channelsHtml += "</div>";
    
    // 额外参数区域（钉钉/PushPlus/Server酱等需要）
    channelsHtml += "<div id=\"extra" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label id=\"key1label" + idx + "\">参数1</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key1\" id=\"key1" + idx + "\" value=\"" + escHtml(config.pushChannels[i].key1) + "\">";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"form-group\" id=\"key2group" + idx + "\">";
    channelsHtml += "<label id=\"key2label" + idx + "\">参数2</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key2\" id=\"key2" + idx + "\" value=\"" + escHtml(config.pushChannels[i].key2) + "\">";
    channelsHtml += "</div>";
    channelsHtml += "</div>";
    
    // 自定义模板区域
    channelsHtml += "<div id=\"custom" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label>";
    channelsHtml += "<textarea name=\"push" + idx + "body\" rows=\"4\" style=\"width:100%;font-family:monospace;\">" + escHtml(config.pushChannels[i].customBody) + "</textarea>";
    channelsHtml += "</div>";
    channelsHtml += "</div>";
    
    // 测试按钮(置于通道体内，启用时可见)
    channelsHtml += "<button type=\"button\" class=\"btn btn-secondary btn-sm\" onclick=\"testPush(" + idx + ")\">测试推送</button>";
    channelsHtml += "<div class=\"result-box\" id=\"pushTestResult" + idx + "\"></div>";

    channelsHtml += "</div></div>";
  }

  // 概览页面的动态值
  long uptimeSec = millis() / 1000;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%ld:%02ld:%02ld", uptimeSec / 3600, (uptimeSec % 3600) / 60, uptimeSec % 60);
  bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                 config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
  int pushCount = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (config.pushChannels[i].enabled) pushCount++;
  }

  // 占位符解析器：命中已知 token 填值返回 true，未知原样保留
  auto resolve = [&](const String& name, String& out) -> bool {
    if (name == "IP") out = WiFi.localIP().toString();
    else if (name == "WIFI_SSID") out = escHtml(String(WiFi.SSID()));
    else if (name == "FREE_HEAP") out = String(ESP.getFreeHeap() / 1024) + " KB";
    else if (name == "UPTIME") out = String(uptimeBuf);
    else if (name == "WEB_USER") out = escHtml(config.webUser);
    else if (name == "WEB_PASS") out = escHtml(config.webPass);
    else if (name == "SMTP_SERVER") out = escHtml(config.smtpServer);
    else if (name == "SMTP_PORT") out = String(config.smtpPort);
    else if (name == "SMTP_USER") out = escHtml(config.smtpUser);
    else if (name == "SMTP_PASS") out = escHtml(config.smtpPass);
    else if (name == "SMTP_SEND_TO") out = escHtml(config.smtpSendTo);
    else if (name == "ADMIN_PHONE") out = escHtml(config.adminPhone);
    else if (name == "NUMBER_BLACK_LIST") out = escHtml(config.numberBlackList);
    else if (name == "FORWARD_RULES") out = escHtml(config.forwardRules);
    else if (name == "SMTP_CHECK") out = emailOk ? "已配置" : "未配置";
    else if (name == "EMAIL_EN") out = config.emailEnabled ? "checked" : "";
    else if (name == "PUSH_EN") out = config.pushEnabled ? "checked" : "";
    else if (name == "MODEM_CHECK") out = modemReady ? "已就绪" : "未就绪";
    else if (name == "PUSH_COUNT") out = String(pushCount);
    else if (name == "INBOX_MAX") out = String(INBOX_MAX);
    else if (name == "NTP") out = escHtml(config.ntpServer);
    else if (name == "RB_CHECKED") out = config.rebootEnabled ? "checked" : "";
    else if (name == "RB_HOUR") out = String(config.rebootHour);
    else if (name == "HB_CHECKED") out = config.hbEnabled ? "checked" : "";
    else if (name == "HB_HOUR") out = String(config.hbHour);
    else if (name == "TZ_OPTIONS") {
      static const int tzMin[] = {480, 540, 420, 330, 60, 0, -300, -360, -480};
      static const char* tzLbl[] = {"UTC+8 北京/香港", "UTC+9 东京", "UTC+7 曼谷", "UTC+5:30 印度",
                                    "UTC+1 中欧", "UTC/GMT", "UTC-5 纽约", "UTC-6 芝加哥", "UTC-8 洛杉矶"};
      String o; o.reserve(512);
      for (unsigned i = 0; i < sizeof(tzMin) / sizeof(tzMin[0]); i++) {
        o += "<option value=\""; o += String(tzMin[i]); o += "\"";
        if (tzMin[i] == config.tzOffsetMin) o += " selected";
        o += ">"; o += tzLbl[i]; o += "</option>";
      }
      out = o;
    }
    else if (name == "DATA_CHECKED") out = config.dataEnabled ? "checked" : "";
    else if (name == "APN") out = escHtml(config.apn);
    else if (name == "PHONE_NUMBER") out = escHtml(config.phoneNumber.length() ? config.phoneNumber : modemPhone);  // 空则预填自动读取号码
    else if (name == "OPERATOR_PLMN") out = escHtml(config.operatorPlmn);
    else if (name == "PUSH_CHANNELS") out = channelsHtml;
    else return false;
    return true;
  };

  // 分块流式发送：CONTENT_LENGTH_UNKNOWN 触发 chunked transfer-encoding。
  // ETag + 304 让刷新时只做轻量校验，避免每次完整重传大页面。
  server.sendHeader("Cache-Control", "private, no-cache, must-revalidate");
  server.sendHeader("ETag", etag);
  server.sendHeader("Vary", "Authorization");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamTemplate(htmlPage,
                 [](const char* p, size_t n) { if (n) server.sendContent_P(p, n); },
                 resolve);
  server.sendContent("");  // 终止 chunk
}

// 处理工具箱页面请求 — 已整合到主页，直接返回主页
void handleToolsPage() {
  handleRoot();
}

// 处理飞行模式控制请求
void handleFlightMode() {
  if (!checkAuth()) return;
  
  String action = server.arg("action");
  String json = "{";
  bool success = false;
  String message = "";
  
  if (action == "query") {
    // 查询当前功能模式
    logCaptureLn("网页端查询飞行模式: AT+CFUN?");
    String resp = sendATCommand("AT+CFUN?", 2000);
    logCaptureLn(String("CFUN查询响应: " + resp));
    
    if (resp.indexOf("+CFUN:") >= 0) {
      success = true;
      int idx = resp.indexOf("+CFUN:");
      int mode = resp.substring(idx + 6).toInt();
      
      String modeStr;
      String statusIcon;
      if (mode == 0) {
        modeStr = "最小功能模式（关机）";
        statusIcon = "";
      } else if (mode == 1) {
        modeStr = "全功能模式（正常）";
        statusIcon = "";
      } else if (mode == 4) {
        modeStr = "飞行模式（射频关闭）";
        statusIcon = "";
      } else {
        modeStr = "未知模式 (" + String(mode) + ")";
        statusIcon = "";
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>当前状态</td><td>" + statusIcon + " " + modeStr + "</td></tr>";
      message += "<tr><td>CFUN值</td><td>" + String(mode) + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (action == "toggle") {
    // 先查询当前状态
    String resp = sendATCommand("AT+CFUN?", 2000);
    logCaptureLn(String("CFUN查询响应: " + resp));
    
    if (resp.indexOf("+CFUN:") >= 0) {
      int idx = resp.indexOf("+CFUN:");
      int currentMode = resp.substring(idx + 6).toInt();
      
      // 切换模式：1(正常) <-> 4(飞行模式)
      int newMode = (currentMode == 1) ? 4 : 1;
      String cmd = "AT+CFUN=" + String(newMode);
      
      logCaptureLn(String("切换飞行模式: " + cmd));
      String setResp = sendATCommand(cmd.c_str(), 5000);
      logCaptureLn(String("CFUN设置响应: " + setResp));
      
      if (setResp.indexOf("OK") >= 0) {
        success = true;
        if (newMode == 4) {
          message = "已开启飞行模式 <br>模组射频已关闭，无法收发短信";
        } else {
          message = "已关闭飞行模式 <br>模组恢复正常工作";
        }
      } else {
        message = "切换失败: " + htmlEscape(setResp);
      }
    } else {
      message = "无法获取当前状态";
    }
  }
  else if (action == "on") {
    // 强制开启飞行模式
    logCaptureLn("网页端强制开启飞行模式: AT+CFUN=4");
    String resp = sendATCommand("AT+CFUN=4", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      message = "已开启飞行模式 ";
    } else {
      message = "开启失败: " + htmlEscape(resp);
    }
  }
  else if (action == "off") {
    // 强制关闭飞行模式
    logCaptureLn("网页端关闭飞行模式: AT+CFUN=1");
    String resp = sendATCommand("AT+CFUN=1", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      message = "已关闭飞行模式 ";
    } else {
      message = "关闭失败: " + htmlEscape(resp);
    }
  }
  else {
    message = "未知操作";
  }
  
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理AT指令测试请求
void handleATCommand() {
  if (!checkAuth()) return;
  
  String cmd = server.arg("cmd");
  bool success = false;
  String message = "";
  
  if (cmd.length() == 0) {
    message = "错误：指令不能为空";
  } else {
    logCaptureLn(String("网页端发送AT指令: " + cmd));
    String resp = sendATCommand(cmd.c_str(), 5000);
    logCaptureLn(String("模组响应: " + resp));
    
    if (resp.length() > 0) {
      success = true;
      message = resp;
    } else {
      message = "超时或无响应";
    }
  }
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理发送短信请求
void handleSendSms() {
  if (!checkAuth()) return;
  
  String phone = server.arg("phone");
  String content = server.arg("content");
  
  phone.trim();
  content.trim();
  
  bool success = false;
  String resultMsg = "";
  
  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (!isValidPhoneNumber(phone)) {
    resultMsg = "错误：目标号码非法（3-20 位数字，可带 + 前缀）";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else if (content.length() > 300) {
    resultMsg = "错误：短信内容超过 300 字符";
  } else {
    logCaptureLn("网页端发送短信请求");
    logCaptureLn(String("目标号码: " + maskPhone(phone)));
    logCaptureLn(String("短信内容: " + bodyPreview(content, SMS_LOG_VERBOSE)));
    
    success = enqueueOutgoingSms(phone.c_str(), content.c_str());
    resultMsg = success ? "已加入发送队列，请稍后在已发送列表查看结果" : "发送队列已满，请稍后再试";
  }

  String j = String("{\"success\":") + (success ? "true" : "false") +
             ",\"queued\":" + (success ? String("true") : String("false")) +
             ",\"message\":\"" + jsonEscape(resultMsg) + "\"}";
  server.send(200, "application/json", j);
}

struct PingJob {
  bool running;
  bool done;
  bool success;
  String host;
  String message;
  unsigned long startedMs;
};
static PingJob pingJob;

static void pumpWebDuringBackgroundWait() { pumpWebDuringWait(); }  // 统一实现见 globals.h

static bool pingHostValid(const String& pingHost, String& err) {
  if (pingHost.length() > 64) { err = "目标地址过长"; return false; }
  for (unsigned i = 0; i < pingHost.length(); i++) {
    char c = pingHost.charAt(i);
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':')) {
      err = "目标地址含非法字符";
      return false;
    }
  }
  return true;
}

static void finishPingJob(bool ok, const String& msg) {
  pingJob.running = false;
  pingJob.done = true;
  pingJob.success = ok;
  pingJob.message = msg;
}

void processPingJob() {
  if (!pingJob.running) return;
  if (smsUrcReceiving()) return;
  if (millis() - pingJob.startedMs < SLOW_WORK_WEB_GRACE_MS) return;  // 先让 /ping 响应和紧随其后的刷新出去

  String host = pingJob.host;
  logCaptureLn(String("后台蜂窝UDP流量发送开始: ") + host);

  logCaptureLn("激活数据连接(CGACT)...");
  String activateResp = sendATCommand("AT+CGACT=1,1", 10000);
  logCaptureLn(String("CGACT响应: " + activateResp));
  if (activateResp.indexOf("OK") < 0) {
    logCaptureLn("数据连接激活失败，尝试继续执行...");
  }

  drainPendingSmsUrc(3000);
  if (smsUrcReceiving()) {
    sendATCommand("AT+CGACT=0,1", 5000);
    finishPingJob(false, "短信接收中，UDP流量发送已取消");
    return;
  }

  unsigned long stableStart = millis();
  while (millis() - stableStart < 500) pumpWebDuringBackgroundWait();

  bool ok = consumeCellularDataBytes(CELLULAR_BURN_BYTES, host.c_str(), 53);
  String msg = ok ? (String("已发送约 ") + String((unsigned long)(CELLULAR_BURN_BYTES / 1024UL)) + "KB UDP 蜂窝上行数据")
                  : "蜂窝UDP流量发送失败，请查看日志";

  // 仅在用户未启用蜂窝数据时关闭 PDP；用户已开启数据则保留连接，不因一次诊断而误关。
  if (!config.dataEnabled) {
    logCaptureLn("关闭PDP上下文(CGACT=0)...");
    String deactivateResp = sendATCommand("AT+CGACT=0,1", 5000);
    logCaptureLn(String("CGACT关闭响应: " + deactivateResp));
  }

  finishPingJob(ok, msg);
}

// 处理蜂窝 UDP 流量请求：只启动/查询后台任务，避免HTTP handler长时间占住WebServer
void handlePing() {
  if (!checkAuth()) return;

  if (server.arg("action") == "status") {
    String j = String("{\"running\":") + (pingJob.running ? "true" : "false") +
               ",\"done\":" + (pingJob.done ? String("true") : String("false")) +
               ",\"success\":" + (pingJob.success ? String("true") : String("false")) +
               ",\"host\":\"" + jsonEscape(pingJob.host) + "\"" +
               ",\"message\":\"" + jsonEscape(pingJob.message) + "\"}";
    server.send(200, "application/json", j);
    return;
  }

  if (pingJob.running) {
    server.send(200, "application/json", "{\"success\":false,\"running\":true,\"message\":\"UDP流量正在发送，请稍候\"}");
    return;
  }

  String pingHost = server.arg("host");
  pingHost.trim();
  if (pingHost.length() == 0) pingHost = CELLULAR_BURN_DEFAULT_HOST;
  String err;
  if (!pingHostValid(pingHost, err)) {
    server.send(200, "application/json", String("{\"success\":false,\"message\":\"") + jsonEscape(err) + "\"}");
    return;
  }
  if (smsUrcReceiving()) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"短信接收中，请稍后重试UDP流量发送\"}");
    return;
  }

  pingJob.running = true;
  pingJob.done = false;
  pingJob.success = false;
  pingJob.host = pingHost;
  pingJob.message = "后台UDP流量发送中";
  pingJob.startedMs = millis();
  logCaptureLn(String("网页端发起后台UDP流量请求: ") + pingHost);
  server.send(200, "application/json", "{\"success\":true,\"running\":true,\"message\":\"已开始后台UDP流量发送，可继续刷新网页\"}");
}

// 处理保存配置请求
void handleSave() {
  if (!checkAuth()) return;

  // 配置写入区与后台 worker 的 config 快照读互斥(pushChannels/smtp* 等)；仅覆盖字段赋值+NVS，
  // 不含网络/AT(modemApply 在响应后、锁外执行)。持锁顺序 workMux→logMux(saveConfig 内可能记日志)。
  muxLock(gWorkMux);
  // 账号管理表单：只在字段存在时更新
  if (server.hasArg("webUser")) {
    String newWebUser = server.arg("webUser");
    if (newWebUser.length() == 0) newWebUser = DEFAULT_WEB_USER;
    config.webUser = newWebUser;
  }
  if (server.hasArg("webPass")) {
    String newWebPass = server.arg("webPass");
    if (newWebPass.length() == 0) newWebPass = DEFAULT_WEB_PASS;
    config.webPass = newWebPass;
  }

  // 邮件通知表单：只在字段存在时更新
  if (server.hasArg("smtpServer")) {
    config.smtpServer = server.arg("smtpServer");
  }
  if (server.hasArg("smtpPort")) {
    config.smtpPort = server.arg("smtpPort").toInt();
    if (config.smtpPort == 0) config.smtpPort = 465;
  }
  if (server.hasArg("smtpUser")) {
    config.smtpUser = server.arg("smtpUser");
  }
  if (server.hasArg("smtpPass")) {
    config.smtpPass = server.arg("smtpPass");
  }
  if (server.hasArg("smtpSendTo")) {
    config.smtpSendTo = server.arg("smtpSendTo");
  }

  // 管理员 & 黑名单表单：只在字段存在时更新
  if (server.hasArg("adminPhone")) {
    config.adminPhone = server.arg("adminPhone");
  }
  if (server.hasArg("numberBlackList")) {
    config.numberBlackList = server.arg("numberBlackList");
  }
  if (server.hasArg("forwardRules")) {
    config.forwardRules = server.arg("forwardRules");
  }

  // 保号定时任务表单（kaLastTime 由调度器/重置按钮管理，不在此处更新）
  if (server.hasArg("kaIntervalDays") || server.hasArg("kaAction") ||
      server.hasArg("kaTarget") || server.hasArg("kaForm")) {
    config.kaEnabled = (server.arg("kaEnabled") == "on");
    if (server.hasArg("kaIntervalDays")) {
      int d = server.arg("kaIntervalDays").toInt();
      if (d > 0 && d < 3650) config.kaIntervalDays = d;
    }
    if (server.hasArg("kaAction")) config.kaAction = (uint8_t)server.arg("kaAction").toInt();
    config.kaTarget = server.arg("kaTarget");
  }

  // 时间 / NTP 表单
  if (server.hasArg("tzForm")) {
    if (server.hasArg("tzOffsetMin")) {
      int tz = server.arg("tzOffsetMin").toInt();
      if (tz >= -720 && tz <= 840) config.tzOffsetMin = tz;
    }
    if (server.hasArg("ntpServer")) config.ntpServer = server.arg("ntpServer");
  }

  // 定时任务表单(定时重启 / 每日心跳)
  if (server.hasArg("schedForm")) {
    config.rebootEnabled = (server.arg("rebootEnabled") == "on");
    if (server.hasArg("rebootHour")) { int h = server.arg("rebootHour").toInt(); if (h >= 0 && h < 24) config.rebootHour = h; }
    config.hbEnabled = (server.arg("hbEnabled") == "on");
    if (server.hasArg("hbHour")) { int h = server.arg("hbHour").toInt(); if (h >= 0 && h < 24) config.hbHour = h; }
  }

  // SIM / 蜂窝数据表单（默认不开流量；变更后即时应用，无需重启）
  bool simChanged = false, opChanged = false;
  if (server.hasArg("simForm")) {
    bool wantData = (server.arg("dataEnabled") == "on");
    String newApn = server.hasArg("apn") ? server.arg("apn") : config.apn;
    String newOp = server.hasArg("operatorPlmn") ? server.arg("operatorPlmn") : config.operatorPlmn;
    newOp.trim();
    if (wantData != config.dataEnabled || newApn != config.apn) simChanged = true;
    if (newOp != config.operatorPlmn) opChanged = true;
    config.dataEnabled = wantData;
    config.apn = newApn;
    config.operatorPlmn = newOp;
    if (server.hasArg("phoneNumber")) config.phoneNumber = server.arg("phoneNumber");
  }

  // 邮件 / 推送 转发总开关(各自表单带 emailForm/pushForm 标记，复选未勾=关)
  if (server.hasArg("emailForm")) config.emailEnabled = (server.arg("emailEnabled") == "on");
  if (server.hasArg("pushForm")) config.pushEnabled = (server.arg("pushEnabled") == "on");

  // 推送通道配置：只在对应通道的字段存在时更新
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enKey = "push" + idx + "en";
    String typeKey = "push" + idx + "type";
    String urlKey = "push" + idx + "url";
    String nameKey = "push" + idx + "name";
    String k1Key = "push" + idx + "key1";
    String k2Key = "push" + idx + "key2";
    String bodyKey = "push" + idx + "body";
    // 只要该通道的任一字段存在，就更新整个通道
    if (server.hasArg(enKey) || server.hasArg(typeKey) || server.hasArg(urlKey) ||
        server.hasArg(nameKey) || server.hasArg(k1Key) || server.hasArg(k2Key) ||
        server.hasArg(bodyKey)) {
      config.pushChannels[i].enabled = server.arg(enKey) == "on";
      int rawType = server.arg(typeKey).toInt();
      if (rawType < PUSH_TYPE_POST_JSON || rawType > PUSH_TYPE_TELEGRAM) rawType = PUSH_TYPE_POST_JSON;
      String url = server.arg(urlKey); url.trim();
      String name = server.arg(nameKey); name.trim();
      String key1 = server.arg(k1Key); key1.trim();
      String key2 = server.arg(k2Key); key2.trim();
      if (rawType == PUSH_TYPE_SERVERCHAN && key1.length() == 0 &&
          url.length() > 0 && !url.startsWith("http://") && !url.startsWith("https://")) {
        key1 = url;   // 用户常把 SendKey 填到 URL 框；保存时自动归位
        url = "";
      }
      config.pushChannels[i].type = (PushType)rawType;
      config.pushChannels[i].url = url;
      config.pushChannels[i].name = name;
      config.pushChannels[i].key1 = key1;
      config.pushChannels[i].key2 = key2;
      config.pushChannels[i].customBody = server.arg(bodyKey);
      if (config.pushChannels[i].name.length() == 0) {
        config.pushChannels[i].name = "通道" + String(i + 1);
      }
    }
  }
  
  saveConfig();
  bumpWebPageCacheRev();
  configValid = isConfigValid();
  muxUnlock(gWorkMux);

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="refresh" content="3;url=/">
  <title>保存成功</title>
  <style>
    body { font-family: -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Helvetica Neue',Arial,sans-serif; background:#fafafa; color:#171717; display:flex; align-items:center; justify-content:center; min-height:100vh; margin:0; }
    .card { padding:24px 32px; border-radius:8px; box-shadow:0 0 0 1px rgba(0,0,0,0.08),0 2px 2px rgba(0,0,0,0.04); background:#fff; text-align:center; max-width:360px; }
    .card h2 { font-size:18px; font-weight:600; margin:0 0 8px; letter-spacing:-0.3px; color:#1a7f37; }
    .card p { color:#888; font-size:13px; margin:4px 0 0; line-height:1.5; }
  </style>
</head>
<body>
  <div class="card">
    <h2>配置已保存</h2>
    <p>3 秒后返回配置页面</p>
    <p>若修改了账号密码，请使用新的凭据登录</p>
  </div>
</body>
</html>
)rawliteral";
  if (server.hasArg("ajax")) server.send(200, "application/json", "{\"ok\":true}");  // AJAX 原地保存：不跳页
  else server.send(200, "text/html", html);                                          // 原生提交降级：保存成功页

  // SIM 数据模式变更：响应已返回后再下发 AT(CGACT/COPS 可能耗时数秒~数十秒)
  if (opChanged && modemReady) modemApplyOperator();
  if (simChanged && modemReady) modemApplyDataMode();

  // 保存动作必须快速返回；推送/邮件连通性请用各通道“测试推送”单独验证，避免 SMTP/HTTPS 阻塞导致前端误判保存失败。
  if (configValid) logCaptureLn("配置有效，保存完成");
}

// 处理日志查询请求 — 返回环形缓冲区中的日志行
// 流式返回，避免一次性拼出 ~10KB String；支持 ?since=<seq> 只回增量。
// 响应: {"seq":<当前累计行号>,"lines":[...]}
void handleLog() {
  if (!checkAuth()) return;

  unsigned long since = 0;
  if (server.hasArg("since")) since = strtoul(server.arg("since").c_str(), nullptr, 10);

  // 在锁内快照计数(worker 可能正在追加日志)，避免边读边变导致越界/错位
  muxLock(gLogMux);
  unsigned long seqSnap = logSeq;
  int countSnap = logBufCount;
  int startPos = (countSnap < LOG_BUF_SIZE) ? 0 : logBufIdx;
  muxUnlock(gLogMux);
  // 缓冲中最旧一行的序号 = logSeq - logBufCount
  unsigned long oldestSeq = (seqSnap >= (unsigned long)countSnap) ? seqSnap - countSnap : 0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent(String("{\"seq\":") + String(seqSnap) + ",\"lines\":[");
  bool first = true;
  for (int i = 0; i < countSnap; i++) {
    unsigned long seq = oldestSeq + i;
    if (seq < since) continue;  // 仅回客户端尚未拥有的新行
    int pos = (startPos + i) % LOG_BUF_SIZE;
    muxLock(gLogMux);                       // 仅在拷贝该行时短暂持锁，不在 sendContent(网络)期间持锁
    String line = logBuffer[pos];
    muxUnlock(gLogMux);
    String item = (first ? String("\"") : String(",\"")) + jsonEscape(line) + "\"";
    server.sendContent(item);
    first = false;
  }
  server.sendContent("]}");
  server.sendContent("");
}

// 机器可读健康状态：供前端概览与外部监控读取
void handleStatus() {
  if (!checkAuth()) return;
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  long up = millis() / 1000;
  String phone = modemPhone.length() ? modemPhone : config.phoneNumber;
  bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                 config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
  int pushCount = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (config.pushChannels[i].enabled) pushCount++;
  }
  String j;
  j.reserve(1450);  // /status 字段较多，预留足够空间减少 String 重分配
  j += "{";
  j += "\"version\":\""; j += FW_VERSION; j += "\",";
  j += "\"modemReady\":"; j += (modemReady ? "true" : "false"); j += ",";
  j += "\"wifiConnected\":"; j += (WiFi.status() == WL_CONNECTED ? "true" : "false"); j += ",";
  j += "\"apMode\":"; j += (apMode ? "true" : "false"); j += ",";
  j += "\"ssid\":\""; j += jsonEscape(WiFi.SSID()); j += "\",";
  j += "\"rssi\":"; j += String(WiFi.RSSI()); j += ",";
  j += "\"csq\":"; j += String(modemCsq); j += ",";          // 4G 信号(0-31,99=未知)
  j += "\"ber\":"; j += String(modemBer); j += ",";          // 误码率(0-7,99=未知)
  j += "\"rsrp\":"; j += String(modemRsrp); j += ",";        // LTE RSRP 实际 dBm(>=0=未知)
  j += "\"tz\":"; j += String(config.tzOffsetMin); j += ",";  // 本地时区分钟偏移(前端格式化用)
  j += "\"nowEpoch\":"; j += String((long)time(nullptr)); j += ",";  // 设备当前 UTC 秒，前端以此统一时间显示
  j += "\"operator\":\""; j += jsonEscape(modemOperator); j += "\",";
  j += "\"imei\":\""; j += jsonEscape(modemImei); j += "\",";
  j += "\"iccid\":\""; j += jsonEscape(modemIccid); j += "\",";
  j += "\"imsi\":\""; j += jsonEscape(modemImsi); j += "\",";       // SIM IMSI
  j += "\"apnSim\":\""; j += jsonEscape(modemApn); j += "\",";      // 模组读到的 APN
  j += "\"mfr\":\""; j += jsonEscape(modemMfr); j += "\",";         // 模组制造商
  j += "\"model\":\""; j += jsonEscape(modemModel); j += "\",";    // 模组型号
  j += "\"fwver\":\""; j += jsonEscape(modemFwVer); j += "\",";    // 模组固件版本
  j += "\"phone\":\""; j += jsonEscape(phone); j += "\",";
  j += "\"dataEnabled\":"; j += (config.dataEnabled ? "true" : "false"); j += ",";
  j += "\"apn\":\""; j += jsonEscape(config.apn); j += "\",";
  j += "\"cellIp\":\""; j += jsonEscape(modemCellIp); j += "\",";
  j += "\"rsrq\":"; j += String(modemRsrq); j += ",";   // dB(999=未知)
  j += "\"sinr\":"; j += String(modemSinr); j += ",";   // dB(999=未知)
  j += "\"pci\":"; j += String(modemPci); j += ",";     // -1=未知
  j += "\"plmn\":\""; j += jsonEscape(modemPlmn); j += "\",";
  j += "\"tac\":\""; j += jsonEscape(modemTac); j += "\",";
  j += "\"inboxCount\":"; j += String(inboxCount()); j += ",";
  j += "\"ip\":\""; j += WiFi.localIP().toString(); j += "\",";
  j += "\"gw\":\""; j += WiFi.gatewayIP().toString(); j += "\",";       // WiFi 网关
  j += "\"mask\":\""; j += WiFi.subnetMask().toString(); j += "\",";    // 子网掩码
  j += "\"dns\":\""; j += WiFi.dnsIP().toString(); j += "\",";          // DNS
  j += "\"mac\":\""; j += WiFi.macAddress(); j += "\",";                // 本机 MAC
  j += "\"bssid\":\""; j += WiFi.BSSIDstr(); j += "\",";                // 路由器 BSSID
  j += "\"chan\":"; j += String(WiFi.channel()); j += ",";              // WiFi 信道
  j += "\"freeHeap\":"; j += String(ESP.getFreeHeap()); j += ",";
  j += "\"minFreeHeap\":"; j += String(ESP.getMinFreeHeap()); j += ",";
  j += "\"maxAllocHeap\":"; j += String(ESP.getMaxAllocHeap()); j += ",";
  j += "\"uptime\":"; j += String(up); j += ",";
  j += "\"queueDepth\":"; j += String(retryQueueDepth()); j += ",";
  j += "\"fwdQueueDepth\":"; j += String(forwardQueueDepth()); j += ",";
  j += "\"outSmsQueueDepth\":"; j += String(outgoingSmsQueueDepth()); j += ",";
  j += "\"emailQueueDepth\":"; j += String(emailQueueDepth()); j += ",";
  j += "\"slowBusy\":"; j += (gSlowWorkBusy ? "true" : "false"); j += ",";
  j += "\"emailEnabled\":"; j += (config.emailEnabled ? "true" : "false"); j += ",";
  j += "\"emailConfigured\":"; j += (emailOk ? "true" : "false"); j += ",";
  j += "\"pushEnabled\":"; j += (config.pushEnabled ? "true" : "false"); j += ",";
  j += "\"pushEnabledCount\":"; j += String(pushCount); j += ",";
  j += "\"adminPhone\":\""; j += jsonEscape(config.adminPhone); j += "\",";
  j += "\"smsTotal\":"; j += String(smsTotalCount); j += ",";
  j += "\"lastSmsEpoch\":"; j += String((long)lastSmsEpoch); j += ",";
  j += "\"resetReason\":"; j += String((int)esp_reset_reason()); j += ",";
  j += "\"configValid\":"; j += (configValid ? "true" : "false"); j += ",";
  j += "\"timeSynced\":"; j += (timeSynced ? "true" : "false"); j += ",";
  j += "\"chipTemp\":"; j += String(temperatureRead(), 1);   // ESP32-C3 片内温度(℃)，非环境/模组温度，仅供过热趋势参考(读数偏高且不精确)
  j += "}";
  server.send(200, "application/json", j);
}

// 通道发送测试：用当前配置向指定通道发一条测试推送，便于配置后即时验证
void handleTestPush() {
  if (!checkAuth()) return;
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch >= MAX_PUSH_CHANNELS) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"通道序号无效\"}");
    return;
  }

  if (server.arg("action") == "status") {
    server.send(200, "application/json", testPushStatusJson((uint8_t)ch));
    return;
  }

  String msg;
  bool ok = enqueueTestPush((uint8_t)ch, msg);
  String j = String("{\"success\":") + (ok ? "true" : "false") +
             ",\"queued\":" + (ok ? String("true") : String("false")) +
             ",\"message\":\"" + jsonEscape(msg) + "\"}";
  server.send(200, "application/json", j);
}

// USSD 查询(如查余额): 发送 AT+CUSD 并等待 +CUSD 结果(最长 ~20s)
void handleUssd() {
  if (!checkAuth()) return;
  String code = server.arg("code");
  code.trim();
  if (code.length() == 0 || code.length() > 24) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"USSD 码为空或过长\"}");
    return;
  }
  logCaptureLn(String("网页端 USSD 查询: " + code));
  drainPendingSmsUrc(3000);
  if (smsUrcReceiving()) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"短信接收中，请稍后重试 USSD\"}");
    return;
  }
  String cmd = "AT+CUSD=1,\"" + code + "\",15";
  if (!modemSerialTryBegin("USSD查询")) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"模组正忙，请稍后重试 USSD\"}");
    return;
  }
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  bool got = false;
  while (millis() - start < 20000) {
    while (Serial1.available()) {
      resp += (char)Serial1.read();
      if (resp.indexOf("+CUSD:") >= 0 && resp.indexOf('\n', resp.indexOf("+CUSD:")) >= 0) { got = true; break; }
      if (resp.indexOf("ERROR") >= 0) { got = true; break; }
    }
    if (got) break;
    yield();  // HTTP handler 内不重入 WebServer，只喂看门狗
  }
  modemSerialEnd();
  processSmsUrcText(resp);
  resp.trim();
  String j = String("{\"success\":") + (resp.indexOf("+CUSD:") >= 0 ? "true" : "false") +
             ",\"message\":\"" + jsonEscape(resp) + "\"}";
  server.send(200, "application/json", j);
}

// 扫描周边 WiFi(供配网选择)。AP_STA 模式下可扫描。
void handleWifiScan() {
  if (!checkAuth()) return;
  int n = WiFi.scanNetworks();
  String j = "[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;               // 跳过隐藏 SSID
    // 去重：同名(多 AP/信道)只保留信号最强的一个；RSSI 相同则保留索引最小，避免重复列出
    bool dup = false;
    for (int k = 0; k < n; k++) {
      if (k != i && WiFi.SSID(k) == ssid &&
          (WiFi.RSSI(k) > WiFi.RSSI(i) || (WiFi.RSSI(k) == WiFi.RSSI(i) && k < i))) { dup = true; break; }
    }
    if (dup) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"ssid\":\""; j += jsonEscape(ssid); j += "\"";
    j += ",\"rssi\":"; j += String(WiFi.RSSI(i));
    j += ",\"enc\":"; j += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "0" : "1");
    j += "}";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

// 保存新 WiFi 凭据并重启接入(切换网络最可靠的方式：保存后重启用新凭据连 STA)
void handleWifiConfig() {
  if (!checkAuth()) return;
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"SSID 不能为空\"}");
    return;
  }
  config.wifiSsid = server.arg("ssid");
  config.wifiPass = server.arg("pass");
  saveConfig();
  logCaptureLn(String("已保存新 WiFi: ") + config.wifiSsid + "，即将重启接入");
  server.send(200, "application/json",
              "{\"success\":true,\"message\":\"WiFi 已保存，设备重启后连接新网络（约 15-20 秒）。若连接失败将重新开启配网热点。\"}");
  delay(500);
  ESP.restart();
}

// 日志下载：把日志环形缓冲以纯文本流式下载
void handleLogDownload() {
  if (!checkAuth()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=sms_log.txt");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  muxLock(gLogMux);
  int total = logBufCount;
  int start = total < LOG_BUF_SIZE ? 0 : logBufIdx;
  muxUnlock(gLogMux);
  for (int i = 0; i < total; i++) {
    int pos = (start + i) % LOG_BUF_SIZE;
    muxLock(gLogMux);
    String line = logBuffer[pos];
    muxUnlock(gLogMux);
    server.sendContent(line);
    server.sendContent("\n");
  }
  server.sendContent("");
}

// 系统维护：重启设备
void handleReboot() {
  if (!checkAuth()) return;
  logCaptureLn("网页端请求重启设备...");
  server.send(200, "application/json", "{\"success\":true,\"message\":\"设备即将重启，请约 15 秒后刷新页面\"}");
  delay(300);
  ESP.restart();
}

// 系统维护：恢复出厂设置(清空 NVS 配置后重启)。WiFi 为编译期宏，重启后仍可联网。
void handleFactory() {
  if (!checkAuth()) return;
  logCaptureLn("网页端请求恢复出厂设置(清空NVS)...");
  server.send(200, "application/json", "{\"success\":true,\"message\":\"已清除配置，设备即将重启为默认设置\"}");
  preferences.begin("sms_config", false);
  preferences.clear();
  preferences.end();
  delay(300);
  ESP.restart();
}

// ---- 配置导出/导入(转义 key=value 文本，无依赖；含凭据，请妥善保存) ----
static String cfgEsc(const String& v) {
  String o; o.reserve(v.length() + 8);
  for (unsigned int i = 0; i < v.length(); i++) {
    char c = v.charAt(i);
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}
static String cfgUnesc(const String& v) {
  String o; o.reserve(v.length());
  for (unsigned int i = 0; i < v.length(); i++) {
    char c = v.charAt(i);
    if (c == '\\' && i + 1 < v.length()) {
      char n = v.charAt(i + 1);
      if (n == 'n') { o += '\n'; i++; }
      else if (n == 'r') { o += '\r'; i++; }
      else if (n == '\\') { o += '\\'; i++; }
      else o += c;
    } else o += c;
  }
  return o;
}

void handleExport() {
  if (!checkAuth()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=sms_config.txt");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  auto L = [&](const char* k, const String& v) { server.sendContent(String(k) + "=" + cfgEsc(v) + "\n"); };
  auto LI = [&](const char* k, int v) { server.sendContent(String(k) + "=" + String(v) + "\n"); };
  L("wifiSsid", config.wifiSsid); L("wifiPass", config.wifiPass);
  L("smtpServer", config.smtpServer); LI("smtpPort", config.smtpPort);
  L("smtpUser", config.smtpUser); L("smtpPass", config.smtpPass); L("smtpSendTo", config.smtpSendTo);
  L("adminPhone", config.adminPhone); L("webUser", config.webUser); L("webPass", config.webPass);
  L("numBlkList", config.numberBlackList);
  L("fwdRules", config.forwardRules);
  LI("tzOffsetMin", config.tzOffsetMin); L("ntpServer", config.ntpServer);
  LI("rebootEnabled", config.rebootEnabled ? 1 : 0); LI("rebootHour", config.rebootHour);
  LI("hbEnabled", config.hbEnabled ? 1 : 0); LI("hbHour", config.hbHour);
  LI("kaEnabled", config.kaEnabled ? 1 : 0); LI("kaIntervalDays", config.kaIntervalDays);
  LI("kaAction", config.kaAction); L("kaTarget", config.kaTarget);
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String p = "push" + String(i);
    LI((p + "en").c_str(), config.pushChannels[i].enabled ? 1 : 0);
    LI((p + "type").c_str(), (int)config.pushChannels[i].type);
    L((p + "url").c_str(), config.pushChannels[i].url);
    L((p + "name").c_str(), config.pushChannels[i].name);
    L((p + "k1").c_str(), config.pushChannels[i].key1);
    L((p + "k2").c_str(), config.pushChannels[i].key2);
    L((p + "body").c_str(), config.pushChannels[i].customBody);
  }
  server.sendContent("");
}

static void applyConfigKey(const String& k, const String& v) {
  if (k == "wifiSsid") config.wifiSsid = v;
  else if (k == "wifiPass") config.wifiPass = v;
  else if (k == "smtpServer") config.smtpServer = v;
  else if (k == "smtpPort") config.smtpPort = v.toInt();
  else if (k == "smtpUser") config.smtpUser = v;
  else if (k == "smtpPass") config.smtpPass = v;
  else if (k == "smtpSendTo") config.smtpSendTo = v;
  else if (k == "adminPhone") config.adminPhone = v;
  else if (k == "webUser") config.webUser = v;
  else if (k == "webPass") config.webPass = v;
  else if (k == "numBlkList") config.numberBlackList = v;
  else if (k == "fwdRules") config.forwardRules = v;
  else if (k == "tzOffsetMin") config.tzOffsetMin = v.toInt();
  else if (k == "ntpServer") config.ntpServer = v;
  else if (k == "rebootEnabled") config.rebootEnabled = (v == "1");
  else if (k == "rebootHour") config.rebootHour = v.toInt();
  else if (k == "hbEnabled") config.hbEnabled = (v == "1");
  else if (k == "hbHour") config.hbHour = v.toInt();
  else if (k == "kaEnabled") config.kaEnabled = (v == "1");
  else if (k == "kaIntervalDays") config.kaIntervalDays = v.toInt();
  else if (k == "kaAction") config.kaAction = (uint8_t)v.toInt();
  else if (k == "kaTarget") config.kaTarget = v;
  else if (k.startsWith("push") && k.length() > 5) {
    int idx = k.charAt(4) - '0';  // ponytail: 单位数索引，依赖 MAX_PUSH_CHANNELS<10(=5)；超过需改多位解析
    if (idx >= 0 && idx < MAX_PUSH_CHANNELS) {
      String s = k.substring(5);
      PushChannel& ch = config.pushChannels[idx];
      if (s == "en") ch.enabled = (v == "1");
      else if (s == "type") ch.type = (PushType)v.toInt();
      else if (s == "url") ch.url = v;
      else if (s == "name") ch.name = v;
      else if (s == "k1") ch.key1 = v;
      else if (s == "k2") ch.key2 = v;
      else if (s == "body") ch.customBody = v;
    }
  }
}

void handleImport() {
  if (!checkAuth()) return;
  String body = server.hasArg("plain") ? server.arg("plain") : server.arg("config");
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"导入内容为空\"}");
    return;
  }
  int applied = 0, start = 0, len = (int)body.length();
  muxLock(gWorkMux);  // 与 worker 的 config 快照读互斥(applyConfigKey 改写 pushChannels/smtp*)
  while (start < len) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = len;
    String line = body.substring(start, nl);
    start = nl + 1;
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    key.trim();
    String val = cfgUnesc(line.substring(eq + 1));
    applyConfigKey(key, val);
    applied++;
  }
  saveConfig();
  bumpWebPageCacheRev();
  configValid = isConfigValid();
  muxUnlock(gWorkMux);
  String j = String("{\"success\":true,\"message\":\"已导入 ") + applied + " 项，建议重启使全部生效\"}";
  server.send(200, "application/json", j);
}

// ---- OTA 固件升级(Update.h，凭据校验，双分区) ----
void handleOtaUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) return;  // 未授权不开始
    logCaptureF("OTA 开始: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logCaptureLn("OTA begin 失败");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning()) Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.isRunning() && Update.end(true)) logCaptureF("OTA 完成: %u 字节\n", (unsigned)up.totalSize);
    else logCaptureLn("OTA 写入失败");
  }
}
void handleOtaFinish() {
  if (!checkAuth()) return;
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(200, "application/json",
              ok ? "{\"success\":true,\"message\":\"升级成功，设备重启中\"}"
                 : "{\"success\":false,\"message\":\"升级失败，请检查固件\"}");
  delay(500);
  if (ok) ESP.restart();
}

// 收件箱：流式返回本地留存的最近短信(时间倒序)
// 响应: [{"id":N,"recv":epoch,"sender":"..","ts":"..","text":"..","fwd":true},...]
void handleMessages() {
  if (!checkAuth()) return;
  bool sentBox = (server.arg("box") == "sent");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  // 批量累积、按 ~6KB 分块 flush：把原先每条一个 TCP chunk(最多 50 个 WiFi 往返)降到 ~4 个，
  // 同时峰值堆受控(不一次性拼整页)。这是收件箱"加载慢"的主因修复。
  String out; out.reserve(6300);
  out += "[";
  int n = sentBox ? sentCount() : inboxCount();
  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 0;
  if (limit > 0 && limit < n) n = limit;
  for (int i = 0; i < n; i++) {
    if (sentBox) {
      const SentEntry* e = sentAtNewest(i);
      if (!e) break;
      out += (i == 0) ? "{" : ",{";
      out += "\"id\":"; out += String(e->id);
      out += ",\"sent\":"; out += String((unsigned long)e->sentEpoch);
      out += ",\"target\":\""; out += jsonEscape(e->target); out += "\"";
      out += ",\"text\":\""; out += jsonEscape(e->text); out += "\"";
      out += ",\"ok\":"; out += (e->ok ? "true" : "false");
      out += "}";
    } else {
      const InboxEntry* e = inboxAtNewest(i);
      if (!e) break;
      out += (i == 0) ? "{" : ",{";
      out += "\"id\":"; out += String(e->id);
      out += ",\"recv\":"; out += String((unsigned long)e->recvEpoch);
      out += ",\"sender\":\""; out += jsonEscape(e->sender); out += "\"";
      out += ",\"ts\":\""; out += jsonEscape(e->ts); out += "\"";
      out += ",\"text\":\""; out += jsonEscape(e->text); out += "\"";
      out += ",\"fwd\":"; out += (e->forwarded ? "true" : "false");
      out += "}";
    }
    if (out.length() > 6000) { server.sendContent(out); out = ""; }  // 分块 flush，控峰值堆
  }
  out += "]";
  server.sendContent(out);
  server.sendContent("");
}

void handleResend() {
  if (!checkAuth()) return;
  uint32_t id = (uint32_t)server.arg("id").toInt();
  const InboxEntry* e = inboxById(id);
  if (!e) { server.send(200, "application/json", "{\"success\":false,\"message\":\"未找到该短信\"}"); return; }
  enqueueForward(e->sender.c_str(), e->text.c_str(), e->ts.c_str(), e->id);
  logCaptureLn(String("网页手动重发 id=") + String(id));
  server.send(200, "application/json", "{\"success\":true,\"message\":\"已重新入队转发\"}");
}

void handleDeleteMsg() {
  if (!checkAuth()) return;
  uint32_t id = (uint32_t)server.arg("id").toInt();
  bool ok = inboxDelete(id);
  server.send(200, "application/json", ok ? "{\"success\":true,\"message\":\"已删除\"}" : "{\"success\":false,\"message\":\"未找到\"}");
}

// 保号: action=status(默认) / run(立即执行) / reset(仅重置基准日)
void handleKeepAlive() {
  if (!checkAuth()) return;
  String action = server.arg("action");
  if (action == "run") {
    String msg;
    bool ok = enqueueKeepAliveRunNow(msg);
    String j = String("{\"success\":") + (ok ? "true" : "false") +
               ",\"queued\":" + (ok ? String("true") : String("false")) +
               ",\"message\":\"" + jsonEscape(msg) + "\"}";
    server.send(200, "application/json", j);
    return;
  }
  if (action == "reset") {
    keepAliveResetBaseline();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"基准日已重置为今天\"}");
    return;
  }
  server.send(200, "application/json", keepAliveStatusJson());
}

// 模组控制命令
void handleModem() {
  if (!checkAuth()) return;

  // 防止重入：modemInit() 内部会调 server.handleClient()，
  // 若浏览器超时重试会导致嵌套调用，最终拖垮 WiFi
  static bool busy = false;
  if (busy) {
    server.send(429, "application/json", "{\"success\":false,\"message\":\"模组正忙，请稍后重试\"}");
    return;
  }
  busy = true;

  String action = server.arg("action");
  String json = "{";
  bool success = false;
  String message = "";

  if (action == "restart") {
    // AT 软重启 — 先响应浏览器再初始化，防止浏览器超时重试
    logCaptureLn("网页端请求软重启模组...");
    server.send(200, "application/json", "{\"success\":true,\"message\":\"正在软重启模组，请等待约 15 秒后刷新页面\"}");
    String resp = sendATCommand("AT+CFUN=1,1", 15000);
    success = (resp.indexOf("OK") >= 0);
    message = success ? "模组软重启成功" : "软重启失败";
    logCaptureLn(String(message + ": " + resp));
    if (success) modemInit();
    busy = false;
    return;
  }
  else if (action == "hardreset") {
    // EN 引脚断电重启（内部已调用 modemInit()）
    logCaptureLn("网页端请求硬重启模组...");
    server.send(200, "application/json", "{\"success\":true,\"message\":\"正在硬重启模组，请等待约 15 秒后刷新页面\"}");
    resetModule();
    busy = false;
    return;
  }
  else if (action == "signal") {
    logCaptureLn("网页端查询信号: AT+CSQ");
    String resp = sendATCommand("AT+CSQ", 3000);
    int csqIdx = resp.indexOf("+CSQ:");
    if (csqIdx >= 0) {
      String csqLine = resp.substring(csqIdx);
      csqLine = csqLine.substring(0, csqLine.indexOf('\n'));
      csqLine.trim();
      int commaIdx = csqLine.indexOf(',');
      if (commaIdx >= 0) {
        int rssi = csqLine.substring(csqLine.indexOf(':') + 1, commaIdx).toInt();
        int ber = csqLine.substring(commaIdx + 1).toInt();
        int dbm = (rssi == 99) ? -999 : (-113 + rssi * 2);
        String quality;
        if (rssi >= 19) quality = "优秀";
        else if (rssi >= 14) quality = "良好";
        else if (rssi >= 10) quality = "一般";
        else if (rssi >= 5) quality = "较差";
        else quality = "很差";
        // 注意：AT+CSQ 给的是 RSSI(总接收功率)，不是 RSRP(服务小区每RE功率)。此前误标为 RSRP，
        // 导致与首页(AT+MUESTATS 真实 RSRP)对不上。这里如实标为信号强度(RSSI)，原始值标为 CSQ。
        message = "信号强度(RSSI): " + String(dbm) + " dBm (" + quality + "), CSQ原始值: " + String(rssi) + ", BER: " + String(ber);
        success = true;
      }
    }
    if (!success) message = "无法获取信号: " + resp;
  }
  else if (action == "operator") {
    logCaptureLn("网页端查询运营商: AT+COPS?");
    String resp = sendATCommand("AT+COPS?", 5000);
    int copsIdx = resp.indexOf("+COPS:");
    if (copsIdx >= 0) {
      String copsLine = resp.substring(copsIdx);
      copsLine = copsLine.substring(0, copsLine.indexOf('\n'));
      copsLine.trim();
      int q1 = copsLine.indexOf('"');
      int q2 = copsLine.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 >= 0) {
        message = copsLine.substring(q1 + 1, q2);
        success = true;
      } else {
        message = copsLine;
        success = true;
      }
    }
    if (!success) message = "无法获取运营商: " + resp;
  }
  else if (action == "imei") {
    logCaptureLn("网页端查询IMEI: AT+CGSN=1/AT+GSN=1/AT+CGSN/AT+GSN");
    String imei, raw;
    if (queryModemImei(imei, &raw, 3000)) {
      modemImei = imei;
      saveModemIdentityCache();
      message = imei;
      success = true;
    } else {
      message = "无法获取 IMEI";
      if (raw.length() > 0) message += ": " + raw;
    }
  }
  else {
    message = "未知操作: " + action;
  }

  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  busy = false;
  server.send(200, "application/json", json);
}

// WiFi 重启
void handleWifi() {
  if (!checkAuth()) return;

  static bool busy = false;
  if (busy) {
    server.send(429, "application/json", "{\"success\":false,\"message\":\"WiFi正忙，请稍后重试\"}");
    return;
  }
  busy = true;

  String action = server.arg("action");
  if (action == "restart") {
    logCaptureLn("网页端请求重启WiFi...");
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi 正在重启，请等待约 5 秒后刷新页面\"}");
    WiFi.disconnect(true);
    delay(500);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.setScanMethod(WIFI_FAST_SCAN);
    String rs = config.wifiSsid.length() ? config.wifiSsid : String(WIFI_SSID);
    String rp = config.wifiSsid.length() ? config.wifiPass : String(WIFI_PASS);
    WiFi.begin(rs.c_str(), rp.c_str());
    logCaptureLn(String("正在重新连接WiFi: " + rs));
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(50);
      yield();  // 已响应浏览器，避免在HTTP handler内重入WebServer
    }
    if (WiFi.status() == WL_CONNECTED) {
      logCaptureLn(String("WiFi 重连成功, IP: " + WiFi.localIP().toString()));
    } else {
      logCaptureLn("WiFi 重连失败，将在后台持续尝试");
    }
  } else {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"未知操作\"}");
  }
  busy = false;
}
