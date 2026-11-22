/**
 * @file pdu.h
 * @brief GSM 03.40 PDU 编解码（自实现，替代 Arduino 的 pdulib）。
 *
 * 支持：
 *  - SMS-SUBMIT 编码：GSM 7bit（纯 ASCII）或 UCS2（含中文/emoji）
 *  - SMS-DELIVER 解码：GSM 7bit / UCS2、字母数字发送方、SCTS 时间戳
 *  - 长短信 UDH 解析（8bit / 16bit 参考号）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PDU_TEXT_MAX 1024
#define PDU_SENDER_MAX 40
#define PDU_TIME_MAX 32

typedef struct {
    char sender[PDU_SENDER_MAX];   /* 国际号码带 '+'，如 +8617613012905 */
    char timestamp[PDU_TIME_MAX];  /* YY/MM/DD HH:MM:SS（模组上报的本地时间） */
    char text[PDU_TEXT_MAX];       /* UTF-8 */
    int dcs;
    int concat_ref;                /* 长短信参考号，0 表示非长短信 */
    int concat_part;               /* 1 起 */
    int concat_total;              /* 0/1 表示单条 */
} sms_deliver_t;

/**
 * 编码 SMS-SUBMIT。
 * @param phone    目标号码，可带或不带 '+'，纯数字
 * @param text      UTF-8 内容
 * @param out_hex   输出十六进制字符串（含 SMSC "00" 前缀）
 * @param out_cap   输出缓冲区大小
 * @return TPDU 字节数（AT+CMGS 参数），失败返回 -1
 */
int pdu_encode_submit(const char *phone, const char *text, char *out_hex, size_t out_cap);

/** 解码一条 SMS-DELIVER 的十六进制 PDU。 */
bool pdu_decode_deliver(const char *hex, sms_deliver_t *out);
