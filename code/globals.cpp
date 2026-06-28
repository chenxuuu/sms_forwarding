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
int modemRsrp = 255;
String modemOperator = "";
String modemImei = "";
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
