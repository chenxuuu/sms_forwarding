#include "core/sbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SBUF_INIT_CAP 128

void sbuf_init(sbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->oom = false;
}

void sbuf_reset(sbuf_t *sb)
{
    sb->len = 0;
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

void sbuf_free(sbuf_t *sb)
{
    free(sb->data);
    sbuf_init(sb);
}

static bool sbuf_reserve(sbuf_t *sb, size_t extra)
{
    if (sb->oom) {
        return false;
    }
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return true;
    }
    size_t cap = sb->cap ? sb->cap : SBUF_INIT_CAP;
    while (cap < need) {
        cap *= 2;
    }
    char *p = realloc(sb->data, cap);
    if (!p) {
        sb->oom = true;
        return false;
    }
    sb->data = p;
    sb->cap = cap;
    return true;
}

void sbuf_append_len(sbuf_t *sb, const char *s, size_t n)
{
    if (!s || n == 0 || !sbuf_reserve(sb, n)) {
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sbuf_append(sbuf_t *sb, const char *s)
{
    if (s) {
        sbuf_append_len(sb, s, strlen(s));
    }
}

void sbuf_append_char(sbuf_t *sb, char c)
{
    sbuf_append_len(sb, &c, 1);
}

void sbuf_printf(sbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char stack_buf[256];
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof(stack_buf)) {
        sbuf_append_len(sb, stack_buf, (size_t)n);
        return;
    }
    /* 超长：按需分配后重排 */
    if (!sbuf_reserve(sb, (size_t)n)) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
    sb->data[sb->len] = '\0';
}

char *sbuf_detach(sbuf_t *sb)
{
    if (!sb->data) {
        char *empty = calloc(1, 1);
        sbuf_init(sb);
        return empty;
    }
    char *out = sb->data;
    sbuf_init(sb);
    return out;
}

const char *sbuf_cstr(const sbuf_t *sb)
{
    return sb->data ? sb->data : "";
}
