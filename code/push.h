#ifndef PUSH_H
#define PUSH_H

#include "globals.h"

void sendEmailNotification(const char* subject, const char* body);
void sendSMSToServer(const char* sender, const char* message, const char* timestamp);

// 异步推送：消息入队后由独立 FreeRTOS 任务执行，不阻塞主循环/Web 服务
void initNotifyTask();
void queueSmsPush(const char* sender, const char* message, const char* timestamp);
void queueEmailNotification(const char* subject, const char* body);
// 等待推送队列清空（用于重启前确保通知发出），返回是否清空
bool waitNotifyQueueEmpty(uint32_t timeoutMs);
void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp);
String urlEncode(const String& str);
String jsonEscape(const String& str);
String dingtalkSign(const String& secret, int64_t timestamp);
int64_t getUtcMillis();

#endif
