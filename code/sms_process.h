#ifndef SMS_PROCESS_H
#define SMS_PROCESS_H

#include "globals.h"

void initConcatBuffer();
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts);
String assembleConcatSms(int slot);
void clearConcatSlot(int slot);
void checkConcatTimeout();
bool concatBufferIdle();  // 是否无半成品长短信(供低堆有序重启判断)
void backfillStoredSms(bool announce);  // 轮询/补收 SIM 暂存短信(转发+按索引删除)
void smsReceiveWatchdogTick();           // 接收看门狗：兜底轮询 + 重申 CNMI(修复只能发不能收)
String readSerialLine(HardwareSerial& port);
bool isHexString(const String& str);
bool smsUrcReceiving();                   // 当前是否处在 +CMT/PDU 接收窗口或已有半行 URC
void drainPendingSmsUrc(unsigned long maxWaitMs = 3000);  // AT 命令前优先消化待处理短信 URC
int processSmsUrcText(const String& text); // 从 AT 响应文本中提取并处理混入的 +CMT/PDU
bool isInNumberBlackList(const char* sender);
bool isAdmin(const char* sender);
void processAdminCommand(const char* sender, const char* text);
void processSmsContent(const char* sender, const char* text, const char* timestamp);
void checkSerial1URC();

#endif
