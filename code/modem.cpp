#include "modem.h"
#include "web_handlers.h"
#include "inbox.h"
#include "sms_process.h"

static bool modemSerialBusy = false;

bool modemSerialTryBegin(const char* label) {
  if (modemSerialBusy) {
    logCaptureLn(String("模组串口正忙，暂缓操作: ") + label);
    return false;
  }
  modemSerialBusy = true;
  return true;
}

void modemSerialEnd() {
  modemSerialBusy = false;
}

static bool beginModemSerialOp(const char* label) {
  return modemSerialTryBegin(label);
}

static void endModemSerialOp() {
  modemSerialEnd();
}

static void pumpWebServerDuringWait() { pumpWebDuringWait(); }  // 统一实现见 globals.h

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  if (modemSerialBusy) {
    logCaptureLn(String("模组串口正忙，暂缓操作: ") + cmd);
    return "";
  }
  static bool preDraining = false;
  if (!preDraining) {
    preDraining = true;
    drainPendingSmsUrc(3000);
    preDraining = false;
  }
  if (smsUrcReceiving()) {
    logCaptureLn(String("短信接收窗口未空闲，暂缓AT命令: ") + cmd);
    return "";
  }
  if (!beginModemSerialOp(cmd)) return "";
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        // 读取剩余数据（最多 50ms）
        unsigned long t = millis();
        while (millis() - t < 50) {
          if (Serial1.available()) resp += (char)Serial1.read();
          pumpWebServerDuringWait();  // web 栈内不重入，内部泵也标记HTTP栈，避免嵌套AT抢串口
        }
        endModemSerialOp();
        processSmsUrcText(resp);
        return resp;
      }
    }
    pumpWebServerDuringWait();  // web 栈内不重入，内部泵也标记HTTP栈，避免嵌套AT抢串口
  }
  endModemSerialOp();
  processSmsUrcText(resp);
  return resp;
}

// 新增"模组断电重启"函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  logCaptureLn("EN 拉低：关闭模组");
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(MODEM_POWERDOWN_MS);  // 关机时间给够（按型号可调）

  logCaptureLn("EN 拉高：开启模组");
  digitalWrite(MODEM_EN_PIN, HIGH);
  // 上电探活：最小安定延时后轮询 AT，模组应答即提前结束，最长仍不超过 MODEM_POWERUP_MS。
  // 省每次开机 3-4s（原固定等 6s，阻塞在 WiFi 之前）。
  // ponytail: 极简上电探针(不泵 web、不判 ERROR)，故未复用 sendATCommand；MIN/上限为按型号校准旋钮——
  //           过早发 AT 个别 ML307 变体会失败，换模组先调 MODEM_POWERUP_MIN_MS。
  delay(MODEM_POWERUP_MIN_MS);
  unsigned long budget = (MODEM_POWERUP_MS > MODEM_POWERUP_MIN_MS) ? (MODEM_POWERUP_MS - MODEM_POWERUP_MIN_MS) : 0;
  for (unsigned long t0 = millis(); millis() - t0 < budget; ) {
    while (Serial1.available()) Serial1.read();
    Serial1.println("AT");
    String r;
    for (unsigned long s = millis(); millis() - s < 250; )
      if (Serial1.available()) r += (char)Serial1.read();
    if (r.indexOf("OK") >= 0) { logCaptureLn("模组上电已应答，提前结束等待"); break; }
  }
}

// 重启模组（EN引脚断电重启 + 重新初始化）
void resetModule() {
  logCaptureLn("正在硬重启模组（EN 断电重启）...");
  modemPowerCycle();
  modemInit();
}

