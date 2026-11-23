#include "web/web_server.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/config.h"
#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "modem/modem.h"
#include "net/mdns.h"
#include "net/wifi.h"
#include "notify/notify.h"
#include "web/web_ui.h"
#include "web_internal.h"

static httpd_handle_t s_server;

/* ---- 认证 ---- */
bool web_auth_ok(httpd_req_t *req)
{
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(hdr, "Basic ", 6) != 0) {
        return false;
    }
    unsigned char decoded[128];
    size_t out_len = 0;
    size_t in_len = strlen(hdr + 6);
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                              (const unsigned char *)hdr + 6, in_len) != 0) {
        return false;
    }
    decoded[out_len] = '\0';
    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        return false;
    }
    *colon = '\0';
    const char *pass = colon + 1;

    app_config_t *cfg = config_lock();
    bool ok = strcmp(cfg->web_user, (const char *)decoded) == 0 &&
              strcmp(cfg->web_pass, pass) == 0;
    config_unlock();
    return ok;
}

bool web_require_auth(httpd_req_t *req)
{
    if (web_auth_ok(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "需要管理员账号密码");
    return false;
}

/* ---- 参数解析 ---- */
static void url_decode(const char *src, char *out, size_t cap)
{
    size_t n = 0;
    while (*src && n + 1 < cap) {
        if (*src == '+') {
            out[n++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            int hi = util_hex_val(src[1]);
            int lo = util_hex_val(src[2]);
            if (hi >= 0 && lo >= 0) {
                out[n++] = (char)((hi << 4) | lo);
                src += 3;
            } else {
                out[n++] = *src++;
            }
        } else {
            out[n++] = *src++;
        }
    }
    out[n] = '\0';
}

bool web_query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char query[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[512];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    url_decode(value, out, cap);
    return true;
}

char *web_read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 40960) {
        return NULL;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) {
        return NULL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

bool web_form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (!body) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        if ((p == body || p[-1] == '&') && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            const char *end = strchr(val, '&');
            size_t len = end ? (size_t)(end - val) : strlen(val);
            char tmp[1024];
            if (len >= sizeof(tmp)) {
                len = sizeof(tmp) - 1;
            }
            memcpy(tmp, val, len);
            tmp[len] = '\0';
            url_decode(tmp, out, cap);
            return true;
        }
        p = strchr(p, '&');
        if (!p) {
            break;
        }
        p++;
    }
    return false;
}

/* ---- 响应 ---- */
void web_send_json(httpd_req_t *req, bool success, const char *message)
{
    sbuf_t out;
    sbuf_init(&out);
    sbuf_printf(&out, "{\"success\":%s,\"message\":\"", success ? "true" : "false");
    util_json_escape(&out, message ? message : "");
    sbuf_append(&out, "\"}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
}

void web_send_html_redirect(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_sendstr(req, html);
}

static esp_err_t page_sink(void *user, const char *data, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)user, data, len);
}

void web_send_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    web_ui_render_to(page_sink, req);
    httpd_resp_send_chunk(req, NULL, 0); /* 结束分块响应 */
}

