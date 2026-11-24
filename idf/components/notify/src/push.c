#include "notify/push.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/config.h"
#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    sbuf_t url;
    sbuf_t body;
    const char *content_type;
    bool is_get;
} push_request_t;

static void request_init(push_request_t *req)
{
    sbuf_init(&req->url);
    sbuf_init(&req->body);
    req->content_type = "application/json";
    req->is_get = false;
}

static void request_free(push_request_t *req)
{
    sbuf_free(&req->url);
    sbuf_free(&req->body);
}

/* 判断 JSON 里某个布尔字段是否为 true（容忍 ": true" 这种带空格的写法） */
static bool json_bool_true(const char *resp, const char *key)
{
    size_t klen = strlen(key);
    const char *p = resp;
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + klen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
            q++;
        }
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                q++;
            }
            return strncmp(q, "true", 4) == 0;
        }
        p += klen;
    }
    return false;
}

/* 部分平台 HTTP 200 但业务失败，需要检查响应体 */
static bool body_is_success(const push_channel_t *ch, const char *resp)
{
    switch (ch->type) {
    case PUSH_TYPE_DINGTALK:
        return strstr(resp, "\"errcode\":0") != NULL;
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_SERVERCHAN:
        return strstr(resp, "\"code\":0") != NULL;
    case PUSH_TYPE_PUSHPLUS:
        return strstr(resp, "\"code\":200") != NULL;
    case PUSH_TYPE_TELEGRAM:
        return json_bool_true(resp, "\"ok\"");
    default:
        return true;
    }
}

typedef struct {
    char buf[512];
    size_t len;
} http_body_t;

/* esp_http_client_perform() 会自己把响应体读掉，不留给 esp_http_client_read_response()。
 * 所以在事件回调里顺手存一份，用来做业务成功判断。 */
