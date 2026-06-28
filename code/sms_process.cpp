#include "sms_process.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "inbox.h"

static char serialLineBuf[SERIAL_BUFFER_SIZE];
static int serialLinePos = 0;

enum SmsUrcState { SMS_URC_IDLE, SMS_URC_WAIT_PDU };
static SmsUrcState smsUrcState = SMS_URC_IDLE;
static bool storedSmsPending = false;  // 收到 +CMTI 后尽快补收 SIM/ME 暂存短信

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      concatBuffer[i].lastPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  logCaptureLn("长短信缓存已满，覆盖最老的槽位");
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  concatBuffer[oldestSlot].lastPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  int totalParts = concatBuffer[slot].totalParts;
  if (totalParts < 0) totalParts = 0;
  if (totalParts > MAX_CONCAT_PARTS) {
    logCaptureF("长短信总分段数异常(%d>%d)，按上限合并\n",
                concatBuffer[slot].totalParts, MAX_CONCAT_PARTS);
    totalParts = MAX_CONCAT_PARTS;
  }
  // P1-4 预估总长度一次预留，避免逐段 += 多次重分配
  size_t total = 0;
  for (int i = 0; i < totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) total += concatBuffer[slot].parts[i].text.length();
    else total += 12;
  }
  String result;
  result.reserve(total + 16);
  for (int i = 0; i < totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].totalParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].lastPartTime >= CONCAT_TIMEOUT_MS) {
        logCaptureLn("长短信超时，强制转发不完整消息");
        logCaptureF("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        // 合并已收到的分段
        String fullText = assembleConcatSms(i);
        
        // 处理短信内容
        processSmsContent(concatBuffer[i].sender.c_str(), 
                         fullText.c_str(), 
                         concatBuffer[i].timestamp.c_str());
        
        // 清空槽位
        clearConcatSlot(i);
      }
    }
  }
}

// 是否无半成品长短信缓存（低堆有序重启前的安全检查）
bool concatBufferIdle() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) return false;
  }
  return true;
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      serialLineBuf[serialLinePos] = 0;
      String res = String(serialLineBuf);
      serialLinePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (serialLinePos < SERIAL_BUFFER_SIZE - 1)
        serialLineBuf[serialLinePos++] = c;
      else
        serialLinePos = 0;  //超长报错保护，重头计
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

// 检查发送者是否在号码黑名单中（逻辑见 sms_logic.h::numberMatchesBlacklist，已主机测试）
bool isInNumberBlackList(const char* sender) {
  return numberMatchesBlacklist(config.numberBlackList, String(sender));
}

// 检查发送者是否为管理员（去 +86 国家码后比较，逻辑复用 sms_logic.h::stripCountryCode）
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  return stripCountryCode(String(sender)) == stripCountryCode(config.adminPhone);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  
  logCaptureLn(String("处理管理员命令: " + cmd));
  
  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);
    
    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);

      targetPhone.trim();
      smsContent.trim();

      logCaptureLn(String("目标号码: " + maskPhone(targetPhone)));
      logCaptureLn(String("短信内容: " + bodyPreview(smsContent, SMS_LOG_VERBOSE)));

      // P0-6 入参校验：目标号码须合法，内容非空且不超长，防畸形/超长污染发送流程
      if (!isValidPhoneNumber(targetPhone)) {
        logCaptureLn("目标号码非法，拒绝执行");
        sendEmailNotification("命令执行失败", "SMS命令目标号码非法（应为 3-20 位数字，可带 + 前缀）");
        return;
      }
      if (smsContent.length() == 0 || smsContent.length() > 300) {
        logCaptureLn("短信内容为空或超长，拒绝执行");
        sendEmailNotification("命令执行失败", "SMS命令内容为空或超过 300 字符");
        return;
      }

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
      logCaptureLn("SMS命令格式错误");
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    // P0-6 防重启风暴：刚启动 60s 内忽略 RESET。攻击者即便积压多条 RESET，
    // 每次重启后的前 60s 也不会立即再次重启，打断"反复重启致设备不可用"的循环。
    if (millis() < 60000UL) {
      logCaptureLn("设备刚启动，忽略RESET命令（防重启风暴）");
      sendEmailNotification("RESET已忽略", "设备启动不足60秒，已忽略RESET命令以防重启风暴。请稍后重试。");
      return;
    }
    logCaptureLn("执行RESET命令");

    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    
    // 重启模组
    resetModule();
    
    // 重启ESP32
    logCaptureLn("正在重启ESP32...");
    delay(1000);
    ESP.restart();
  }
  else {
    logCaptureLn(String("未知命令: " + cmd));
  }
}

