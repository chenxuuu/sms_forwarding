#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <pdulib.h>
#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include <HTTPClient.h>

//串口映射
#define TXD 3
#define RXD 4
//WIFI
#define WIFI_SSID "你家wifi"
#define WIFI_PASS "你家wifi密码"
//SMTP
#define SMTP_SERVER "smtp.qq.com"//smtp服务器
#define SMTP_SERVER_PORT 465//smtp服务器端口
#define SMTP_USER "xxxxx@qq.com"//登陆邮箱号
#define SMTP_PASS "xxxxxxxxxx"//登录密码，注意qq邮箱需要去生成专用授权码
#define SMTP_SEND_TO "xxxxx@qq.com"//收邮件的邮箱号


WiFiMulti WiFiMulti;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);

#define SERIAL_BUFFER_SIZE 500
#define MAX_PDU_LENGTH 300
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// 发送短信数据到服务器，按需修改，仅供例子
void sendSMSToServer(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED)
    return;
  HTTPClient http;
  Serial.println("\n发送短信数据到服务器...");
  // 替换成你的服务器地址
  http.begin("http://your-server.com/api/sms");
  http.addHeader("Content-Type", "application/json");
  
  // 构造JSON
  String jsonData = "{";
  jsonData += "\"sender\":\"" + String(sender) + "\",";
  jsonData += "\"message\":\"" + String(message) + "\",";
  jsonData += "\"timestamp\":\"" + String(timestamp) + "\"";
  jsonData += "}";
  Serial.println("发送数据: " + jsonData);
  int httpCode = http.POST(jsonData);
  if (httpCode > 0) {
    Serial.printf("服务器响应码: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      String response = http.getString();
      Serial.println("服务器响应: " + response);
    }
  } else {
    Serial.printf("HTTP请求失败: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 处理URC和PDU
void checkSerial1URC() {
  static enum { IDLE,
                WAIT_PDU } state = IDLE;

  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  // 打印到调试串口
  Serial.println("Debug> " + line);

  if (state == IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      Serial.println("检测到+CMT，等待PDU数据...");
      state = WAIT_PDU;
    }
  } else if (state == WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }
    
    // 如果是十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      Serial.println("收到PDU数据: " + line);
      Serial.println("PDU长度: " + String(line.length()) + " 字符");
      
      // 解析PDU
      if (!pdu.decodePDU(line.c_str())) {
        Serial.println("❌ PDU解析失败！");
      } else {
        Serial.println("✓ PDU解析成功");
        Serial.println("=== 短信内容 ===");
        Serial.println("发送者: " + String(pdu.getSender()));
        Serial.println("时间戳: " + String(pdu.getTimeStamp()));
        Serial.println("内容: " + String(pdu.getText()));
        Serial.println("===============");

        //发送通知http
        //sendSMSToServer(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        //发送通知邮件
        auto statusCallback = [](SMTPStatus status) {
          Serial.println(status.text);
        };
        smtp.connect(SMTP_SERVER, SMTP_SERVER_PORT, statusCallback);
        if (smtp.isConnected()) {
          smtp.authenticate(SMTP_USER, SMTP_PASS, readymail_auth_password);

          SMTPMessage msg;
          String from = "sms notify <"; from+=SMTP_USER; from+=">"; 
          msg.headers.add(rfc822_from, from.c_str());
          String to = "your_email <"; to+=SMTP_SEND_TO; to+=">"; 
          msg.headers.add(rfc822_to, to.c_str());
          String subject = ""; subject+="短信";subject+=pdu.getSender();subject+=",";subject+=pdu.getText();
          msg.headers.add(rfc822_subject, subject.c_str());
          String body = ""; body+="来自：";body+=pdu.getSender();body+="，时间：";body+=pdu.getTimeStamp();body+="，内容：";body+=pdu.getText();
          msg.text.body(body.c_str());
          configTime(0, 0, "ntp.ntsc.ac.cn");
          while (time(nullptr) < 100000) delay(100);
          msg.timestamp = time(nullptr);
          smtp.send(msg);
        }
      }
      
      // 返回IDLE状态
      state = IDLE;
    } 
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      Serial.println("收到非PDU数据，返回IDLE状态");
      state = IDLE;
    }
  }
}

void blink_short(unsigned long gap_time = 500) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

bool waitCGATT1() {
  Serial1.println("AT+CGATT?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 2000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("+CGATT: 1") >= 0) return true;
      if (resp.indexOf("+CGATT: 0") >= 0) return false;
    }
  }
  return false;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  Serial.println("连接wifi");
  while (WiFiMulti.run() != WL_CONNECTED) blink_short();
  Serial.println("wifi已连接");
  ssl_client.setInsecure();
  while (!sendATandWaitOK("AT", 1000)) {
    Serial.println("AT未响应，重试...");
    blink_short();
  }
  Serial.println("模组AT响应正常");
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    Serial.println("设置CNMI失败，重试...");
    blink_short();
  }
  Serial.println("CNMI参数设置完成");
  while (!waitCGATT1()) {
    Serial.println("等待CGATT附着...");
    blink_short();
  }
  Serial.println("CGATT已附着");
  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  // 本地透传
  if (Serial.available()) Serial1.write(Serial.read());
  // 检查URC和解析
  checkSerial1URC();
}