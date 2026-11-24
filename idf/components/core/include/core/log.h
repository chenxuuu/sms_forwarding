/**
 * @file log.h
 * @brief 双通道日志：串口控制台 + Web 端可见的环形缓冲区。
 *
 * 约定（与旧 Arduino 工程保持一致）：
 *  - log_write()  追加内容但不换行，用于拼接一行内的多段信息；
 *  - log_line()   追加内容并提交当前行到环形缓冲区；
 *  - 所有对外日志统一走这里，不要直接 printf，否则 Web 日志页看不到。
 */
#pragma once

#include <stddef.h>

#include "core/sbuf.h"

#define LOG_RING_LINES 200
/* 单行上限 192 字节：200 × 192 = 37.5 KB，比 256 省 12.5 KB 静态内存。
 * 日志页还是 200 行，只有超长的 AT 应答会被截断（本来也超过一屏宽度）。 */
#define LOG_LINE_MAX 192

void log_init(void);

void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** 把环形缓冲区导出为 JSON 字符串数组（自动转义）。 */
void log_append_json(sbuf_t *out);

/** 当前缓存的行数（最多 LOG_RING_LINES 行）。 */
int log_line_count(void);

/** 按时间顺序取第 index 行（0 = 最旧）；行可能被覆盖，属于尽力而为的读取。 */
bool log_copy_line(int index, char *out, size_t cap);