static esp_err_t http_on_data(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        http_body_t *body = (http_body_t *)evt->user_data;
        if (evt->data_len > 0 && body->len < sizeof(body->buf) - 1) {
            size_t room = sizeof(body->buf) - 1 - body->len;
            size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
            memcpy(body->buf + body->len, evt->data, n);
            body->len += n;
            body->buf[body->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool execute_once(const push_channel_t *ch, const push_request_t *req,
                         const char *name, int *status_out)
{
    http_body_t acc = {0};
    esp_http_client_config_t cfg = {
        .url = sbuf_cstr(&req->url),
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_on_data,
        .user_data = &acc,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        log_line("[%s] HTTP 客户端创建失败", name);
        return false;
    }

    if (req->is_get) {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", req->content_type);
        esp_http_client_set_post_field(client, sbuf_cstr(&req->body), (int)req->body.len);
    }

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        log_line("[%s] HTTP 请求失败: %s（errno=%d，可用堆=%u KB）", name, esp_err_to_name(err),
                 esp_http_client_get_errno(client), (unsigned)(esp_get_free_heap_size() / 1024));
        *status_out = 0;
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    *status_out = status;
    log_line("[%s] 响应码: %d", name, status);

    const char *resp = acc.buf;
    if (acc.len > 0) {
        log_line("[%s] 响应: %s", name, resp);
    } else {
        log_line("[%s] 响应体为空", name);
    }

    if (status >= 200 && status < 300) {
        ok = body_is_success(ch, resp);
        if (!ok) {
            log_line("[%s] 平台返回业务失败", name);
        }
    }
    esp_http_client_cleanup(client);

    if (ok) {
        log_line("[%s] 推送成功", name);
    }
    return ok;
}

/**
 * 统一文案：
 *  - 短信：📱短信通知 / 发送者 / 内容 / 时间
 *  - 系统通知：🔔 标题 / 正文 / 时间
 * JSON 需要字面 \n（两个字符），表单编码需要真实换行，所以准备两份。
 */
static void build_texts(const char *title, const char *message, const char *timestamp,
                        const char *self_phone, bool is_notice, sbuf_t *json_safe, sbuf_t *plain)
{
    sbuf_t s_title, s_msg, s_time;
    sbuf_init(&s_title);
    sbuf_init(&s_msg);
    sbuf_init(&s_time);
    util_json_escape(&s_title, title);
    util_json_escape(&s_msg, message);
    util_json_escape(&s_time, timestamp);

    if (is_notice) {
        sbuf_printf(json_safe, "🔔 %s", sbuf_cstr(&s_title));
        sbuf_printf(plain, "%s", title);
        if (message[0]) {
            sbuf_append(json_safe, "\\n");
            sbuf_printf(json_safe, "%s", sbuf_cstr(&s_msg));
            sbuf_append(plain, "\n");
            sbuf_append(plain, message);
        }
    } else {
        sbuf_printf(json_safe, "📱短信通知\\n发送者: %s\\n内容: %s", sbuf_cstr(&s_title),
                    sbuf_cstr(&s_msg));
        sbuf_printf(plain, "发送者: %s\n内容: %s", title, message);
    }
    if (timestamp[0]) {
        sbuf_append(json_safe, "\\n时间: ");
        sbuf_printf(json_safe, "%s", sbuf_cstr(&s_time));
        sbuf_printf(plain, "\n时间: %s", timestamp);
    }

    /* 本机号码（SIM 卡号码）统一追加在末尾，所有文本型通道自动带上 */
    if (self_phone && self_phone[0]) {
        sbuf_t s_phone;
        sbuf_init(&s_phone);
        util_json_escape(&s_phone, self_phone);
        sbuf_printf(json_safe, "\\n本机号码: %s", sbuf_cstr(&s_phone));
        sbuf_printf(plain, "\n本机号码: %s", self_phone);
        sbuf_free(&s_phone);
    }

    sbuf_free(&s_title);
    sbuf_free(&s_msg);
    sbuf_free(&s_time);
}

static void build_request(const push_channel_t *ch, const char *title, const char *message,
                          const char *timestamp, bool is_notice, push_request_t *req)
{
    request_init(req);

    sbuf_t s_title, s_msg, s_time, s_phone, content, plain;
    sbuf_init(&s_title);
    sbuf_init(&s_msg);
    sbuf_init(&s_time);
    sbuf_init(&s_phone);
    sbuf_init(&content);
    sbuf_init(&plain);
    util_json_escape(&s_title, title ? title : "");
    util_json_escape(&s_msg, message ? message : "");
    util_json_escape(&s_time, timestamp ? timestamp : "");

    /* 本机号码：来自配置（可由用户手填或启动时自动获取） */
    char self_phone[sizeof(((app_config_t *)0)->self_phone)];
    app_config_t *cfg = config_lock();
    snprintf(self_phone, sizeof(self_phone), "%s", cfg->self_phone);
    config_unlock();
    util_json_escape(&s_phone, self_phone);

    build_texts(title ? title : "", message ? message : "", timestamp ? timestamp : "", self_phone,
                is_notice, &content, &plain);

    switch (ch->type) {
    case PUSH_TYPE_POST_JSON:
        sbuf_printf(&req->url, "%s", ch->url);
        sbuf_printf(&req->body,
                    "{\"sender\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%s\","
                    "\"self_phone\":\"%s\"}",
                    sbuf_cstr(&s_title), sbuf_cstr(&s_msg), sbuf_cstr(&s_time),
                    sbuf_cstr(&s_phone));
        break;

    case PUSH_TYPE_BARK:
        sbuf_printf(&req->url, "%s", ch->url);
        sbuf_printf(&req->body, "{\"title\":\"%s\",\"body\":\"%s", sbuf_cstr(&s_title),
                    sbuf_cstr(&s_msg));
        if (self_phone[0]) {
            sbuf_printf(&req->body, "\\n本机号码: %s", sbuf_cstr(&s_phone));
        }
        sbuf_append(&req->body, "\"}");
        break;

    case PUSH_TYPE_GET:
        sbuf_printf(&req->url, "%s%c", ch->url, strchr(ch->url, '?') ? '&' : '?');
        sbuf_append(&req->url, "sender=");
        util_url_encode(&req->url, title ? title : "");
        sbuf_append(&req->url, "&message=");
        util_url_encode(&req->url, message ? message : "");
        sbuf_append(&req->url, "&timestamp=");
        util_url_encode(&req->url, timestamp ? timestamp : "");
        sbuf_append(&req->url, "&self_phone=");
        util_url_encode(&req->url, self_phone);
        req->is_get = true;
        break;

    case PUSH_TYPE_DINGTALK: {
        sbuf_append(&req->url, ch->url);
        if (ch->key1[0]) {
            int64_t ts = util_utc_ms();
            sbuf_t sign_to_sign, sign;
            sbuf_init(&sign_to_sign);
            sbuf_init(&sign);
            sbuf_printf(&sign_to_sign, "%lld\n%s", (long long)ts, ch->key1);
            util_hmac_sha256_b64(&sign, ch->key1, sbuf_cstr(&sign_to_sign));
            sbuf_append(&req->url, strchr(ch->url, '?') ? "&" : "?");
            sbuf_printf(&req->url, "timestamp=%lld&sign=", (long long)ts);
            util_url_encode(&req->url, sbuf_cstr(&sign));
            sbuf_free(&sign_to_sign);
            sbuf_free(&sign);
        }
        sbuf_printf(&req->body, "{\"msgtype\":\"text\",\"text\":{\"content\":\"%s\"}}",
                    sbuf_cstr(&content));
        break;
    }

    case PUSH_TYPE_PUSHPLUS: {
        sbuf_printf(&req->url, "%s", ch->url[0] ? ch->url : "http://www.pushplus.plus/send");
        const char *channel = "wechat";
        if (ch->key2[0] && (strcmp(ch->key2, "wechat") == 0 ||
                            strcmp(ch->key2, "extension") == 0 ||
                            strcmp(ch->key2, "app") == 0)) {
            channel = ch->key2;
        }
        sbuf_printf(&req->body,
                    "{\"token\":\"%s\",\"title\":\"%s\",\"content\":\"%s\",\"channel\":\"%s\"}",
                    ch->key1, sbuf_cstr(&s_title), sbuf_cstr(&content), channel);
        break;
    }

    case PUSH_TYPE_SERVERCHAN: {
        if (ch->url[0]) {
            sbuf_printf(&req->url, "%s", ch->url);
        } else {
            sbuf_printf(&req->url, "https://sctapi.ftqq.com/%s.send", ch->key1);
        }
        req->content_type = "application/x-www-form-urlencoded";
        sbuf_append(&req->body, "title=");
        util_url_encode(&req->body, title ? title : "");
        sbuf_append(&req->body, "&desp=");
        util_url_encode(&req->body, sbuf_cstr(&plain));
        break;
    }

    case PUSH_TYPE_CUSTOM: {
        sbuf_printf(&req->url, "%s", ch->url);
        const char *p = ch->custom_body;
        while (*p) {
            if (strncmp(p, "{sender}", 8) == 0) {
                sbuf_append(&req->body, sbuf_cstr(&s_title));
                p += 8;
            } else if (strncmp(p, "{message}", 9) == 0) {
                sbuf_append(&req->body, sbuf_cstr(&s_msg));
                p += 9;
            } else if (strncmp(p, "{timestamp}", 11) == 0) {
                sbuf_append(&req->body, sbuf_cstr(&s_time));
                p += 11;
            } else if (strncmp(p, "{self_phone}", 12) == 0) {
                sbuf_append(&req->body, sbuf_cstr(&s_phone));
                p += 12;
            } else {
                sbuf_append_char(&req->body, *p++);
            }
        }
        break;
    }

    case PUSH_TYPE_FEISHU: {
        sbuf_append(&req->body, "{");
        if (ch->key1[0]) {
            time_t ts = time(NULL);
            sbuf_t to_sign, sign;
            sbuf_init(&to_sign);
            sbuf_init(&sign);
            sbuf_printf(&to_sign, "%lld\n%s", (long long)ts, ch->key1);
            util_hmac_sha256_b64(&sign, sbuf_cstr(&to_sign), "");
            sbuf_printf(&req->body, "\"timestamp\":\"%lld\",\"sign\":\"%s\",", (long long)ts,
                        sbuf_cstr(&sign));
            sbuf_free(&to_sign);
            sbuf_free(&sign);
        }
        sbuf_printf(&req->body, "\"msg_type\":\"text\",\"content\":{\"text\":\"%s\"}}",
                    sbuf_cstr(&content));
        sbuf_printf(&req->url, "%s", ch->url);
        break;
    }

    case PUSH_TYPE_GOTIFY: {
        sbuf_append(&req->url, ch->url);
        if (ch->url[strlen(ch->url) - 1] != '/') {
            sbuf_append_char(&req->url, '/');
        }
        sbuf_printf(&req->url, "message?token=%s", ch->key1);
        sbuf_printf(&req->body, "{\"title\":\"%s\",\"message\":\"%s\",\"priority\":5}",
                    sbuf_cstr(&s_title), sbuf_cstr(&content));
        break;
    }

    case PUSH_TYPE_TELEGRAM: {
        sbuf_append(&req->url, ch->url[0] ? ch->url : "https://api.telegram.org");
        while (req->url.len > 0 && req->url.data[req->url.len - 1] == '/') {
            req->url.data[--req->url.len] = '\0';
        }
        sbuf_printf(&req->url, "/bot%s/sendMessage", ch->key2);
        sbuf_printf(&req->body, "{\"chat_id\":\"%s\",\"text\":\"%s\"}", ch->key1,
                    sbuf_cstr(&content));
        break;
    }

    default:
        log_line("[%s] 未知推送类型 %d", ch->name, ch->type);
        break;
    }

    sbuf_free(&s_title);
    sbuf_free(&s_msg);
    sbuf_free(&s_time);
    sbuf_free(&s_phone);
    sbuf_free(&content);
    sbuf_free(&plain);
}

bool push_send_channel(const push_channel_t *channel, const char *title, const char *message,
                       const char *timestamp, bool is_notice)
{
    if (!channel->enabled) {
        return false;
    }
    const char *name = channel->name[0] ? channel->name : "通道";

    push_request_t req;
    build_request(channel, title, message, timestamp, is_notice, &req);
    if (req.url.len == 0) {
        log_line("[%s] 推送 URL 为空，跳过", name);
        request_free(&req);
        return false;
    }
    log_line("发送到推送通道: %s", name);

    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (attempt > 1) {
            vTaskDelay(pdMS_TO_TICKS(500 * (attempt - 1)));
            log_line("[%s] 重试 (%d/%d)...", name, attempt, max_attempts);
        }
        int status = 0;
        if (execute_once(channel, &req, name, &status)) {
            request_free(&req);
            return true;
        }
        /* 只有"可能没送到"的情况才重试：网络错误（status==0）、5xx、429。
         * 2xx 说明平台已经收到并给出应答，即使业务字段判失败，重试也只会让用户收到重复通知。 */
        const bool retryable = (status == 0) || status >= 500 || status == 429;
        if (!retryable) {
            if (status >= 200 && status < 300) {
                log_line("[%s] 请求已送达（HTTP %d），不再重试", name, status);
            } else {
                log_line("[%s] 客户端错误 %d，跳过重试", name, status);
            }
            break;
        }
    }

    request_free(&req);
    log_line("[%s] 多次重试后仍失败", name);
    return false;
}

void push_send_all(const char *title, const char *message, const char *timestamp, bool is_notice)
{
    app_config_t *cfg = config_lock();
    int total = cfg->channel_count;
    push_channel_t *snap = calloc((size_t)(total > 0 ? total : 1), sizeof(push_channel_t));
    if (!snap) {
        config_unlock();
        log_line("内存不足，跳过推送");
        return;
    }
    int count = 0;
    for (int i = 0; i < total; i++) {
        if (config_push_channel_valid(&cfg->channels[i])) {
            snap[count++] = cfg->channels[i];
        }
    }
    config_unlock();

    if (count == 0) {
        free(snap);
        log_line("没有启用的推送通道");
        return;
    }

    log_line("=== 开始多通道推送（%d 个，%s）===", count, is_notice ? "系统通知" : "短信");
    for (int i = 0; i < count; i++) {
        push_send_channel(&snap[i], title, message, timestamp, is_notice);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    log_line("=== 多通道推送完成 ===");
    free(snap);
}
