#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/sbuf.h"
#include "core/util.h"
#include "web_internal.h"

/* web 接口共用的 JSON 响应工具：统一 success/message 结构，消息按 JSON 转义。 */
/* ---- JSON 小工具 ---- */

static void json_result(sbuf_t *out, bool ok, const char *fmt, va_list ap)
{
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    sbuf_printf(out, "{\"success\":%s,\"message\":\"", ok ? "true" : "false");
    util_json_escape(out, msg);
    sbuf_append(out, "\"}");
}

void web_json_ok(sbuf_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    json_result(out, true, fmt, ap);
    va_end(ap);
}

void web_json_fail(sbuf_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    json_result(out, false, fmt, ap);
    va_end(ap);
}

void web_json_table_row(sbuf_t *out, const char *name, const char *value)
{
    sbuf_printf(out, "<tr><td>%s</td><td>", name);
    sbuf_append(out, value);
    sbuf_append(out, "</td></tr>");
}