// 模组 AT 初始化流程（setup 中调用，resetModule 后也调用）
// P0-2：所有原"无限 while 重试"改为有限重试，握手彻底失败时断电重启一次再试，
// 仍失败则标记 modemReady=false 退出，绝不永久阻塞（后续由 modemHealthTick 自动重试恢复）。
void modemInit() {
  // 清掉上电噪声/残留
  while (Serial1.available()) Serial1.read();

  bool atOk = false;
  for (int round = 0; round < 2 && !atOk; round++) {
    for (int i = 0; i < MODEM_INIT_AT_RETRIES; i++) {
      if (sendATandWaitOK("AT", 1000)) { atOk = true; break; }
      logCaptureLn("AT未响应，重试...");
      blink_short();
    }
    if (!atOk && round == 0) {
      logCaptureLn("AT多次无响应，断电重启模组后再试...");
      modemPowerCycle();
    }
  }
  if (!atOk) {
    logCaptureLn("模组AT无响应，标记未就绪（健康检查将稍后自动重试）");
    modemReady = false;
    return;
  }
  logCaptureLn("模组AT响应正常");

  //判断型号，做一些特定操作
  bool need_set_CGACT = true;
  String resp = sendATCommand("ATI", 2000);
  logCaptureLn(String("ATI响应: " + resp));
  if (resp.indexOf("OK") >= 0) {
    String manufacturer = "未知", model = "未知", version = "未知";
    parseATI(resp, manufacturer, model, version);
    //这个模组这条命令有bug
    if (model == "ML307Y") need_set_CGACT = false;
  }

  if(need_set_CGACT) {
    if (config.dataEnabled) {
      // 用户已在“SIM 卡”页允许蜂窝数据：下发 APN(若填写)并激活 PDP
      if (config.apn.length() > 0)
        sendATandWaitOK(("AT+CGDCONT=1,\"IP\",\"" + config.apn + "\"").c_str(), 3000);
      int tries = 0;
      while (!sendATandWaitOK("AT+CGACT=1,1", 10000) && tries < MODEM_INIT_CMD_RETRIES) {
        logCaptureLn("激活数据连接失败，重试...");
        blink_short();
        tries++;
      }
      logCaptureLn("已按配置启用蜂窝数据(AT+CGACT=1,1)");
    } else {
      int tries = 0;
      while (!sendATandWaitOK("AT+CGACT=0,1", 5000) && tries < MODEM_INIT_CMD_RETRIES) {
        logCaptureLn("设置CGACT失败，重试...");
        blink_short();
        tries++;
      }
      logCaptureLn("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗");
    }
  } else {
    logCaptureLn("该型号无法配置(AT+CGACT=0,1)，跳过该命令，会不会消耗流量？自求多福");
  }
  if (sendATandWaitOK("AT+CPMS=\"MT\",\"MT\",\"MT\"", 2000)) {
    logCaptureLn("短信存储已设为 MT(合并存储)");
  } else if (sendATandWaitOK("AT+CPMS=\"SM\",\"SM\",\"SM\"", 2000)) {
    logCaptureLn("短信存储已设为 SM(SIM卡)");
  } else {
    logCaptureLn("短信存储设置失败，沿用模组默认存储");
  }
  for (int tries = 0; tries < MODEM_INIT_CMD_RETRIES && !sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000); tries++) {
    logCaptureLn("设置CNMI失败，重试...");
    blink_short();
  }
  logCaptureLn("CNMI参数设置完成");
  for (int tries = 0; tries < MODEM_INIT_CMD_RETRIES && !sendATandWaitOK("AT+CMGF=0", 1000); tries++) {
    logCaptureLn("设置PDU模式失败，重试...");
    blink_short();
  }
  logCaptureLn("PDU模式设置完成");
  sampleModemIdentity();   // 启动只采样一次并写入 NVS；失败则沿用旧缓存/显示为空，不后台重试
  int ceregRetry = 0;
  while (!waitCEREG() && ceregRetry < 30) {
    logCaptureLn("等待网络注册...");
    ceregRetry++;
    blink_short();
  }
  if (ceregRetry < 30) {
    logCaptureLn("网络已注册");
    modemReady = true;
    lastModemOkMs = millis();
    // 采样运营商名(开机一次，供首页显示)
    String cops = sendATCommand("AT+COPS?", 3000);
    int q1 = cops.indexOf('"');
    int q2 = cops.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) modemOperator = cops.substring(q1 + 1, q2);
    if (config.operatorPlmn.length()) modemApplyOperator();  // 仅在用户显式锁定运营商时下发
    sampleSignalDetail();    // RSRP/RSRQ/SINR/PCI/PLMN/TAC
    if (config.dataEnabled) sampleCellIp();
  } else {
    logCaptureLn("网络注册超时（无SIM卡或信号差），模组功能暂不可用");
    modemReady = false;
  }
}

