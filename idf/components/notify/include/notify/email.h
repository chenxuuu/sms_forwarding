#pragma once

#include <stdbool.h>

/** 使用配置中的 SMTP 参数发送纯文本邮件（阻塞，含重试）。 */
bool email_send(const char *subject, const char *body);
