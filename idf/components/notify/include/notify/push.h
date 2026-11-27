#pragma once

#include <stdbool.h>

#include "core/config.h"

/** 通道是否依赖系统时间：钉钉/飞书配了加签密钥时用当前时间生成签名，
 *  NTP 未同步（时间还停在 1970）会被服务器拒签。 */
bool push_channel_needs_time(const push_channel_t *channel);

/** 按是否依赖系统时间筛选推送通道。 */
typedef enum {
    PUSH_FILTER_ALL = 0,        /* 全部启用通道 */
    PUSH_FILTER_INDEPENDENT,    /* 只发不依赖时间的通道（普通推送） */
    PUSH_FILTER_TIME_DEPENDENT, /* 只发依赖时间的通道（钉钉/飞书加签） */
} push_time_filter_t;

/**
 * 向单个通道推送（阻塞，含重试）。
 * @param title 短信场景为发送者号码；系统通知场景为标题
 * @param is_notice false=短信通知文案，true=系统通知文案
 */
bool push_send_channel(const push_channel_t *channel, const char *title, const char *message,
                       const char *timestamp, bool is_notice);

/** 向筛中的已启用且配置有效的通道推送。 */
void push_send_all_filtered(const char *title, const char *message, const char *timestamp,
                            bool is_notice, push_time_filter_t filter);

/** 向所有已启用且配置有效的通道推送。 */
void push_send_all(const char *title, const char *message, const char *timestamp, bool is_notice);
