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

