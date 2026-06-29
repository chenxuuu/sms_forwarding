#include "globals.h"
#include <ESPmDNS.h>
#include <esp_system.h>
#include <DNSServer.h>
#include "wifi_config.h"
#include "config.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "sms_process.h"
#include "scheduler.h"

static DNSServer dnsServer;  // 配网模式的强制门户 DNS

// 开启配网热点(SoftAP + 强制门户)：STA 连不上/未配置时进入，等用户网页配 WiFi
void startProvisioningAP() {
  apMode = true;
  String apName = String(AP_SSID_PREFIX) + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP_STA);            // AP_STA：开热点同时可扫描周边 WiFi
  WiFi.softAP(apName.c_str());       // 开放热点便于连接；管理页仍受 Basic Auth 保护
  IPAddress ip = WiFi.softAPIP();
  dnsServer.start(53, "*", ip);      // 强制门户：所有域名解析到本机，连入即弹配网页
  logCaptureLn(String("未连上 WiFi，已开启配网热点: ") + apName);
  logCaptureLn(String("请连接该热点后打开 http://") + ip.toString() + " 配置 WiFi");
}

// 按配置应用 NTP/时区(time() 仍为 UTC，偏移仅用于本地时间显示；前端按 tzOffsetMin 格式化)
void applyTimeConfig() {
  long off = (long)config.tzOffsetMin * 60;
  const char* s1 = config.ntpServer.length() ? config.ntpServer.c_str() : "ntp.aliyun.com";
  configTime(off, 0, s1, "ntp.ntsc.ac.cn", "pool.ntp.org");
}

void setup() {
  initConcurrency();   // 先建好 gLogMux/gWorkMux，使后续 logCapture 与 worker 线程安全
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  logCaptureF("固件 %s 启动，复位原因=%d\n", FW_VERSION, (int)esp_reset_reason());  // D3
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();

  // ---- WiFi 连接 ----
  // 凭据优先用 NVS(网页配过)；为空则回退到 wifi_config.h 宏；仍为空则直接进配网 AP。
  String ssid = config.wifiSsid.length() ? config.wifiSsid : String(WIFI_SSID);
  String pass = config.wifiSsid.length() ? config.wifiPass : String(WIFI_PASS);
  bool connected = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.setScanMethod(WIFI_FAST_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.begin(ssid.c_str(), pass.c_str());
    logCaptureLn(String("连接wifi: ") + ssid);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
      blink_short(200);
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (connected) {
    logCaptureLn("wifi已连接");
    logCapture("IP地址: ");
    logCaptureLn(WiFi.localIP().toString());
    logCapture("信号强度(RSSI): ");
    logCaptureLn(String(WiFi.RSSI()) + " dBm");
  } else {
    // 连不上/未配置 → 开配网热点(强制门户)，等用户网页配 WiFi，不再重启死循环
    startProvisioningAP();
  }

  static const char* headerKeys[] = {"If-None-Match"};
  server.collectHeaders(headerKeys, 1);  // 首页缓存校验用：命中后返回 304，刷新不再完整重传大页面

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleRoot);
  server.on("/sms", handleRoot);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/resend", HTTP_POST, handleResend);
  server.on("/delete", HTTP_POST, handleDeleteMsg);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/at", handleATCommand);
  server.on("/log", handleLog);
  server.on("/status", handleStatus);
  server.on("/messages", handleMessages);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/factory", HTTP_POST, handleFactory);
  server.on("/export", handleExport);
  server.on("/import", HTTP_POST, handleImport);
  server.on("/logdownload", handleLogDownload);
  server.on("/update", HTTP_POST, handleOtaFinish, handleOtaUpload);
  server.on("/keepalive", handleKeepAlive);
  server.on("/testpush", handleTestPush);
  server.on("/ussd", handleUssd);
  server.on("/modem", handleModem);
  server.on("/wifi", handleWifi);
  server.on("/wifiscan", handleWifiScan);
  server.on("/wificonfig", HTTP_POST, handleWifiConfig);
  server.onNotFound([]() {
    if (apMode) {  // 强制门户：未知路径重定向到配网页
      server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });
  server.begin();
  logCaptureLn("HTTP服务器已启动");

  // ---- D1 mDNS：可用 http://sms.local 访问，免记 IP ----
  if (MDNS.begin("sms")) {
    MDNS.addService("http", "tcp", 80);
    logCaptureLn("mDNS 已启动: http://sms.local");
  }

  // ---- NTP 时间同步 ----
  logCaptureLn("正在同步NTP时间...");
  applyTimeConfig();
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) {
    delay(1);
    server.handleClient();
    ntpRetry++;
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    logCaptureLn("NTP时间同步成功");
    time_t now = time(nullptr);
    logCapture("当前UTC时间戳: ");
    logCaptureLn(String(now));
  } else {
    logCaptureLn("NTP时间同步失败，将使用设备时间");
  }

  ssl_client.setInsecure();
  startPushWorker();   // 启动推送/邮件后台 worker(此后 ssl_client/smtp 仅由该任务使用)
  digitalWrite(LED_BUILTIN, LOW);

  // ---- 模组初始化（较慢，但网页已可访问；使短信收发尽早就绪） ----
  modemInit();

  // ---- A2 开机补收 SIM 中暂存的短信（断电期间到达的消息不丢） ----
  if (modemReady) backfillStoredSms(true);

  // 启动只写日志，不再发送邮件，避免断电/重启后产生多余通知。
  if (configValid && !apMode) {
    logCaptureLn("配置有效，启动通知邮件已禁用");
  }
}