bool queryModemImei(String& imeiOut, String* rawResp, unsigned long timeout) {
  imeiOut = "";
  if (rawResp) *rawResp = "";
  const char* imeiCmds[] = {"AT+CGSN=1", "AT+GSN=1", "AT+CGSN", "AT+GSN"};
  for (unsigned i = 0; i < sizeof(imeiCmds) / sizeof(imeiCmds[0]); i++) {
    String resp = sendATCommand(imeiCmds[i], timeout);
    if (rawResp) {
      if (rawResp->length() > 0) *rawResp += "\n";
      *rawResp += imeiCmds[i];
      *rawResp += ": ";
      String one = resp;
      one.trim();
      if (one.length() > 180) one = one.substring(0, 180) + "...";
      *rawResp += one;
    }
    String found = extractImei(resp);
    if (found.length() > 0) {
      imeiOut = found;
      return true;
    }
  }
  return false;
}

// 采样 IMEI 与本机号码(开机一次，纯本地 AT，不产生流量)。
void sampleModemIdentity() {
  String oldImei = modemImei;
  String oldIccid = modemIccid;
  String foundImei;
  if (queryModemImei(foundImei, nullptr, 2000)) modemImei = foundImei;
  String n = sendATCommand("AT+CNUM", 2000);   // +CNUM: "","+86...",145 (常为空)
  int c = n.indexOf("+CNUM:");
  if (c >= 0) {
    int q1 = n.indexOf('"', n.indexOf(',', c));  // 第二个字段是号码
    int q2 = (q1 >= 0) ? n.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1 + 1) modemPhone = n.substring(q1 + 1, q2);
  }
  // ICCID：ML307 用 AT+MCCID(+MCCID:)，回退标准 AT+ICCID(+ICCID:)
  String ic = sendATCommand("AT+MCCID", 2000);
  int icp = ic.indexOf("+MCCID:");
  if (icp < 0) { ic = sendATCommand("AT+ICCID", 2000); icp = ic.indexOf("+ICCID:"); }
  if (icp >= 0) {
    int st = ic.indexOf(':', icp) + 1;
    String v = ic.substring(st); v.replace("\"", "");
    int nl = v.indexOf('\r'); if (nl < 0) nl = v.indexOf('\n');
    if (nl >= 0) v = v.substring(0, nl);
    v.trim();
    if (v.length() >= 15) modemIccid = v;
  }
  if (modemImei != oldImei || modemIccid != oldIccid) saveModemIdentityCache();
}

void saveModemIdentityCache() {
  if (modemImei.length() == 0 && modemIccid.length() == 0) return;
  preferences.begin("sms_config", false);
  if (modemImei.length() >= 14) preferences.putString("modemImei", modemImei);
  if (modemIccid.length() >= 15) preferences.putString("modemIccid", modemIccid);
  preferences.end();
  logCaptureLn("模组身份信息已写入缓存");
}

// 读取蜂窝 PDP 地址(仅 dataEnabled 时有意义)。不带 cid 返回所有已定义上下文，取首个有效 IP。
void sampleCellIp() {
  String r = sendATCommand("AT+CGPADDR", 3000);  // +CGPADDR: <cid>,"10.x.x.x"
  modemCellIp = "";
  int c = r.indexOf("+CGPADDR:");
  while (c >= 0) {
    int comma = r.indexOf(',', c);
    int eol = r.indexOf('\n', c); if (eol < 0) eol = r.length();
    if (comma >= 0 && comma < eol) {
      String ip = r.substring(comma + 1, eol);
      ip.replace("\"", ""); ip.trim();
      if (ip.length() >= 7 && ip != "0.0.0.0") { modemCellIp = ip; break; }
    }
    c = r.indexOf("+CGPADDR:", eol);
  }
}

