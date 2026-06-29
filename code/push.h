#ifndef PUSH_H
#define PUSH_H

#include "globals.h"

bool sendEmailNotification(const char* subject, const char* body);
bool enqueueEmailNotification(const char* subject, const char* body);  // 入队发送邮件，避免调用方同步阻塞
bool sendSMSToServer(const char* sender, const char* message, const char* timestamp, unsigned chMask = 0xFF);  // 返回是否确有通道被发送/入队
bool sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp);
// urlEncode / jsonEscape 现由 sms_logic.h 提供（inline，设备与主机测试共用）
String dingtalkSign(const String& secret, int64_t timestamp);
int64_t getUtcMillis();

// 有界推送队列：初次发送逐通道排队；WiFi 断网或通道失败时恢复后指数退避重试
bool processRetryQueue();  // worker 周期调用；返回本轮是否处理了一项到期推送
int retryQueueDepth();     // 当前待推送/待重试条数(用于 /status)
bool processEmailQueue();  // worker 周期调用；返回本轮是否处理了一封邮件
int emailQueueDepth();     // 当前待发送邮件条数(用于 /status/重启空闲判断)

// 测试推送后台队列：/testpush 只入队/查状态，真实网络发送在 worker 中执行
bool enqueueTestPush(uint8_t ch, String& message);
bool processTestPushQueue();
String testPushStatusJson(uint8_t ch);

// 接收/转发解耦：已收短信入队，loop() 异步逐条转发(慢推送不阻塞 URC 接收)
void enqueueForward(const char* sender, const char* text, const char* timestamp, uint32_t inboxId);
void processForwardQueue();  // loop() 周期调用，拆分少量转发任务入 worker 队列
int forwardQueueDepth();

#endif
