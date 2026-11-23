#pragma once

#include <stdbool.h>

#include <stdint.h>

/** 使用配置中的 SMTP 参数发送纯文本邮件（阻塞，含重试）。 */
bool email_send(const char *subject, const char *body);

/** 用显式参数发送邮件（不读配置），供网页端「测试邮件」使用。 */
bool email_send_direct(const char *host, uint16_t port, const char *user, const char *pass,
                       const char *to, const char *subject, const char *body);
