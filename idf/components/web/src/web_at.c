#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "web_internal.h"

/* AT 应答的取行/取字段小工具：query_* 与模组动作共用，只做字符串处理。 */
const char *web_at_find_line(const char *resp, const char *keyword)
{
    if (!resp) {
        return NULL;
    }
    const char *p = strstr(resp, keyword);
    return p;
}

/* 取关键字行冒号后的内容（到行尾），去掉引号 */
void web_at_take_after(const char *line, char sep, char *out, size_t cap)
{
    out[0] = '\0';
    if (!line) {
        return;
    }
    const char *p = strchr(line, sep);
    if (!p) {
        return;
    }
    p++;
    while (*p == ' ') {
        p++;
    }
    size_t n = 0;
    while (*p && *p != '\r' && *p != '\n' && n + 1 < cap) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    util_trim(out);
}

/* 取 +XXX: 之后、逗号分隔的第 index 个字段 */
bool web_at_take_field(const char *line, char sep, int index, char *out, size_t cap)
{
    out[0] = '\0';
    if (!line) {
        return false;
    }
    const char *p = strchr(line, sep);
    if (!p) {
        return false;
    }
    p++;
    int idx = 0;
    while (*p) {
        const char *end = p;
        while (*end && *end != ',' && *end != '\r' && *end != '\n') {
            end++;
        }
        if (idx == index) {
            size_t n = (size_t)(end - p);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, p, n);
            out[n] = '\0';
            util_trim(out);
            return true;
        }
        if (*end != ',') {
            break;
        }
        p = end + 1;
        idx++;
    }
    return false;
}