// 详细服务小区信息：ML307 AT+MUESTATS="cell"。RSRP/RSRQ/RSSI/SINR 为 0.1 单位需 /10。
// scell 行: +MUESTATS: "scell",<rat>,<mcc>,<mnc>,<earfcn>,<offset>,<pci>,<rsrp>,<rsrq>,<rssi>,<sinr>[,<bw>]
// TAC 不在此行 → 从 AT+CEREG? 的十六进制 <tac> 字段取。-32768 为无效哨兵。
void sampleSignalDetail() {
  String r = sendATCommand("AT+MUESTATS=\"cell\"", 2000);
  int line = r.indexOf("\"scell\"");
  if (line >= 0) {
    int eol = r.indexOf('\n', line); if (eol < 0) eol = r.length();
    String ln = r.substring(line, eol);   // "scell",4,460,00,...
    String parts[12]; int np = 0, from = 0;
    while (np < 12) {
      int comma = ln.indexOf(',', from);
      if (comma < 0) { parts[np++] = ln.substring(from); break; }
      parts[np++] = ln.substring(from, comma);
      from = comma + 1;
    }
    // parts[0]="scell" parts[1]=rat parts[2]=mcc parts[3]=mnc parts[6]=pci parts[7]=rsrp parts[8]=rsrq parts[10]=sinr
    if (np > 10) {
      long mcc = parts[2].toInt(), mnc = parts[3].toInt();
      long pci = parts[6].toInt();
      String prsrp = parts[7]; prsrp.trim();
      String prsrq = parts[8]; prsrq.trim();
      String psinr = parts[10]; psinr.trim();
      if (parts[2].length() && parts[3].length()) {
        char b[12]; snprintf(b, sizeof(b), "%ld%02ld", mcc, mnc); modemPlmn = b;
      }
      if (parts[6].length() && pci >= 0 && pci != 65535) modemPci = (int)pci;
      if (prsrp.length()) { long v = prsrp.toInt(); if (v > -32768) modemRsrp = (int)(v / 10); }
      if (prsrq.length()) { long v = prsrq.toInt(); if (v > -32768) modemRsrq = (int)(v / 10); }
      if (psinr.length()) { long v = psinr.toInt(); if (v > -32768) modemSinr = (int)(v / 10); }
    }
  } else {
    // 回退：标准 AT+CESQ 取 RSRP/RSRQ 索引并换算(无 SINR/PCI)
    String ce = sendATCommand("AT+CESQ", 2000);
    int cei = ce.indexOf("+CESQ:");
    if (cei >= 0) {
      int e = ce.indexOf('\n', cei); if (e < 0) e = ce.length();
      String l = ce.substring(cei, e);
      int lc = l.lastIndexOf(',');
      int pc = l.lastIndexOf(',', lc - 1);
      if (lc >= 0) { int idx = l.substring(lc + 1).toInt(); if (idx >= 0 && idx <= 97) modemRsrp = idx - 141; }
      if (pc >= 0 && lc > pc) { int idx = l.substring(pc + 1, lc).toInt(); if (idx >= 0 && idx <= 34) modemRsrq = idx / 2 - 20; }
    }
  }
  // TAC：+CEREG: <n>,<stat>,"<tac>","<ci>",<AcT>
  String c = sendATCommand("AT+CEREG?", 2000);
  int ce = c.indexOf("+CEREG:");
  if (ce >= 0) {
    int q1 = c.indexOf('"', ce);
    int q2 = (q1 >= 0) ? c.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1 + 1) modemTac = c.substring(q1 + 1, q2);
  }
}

// 信号采样调度(与 SMS 接收轮询解耦，避免一次 watchdog tick 串行堆叠多条 AT 造成 ~9s 长阻塞)：
// CSQ 信号条高频采样(快, 首页数值跟手)；RSRP/SINR/PCI 等详细指标变化慢，低频采样省阻塞。
void signalSampleTick() {
  if (!modemReady) return;
  if (smsRecvGuardUntil && millis() < smsRecvGuardUntil) return;  // 短信接收窗口内不采样，避免AT清缓冲冲掉URC
  static unsigned long lastFast = 0, lastDetail = 0;
  unsigned long now = millis();
  if (now - lastFast >= SIGNAL_FAST_INTERVAL_MS) {
    lastFast = now;
    String csq = sendATCommand("AT+CSQ", 1500);
    int ci = csq.indexOf("+CSQ:");
    if (ci >= 0) {
      int colon = csq.indexOf(':', ci);
      int comma = csq.indexOf(',', colon);
      if (colon >= 0 && comma > colon) modemCsq = csq.substring(colon + 1, comma).toInt();
    }
  }
  if (now - lastDetail >= SIGNAL_DETAIL_INTERVAL_MS) {
    lastDetail = now;
    sampleSignalDetail();
  }
}