// ---- P1-6 短信去重：最近哈希环(有界)，防设备/上游重复上报导致重复转发 ----
// 用 filled 计数只比较"已填充"的槽，避免与零初始化的空槽误匹配；因此无需对 h==0 特判，
// 哈希恰为 0 的正常短信也能被正确去重。
static uint32_t recentSmsHashes[DEDUP_WINDOW];
static int recentSmsIdx = 0;
static int recentSmsFilled = 0;
static bool smsSeenRecently(uint32_t h) {
  for (int i = 0; i < recentSmsFilled; i++) {
    if (recentSmsHashes[i] == h) return true;
  }
  return false;
}
static void rememberSms(uint32_t h) {
  recentSmsHashes[recentSmsIdx] = h;
  recentSmsIdx = (recentSmsIdx + 1) % DEDUP_WINDOW;
  if (recentSmsFilled < DEDUP_WINDOW) recentSmsFilled++;
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  // P0-5 脱敏：默认仅记录脱敏号码与正文长度，完整内容不入(网页可见的)日志环形缓冲
  logCaptureLn("=== 处理短信内容 ===");
  logCaptureLn(String("发送者: " + maskPhone(String(sender))));
  logCaptureLn(String("时间戳: " + String(timestamp)));
  logCaptureLn(String("内容: " + bodyPreview(String(text), SMS_LOG_VERBOSE)));
  logCaptureLn("====================");

  // 检查是否在号码黑名单中
  if (isInNumberBlackList(sender)) {
    logCaptureLn("发送者在号码黑名单中，忽略该短信");
    return;
  }

  // P1-6 幂等去重：同发件人+时间戳+内容在最近窗口内重复则忽略
  uint32_t h = fnv1a32(String(sender) + "|" + String(timestamp) + "|" + String(text));
  if (smsSeenRecently(h)) {
    logCaptureLn("重复短信(发件人/时间/内容一致)，已忽略");
    return;
  }
  rememberSms(h);

  // D4 统计：累计已处理短信数与最近一条时间
  smsTotalCount++;
  lastSmsEpoch = time(nullptr);

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    logCaptureLn("收到管理员短信，检查命令...");
    String smsText = String(text);
    smsText.trim();
    
    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return;
    }
  }

  // 存入本地收件箱(供网页查看)，并接收/转发解耦：入队由 loop() 异步推送+发邮件，
  // 避免慢速 HTTP/SMTP 阻塞 URC 接收。
  uint32_t mid = inboxAdd(sender, text, timestamp);
  enqueueForward(sender, text, timestamp, mid);
}

// 处理一条已 decodePDU 成功的短信（被实时 URC 与开机 SIM 补收复用）。操作全局 pdu。
static void handleDecodedPdu() {
  logCaptureLn("PDU解析成功");
  logCaptureLn("=== 短信内容 ===");
  logCaptureLn(String("发送者: " + maskPhone(String(pdu.getSender()))));
  logCaptureLn(String("时间戳: " + String(pdu.getTimeStamp())));
  logCaptureLn(String("内容: " + bodyPreview(String(pdu.getText()), SMS_LOG_VERBOSE)));

  int* concatInfo = pdu.getConcatInfo();
  int refNumber = concatInfo[0];
  int partNumber = concatInfo[1];
  int totalParts = concatInfo[2];
  logCaptureF("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);

  if (totalParts > 1 && partNumber > 0) {
    if (totalParts > MAX_CONCAT_PARTS || partNumber > totalParts) {
      logCaptureF("长短信分段参数超限，按单条分段处理: 当前=%d, 总计=%d, 上限=%d\n",
                  partNumber, totalParts, MAX_CONCAT_PARTS);
      processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
      return;
    }
    logCaptureF("收到长短信分段 %d/%d\n", partNumber, totalParts);
    int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
    int partIndex = partNumber - 1;
    if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
      if (!concatBuffer[slot].parts[partIndex].valid) {
        concatBuffer[slot].parts[partIndex].valid = true;
        concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
        concatBuffer[slot].receivedParts++;
        concatBuffer[slot].lastPartTime = millis();  // P1-5 刷新超时基准
        if (concatBuffer[slot].receivedParts == 1) {
          concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
        }
        logCaptureF("  已缓存分段 %d，当前已收到 %d/%d\n",
                    partNumber, concatBuffer[slot].receivedParts, totalParts);
      } else {
        logCaptureF("  分段 %d 已存在，跳过\n", partNumber);
      }
    }
    if (concatBuffer[slot].receivedParts >= totalParts) {
      logCaptureLn("长短信已收齐，开始合并转发");
      String fullText = assembleConcatSms(slot);
      processSmsContent(concatBuffer[slot].sender.c_str(), fullText.c_str(),
                        concatBuffer[slot].timestamp.c_str());
      clearConcatSlot(slot);
    }
  } else {
    processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
  }
}

