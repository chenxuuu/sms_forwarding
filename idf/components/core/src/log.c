#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char s_ring[LOG_RING_LINES][LOG_LINE_MAX];
static int s_head;   /* 下一条写入位置 */
static int s_count;  /* 已存行数 */

static char s_pending[LOG_LINE_MAX];
static size_t s_pending_len;

static SemaphoreHandle_t s_mutex;

void log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_head = 0;
    s_count = 0;
    s_pending_len = 0;
    s_pending[0] = '\0';
}

static void log_emit(const char *text)
{
    fputs(text, stdout);
    fflush(stdout);
}

/* 追加到待提交行（调用者需持锁） */
/* 按 UTF-8 字符边界截断，避免把汉字劈成半个导致 JSON 非法 */
static size_t utf8_safe_len(const char *s, size_t max)
{
    if (max == 0) {
        return 0;
    }
    size_t i = max;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
        i--;
    }
    if (i == 0) {
        return max;
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = 1;
    if ((lead & 0xE0) == 0xC0) {
        need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
    }
    return (i - 1 + need <= max) ? max : i - 1;
}

static void pending_append(const char *text)
{
    size_t len = strlen(text);
    if (len > sizeof(s_pending) - 1 - s_pending_len) {
        len = utf8_safe_len(text, sizeof(s_pending) - 1 - s_pending_len);
    }
    memcpy(s_pending + s_pending_len, text, len);
    s_pending_len += len;
    s_pending[s_pending_len] = '\0';
}

static void ring_push(const char *line)
{
    size_t len = strlen(line);
    if (len > LOG_LINE_MAX - 1) {
        len = utf8_safe_len(line, LOG_LINE_MAX - 1);
    }
    memcpy(s_ring[s_head], line, len);
    s_ring[s_head][len] = '\0';
    s_head = (s_head + 1) % LOG_RING_LINES;
    if (s_count < LOG_RING_LINES) {
        s_count++;
    }
}

static void pending_commit(void)
{
    while (s_pending_len > 0 && (s_pending[s_pending_len - 1] == '\n' ||
                                 s_pending[s_pending_len - 1] == '\r' ||
                                 s_pending[s_pending_len - 1] == ' ')) {
        s_pending[--s_pending_len] = '\0';
    }
    if (s_pending_len > 0) {
        ring_push(s_pending);
    }
    s_pending_len = 0;
    s_pending[0] = '\0';
}

void log_write(const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    log_emit(buf);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pending_append(buf);
    xSemaphoreGive(s_mutex);
}

void log_line(const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    log_emit(buf);
    log_emit("\n");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pending_append(buf);
    pending_commit();
    xSemaphoreGive(s_mutex);
}

void log_append_json(sbuf_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int start = (s_count < LOG_RING_LINES) ? 0 : s_head;
    sbuf_append_char(out, '[');
    for (int i = 0; i < s_count; i++) {
        int pos = (start + i) % LOG_RING_LINES;
        if (i > 0) {
            sbuf_append_char(out, ',');
        }
        sbuf_append_char(out, '"');
        for (const char *p = s_ring[pos]; *p; p++) {
            switch (*p) {
            case '"':  sbuf_append(out, "\\\""); break;
            case '\\': sbuf_append(out, "\\\\"); break;
            case '\n': sbuf_append(out, "\\n"); break;
            case '\r': sbuf_append(out, "\\r"); break;
            case '\t': sbuf_append(out, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    sbuf_printf(out, "\\u%04x", (unsigned char)*p);
                } else {
                    sbuf_append_char(out, *p);
                }
            }
        }
        sbuf_append_char(out, '"');
    }
    sbuf_append_char(out, ']');
    xSemaphoreGive(s_mutex);
}