// P0-3 WiFi 掉线兜底：周期检查，未连接则触发重连；恢复后重新对时。
// 与 setAutoReconnect(true) 互补（后者负责即时重连，本函数负责日志与对时刷新）。
void wifiEnsureConnected() {
  static unsigned long lastCheck = 0;
  static bool wasDown = false;
  if (millis() - lastCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (apMode) {
      // 开机时连不上而进了配网热点，但 STA 现已连上(自动重连/凭据可用)：拆掉热点+强制门户，
      // 回到纯 STA。否则瞬时开机断网会让设备永久卡在开放热点+DNS 劫持里(apMode 此前永不清除)。
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apMode = false;
      logCaptureLn(String("WiFi 已连上，关闭配网热点。IP: " + WiFi.localIP().toString()));
    }
    if (wasDown) {
      wasDown = false;
      logCaptureLn(String("WiFi 已恢复, IP: " + WiFi.localIP().toString()));
      applyTimeConfig();  // 重连后重对时
    }
    return;
  }
  wasDown = true;
  logCaptureLn("WiFi 未连接，尝试重连...");
  WiFi.reconnect();
}

// P1-7 低堆有序重启兜底：空闲堆过低且当前空闲(无半成品长短信、无待重试)时，
// 主动重启优于稍后 OOM 崩溃/看门狗硬复位。对只收转发设备而言短暂重启可接受。
void heapGuardTick() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  if (ESP.getFreeHeap() < LOW_HEAP_RESTART_BYTES && concatBufferIdle() && !gSlowWorkBusy &&
      retryQueueDepth() == 0 && forwardQueueDepth() == 0 &&
      outgoingSmsQueueDepth() == 0 && emailQueueDepth() == 0) {
    logCaptureF("空闲堆过低(%u<%u)且空闲，执行有序重启恢复\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)LOW_HEAP_RESTART_BYTES);
    delay(200);
    ESP.restart();
  }
}

void loop() {
  // 请求处理期间置位：AT 函数(sendATCommand 等)内部的 server.handleClient() 泵会因此跳过，
  // 杜绝"web handler 调阻塞 AT → AT 泵 handleClient → 重入 WebServer"导致的崩溃(如开启蜂窝数据保存)。
  gInWebRequest = true; server.handleClient(); gInWebRequest = false;
  if (apMode) { dnsServer.processNextRequest(); }  // 配网模式强制门户 DNS
  if (!configValid) {
    if (millis() - lastPrintTime >= 60000) {   // 60s 一次，避免刷屏占满日志环
      lastPrintTime = millis();
      logCaptureLn(String("请访问 " + getDeviceUrl() + " 配置系统参数"));
    }
  }
  checkConcatTimeout();
  if (Serial.available()) Serial1.write(Serial.read());
  checkSerial1URC();
  processPingJob();       // 诊断 UDP 流量后台执行，避免 /ping 请求占住 WebServer
  processForwardQueue();   // 接收/转发解耦：每帧最多转发一条(仅规则判定+入队，无网络，开销极小)
  processOutgoingSmsQueue(); // 网页发短信异步出队，避免HTTP请求阻塞等待AT+CMGS
  // 推送/邮件/测试推送已移到后台 worker 线程(pushWorkerTask)，不再占用 loop —— 转发/邮件不阻塞收信与网页。
  wifiEnsureConnected();   // P0-3 WiFi 掉线兜底重连
  modemHealthTick();       // P0-2 模组健康探测/自动恢复
  signalSampleTick();      // 4G 信号采样(CSQ 高频/详情低频，与接收轮询解耦防长阻塞)
  smsReceiveWatchdogTick();// 修复"运行数天后只能发不能收"：兜底轮询暂存短信 + 重申 CNMI
  heapGuardTick();         // P1-7 低堆有序重启兜底
  keepAliveTick();         // E0 保号定时任务(绝对日期)
  dailyTasksTick();        // 定时重启 / 每日心跳
  yield();                 // 让出 CPU，喂看门狗与后台(WiFi/lwIP)任务
}
