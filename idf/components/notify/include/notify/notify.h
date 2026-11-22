/**
 * @file notify.h
 * @brief 通知分发：邮件 + 多通道 HTTP 推送。
 *
 * 所有网络 IO 都在内部工作任务的上下文中完成，调用方只负责投递，
 * 因此短信接收链路不会被慢速 TLS 握手拖住。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 启动通知任务。 */
void notify_init(void);

/** 推送一条短信（邮件 + 所有已启用通道）。 */
void notify_sms(const char *sender, const char *message, const char *timestamp);

/** 仅发送邮件（启动通知、命令结果等）。 */
void notify_email(const char *subject, const char *body);

/** 等待队列清空（重启前调用），返回是否已清空。 */
bool notify_drain(uint32_t timeout_ms);
