#pragma once

#include "core/config.h"

/**
 * 向单个通道推送（阻塞，含重试）。
 * @param title 短信场景为发送者号码；系统通知场景为标题
 * @param is_notice false=短信通知文案，true=系统通知文案
 */
bool push_send_channel(const push_channel_t *channel, const char *title, const char *message,
                       const char *timestamp, bool is_notice);

/** 向所有已启用且配置有效的通道推送。 */
void push_send_all(const char *title, const char *message, const char *timestamp, bool is_notice);
