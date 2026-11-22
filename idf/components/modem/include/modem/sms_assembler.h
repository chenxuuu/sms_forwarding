/**
 * @file sms_assembler.h
 * @brief 长短信（Concatenated SMS）合并。
 *
 * 单条短信直接回调；分段短信按 (参考号, 发送者) 聚合，收齐或 30 秒超时后
 * 按顺序拼接并回调（缺失分段以 [缺失分段N] 标注）。
 */
#pragma once

#include "modem/pdu.h"

#define SMS_CONCAT_MAX_PARTS 10
#define SMS_CONCAT_MAX_GROUPS 5
#define SMS_CONCAT_TIMEOUT_MS 30000

typedef void (*sms_complete_cb_t)(const char *sender, const char *text, const char *timestamp);

void sms_assembler_init(sms_complete_cb_t cb);
void sms_assembler_feed(const sms_deliver_t *msg);
void sms_assembler_reset(void);