// 应用运营商选择(AT+COPS)。空=自动注册(COPS=0)；否则锁定 PLMN(COPS=1,2,"<plmn>")。
// 注意：只能选 SIM 允许接入的网络；锁定不可达 PLMN 会失网。COPS 可能耗时较长(最长 ~30s)。
void modemApplyOperator() {
  if (!modemReady) return;
  if (config.operatorPlmn.length() == 0) {
    sendATandWaitOK("AT+COPS=0", 30000);
    logCaptureLn("运营商: 自动注册(COPS=0)");
  } else {
    String cmd = "AT+COPS=1,2,\"" + config.operatorPlmn + "\"";
    bool ok = sendATandWaitOK(cmd.c_str(), 30000);
    logCaptureLn(String("运营商: 锁定 PLMN " + config.operatorPlmn + (ok ? " 成功" : " 失败(可能不可达)")));
  }
}

// 按当前配置应用蜂窝数据模式(供 SIM 页保存后即时生效，无需重启)。
// 单线程模型下与 checkSerial1URC 不会并发，缓冲中的 URC 下个 loop 仍会被读取。
void modemApplyDataMode() {
  if (!modemReady) return;
  if (config.dataEnabled) {
    if (config.apn.length() > 0)
      sendATandWaitOK(("AT+CGDCONT=1,\"IP\",\"" + config.apn + "\"").c_str(), 3000);
    sendATandWaitOK("AT+CGACT=1,1", 10000);
    sampleCellIp();
    logCaptureLn(String("蜂窝数据已启用(APN=" + (config.apn.length() ? config.apn : String("自动")) +
                        ", IP=" + (modemCellIp.length() ? modemCellIp : String("获取中")) + ")"));
  } else {
    sendATandWaitOK("AT+CGACT=0,1", 5000);
    modemCellIp = "";
    logCaptureLn("蜂窝数据已禁用(零流量)");
  }
}

// P0-2 模组健康探测：loop() 周期调用。仅在"距上次确认存活已超过探测周期"时才主动发
// AT+CEREG? —— 任何收到的串口行(含短信URC)都会刷新 lastModemOkMs(见 checkSerial1URC)，
// 因此有真实短信流量或近期 AT 操作时不会主动探测，最大限度降低探测 AT 抢占到达短信 URC 的概率。
// 连续失败达阈值则自动断电重启并重新初始化恢复。探测仅走本地 AT，不产生流量/资费。
void modemHealthTick() {
  static int failCount = 0;
  static bool inTick = false;
  if (inTick) return;                                   // 防重入（恢复期间 modemInit 会 pump handleClient）
  if (millis() - lastModemOkMs < MODEM_HEALTH_INTERVAL_MS) return;
  inTick = true;

  if (waitCEREG()) {
    modemReady = true;
    lastModemOkMs = millis();
    failCount = 0;
  } else {
    failCount++;
    logCaptureF("模组健康探测失败 %d/%d\n", failCount, MODEM_HEALTH_FAIL_LIMIT);
    if (failCount >= MODEM_HEALTH_FAIL_LIMIT) {
      logCaptureLn("模组连续无响应，自动断电重启恢复...");
      modemReady = false;
      modemPowerCycle();
      modemInit();
      failCount = 0;
    }
    lastModemOkMs = millis();                            // 失败也刷新，避免下一帧立即重复探测
  }
  inTick = false;
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

// 发 AT 并判 OK：复用 sendATCommand 的读循环(含防重入泵/yield)，避免重复读逻辑
bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  return sendATCommand(cmd, timeout).indexOf("OK") >= 0;
}

// 检测网络注册状态（LTE/4G）。CEREG状态: 1=已注册本地, 5=已注册漫游，其余(0/2/3/4)视为未注册
bool waitCEREG() {
  String resp = sendATCommand("AT+CEREG?", 2000);
  if (resp.indexOf("+CEREG:") < 0) return false;
  return resp.indexOf(",1") >= 0 || resp.indexOf(",5") >= 0;
}

