#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core/sbuf.h"
#include "esp_http_server.h"

/* ---- 认证 ---- */
bool web_auth_ok(httpd_req_t *req);
/** 未通过认证时自动发送 401，返回 false。 */
bool web_require_auth(httpd_req_t *req);

/* ---- 请求参数 ---- */
/** 读取并 URL 解码查询参数，不存在返回 false。 */
bool web_query_get(httpd_req_t *req, const char *key, char *out, size_t cap);
/** 读取 POST 表单（application/x-www-form-urlencoded），返回 malloc 的原文。 */
char *web_read_body(httpd_req_t *req);
/** 从表单原文中取字段（URL 解码）。 */
bool web_form_get(const char *body, const char *key, char *out, size_t cap);

/* ---- 响应 ---- */
void web_send_json(httpd_req_t *req, bool success, const char *message);
void web_send_html_redirect(httpd_req_t *req, const char *html);
void web_send_page(httpd_req_t *req);

/* ---- 异步处理（长耗时操作不阻塞 httpd 任务） ---- */
typedef struct web_async_ctx web_async_ctx_t;
typedef void (*web_async_fn)(web_async_ctx_t *ctx, sbuf_t *out, const char **content_type);

struct web_async_ctx {
    httpd_req_t *req;
    web_async_fn fn;
    char arg1[512];  /* 可能承载整条 PDU（最长约 350 字符） */
    char arg2[1024]; /* 短信正文 */
    char arg3[16];   /* 附加模式，例如 "json" */
};

/** 把当前请求转为异步处理，工作函数在独立任务中执行。 */
esp_err_t web_spawn_async(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2);

/** 同上，额外带一个模式参数（如 "json" 表示返回 JSON）。 */
esp_err_t web_spawn_async3(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2,
                           const char *arg3);

/**
 * 同上，但可以指定任务栈。
 * C3 空闲堆虽大，最大连续块常常只有 6–8 KB，10 KB 栈会直接分配失败；
 * 只做短操作的处理器（如 eSIM 读卡）用 4 KB 就够。
 */
esp_err_t web_spawn_async3_ex(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2,
                              const char *arg3, size_t stack);

/* ---- 共用小工具（web_json.c / web_at.c） ---- */
/** 输出 {"success":..,"message":".."}，消息按 JSON 转义。 */
void web_json_ok(sbuf_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void web_json_fail(sbuf_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/** 追加一行 <tr><td>name</td><td>value</td></tr>（value 由调用方保证是安全 HTML）。 */
void web_json_table_row(sbuf_t *out, const char *name, const char *value);

/** 在 AT 应答里找包含关键字的行，返回该行起点。 */
const char *web_at_find_line(const char *resp, const char *keyword);
/** 取分隔符之后到行尾的内容（去引号/空格）。 */
void web_at_take_after(const char *line, char sep, char *out, size_t cap);
/** 取分隔符之后第 index 个逗号字段。 */
bool web_at_take_field(const char *line, char sep, int index, char *out, size_t cap);

/* ---- 路由处理器 ---- */
esp_err_t handler_root(httpd_req_t *req);
esp_err_t handler_push_test(httpd_req_t *req);
esp_err_t handler_mail_test(httpd_req_t *req);
esp_err_t handler_tasks(httpd_req_t *req);
esp_err_t handler_save(httpd_req_t *req);
esp_err_t handler_log(httpd_req_t *req);
esp_err_t handler_status(httpd_req_t *req);
esp_err_t handler_config(httpd_req_t *req);
esp_err_t handler_decode(httpd_req_t *req);
esp_err_t handler_selftest(httpd_req_t *req);
esp_err_t handler_favicon(httpd_req_t *req);
esp_err_t handler_sendsms(httpd_req_t *req);
esp_err_t handler_ping(httpd_req_t *req);
esp_err_t handler_keepalive_http(httpd_req_t *req);
esp_err_t handler_query(httpd_req_t *req);
esp_err_t handler_flight(httpd_req_t *req);
esp_err_t handler_at(httpd_req_t *req);
esp_err_t handler_modem(httpd_req_t *req);
esp_err_t handler_wifi(httpd_req_t *req);
esp_err_t handler_system(httpd_req_t *req);
esp_err_t handler_ota(httpd_req_t *req);
esp_err_t handler_ota_upload(httpd_req_t *req);
esp_err_t handler_esim(httpd_req_t *req);
esp_err_t handler_esim_enable(httpd_req_t *req);
esp_err_t handler_esim_apdu(httpd_req_t *req);

