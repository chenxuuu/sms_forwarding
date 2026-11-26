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
#include "ota/ota.h"
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
    /* 查询串长度不固定：/decode 的 PDU、/at 的命令都可能上百上千字节。
     * 以前 query/value 都写死 512，长参数会被静默截断，这里按实际长度从堆上分配。 */
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 4096) {
        return false;
    }
    char *query = malloc(qlen + 1);
    char *value = malloc(qlen + 1);
    bool ok = query && value &&
              httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK &&
              httpd_query_key_value(query, key, value, qlen + 1) == ESP_OK;
    if (ok) {
        url_decode(value, out, cap);
    }
    free(query);
    free(value);
    return ok;
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

    /* 栈余量不足时留条线索，避免以后加大处理逻辑时莫名其妙崩在别的任务里 */
    UBaseType_t stack_left = uxTaskGetStackHighWaterMark(NULL);
    if (stack_left < 512) {
        log_line("异步处理栈只剩 %u 字节，请给该处理器加大栈大小", (unsigned)stack_left);
    }

    httpd_resp_set_type(ctx->req, content_type);
    httpd_resp_set_hdr(ctx->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(ctx->req, sbuf_cstr(&out), out.len);
    httpd_req_async_handler_complete(ctx->req);

    sbuf_free(&out);
    free(ctx);
    vTaskDelete(NULL);
}

/* 异步任务起不来时也要给浏览器一个明确答复，否则前端只会看到连接被断开 */
static void async_spawn_fail(httpd_req_t *req, bool copy_ready, httpd_req_t *copy)
{
    httpd_resp_set_status(copy_ready ? copy : req, "503 Service Unavailable");
    httpd_resp_set_type(copy_ready ? copy : req, "application/json; charset=utf-8");
    httpd_resp_sendstr(copy_ready ? copy : req,
                       "{\"success\":false,\"message\":\"设备内存紧张，请稍后重试\"}");
    if (copy_ready) {
        httpd_req_async_handler_complete(copy);
    }
}

