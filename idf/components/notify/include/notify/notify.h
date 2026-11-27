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

#include "notify/push.h"

/** 启动通知任务。 */
void notify_init(void);

/** 推送一条短信（邮件 + 所有已启用通道）。 */
void notify_sms(const char *sender, const char *message, const char *timestamp);

/** 仅发送邮件（管理员命令结果等）。 */
void notify_email(const char *subject, const char *body);

/** 系统通知：邮件 + 所有已启用推送通道（启动、IP 变化、网络恢复、配置更新）。 */
void notify_system(const char *title, const char *body);

/**
 * 系统通知，按通道是否依赖系统时间筛选。
 * filter=PUSH_FILTER_TIME_DEPENDENT 时只发钉钉/飞书加签通道，且不发邮件。
 */
void notify_system_ex(const char *title, const char *body, push_time_filter_t filter);

/** 等待队列清空（重启前调用），返回是否已清空。 */
bool notify_drain(uint32_t timeout_ms);
