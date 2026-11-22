#pragma once

#include "core/config.h"

/** 向单个通道推送一条短信（阻塞，含重试）。 */
bool push_send_channel(const push_channel_t *channel, const char *sender,
                       const char *message, const char *timestamp);

/** 向所有已启用且配置有效的通道推送。 */
void push_send_all(const char *sender, const char *message, const char *timestamp);
