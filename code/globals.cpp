#include "globals.h"

Config config;
Preferences preferences;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);
bool configValid = false;
bool timeSynced = false;
bool modemReady = false;
bool apMode = false;
volatile bool gInWebRequest = false;
volatile unsigned long smsRecvGuardUntil = 0;
unsigned long lastWebRequestMs = 0;
unsigned long lastPrintTime = 0;
unsigned long lastModemOkMs = 0;
unsigned long smsTotalCount = 0;
time_t lastSmsEpoch = 0;
int modemCsq = 99;
int modemBer = 99;   // AT+CSQ 误码率(0-7，99=未知；LTE 通常恒为 99)
int modemRsrp = 255;
String modemOperator = "";
String modemImei = "";
String modemImsi = "";   // SIM IMSI(AT+CIMI，开机采样一次)
String modemApn = "";    // SIM 默认/已配置 APN(AT+CGDCONT? 读取)
String modemMfr = "";    // 模组制造商(ATI 解析)
String modemModel = "";  // 模组型号(ATI 解析)
String modemFwVer = "";  // 模组固件版本(ATI 解析)
String modemIccid = "";
String modemPhone = "";
String modemCellIp = "";
int modemRsrq = 999;
int modemSinr = 999;
int modemPci = -1;
String modemPlmn = "";
String modemTac = "";
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

SemaphoreHandle_t gLogMux = nullptr;
SemaphoreHandle_t gWorkMux = nullptr;
volatile bool gSlowWorkBusy = false;

void initConcurrency() {
  if (!gLogMux)  gLogMux  = xSemaphoreCreateMutex();
  if (!gWorkMux) gWorkMux = xSemaphoreCreateMutex();
}