// 发送短信（PDU模式）
static bool sendSMSImpl(const char* phoneNumber, const char* message) {
  logCaptureLn("准备发送短信...");
  logCapture("目标号码: "); logCaptureLn(maskPhone(String(phoneNumber)));
  logCapture("短信内容: "); logCaptureLn(bodyPreview(String(message), SMS_LOG_VERBOSE));

  if (modemSerialBusy) {
    logCaptureLn("模组串口正忙，取消本次短信发送");
    return false;
  }
  drainPendingSmsUrc(3000);
  if (smsUrcReceiving()) {
    logCaptureLn("短信接收窗口未空闲，取消本次短信发送以避免冲掉接收PDU");
    return false;
  }
  if (!beginModemSerialOp("发送短信")) return false;

  // 使用pdulib编码PDU；pdu 是全局对象，必须在串口锁内使用，防止嵌套发送覆盖缓冲。
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    logCapture("PDU编码失败，错误码: ");
    logCaptureLn(String(pduLen));
    endModemSerialOp();
    return false;
  }
  
#if SMS_LOG_VERBOSE
  logCapture("PDU数据: "); logCaptureLn(String(pdu.getSMS()));
#else
  logCaptureLn("PDU数据: [hidden]");
#endif
  logCapture("PDU长度: "); logCaptureLn(String(pduLen));
  
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
      logCapture(String(c));
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    pumpWebServerDuringWait();  // 发送等待期间允许网页轻量响应，但禁止嵌套AT抢串口
  }
  
  if (!gotPrompt) {
    logCaptureLn("未收到>提示符");
    endModemSerialOp();
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
      logCapture(String(c));
      if (resp.indexOf("OK") >= 0) {
        endModemSerialOp();
        processSmsUrcText(resp);
        logCaptureLn("\n短信发送成功");
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        endModemSerialOp();
        processSmsUrcText(resp);
        logCaptureLn("\n短信发送失败");
        return false;
      }
    }
    pumpWebServerDuringWait();  // 发送等待期间允许网页轻量响应，但禁止嵌套AT抢串口
    yield();  // 长等待中让出 CPU，喂看门狗
  }
  endModemSerialOp();
  processSmsUrcText(resp);
  logCaptureLn("短信发送超时");
  return false;
}

// 公开入口：统一记录到“已发送”环形缓冲(供网页查看)，覆盖网页发送/管理员指令/保号短信
bool sendSMS(const char* phoneNumber, const char* message) {
  bool ok = sendSMSImpl(phoneNumber, message);
  sentAdd(phoneNumber, message, ok);
  return ok;
}

static bool sendUdpDataChunk(uint16_t len) {
  if (!beginModemSerialOp("发送UDP流量数据")) return false;
  while (Serial1.available()) Serial1.read();
  Serial1.print("AT+MIPSEND=0,");
  Serial1.println(len);

  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      if (c == '>') { gotPrompt = true; break; }
    }
    pumpWebServerDuringWait();
  }
  if (!gotPrompt) {
    logCaptureLn("MIPSEND 未收到 > 提示符");
    endModemSerialOp();
    return false;
  }

  static const char payload[128] = {
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A'
  };
  uint16_t left = len;
  while (left > 0) {
    uint16_t n = left > sizeof(payload) ? sizeof(payload) : left;
    Serial1.write((const uint8_t*)payload, n);
    left -= n;
    yield();
  }

  start = millis();
  String resp;
  while (millis() - start < 8000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) {
        endModemSerialOp();
        processSmsUrcText(resp);
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        endModemSerialOp();
        processSmsUrcText(resp);
        return false;
      }
    }
    pumpWebServerDuringWait();
  }
  endModemSerialOp();
  processSmsUrcText(resp);
  logCaptureLn("MIPSEND 响应超时");
  return false;
}

