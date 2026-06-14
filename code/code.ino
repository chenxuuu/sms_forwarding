#include "globals.h"
#include "wifi_config.h"
#include "config.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "sms_process.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  delay(1500);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();
  while (!sendATandWaitOK("AT", 1000)) {
    Serial.println("AT未响应，重试...");
    blink_short();
  }
  Serial.println("模组AT响应正常");
  while (!sendATandWaitOK("AT+CGACT=0,1", 5000)) {
    Serial.println("设置CGACT失败，重试...");
    blink_short();
  }
  Serial.println("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗");
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    Serial.println("设置CNMI失败，重试...");
    blink_short();
  }
  Serial.println("CNMI参数设置完成");
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    Serial.println("设置PDU模式失败，重试...");
    blink_short();
  }
  Serial.println("PDU模式设置完成");
  while (!waitCEREG()) {
    Serial.println("等待网络注册...");
    blink_short();
  }
  Serial.println("网络已注册");
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 0, nullptr, true);
  Serial.println("连接wifi");
  Serial.println(WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) blink_short();
  Serial.println("wifi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
  Serial.println("正在同步NTP时间...");
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) {
    delay(100);
    ntpRetry++;
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    Serial.println("NTP时间同步成功");
    time_t now = time(nullptr);
    Serial.print("当前UTC时间戳: ");
    Serial.println(now);
  } else {
    Serial.println("NTP时间同步失败，将使用设备时间");
  }
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleToolsPage);
  server.on("/sms", handleToolsPage);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/at", handleATCommand);
  server.begin();
  Serial.println("HTTP服务器已启动");
  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);
  if (configValid) {
    Serial.println("配置有效，发送启动通知...");
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

void loop() {
  server.handleClient();
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      Serial.println("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数");
    }
  }
  checkConcatTimeout();
  if (Serial.available()) Serial1.write(Serial.read());
  checkSerial1URC();
}
