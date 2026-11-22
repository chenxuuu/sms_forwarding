#include "core/util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

void util_trim(char *s)
{
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

bool util_starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

bool util_equals(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

bool util_equals_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

char *util_strdup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0) {
        out = NULL;
    }
    va_end(ap);
    return out;
}

void util_json_escape(sbuf_t *out, const char *src)
{
    if (!src) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        switch (*p) {
        case '"':  sbuf_append(out, "\\\""); break;
        case '\\': sbuf_append(out, "\\\\"); break;
        case '\n': sbuf_append(out, "\\n"); break;
        case '\r': sbuf_append(out, "\\r"); break;
        case '\t': sbuf_append(out, "\\t"); break;
        default:
            if (*p < 0x20) {
                sbuf_printf(out, "\\u%04x", *p);
            } else {
                sbuf_append_char(out, (char)*p);
            }
        }
    }
}

void util_url_encode(sbuf_t *out, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!src) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p == ' ') {
            sbuf_append_char(out, '+');
        } else if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            sbuf_append_char(out, (char)*p);
        } else {
            sbuf_append_char(out, '%');
            sbuf_append_char(out, hex[*p >> 4]);
            sbuf_append_char(out, hex[*p & 0x0F]);
        }
    }
}

void util_base64_encode(sbuf_t *out, const uint8_t *data, size_t len)
{
    size_t need = 0;
    if (mbedtls_base64_encode(NULL, 0, &need, data, len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        /* 空输入等边界情况 */
        return;
    }
    char *buf = malloc(need);
    if (!buf) {
        return;
    }
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)buf, need, &written, data, len) == 0) {
        sbuf_append_len(out, buf, written);
    }
    free(buf);
}

void util_base64_encode_str(sbuf_t *out, const char *text)
{
    util_base64_encode(out, (const uint8_t *)text, strlen(text));
}

void util_hmac_sha256_b64(sbuf_t *out, const char *key, const char *msg)
{
    uint8_t digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return;
    }
    if (mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key),
                        (const unsigned char *)msg, strlen(msg), digest) != 0) {
        return;
    }
    util_base64_encode(out, digest, sizeof(digest));
}

int64_t util_utc_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    }
    return 0;
}

int util_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool util_is_hex_string(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    for (; *s; s++) {
        if (util_hex_val(*s) < 0) {
            return false;
        }
    }
    return true;
}