bool consumeCellularDataBytes(unsigned long targetBytes, const char* host, uint16_t port) {
  if (targetBytes == 0) return true;
  if (!host || !host[0]) host = CELLULAR_BURN_DEFAULT_HOST;
  if (port == 0) port = 53;
  if (targetBytes > CELLULAR_BURN_MAX_BYTES) {
    logCaptureF("蜂窝UDP发送字节数 %lu 超过上限 %lu，已按上限执行\n",
                targetBytes, (unsigned long)CELLULAR_BURN_MAX_BYTES);
    targetBytes = CELLULAR_BURN_MAX_BYTES;
  }

  logCaptureF("准备发送蜂窝UDP流量: 目标约 %lu 字节 -> %s:%u\n",
              targetBytes, host, (unsigned)port);
  sendATCommand("AT+MIPCLOSE=0", 2000);  // 清理旧 socket；失败也可继续
  String openCmd = String("AT+MIPOPEN=0,\"UDP\",\"") + host + "\"," + String(port);
  String openResp = sendATCommand(openCmd.c_str(), 10000);
  if (openResp.indexOf("OK") < 0) {
    logCaptureLn(String("打开 UDP socket 失败: ") + openResp);
    return false;
  }

  if (targetBytes > 65535UL) {
    logCaptureLn("蜂窝UDP单次发送超过 MIPSEND 长度上限，已取消");
    sendATCommand("AT+MIPCLOSE=0", 3000);
    return false;
  }
  if (targetBytes != CELLULAR_BURN_MIPSEND_BYTES) {
    logCaptureF("蜂窝UDP本次按单次 MIPSEND 发送 %lu 字节\n", targetBytes);
  }
  if (!sendUdpDataChunk((uint16_t)targetBytes)) {
    logCaptureF("蜂窝UDP单次发送失败: %lu 字节\n", targetBytes);
    sendATCommand("AT+MIPCLOSE=0", 3000);
    return false;
  }

  sendATCommand("AT+MIPCLOSE=0", 3000);
  logCaptureF("蜂窝UDP发送完成: 约 %lu 字节\n", targetBytes);
  return true;
}

// 保号: 通过模组 TCP 向 baidu 发起一次 HTTP GET 以产生真实蜂窝流量(动账)。
// 相比 UDP 打流量，HTTP GET 的下行响应(baidu 首页/跳转)会被模组自动以 +MIPURC:"rtcp" 收下 ——
// 这部分下行字节即为消耗的流量，运营商按真实数据会话计费，更稳妥地维持号码活跃。
// 全程占用模组串口(beginModemSerialOp)，与 UDP 路径一致；只用 MIPOPEN/MIPSEND/MIPCLOSE，不引入HTTP库。
// 前置条件: PDP 已激活(调用方先 AT+CGACT=1,1)。
bool consumeCellularViaHttpGet(const char* host) {
  if (!host || !host[0]) host = KEEPALIVE_HTTP_HOST;
  logCaptureF("保号: 准备 HTTP GET 消耗流量 http://%s/\n", host);
  if (!beginModemSerialOp("HTTP GET 保号")) return false;

  // 等待 Serial1 出现 needle 或 fail 标记，期间泵网页。命中 needle 返回 true。
  auto waitToken = [](const char* needle, const char* failTok, unsigned long timeout) -> bool {
    String buf; unsigned long start = millis();
    while (millis() - start < timeout) {
      while (Serial1.available()) {
        buf += (char)Serial1.read();
        if (buf.indexOf(needle) >= 0) return true;
        if (failTok && failTok[0] && buf.indexOf(failTok) >= 0) return false;
        if (buf.length() > 600) buf.remove(0, buf.length() - 80);  // 限长防膨胀，保留尾部
      }
      pumpWebServerDuringWait();
    }
    return false;
  };

  bool ok = false;
  do {
    // 1) 清理旧 socket(失败也继续)
    while (Serial1.available()) Serial1.read();
    Serial1.println("AT+MIPCLOSE=0");
    waitToken("OK", "ERROR", 2000);

    // 2) 打开 TCP 到 host:80；OK 仅表示命令受理，连接结果由 +MIPOPEN: 0,<r> URC 给出(0=成功)
    while (Serial1.available()) Serial1.read();
    Serial1.println(String("AT+MIPOPEN=0,\"TCP\",\"") + host + "\",80");
    // 先等 +MIPOPEN: 0,0(成功)；DNS+握手可能数秒。失败码(非0)亦以 +MIPOPEN: 0, 形式出现 -> 由超时兜底。
    if (!waitToken("+MIPOPEN: 0,0", nullptr, 15000)) {
      logCaptureLn("保号: TCP 连接 baidu 未成功(+MIPOPEN 非0或超时)");
      break;
    }

    // 3) 发送 HTTP GET 请求(MIPSEND 提示符方式，原样写入请求字节)
    String req = String("GET / HTTP/1.0\r\nHost: ") + host +
                 "\r\nUser-Agent: ESP32-SMS-Forwarder\r\nConnection: close\r\n\r\n";
    while (Serial1.available()) Serial1.read();
    Serial1.print("AT+MIPSEND=0,");
    Serial1.println(req.length());
    if (!waitToken(">", "ERROR", 5000)) { logCaptureLn("保号: MIPSEND 未收到 > 提示符"); break; }
    Serial1.print(req);
    if (!waitToken("OK", "ERROR", 5000)) { logCaptureLn("保号: HTTP 请求发送未确认"); break; }

    // 4) 读取响应：这些下行字节(+MIPURC:"rtcp")即为消耗的流量；不解析内容，仅计数。
    //    收到响应后连续静默 KEEPALIVE_HTTP_IDLE_MS 即提前结束(响应已收完)，省去固定 5s 空转、
    //    并收窄"读取期间吞掉随后到达的 +CMT 短信"的窗口；硬上限仍为 KEEPALIVE_HTTP_DRAIN_MS。
    unsigned long rx = 0, start = millis(), lastByte = start;
    while (millis() - start < KEEPALIVE_HTTP_DRAIN_MS) {
      bool got = false;
      while (Serial1.available()) { Serial1.read(); rx++; got = true; }
      if (got) lastByte = millis();
      else if (rx > 0 && millis() - lastByte > KEEPALIVE_HTTP_IDLE_MS) break;
      pumpWebServerDuringWait();
    }
    logCaptureF("保号: HTTP GET 完成，下行已收约 %lu 字节(含协议头与URC封装)\n", rx);
    // 仅当下行达到阈值才算真实数据会话：RST/URC 封装等少量噪声(rx 很小)不应被误判为保号成功，
    // 否则会把 kaLastTime 推进整个周期、却没真正动账，导致 SIM 静默失活。
    ok = (rx >= KEEPALIVE_HTTP_MIN_RX);
  } while (false);

  // 5) 关闭 socket
  while (Serial1.available()) Serial1.read();
  Serial1.println("AT+MIPCLOSE=0");
  waitToken("OK", "ERROR", 3000);

  endModemSerialOp();
  return ok;
}

