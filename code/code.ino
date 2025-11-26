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
//管理员手机号码，只有管理员发送的命令短信才会被执行
#define ADMIN_PHONE "13800138000"


WiFiMulti WiFiMulti;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);

#define SERIAL_BUFFER_SIZE 500
#define MAX_PDU_LENGTH 300
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// 发送邮件通知函数
void sendEmailNotification(const char* subject, const char* body) {
  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
  };
  smtp.connect(SMTP_SERVER, SMTP_SERVER_PORT, statusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(SMTP_USER, SMTP_PASS, readymail_auth_password);

    SMTPMessage msg;
    String from = "sms notify <"; from += SMTP_USER; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += SMTP_SEND_TO; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    configTime(0, 0, "ntp.ntsc.ac.cn");
    while (time(nullptr) < 100000) delay(100);
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    Serial.println("邮件发送完成");
  } else {
    Serial.println("邮件服务器连接失败");
  }
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  Serial.println("准备发送短信...");
  Serial.print("目标号码: "); Serial.println(phoneNumber);
  Serial.print("短信内容: "); Serial.println(message);

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    Serial.print("PDU编码失败，错误码: ");
    Serial.println(pduLen);
    return false;
  }
  
  Serial.print("PDU数据: "); Serial.println(pdu.getSMS());
  Serial.print("PDU长度: "); Serial.println(pduLen);
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;
  
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmgsCmd);
  
  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      Serial.print(c);
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
  }
  
  if (!gotPrompt) {
    Serial.println("未收到>提示符");
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);
      if (resp.indexOf("OK") >= 0) {
        Serial.println("\n短信发送成功");
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        Serial.println("\n短信发送失败");
        return false;
      }
    }
  }
  Serial.println("短信发送超时");
  return false;
}

// 重启模组
void resetModule() {
  Serial.println("正在重启模组...");
  Serial1.println("AT+CFUN=1,1");
  delay(3000);
}

// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = String(ADMIN_PHONE);
  
  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }
  
  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  
  Serial.println("处理管理员命令: " + cmd);
  
  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);
    
    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);
      
      targetPhone.trim();
      smsContent.trim();
      
      Serial.println("目标号码: " + targetPhone);
      Serial.println("短信内容: " + smsContent);
      
      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());
      
      // 发送邮件通知结果
      String subject = success ? "短信发送成功" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "成功" : "失败");
      
      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      Serial.println("SMS命令格式错误");
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    Serial.println("执行RESET命令");
    
    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    
    // 重启模组
    resetModule();
    
    // 重启ESP32
    Serial.println("正在重启ESP32...");
    delay(1000);
    ESP.restart();
  }
  else {
    Serial.println("未知命令: " + cmd);
  }
}

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

        // 检查是否为管理员命令
        if (isAdmin(pdu.getSender())) {
          Serial.println("收到管理员短信，检查命令...");
          String smsText = String(pdu.getText());
          smsText.trim();
          
          // 检查是否为命令格式
          if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
            processAdminCommand(pdu.getSender(), pdu.getText());
            // 命令已处理，不再发送普通通知邮件
            state = IDLE;
            return;
          }
        }

        //发送通知http
        //sendSMSToServer(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        //发送通知邮件
        String subject = ""; subject+="短信";subject+=pdu.getSender();subject+=",";subject+=pdu.getText();
        String body = ""; body+="来自：";body+=pdu.getSender();body+="，时间：";body+=pdu.getTimeStamp();body+="，内容：";body+=pdu.getText();
        sendEmailNotification(subject.c_str(), body.c_str());
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
  //设置短信自动上报
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    Serial.println("设置CNMI失败，重试...");
    blink_short();
  }
  Serial.println("CNMI参数设置完成");
  //配置PDU模式
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    Serial.println("设置PDU模式失败，重试...");
    blink_short();
  }
  Serial.println("PDU模式设置完成");
  //等待CGATT附着
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