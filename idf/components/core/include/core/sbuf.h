/**
 * @file sbuf.h
 * @brief 极简动态字符串构建器。
 *
 * 替代 Arduino String 的拼接写法，便于在 C 代码中安全地构建 JSON/HTML。
 * 所有 append 操作在内存不足时会置 oom 标志并停止写入，不会崩溃。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} sbuf_t;

void sbuf_init(sbuf_t *sb);
void sbuf_reset(sbuf_t *sb);
void sbuf_free(sbuf_t *sb);

void sbuf_append(sbuf_t *sb, const char *s);
void sbuf_append_len(sbuf_t *sb, const char *s, size_t n);
void sbuf_append_char(sbuf_t *sb, char c);
void sbuf_printf(sbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** 交出内部缓冲区（调用者负责 free），并把 sbuf 复位为空。 */
char *sbuf_detach(sbuf_t *sb);

/** 内部缓冲区指针，永不为 NULL（空串时为 ""）。 */
const char *sbuf_cstr(const sbuf_t *sb);