// ---- 网页端待发短信队列 ----
// HTTP 请求只入队并立即返回；真正发送在 loop() 中执行，避免浏览器长等 AT+CMGS 导致超时/崩溃。
struct OutgoingSmsItem {
  String phone;
  String text;
  unsigned long queuedMs;  // 入队时刻：用于限制网页避让(grace)对本条的最长拖延，防被持续轮询饿死
};

static OutgoingSmsItem outSmsQueue[OUT_SMS_QUEUE_MAX];
static int outSmsHead = 0;
static int outSmsCount = 0;

static void clearOutgoingSmsSlot(int i) {
  outSmsQueue[i].phone = "";
  outSmsQueue[i].text = "";
}

int outgoingSmsQueueDepth() {
  return outSmsCount;
}

bool enqueueOutgoingSms(const char* phoneNumber, const char* message) {
  if (outSmsCount >= OUT_SMS_QUEUE_MAX) {
    logCaptureLn("网页待发短信队列已满，拒绝新的发送请求");
    return false;
  }
  int tail = (outSmsHead + outSmsCount) % OUT_SMS_QUEUE_MAX;
  outSmsQueue[tail].phone = phoneNumber;
  outSmsQueue[tail].text = message;
  outSmsQueue[tail].queuedMs = millis();
  outSmsCount++;
  logCaptureF("网页短信已入队，当前待发=%d\n", outSmsCount);
  return true;
}

void processOutgoingSmsQueue() {
  if (outSmsCount == 0) return;
  if (!modemReady) return;       // 模组未就绪时保留队列，待健康检查恢复后再发
  if (smsUrcReceiving()) return; // 正在接收短信时避让，防止冲掉 PDU
  // 网页刚活跃则避让模组AT，但有上限：避让超过 SLOW_WORK_MAX_DEFER_MS 仍强制发出，防 SPA 持续轮询饿死本条。
  if (lastWebRequestMs != 0 && millis() - lastWebRequestMs < SLOW_WORK_WEB_GRACE_MS &&
      millis() - outSmsQueue[outSmsHead].queuedMs < SLOW_WORK_MAX_DEFER_MS) return;

  OutgoingSmsItem it = outSmsQueue[outSmsHead];
  clearOutgoingSmsSlot(outSmsHead);
  outSmsHead = (outSmsHead + 1) % OUT_SMS_QUEUE_MAX;
  outSmsCount--;

  logCaptureLn(String("开始处理网页待发短信，目标: ") + maskPhone(it.phone));
  sendSMS(it.phone.c_str(), it.text.c_str());
}
