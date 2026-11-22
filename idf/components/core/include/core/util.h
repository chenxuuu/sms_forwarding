/**
 * @file util.h
 * @brief 无状态的通用工具：字符串、编码、时间戳。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sbuf.h"

void util_trim(char *s);
bool util_starts_with(const char *s, const char *prefix);
bool util_equals_ci(const char *a, const char *b);
bool util_equals(const char *a, const char *b);

/** 按格式分配字符串，调用者 free。 */
char *util_strdup_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void util_json_escape(sbuf_t *out, const char *src);
void util_url_encode(sbuf_t *out, const char *src);
void util_base64_encode(sbuf_t *out, const uint8_t *data, size_t len);
void util_base64_encode_str(sbuf_t *out, const char *text);

/** HMAC-SHA256 后做 Base64（钉钉/飞书签名用）。 */
void util_hmac_sha256_b64(sbuf_t *out, const char *key, const char *msg);

/** 当前 UTC 毫秒时间戳。 */
int64_t util_utc_ms(void);

int util_hex_val(char c);
bool util_is_hex_string(const char *s);