/* ---- 异步任务 ---- */
static void async_task(void *arg)
{
    web_async_ctx_t *ctx = (web_async_ctx_t *)arg;
    sbuf_t out;
    sbuf_init(&out);
    const char *content_type = "application/json; charset=utf-8";
    ctx->fn(ctx, &out, &content_type);

    httpd_resp_set_type(ctx->req, content_type);
    httpd_resp_set_hdr(ctx->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(ctx->req, sbuf_cstr(&out), out.len);
    httpd_req_async_handler_complete(ctx->req);

    sbuf_free(&out);
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t web_spawn_async3(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2,
                           const char *arg3)
{
    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        log_line("异步响应初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    web_async_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_req_async_handler_complete(copy);
        return ESP_ERR_NO_MEM;
    }
    ctx->req = copy;
    ctx->fn = fn;
    snprintf(ctx->arg1, sizeof(ctx->arg1), "%s", arg1 ? arg1 : "");
    snprintf(ctx->arg2, sizeof(ctx->arg2), "%s", arg2 ? arg2 : "");
    snprintf(ctx->arg3, sizeof(ctx->arg3), "%s", arg3 ? arg3 : "");
    if (xTaskCreate(async_task, "web_async", 10240, ctx, 4, NULL) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
/* ---- 页面与配置 ---- */
esp_err_t handler_root(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    web_send_page(req);
    return ESP_OK;
}

esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const char SAVE_OK_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta http-equiv=\"refresh\" content=\"3;url=/\"><title>保存成功</title>"
    "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:100px;background:#f5f5f5}"
    ".success{background:#4CAF50;color:#fff;padding:20px;border-radius:10px;display:inline-block}"
    "</style></head><body><div class=\"success\"><h2>✅ 配置保存成功！</h2>"
    "<p>3秒后返回配置页面...</p>"
    "<p>如果修改了账号密码，请使用新的账号密码登录</p></div></body></html>";

esp_err_t handler_save(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }

    char *body = web_read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }

    char value[1024];
    app_config_t *cfg = config_lock();

    if (web_form_get(body, "webUser", value, sizeof(value))) {
        snprintf(cfg->web_user, sizeof(cfg->web_user), "%s",
                 value[0] ? value : DEFAULT_WEB_USER);
    }
    if (web_form_get(body, "webPass", value, sizeof(value))) {
        snprintf(cfg->web_pass, sizeof(cfg->web_pass), "%s",
                 value[0] ? value : DEFAULT_WEB_PASS);
    }
    if (web_form_get(body, "smtpServer", value, sizeof(value))) {
        snprintf(cfg->smtp_server, sizeof(cfg->smtp_server), "%s", value);
    }
    if (web_form_get(body, "smtpPort", value, sizeof(value))) {
        int port = atoi(value);
        cfg->smtp_port = port > 0 ? (uint16_t)port : 465;
    }
    if (web_form_get(body, "smtpUser", value, sizeof(value))) {
        snprintf(cfg->smtp_user, sizeof(cfg->smtp_user), "%s", value);
    }
    if (web_form_get(body, "smtpPass", value, sizeof(value))) {
        snprintf(cfg->smtp_pass, sizeof(cfg->smtp_pass), "%s", value);
    }
    if (web_form_get(body, "smtpSendTo", value, sizeof(value))) {
        snprintf(cfg->smtp_to, sizeof(cfg->smtp_to), "%s", value);
    }
    if (web_form_get(body, "adminPhone", value, sizeof(value))) {
        snprintf(cfg->admin_phone, sizeof(cfg->admin_phone), "%s", value);
    }
    if (web_form_get(body, "numberBlackList", value, sizeof(value))) {
        snprintf(cfg->blacklist, sizeof(cfg->blacklist), "%s", value);
    }

    char cnt_str[8] = {0};
    if (web_form_get(body, "pushCount", cnt_str, sizeof(cnt_str))) {
        int n = atoi(cnt_str);
        if (n != cfg->channel_count) {
            config_set_channel_count(n);
        }
    }
    for (int i = 0; i < cfg->channel_count; i++) {
        char en_key[24], type_key[24], url_key[24], name_key[24], k1_key[24], k2_key[24],
            body_key[24];
        snprintf(en_key, sizeof(en_key), "push%den", i);
        snprintf(type_key, sizeof(type_key), "push%dtype", i);
        snprintf(url_key, sizeof(url_key), "push%durl", i);
        snprintf(name_key, sizeof(name_key), "push%dname", i);
        snprintf(k1_key, sizeof(k1_key), "push%dkey1", i);
        snprintf(k2_key, sizeof(k2_key), "push%dkey2", i);
        snprintf(body_key, sizeof(body_key), "push%dbody", i);

        bool present = web_form_get(body, en_key, value, sizeof(value)) ||
                       web_form_get(body, type_key, value, sizeof(value)) ||
                       web_form_get(body, url_key, value, sizeof(value)) ||
                       web_form_get(body, name_key, value, sizeof(value)) ||
                       web_form_get(body, k1_key, value, sizeof(value)) ||
                       web_form_get(body, k2_key, value, sizeof(value)) ||
                       web_form_get(body, body_key, value, sizeof(value));
        if (!present) {
            continue;
        }

        push_channel_t *ch = &cfg->channels[i];
        ch->enabled = web_form_get(body, en_key, value, sizeof(value)) &&
                      strcmp(value, "on") == 0;
        if (web_form_get(body, type_key, value, sizeof(value))) {
            int type = atoi(value);
            if (type < PUSH_TYPE_NONE || type > PUSH_TYPE_TELEGRAM) {
                type = PUSH_TYPE_POST_JSON;
            }
            ch->type = (uint8_t)type;
        }
        if (web_form_get(body, url_key, value, sizeof(value))) {
            snprintf(ch->url, sizeof(ch->url), "%s", value);
        }
        if (web_form_get(body, name_key, value, sizeof(value)) && value[0]) {
            snprintf(ch->name, sizeof(ch->name), "%s", value);
        } else {
            snprintf(ch->name, sizeof(ch->name), "通道%d", i + 1);
        }
        if (web_form_get(body, k1_key, value, sizeof(value))) {
            snprintf(ch->key1, sizeof(ch->key1), "%s", value);
        }
        if (web_form_get(body, k2_key, value, sizeof(value))) {
            snprintf(ch->key2, sizeof(ch->key2), "%s", value);
        }
        if (web_form_get(body, body_key, value, sizeof(value))) {
            snprintf(ch->custom_body, sizeof(ch->custom_body), "%s", value);
        }
    }

    config_save_locked();
    config_unlock();
    free(body);

    char json_flag[8] = {0};
    bool want_json = web_query_get(req, "json", json_flag, sizeof(json_flag)) &&
                     strcmp(json_flag, "1") == 0;
    if (want_json) {
        web_send_json(req, true, "配置已保存");
    } else {
        web_send_html_redirect(req, SAVE_OK_HTML);
    }

    if (config_any_valid()) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s/", wifi_ip());
        notify_email("短信转发器配置已更新", "设备配置已更新，如修改账号密码请使用新密码登录。");
        log_line("配置有效，新的设备地址: %s", url);
    }
    return ESP_OK;
}

esp_err_t handler_log(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    sbuf_t out;
    sbuf_init(&out);
    log_append_json(&out);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ESP_OK;
}

/* ---- 实时状态（前端轮询，不碰 AT）---- */
esp_err_t handler_status(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    wifi_info_t info;
    wifi_get_info(&info);

    char host[33];
    net_device_name(host, sizeof(host));

    sbuf_t out;
    sbuf_init(&out);
    sbuf_append(&out, "{\"ip\":\"");
    util_json_escape(&out, wifi_ip());
    sbuf_append(&out, "\",\"ssid\":\"");
    util_json_escape(&out, info.ssid);
    sbuf_printf(&out, "\",\"rssi\":%d,\"heap_kb\":%u,\"uptime_s\":%lld,", info.rssi,
                (unsigned)(esp_get_free_heap_size() / 1024),
                (long long)(esp_timer_get_time() / 1000000));
    sbuf_printf(&out, "\"modem_ready\":%s,", modem_ready() ? "true" : "false");

    app_config_t *cfg = config_lock();
    bool email_ok = config_email_valid(cfg);
    int push_count = 0;
    for (int i = 0; i < cfg->channel_count; i++) {
        if (config_push_channel_valid(&cfg->channels[i])) {
            push_count++;
        }
    }
    config_unlock();

    sbuf_printf(&out, "\"email_ok\":%s,\"push_count\":%d,\"device_name\":\"%s\",\"model\":\"%s\"}",
                email_ok ? "true" : "false", push_count, host, modem_model());
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ESP_OK;
}

/* ---- 配置快照（前端渲染表单）---- */
static void append_str_field(sbuf_t *out, const char *key, const char *value, bool comma)
{
    sbuf_printf(out, "\"%s\":\"", key);
    util_json_escape(out, value ? value : "");
    sbuf_append(out, comma ? "\"," : "\"");
}

esp_err_t handler_config(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    sbuf_t out;
    sbuf_init(&out);
    app_config_t *cfg = config_lock();

    sbuf_append(&out, "{");
    append_str_field(&out, "web_user", cfg->web_user, true);
    append_str_field(&out, "web_pass", cfg->web_pass, true);
    append_str_field(&out, "smtp_server", cfg->smtp_server, true);
    sbuf_printf(&out, "\"smtp_port\":%u,", (unsigned)cfg->smtp_port);
    append_str_field(&out, "smtp_user", cfg->smtp_user, true);
    append_str_field(&out, "smtp_pass", cfg->smtp_pass, true);
    append_str_field(&out, "smtp_to", cfg->smtp_to, true);
    append_str_field(&out, "admin_phone", cfg->admin_phone, true);
    append_str_field(&out, "blacklist", cfg->blacklist, true);
    sbuf_append(&out, "\"channels\":[");
    for (int i = 0; i < cfg->channel_count; i++) {
        const push_channel_t *ch = &cfg->channels[i];
        sbuf_printf(&out, "%s{\"enabled\":%s,\"type\":%u,", i ? "," : "",
                    ch->enabled ? "true" : "false", (unsigned)ch->type);
        append_str_field(&out, "name", ch->name, true);
        append_str_field(&out, "url", ch->url, true);
        append_str_field(&out, "key1", ch->key1, true);
        append_str_field(&out, "key2", ch->key2, true);
        append_str_field(&out, "body", ch->custom_body, false);
        sbuf_append(&out, "}");
    }
    sbuf_append(&out, "]}");
    config_unlock();

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ESP_OK;
}

/* ---- 启动 ---- */
static esp_err_t register_uri(httpd_handle_t server, const char *uri, httpd_method_t method,
                              esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t item = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &item);
    if (err != ESP_OK) {
        log_line("注册路由失败 %s: %s", uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;
    cfg.task_priority = 5;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.max_uri_handlers = 32;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        log_line("HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }

    register_uri(s_server, "/", HTTP_GET, handler_root);
    register_uri(s_server, "/tools", HTTP_GET, handler_root);
    register_uri(s_server, "/sms", HTTP_GET, handler_root);
    register_uri(s_server, "/favicon.ico", HTTP_GET, handler_favicon);
    register_uri(s_server, "/save", HTTP_POST, handler_save);
    register_uri(s_server, "/sendsms", HTTP_POST, handler_sendsms);
    register_uri(s_server, "/ping", HTTP_POST, handler_ping);
    register_uri(s_server, "/query", HTTP_GET, handler_query);
    register_uri(s_server, "/query", HTTP_POST, handler_query);
    register_uri(s_server, "/flight", HTTP_GET, handler_flight);
    register_uri(s_server, "/flight", HTTP_POST, handler_flight);
    register_uri(s_server, "/at", HTTP_GET, handler_at);
    register_uri(s_server, "/at", HTTP_POST, handler_at);
    register_uri(s_server, "/log", HTTP_GET, handler_log);
    register_uri(s_server, "/decode", HTTP_GET, handler_decode);
    register_uri(s_server, "/status", HTTP_GET, handler_status);
    register_uri(s_server, "/config", HTTP_GET, handler_config);
    register_uri(s_server, "/selftest", HTTP_GET, handler_selftest);
    register_uri(s_server, "/modem", HTTP_GET, handler_modem);
    register_uri(s_server, "/modem", HTTP_POST, handler_modem);
    register_uri(s_server, "/wifi", HTTP_GET, handler_wifi);
    register_uri(s_server, "/wifi", HTTP_POST, handler_wifi);
    register_uri(s_server, "/system", HTTP_GET, handler_system);
    register_uri(s_server, "/system", HTTP_POST, handler_system);

    log_line("HTTP 服务器已启动: http://%s/", wifi_ip());
    return ESP_OK;
}

esp_err_t web_spawn_async(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2)
{
    return web_spawn_async3(req, fn, arg1, arg2, "");
}