// 轮询 SIM/ME 暂存短信：列出全部 PDU、逐条解析转发、按索引逐条删除释放存储。
// 既用于开机补收(A2 防丢)，也作为运行期的兜底接收 —— 即使 +CMT URC 失效(长时间运行后
// 模组把 CNMI 直传重置，导致"只能发不能收"的已知 bug)，短信被存到 SIM 仍能被本轮询取回转发。
// handleDecodedPdu 内已做去重，故 URC + 轮询双路径不会重复转发。
void backfillStoredSms(bool announce) {
  if (!modemReady) return;
  if (announce) logCaptureLn("检查 SIM 暂存短信(CMGL)...");
  String resp = sendATCommand("AT+CMGL=4", 5000);  // PDU 模式列出全部
  int processed = 0;   // 成功解码并转发的条数(用于日志)
  int handled = 0;     // 成功 + 删除的总条数(安全上限计数)
  int start = 0;
  int len = (int)resp.length();
  while (start < len) {
    int nl = resp.indexOf('\n', start);
    if (nl < 0) nl = len;
    String lineStr = resp.substring(start, nl);
    lineStr.trim();
    start = nl + 1;
    if (lineStr.startsWith("+CMGL:")) {
      // 解析消息索引: "+CMGL: <index>,<stat>,..."
      String after = lineStr.substring(6);
      after.trim();
      int comma = after.indexOf(',');
      String idxStr = (comma >= 0) ? after.substring(0, comma) : after;
      idxStr.trim();
      int msgIndex = idxStr.toInt();
      // 下一非空行为 PDU 十六进制
      int nl2 = resp.indexOf('\n', start);
      if (nl2 < 0) nl2 = len;
      String pduLine = resp.substring(start, nl2);
      pduLine.trim();
      start = nl2 + 1;
      bool decoded = false;
      if (isHexString(pduLine) && (pduLine.length() % 2) == 0 &&
          pduLine.length() <= (unsigned)(MAX_PDU_LENGTH * 2)) {
        if (pdu.decodePDU(pduLine.c_str())) {
          handleDecodedPdu();
          processed++;
          decoded = true;
        }
      }
      // 无论成功转发还是无法解析(畸形/不支持/损坏)，只要是 SIM 上的暂存条目都按索引删除：
      // 否则无法解析的垃圾 PDU 会永久残留，每次轮询(默认60s)重复读取，并最终占满 SIM 存储
      // (通常仅 20-50 条)导致模组无法再接收任何新短信。
      if (msgIndex >= 0) {
        if (!decoded) logCaptureF("PDU 无法解析(索引=%d)，删除以释放 SIM 存储\n", msgIndex);
        sendATCommand((String("AT+CMGD=") + msgIndex).c_str(), 5000);
        handled++;
      }
      if (handled >= 50) break;  // 安全上限，避免极端情况长时间阻塞(成功+删除合计)
    }
  }
  if (processed > 0) logCaptureF("SIM 暂存短信处理并删除 %d 条\n", processed);
  else if (announce) logCaptureLn("SIM 无暂存短信");
}

// 接收看门狗(修复"运行数天后只能发不能收")：周期性兜底轮询 SIM 暂存短信；并每 N 次重申
// CMGF/CNMI，恢复被模组重置的 URC 直传。参照 esp32-sms-bridge 的 storage-reader + receive-watchdog。
void smsReceiveWatchdogTick() {
  static unsigned long last = 0;
  static int pollCount = 0;
  if (!modemReady) return;
  if (smsRecvGuardUntil && millis() < smsRecvGuardUntil) return;  // 接收窗口内不发兜底AT，避免抢占正在直传的PDU
  if (storedSmsPending) {
    storedSmsPending = false;
    backfillStoredSms(false);
    return;
  }
  if (millis() - last < SMS_POLL_INTERVAL_MS) return;
  last = millis();
  if ((pollCount % SMS_CNMI_REASSERT_EVERY) == 0) {
    sendATandWaitOK("AT+CMGF=0", 1000);            // 重申 PDU 模式
    sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000);    // 重申 +CMT 直传(防长时间运行后被重置)
  }
  pollCount++;
  backfillStoredSms(false);  // 兜底接收 + 清存储
  // 信号采样(CSQ/RSRP/...)已解耦到 signalSampleTick()，不再与 CMGL 串行堆叠造成长阻塞
}

