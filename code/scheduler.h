#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "globals.h"

// 保号定时任务（绝对日期、断电不忘）
void keepAliveTick();           // loop() 周期调用：到期则触发动作
bool keepAliveRunNow();         // 立即执行一次保号动作并更新基准，返回是否成功
bool enqueueKeepAliveRunNow(String& message);  // 手动/定时触发只入队，真实动作由 keepAliveTick 后台执行
void keepAliveResetBaseline();  // 仅把基准日重置为当前时间(不执行动作)
String keepAliveStatusJson();   // 保号状态 JSON（供网页倒计时显示）
void dailyTasksTick();          // 每日定时重启 / 心跳

#endif