esp_err_t web_spawn_async3_ex(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2,
                              const char *arg3, size_t stack)
{
    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        log_line("异步响应初始化失败: %s", esp_err_to_name(err));
        async_spawn_fail(req, false, NULL);
        return err;
    }
    web_async_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        async_spawn_fail(req, true, copy);
        return ESP_ERR_NO_MEM;
    }
    ctx->req = copy;
    ctx->fn = fn;
    snprintf(ctx->arg1, sizeof(ctx->arg1), "%s", arg1 ? arg1 : "");
    snprintf(ctx->arg2, sizeof(ctx->arg2), "%s", arg2 ? arg2 : "");
    snprintf(ctx->arg3, sizeof(ctx->arg3), "%s", arg3 ? arg3 : "");
    if (xTaskCreate(async_task, "web_async", stack, ctx, 4, NULL) != pdPASS) {
        log_line("异步任务创建失败（栈 %u 字节），当前最大连续堆 %u 字节", (unsigned)stack,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(ctx);
        async_spawn_fail(req, true, copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t web_spawn_async3(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2,
                           const char *arg3)
{
    /* 默认 6 KB：绝大多数处理器只是拼字符串查 AT，1~2 KB 就够。
     * C3 空闲堆里最大的连续块常常只有 6~8 KB，10 KB 栈会让请求直接起不来。
     * 走 TLS 的处理器（推送测试、邮件测试、自检）单独要 8 KB，见各自调用处。 */
    return web_spawn_async3_ex(req, fn, arg1, arg2, arg3, 6144);
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

static uint32_t fnv1a(const void *data, size_t len, uint32_t h)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/** 配置分区哈希：0=账号密码 1=邮件 2=管理员 3=黑名单 4=推送通道 5=选网 6=本机号码 */
static uint32_t hash_section(const app_config_t *c, int section)
{
    uint32_t h = 2166136261u;
    switch (section) {
    case 0:
        h = fnv1a(c->web_user, sizeof(c->web_user), h);
        h = fnv1a(c->web_pass, sizeof(c->web_pass), h);
        break;
    case 1:
        h = fnv1a(&c->mail_enabled, sizeof(c->mail_enabled), h);
        h = fnv1a(c->smtp_server, sizeof(c->smtp_server), h);
        h = fnv1a(&c->smtp_port, sizeof(c->smtp_port), h);
        h = fnv1a(c->smtp_user, sizeof(c->smtp_user), h);
        h = fnv1a(c->smtp_pass, sizeof(c->smtp_pass), h);
        h = fnv1a(c->smtp_to, sizeof(c->smtp_to), h);
        break;
    case 2:
        h = fnv1a(c->admin_phone, sizeof(c->admin_phone), h);
        break;
    case 3:
        h = fnv1a(c->blacklist, sizeof(c->blacklist), h);
        break;
    case 4:
        h = fnv1a(c->channels, (size_t)c->channel_count * sizeof(push_channel_t), h);
        h = fnv1a(&c->channel_count, sizeof(c->channel_count), h);
        break;
    case 6:
        h = fnv1a(c->self_phone, sizeof(c->self_phone), h);
        break;
    case 7:
        h = fnv1a(&c->net_static, sizeof(c->net_static), h);
        h = fnv1a(c->net_ip, sizeof(c->net_ip), h);
        h = fnv1a(c->net_gw, sizeof(c->net_gw), h);
        h = fnv1a(c->net_mask, sizeof(c->net_mask), h);
        h = fnv1a(c->net_dns, sizeof(c->net_dns), h);
        break;
    default:
        h = fnv1a(&c->operator_mode, sizeof(c->operator_mode), h);
        h = fnv1a(&c->operator_act, sizeof(c->operator_act), h);
        h = fnv1a(c->operator_code, sizeof(c->operator_code), h);
        h = fnv1a(c->apn, sizeof(c->apn), h);
        h = fnv1a(c->apn_user, sizeof(c->apn_user), h);
        h = fnv1a(c->apn_pass, sizeof(c->apn_pass), h);
        break;
    }
    return h;
}

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

    uint32_t h_before[8];
    for (int sc = 0; sc < 8; sc++) {
        h_before[sc] = hash_section(cfg, sc);
    }

    if (web_form_get(body, "webUser", value, sizeof(value))) {
        snprintf(cfg->web_user, sizeof(cfg->web_user), "%s",
                 value[0] ? value : DEFAULT_WEB_USER);
    }
    if (web_form_get(body, "webPass", value, sizeof(value))) {
        snprintf(cfg->web_pass, sizeof(cfg->web_pass), "%s",
                 value[0] ? value : DEFAULT_WEB_PASS);
    }
    if (web_form_get(body, "mailEnabled", value, sizeof(value))) {
        cfg->mail_enabled = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
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
    if (web_form_get(body, "selfPhone", value, sizeof(value))) {
        util_trim(value);
        snprintf(cfg->self_phone, sizeof(cfg->self_phone), "%s", value);
    }
    if (web_form_get(body, "numberBlackList", value, sizeof(value))) {
        snprintf(cfg->blacklist, sizeof(cfg->blacklist), "%s", value);
    }

    /* 选网配置：先完整校验再落盘，避免存进半套配置 */
    char op_mode_str[8] = {0}, op_code[16] = {0}, op_act_str[8] = {0};
    bool has_op_mode = web_form_get(body, "operatorMode", op_mode_str, sizeof(op_mode_str));
    bool has_op_code = web_form_get(body, "operatorCode", op_code, sizeof(op_code));
    bool has_op_act = web_form_get(body, "operatorAct", op_act_str, sizeof(op_act_str));
    if (has_op_mode || has_op_code || has_op_act) {
        int mode = has_op_mode ? atoi(op_mode_str) : cfg->operator_mode;
        int act = has_op_act ? atoi(op_act_str) : cfg->operator_act;
        if (has_op_code) {
            util_trim(op_code);
        }
        if (mode != OPERATOR_MODE_AUTO && mode != OPERATOR_MODE_MANUAL) {
            config_unlock();
            free(body);
            web_send_json(req, false, "选网模式无效");
            return ESP_OK;
        }
        if (act != OPERATOR_ACT_2G && act != OPERATOR_ACT_3G && act != OPERATOR_ACT_LTE) {
            config_unlock();
            free(body);
            web_send_json(req, false, "网络类型无效");
            return ESP_OK;
        }
        const char *code = has_op_code ? op_code : cfg->operator_code;
        if (mode == OPERATOR_MODE_MANUAL) {
            size_t clen = strlen(code);
            bool digits = clen >= 5 && clen <= 6;
            for (size_t k = 0; digits && k < clen; k++) {
                if (code[k] < '0' || code[k] > '9') {
                    digits = false;
                }
            }
            if (!digits) {
                config_unlock();
                free(body);
                web_send_json(req, false, "手动选网需填写 5~6 位数字 PLMN（如 46000）");
                return ESP_OK;
            }
        }
        cfg->operator_mode = (uint8_t)mode;
        cfg->operator_act = (uint8_t)act;
        if (has_op_code) {
            snprintf(cfg->operator_code, sizeof(cfg->operator_code), "%s", op_code);
        }
    }

    /* 数据 APN：留空 = 模组自动；手填只允许合法字符，避免带引号拼进 AT 指令。
     * 用户名/密码是可选的（多数卡不需要），模组限制各 16 字符、认证类型固定 PAP。 */
    char apn_val[CFG_MED_LEN] = {0};
    char apn_user_val[24] = {0};
    char apn_pass_val[24] = {0};
    bool has_apn = web_form_get(body, "apn", apn_val, sizeof(apn_val));
    bool has_apn_user = web_form_get(body, "apnUser", apn_user_val, sizeof(apn_user_val));
    bool has_apn_pass = web_form_get(body, "apnPass", apn_pass_val, sizeof(apn_pass_val));
    if (has_apn || has_apn_user || has_apn_pass) {
        util_trim(apn_val);
        util_trim(apn_user_val);
        util_trim(apn_pass_val);
        const char *a = has_apn ? apn_val : cfg->apn;
        const char *u = has_apn_user ? apn_user_val : cfg->apn_user;
        const char *pw = has_apn_pass ? apn_pass_val : cfg->apn_pass;
        bool apn_ok = strlen(a) < sizeof(cfg->apn) &&
                      strlen(u) < sizeof(cfg->apn_user) && strlen(u) <= 16 &&
                      strlen(pw) < sizeof(cfg->apn_pass) && strlen(pw) <= 16;
        for (size_t k = 0; apn_ok && a[k]; k++) {
            char ch = a[k];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '_')) {
                apn_ok = false;
            }
        }
        for (size_t k = 0; apn_ok && u[k]; k++) {
            if (u[k] < 0x20 || u[k] > 0x7e || u[k] == '"' || u[k] == '\\') {
                apn_ok = false;
            }
        }
        for (size_t k = 0; apn_ok && pw[k]; k++) {
            if (pw[k] < 0x20 || pw[k] > 0x7e || pw[k] == '"' || pw[k] == '\\') {
                apn_ok = false;
            }
        }
        if (!apn_ok) {
            config_unlock();
            free(body);
            web_send_json(req, false, "APN 只能含字母数字和 . - _（≤63 字符）；用户名/密码为 ≤16 字符的 ASCII");
            return ESP_OK;
        }
        if (has_apn) {
            snprintf(cfg->apn, sizeof(cfg->apn), "%s", apn_val);
        }
        if (has_apn_user) {
            snprintf(cfg->apn_user, sizeof(cfg->apn_user), "%s", apn_user_val);
        }
        if (has_apn_pass) {
            snprintf(cfg->apn_pass, sizeof(cfg->apn_pass), "%s", apn_pass_val);
        }
    }

    /* 静态 IP：只有「固定 IP」模式才校验；字段保留，方便以后切回来。
     * 保存后需要重启设备或点「重连 WiFi」才生效。 */
    char net_mode[8] = {0};
    if (web_form_get(body, "netMode", net_mode, sizeof(net_mode))) {
        char n_ip[16] = {0}, n_gw[16] = {0}, n_mask[16] = {0}, n_dns[16] = {0};
        bool use_static = strcmp(net_mode, "1") == 0;
        web_form_get(body, "netIp", n_ip, sizeof(n_ip));
        web_form_get(body, "netGw", n_gw, sizeof(n_gw));
        web_form_get(body, "netMask", n_mask, sizeof(n_mask));
        web_form_get(body, "netDns", n_dns, sizeof(n_dns));
        util_trim(n_ip);
        util_trim(n_gw);
        util_trim(n_mask);
        util_trim(n_dns);
        char nerr[96] = {0};
        if (use_static && !wifi_net_config_check(n_ip, n_gw, n_mask, n_dns, nerr, sizeof(nerr))) {
            config_unlock();
            free(body);
            web_send_json(req, false, nerr);
            return ESP_OK;
        }
        cfg->net_static = use_static;
        if (use_static) {
            snprintf(cfg->net_ip, sizeof(cfg->net_ip), "%s", n_ip);
            snprintf(cfg->net_gw, sizeof(cfg->net_gw), "%s", n_gw);
            snprintf(cfg->net_mask, sizeof(cfg->net_mask), "%s", n_mask);
            snprintf(cfg->net_dns, sizeof(cfg->net_dns), "%s", n_dns);
        }
    }

    char cnt_str[8] = {0};
    if (web_form_get(body, "pushCount", cnt_str, sizeof(cnt_str))) {
        int n = atoi(cnt_str);
        if (n != cfg->channel_count && !config_set_channel_count(n)) {
            /* 调整通道数要重新分配数组；失败时不能继续，否则会出现
             * 「提示保存成功、实际通道数没变」的假成功。 */
            config_unlock();
            free(body);
            web_send_json(req, false, "内存不足，无法调整通道数量");
            return ESP_OK;
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

    char json_flag[8] = {0};
    bool want_json = web_query_get(req, "json", json_flag, sizeof(json_flag)) &&
                     strcmp(json_flag, "1") == 0;

    bool ch_creds = h_before[0] != hash_section(cfg, 0);
    bool ch_mail = h_before[1] != hash_section(cfg, 1);
    bool ch_admin = h_before[2] != hash_section(cfg, 2);
    bool ch_black = h_before[3] != hash_section(cfg, 3);
    bool ch_push = h_before[4] != hash_section(cfg, 4);
    bool ch_oper = h_before[5] != hash_section(cfg, 5);
    bool ch_self = h_before[6] != hash_section(cfg, 6);
    bool ch_net = h_before[7] != hash_section(cfg, 7);
    bool changed = ch_creds || ch_mail || ch_admin || ch_black || ch_push || ch_oper || ch_self ||
                   ch_net;

    config_save_locked();
    config_unlock();
    free(body);

    if (want_json) {
        web_send_json(req, true, changed ? "配置已保存" : "配置未变化");
    } else {
        web_send_html_redirect(req, SAVE_OK_HTML);
    }

    if (!changed) {
        log_line("配置未变化，不发送通知");
        return ESP_OK;
    }

    sbuf_t items;
    sbuf_init(&items);
    if (ch_creds) {
        sbuf_append(&items, "\n· 管理账号与密码");
    }
    if (ch_mail) {
        sbuf_append(&items, "\n· 邮件设置");
    }
    if (ch_push) {
        sbuf_printf(&items, "\n· 推送通道（当前 %d 个）", cfg->channel_count);
    }
    if (ch_admin) {
        sbuf_append(&items, "\n· 管理员号码");
    }
    if (ch_black) {
        sbuf_append(&items, "\n· 号码黑名单");
    }
    if (ch_oper) {
        sbuf_append(&items, "\n· 选网配置（重启设备后生效）");
    }
    if (ch_self) {
        sbuf_append(&items, "\n· 本机号码");
    }
    if (ch_net) {
        sbuf_append(&items, "\n· 网络 IP 配置（重启设备后生效）");
    }

    sbuf_t text;
    sbuf_init(&text);
    sbuf_printf(&text, "以下配置已更新：%s\n设备地址: http://%s/", sbuf_cstr(&items), wifi_ip());
    if (ch_creds) {
        sbuf_append(&text, "\n\n管理账号已修改，请使用新账号密码登录。");
    }
    if (ch_oper) {
        sbuf_append(&text, "\n\n选网配置已保存，重启设备后生效。");
    }
    if (ch_net) {
        sbuf_append(&text, "\n\n网络 IP 配置已保存，重启设备或点「重连 WiFi」后生效。");
    }
    notify_system("短信转发器配置已更新", sbuf_cstr(&text));
    log_line("配置已更新：%s", sbuf_cstr(&items));
    sbuf_free(&items);
    sbuf_free(&text);
    return ESP_OK;
}

/**
 * 日志量大会超过最大连续堆块（C3 碎片严重时只有 7-8KB），
 * 所以不再一次性拼 JSON，而是按 2KB 分块流式发送。
 */
typedef struct {
    httpd_req_t *req;
    char buf[2048];
    size_t len;
    bool first;
    bool failed;
} log_chunk_t;

static void log_flush(log_chunk_t *c)
{
    if (!c->failed && c->len > 0) {
        if (httpd_resp_send_chunk(c->req, c->buf, c->len) != ESP_OK) {
            c->failed = true;
        }
    }
    c->len = 0;
}

static void log_putc(log_chunk_t *c, char ch)
{
    if (c->failed) {
        return;
    }
    if (c->len + 1 >= sizeof(c->buf)) {
        log_flush(c);
    }
    if (!c->failed) {
        c->buf[c->len++] = ch;
    }
}

static void log_puts(log_chunk_t *c, const char *s)
{
    while (*s) {
        log_putc(c, *s++);
    }
}

esp_err_t handler_log(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    log_chunk_t c = {.req = req, .first = true};
    log_putc(&c, '[');

    int n = log_line_count();
    for (int i = 0; i < n && !c.failed; i++) {
        char line[LOG_LINE_MAX];
        if (!log_copy_line(i, line, sizeof(line))) {
            break;
        }
        if (!c.first) {
            log_putc(&c, ',');
        }
        log_putc(&c, '"');
        for (const char *p = line; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            switch (ch) {
            case '"': log_puts(&c, "\\\""); break;
            case '\\': log_puts(&c, "\\\\"); break;
            case '\n': log_puts(&c, "\\n"); break;
            case '\r': log_puts(&c, "\\r"); break;
            case '\t': log_puts(&c, "\\t"); break;
            default:
                if (ch < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
                    log_puts(&c, tmp);
                } else {
                    log_putc(&c, (char)ch);
                }
            }
        }
        log_putc(&c, '"');
        c.first = false;
    }
    log_putc(&c, ']');
    if (c.failed) {
        return ESP_OK; /* 客户端提前断开，响应已作废 */
    }
    log_flush(&c);
    httpd_resp_send_chunk(req, NULL, 0);
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
    modem_reg_t reg;
    modem_net_info(&reg);
    sbuf_printf(&out, "\"modem_ready\":%s,\"net_stat\":%d,\"net_csq\":%d,\"net_sim\":%s,",
                modem_ready() ? "true" : "false", reg.stat, reg.csq,
                reg.sim_ready ? "true" : "false");

    app_config_t *cfg = config_lock();
    bool email_ok = config_mail_active(cfg);
    bool mail_enabled = cfg->mail_enabled;
    char self_phone[sizeof(((app_config_t *)0)->self_phone)];
    snprintf(self_phone, sizeof(self_phone), "%s", cfg->self_phone);
    int push_count = 0;
    for (int i = 0; i < cfg->channel_count; i++) {
        if (config_push_channel_valid(&cfg->channels[i])) {
            push_count++;
        }
    }
    config_unlock();

    ota_info_t ota;
    ota_get_info(&ota);

    /* self_phone 是用户手填的，可能带引号；device_name / model 也一样，
     * 直接塞进 JSON 会把整段状态响应弄坏，前端连状态都读不出来。 */
    sbuf_printf(&out, "\"email_ok\":%s,\"mail_enabled\":%s,\"push_count\":%d,\"self_phone\":\"",
                email_ok ? "true" : "false", mail_enabled ? "true" : "false", push_count);
    util_json_escape(&out, self_phone);
    sbuf_append(&out, "\",\"version\":\"");
    util_json_escape(&out, ota.current);
    sbuf_printf(&out, "\",\"ota_state\":%d,\"device_name\":\"", (int)ota.state);
    util_json_escape(&out, host);
    sbuf_append(&out, "\",\"model\":\"");
    util_json_escape(&out, modem_model());
    sbuf_append(&out, "\"}");
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
    sbuf_printf(&out, "\"mail_enabled\":%s,", cfg->mail_enabled ? "true" : "false");
    append_str_field(&out, "smtp_server", cfg->smtp_server, true);
    sbuf_printf(&out, "\"smtp_port\":%u,", (unsigned)cfg->smtp_port);
    append_str_field(&out, "smtp_user", cfg->smtp_user, true);
    append_str_field(&out, "smtp_pass", cfg->smtp_pass, true);
    append_str_field(&out, "smtp_to", cfg->smtp_to, true);
    append_str_field(&out, "admin_phone", cfg->admin_phone, true);
    append_str_field(&out, "self_phone", cfg->self_phone, true);
    append_str_field(&out, "blacklist", cfg->blacklist, true);
    sbuf_printf(&out, "\"operator_mode\":%u,\"operator_act\":%u,",
                (unsigned)cfg->operator_mode, (unsigned)cfg->operator_act);
    append_str_field(&out, "operator_code", cfg->operator_code, true);
    append_str_field(&out, "apn", cfg->apn, true);
    append_str_field(&out, "apn_user", cfg->apn_user, true);
    append_str_field(&out, "apn_pass", cfg->apn_pass, true);
    sbuf_printf(&out, "\"net_static\":%s,", cfg->net_static ? "true" : "false");
    append_str_field(&out, "net_ip", cfg->net_ip, true);
    append_str_field(&out, "net_gw", cfg->net_gw, true);
    append_str_field(&out, "net_mask", cfg->net_mask, true);
    append_str_field(&out, "net_dns", cfg->net_dns, true);
    sbuf_append(&out, "\"channels\":[");
    for (int i = 0; i < cfg->channel_count; i++) {
        const push_channel_t *ch = &cfg->channels[i];
        sbuf_printf(&out, "%s{\"enabled\":%s,\"type\":%u,", i ? "," : "",
                    ch->enabled ? "true" : "false", (unsigned)ch->type);
        append_str_field(&out, "name", ch->name, true);
        append_str_field(&out, "url", ch->url, true);
        append_str_field(&out, "key1", ch->key1, true);
        append_str_field(&out, "key2", ch->key2, true);
        /* 键名必须和前端 / 配置结构体一致（custom_body）：之前这里写成 body，
         * 前端读不到模板内容，每次保存都会被空串覆盖，自定义模板等于存不进去。 */
        append_str_field(&out, "custom_body", ch->custom_body, false);
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
    cfg.max_uri_handlers = 40;

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
    register_uri(s_server, "/keepalive_http", HTTP_GET, handler_keepalive_http);
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
    register_uri(s_server, "/push_test", HTTP_POST, handler_push_test);
    register_uri(s_server, "/mail_test", HTTP_POST, handler_mail_test);
    register_uri(s_server, "/tasks", HTTP_GET, handler_tasks);
    register_uri(s_server, "/tasks", HTTP_POST, handler_tasks);
    register_uri(s_server, "/modem", HTTP_GET, handler_modem);
    register_uri(s_server, "/modem", HTTP_POST, handler_modem);
    register_uri(s_server, "/wifi", HTTP_GET, handler_wifi);
    register_uri(s_server, "/wifi", HTTP_POST, handler_wifi);
    register_uri(s_server, "/system", HTTP_GET, handler_system);
    register_uri(s_server, "/system", HTTP_POST, handler_system);
    register_uri(s_server, "/ota", HTTP_GET, handler_ota);
    register_uri(s_server, "/ota", HTTP_POST, handler_ota_upload);
    register_uri(s_server, "/esim", HTTP_GET, handler_esim);
    register_uri(s_server, "/esim", HTTP_POST, handler_esim_enable);
    register_uri(s_server, "/esim/apdu", HTTP_POST, handler_esim_apdu);

    log_line("HTTP 服务器已启动: http://%s/", wifi_ip());
    return ESP_OK;
}

esp_err_t web_spawn_async(httpd_req_t *req, web_async_fn fn, const char *arg1, const char *arg2)
{
    return web_spawn_async3(req, fn, arg1, arg2, "");
}