static void expireSmsUrcWindow() {
  if (smsUrcState == SMS_URC_WAIT_PDU && smsRecvGuardUntil &&
      (int32_t)(millis() - smsRecvGuardUntil) >= 0) {
    logCaptureLn("等待PDU超时，关闭短信接收窗口");
    smsUrcState = SMS_URC_IDLE;
    smsRecvGuardUntil = 0;
  }
}

// 当前是否已有半行串口数据或正在等待 +CMT 后续 PDU。AT 命令入口据此避让，防止清缓冲误删短信。
bool smsUrcReceiving() {
  expireSmsUrcWindow();
  return serialLinePos > 0 || smsUrcState == SMS_URC_WAIT_PDU ||
         (smsRecvGuardUntil && millis() < smsRecvGuardUntil);
}

// AT 命令前短暂优先处理已到达的短信 URC；如果只到了 +CMT 头，会等待 PDU 行到达。
void drainPendingSmsUrc(unsigned long maxWaitMs) {
  unsigned long start = millis();
  do {
    expireSmsUrcWindow();
    bool hadBytes = Serial1.available() > 0;
    checkSerial1URC();
    if (!hadBytes && !smsUrcReceiving()) break;
    delay(1);
    yield();
  } while (millis() - start < maxWaitMs);
  expireSmsUrcWindow();
  if (serialLinePos > 0 && !Serial1.available() && (millis() - start) >= maxWaitMs) {
    logCaptureLn("串口半行等待超时，丢弃残留");
    serialLinePos = 0;
  }
}

// 有些 AT 响应等待期间会混入 +CMT/PDU。调用方虽然已把字节读走，但这里再按行提取，
// 避免"网页查信号/USSD/UDP诊断 时刚好来短信"导致 URC 被当作普通响应吞掉。
int processSmsUrcText(const String& text) {
  int processed = 0;
  bool waitPdu = false;
  int start = 0;
  int len = (int)text.length();
  while (start < len) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = len;
    String line = text.substring(start, nl);
    line.trim();
    start = nl + 1;

    if (waitPdu) {
      if (line.length() == 0) continue;
      if (isHexString(line) && (line.length() % 2) == 0 &&
          line.length() <= (unsigned)(MAX_PDU_LENGTH * 2)) {
        if (pdu.decodePDU(line.c_str())) {
          logCaptureLn("从AT响应中提取到短信URC");
          handleDecodedPdu();
          processed++;
        } else {
          logCaptureLn("AT响应内PDU解析失败");
        }
      } else {
        logCaptureLn("AT响应内+CMT后未找到合法PDU");
      }
      waitPdu = false;
      continue;
    }

    if (line.startsWith("+CMT:")) {
      waitPdu = true;
    } else if (line.startsWith("+CMTI:")) {
      logCaptureLn("AT响应中发现+CMTI，标记补收存储短信");
      storedSmsPending = true;
    }
  }
  return processed;
}

// 处理URC和PDU
void checkSerial1URC() {
  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  lastModemOkMs = millis();  // 收到任何串口行即证明模组存活，抑制不必要的主动健康探测

  // 打印到调试串口
  logCaptureLn(String("Debug> " + line));

  if (smsUrcState == SMS_URC_IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      logCaptureLn("检测到+CMT，等待PDU数据...");
      smsUrcState = SMS_URC_WAIT_PDU;
      smsRecvGuardUntil = millis() + 3000;  // 开接收窗口：暂停信号采样AT，防其清空缓冲冲掉随后到达的PDU行
    } else if (line.startsWith("+CMTI:")) {
      logCaptureLn("检测到+CMTI，标记补收存储短信");
      storedSmsPending = true;
    }
  } else if (smsUrcState == SMS_URC_WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }
    
    // 如果是十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      logCaptureLn(String("收到PDU数据: " + line));
      logCaptureLn(String("PDU长度: " + String(line.length()) + " 字符"));

      // P0-4 输入校验：长度须为偶数(每字节2个hex)且不超过缓冲上限，
      // 防止畸形/截断 PDU 触发 pdulib 越界访问。
      if ((line.length() % 2) != 0 || line.length() > (unsigned)(MAX_PDU_LENGTH * 2)) {
        logCaptureLn("PDU长度非法(奇数或超限)，丢弃");
        smsUrcState = SMS_URC_IDLE; smsRecvGuardUntil = 0;
        return;
      }

      // 解析PDU
      if (!pdu.decodePDU(line.c_str())) {
        logCaptureLn("PDU解析失败");
      } else {
        handleDecodedPdu();
      }

      // 返回IDLE状态
      smsUrcState = SMS_URC_IDLE; smsRecvGuardUntil = 0;
    } 
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      logCaptureLn("收到非PDU数据，返回IDLE状态");
      smsUrcState = SMS_URC_IDLE; smsRecvGuardUntil = 0;
    }
  }
}
