#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/wifi.h"
#include "notify/email.h"
#include "notify/push.h"
#include "sched/sched.h"
#include "web_internal.h"

/* 设备自身：重连 WiFi / 重启、推送与邮件测试、定时任务列表。 */
/* ---- WiFi / 系统 ---- */
static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    if (wifi_reconnect(20000) == ESP_OK) {
        log_line("WiFi 重连成功，IP: %s", wifi_ip());
    } else {
        log_line("WiFi 重连超时，后台将继续重试");
    }
    vTaskDelete(NULL);
}

esp_err_t handler_wifi(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char action[24] = {0};
    web_query_get(req, "action", action, sizeof(action));
    if (!action[0]) {
        char *body = web_read_body(req);
        if (body) {
            web_form_get(body, "action", action, sizeof(action));
            free(body);
        }
    }
    if (strcmp(action, "restart") == 0) {
        log_line("网页端请求重启 WiFi...");
        web_send_json(req, true, "WiFi 正在重启，请等待约 5 秒后刷新页面");
        xTaskCreate(wifi_reconnect_task, "wifi_reconn", 4096, NULL, 3, NULL);
        return ESP_OK;
    }
    web_send_json(req, false, "未知操作");
    return ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    log_line("系统重启中...");
    esp_restart();
}

esp_err_t handler_system(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char action[24] = {0};
    web_query_get(req, "action", action, sizeof(action));
    if (!action[0]) {
        char *body = web_read_body(req);
        if (body) {
            web_form_get(body, "action", action, sizeof(action));
            free(body);
        }
    }
    if (strcmp(action, "restart") == 0) {
        log_line("网页端请求重启系统...");
        web_send_json(req, true, "系统正在重启，请等待约 30 秒后刷新页面");
        xTaskCreate(restart_task, "sys_restart", 3072, NULL, 3, NULL);
        return ESP_OK;
    }
    web_send_json(req, false, "未知操作");
    return ESP_OK;
}


/* ---- 推送通道测试：用当前表单值发一条测试消息，不需要先保存 ---- */
static void push_test_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";

    push_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.enabled = true; /* 测试不受「启用」开关影响，方便添加后先验证 */
    char val[300] = {0};
    if (web_form_get(ctx->arg2, "type", val, sizeof(val))) {
        int t = atoi(val);
        if (t < PUSH_TYPE_NONE || t > PUSH_TYPE_TELEGRAM) {
            t = PUSH_TYPE_POST_JSON;
        }
        ch.type = (uint8_t)t;
    }
    if (ch.type == PUSH_TYPE_NONE) {
        ch.type = PUSH_TYPE_POST_JSON;
    }
    web_form_get(ctx->arg2, "name", ch.name, sizeof(ch.name));
    web_form_get(ctx->arg2, "url", ch.url, sizeof(ch.url));
    web_form_get(ctx->arg2, "key1", ch.key1, sizeof(ch.key1));
    web_form_get(ctx->arg2, "key2", ch.key2, sizeof(ch.key2));
    web_form_get(ctx->arg2, "body", ch.custom_body, sizeof(ch.custom_body));
    if (ch.name[0] == '\0') {
        snprintf(ch.name, sizeof(ch.name), "测试通道");
    }

    if (!config_push_channel_valid(&ch)) {
        web_json_fail(out, "通道参数不完整：先填好该类型需要的 URL 或密钥再测试");
        return;
    }

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%y/%m/%d %H:%M:%S", &tmv);

    log_line("网页端测试推送通道: %s（类型 %u）", ch.name, (unsigned)ch.type);
    bool ok = push_send_channel(&ch, "推送通道测试",
                                "这是一条来自设备的测试消息，收到即说明该通道配置可用。", ts, true);
    if (ok) {
        web_json_ok(out, "测试消息已发送到「%s」", ch.name);
    } else {
        web_json_fail(out, "「%s」未收到成功应答，请检查 URL、密钥和网络", ch.name);
    }
}

esp_err_t handler_push_test(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }
    if (strlen(body) >= 1000) {
        free(body);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"通道参数过长\"}");
        return ESP_OK;
    }
    /* 走 HTTPS 通道需要 TLS 握手，栈给 8 KB；任务起不来时 spawn 内部已经回过 503 JSON，不能再发第二遍 */
    web_spawn_async3_ex(req, push_test_async, "", body, "", 8192);
    free(body);
    return ESP_OK;
}



/* ---- 邮件通道测试：用当前表单值发一封测试邮件，不需要先保存 ---- */
static void mail_test_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";

    char host[96] = {0}, user[96] = {0}, pass[96] = {0}, to[96] = {0}, port_s[8] = {0};
    web_form_get(ctx->arg2, "smtpServer", host, sizeof(host));
    web_form_get(ctx->arg2, "smtpPort", port_s, sizeof(port_s));
    web_form_get(ctx->arg2, "smtpUser", user, sizeof(user));
    web_form_get(ctx->arg2, "smtpPass", pass, sizeof(pass));
    web_form_get(ctx->arg2, "smtpTo", to, sizeof(to));

    if (!host[0] || !user[0] || !pass[0] || !to[0]) {
        web_json_fail(out, "邮件配置不完整：服务器、账号、密码、收件地址都要填写");
        return;
    }
    int port = atoi(port_s);
    if (port <= 0 || port > 65535) {
        port = 465;
    }

    char ip[16] = {0};
    config_state_get_last_ip(ip, sizeof(ip));
    char self_phone[sizeof(((app_config_t *)0)->self_phone)] = {0};
    app_config_t *cfg = config_lock();
    snprintf(self_phone, sizeof(self_phone), "%s", cfg->self_phone);
    config_unlock();

    char body[384];
    snprintf(body, sizeof(body),
             "这是一条来自设备的测试邮件，收到即说明邮件通道配置可用。\n"
             "本机号码: %s\n设备地址: http://%s/\n",
             self_phone[0] ? self_phone : "未设置", ip[0] ? ip : "未知");

    log_line("网页端测试邮件通道: %s:%d -> %s", host, port, to);
    if (email_send_direct(host, (uint16_t)port, user, pass, to, "邮件通道测试", body)) {
        web_json_ok(out, "测试邮件已发送到 %s", to);
    } else {
        web_json_fail(out, "发送失败，请检查服务器地址、端口与授权码（日志里有失败阶段）");
    }
}

esp_err_t handler_mail_test(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }
    web_spawn_async3_ex(req, mail_test_async, "", body, "", 8192); /* 465 端口要跑 TLS 握手 */
    free(body);
    return ESP_OK;
}


/* ---- 定时任务 ---- */
static void append_json_str(sbuf_t *out, const char *key, const char *value, bool comma)
{
    sbuf_printf(out, "\"%s\":\"", key);
    util_json_escape(out, value ? value : "");
    sbuf_append(out, comma ? "\"," : "\"");
}

static void tasks_to_json(sbuf_t *out)
{
    int n = sched_task_count();
    sbuf_append(out, "{\"success\":true,\"tasks\":[");
    for (int i = 0; i < n; i++) {
        sched_task_t t;
        if (!sched_task_get(i, &t)) {
            break;
        }
        sbuf_printf(out, "%s{\"index\":%d,\"enabled\":%s,\"type\":%u,", i ? "," : "", i,
                    t.enabled ? "true" : "false", (unsigned)t.type);
        append_json_str(out, "phone", t.phone, true);
        append_json_str(out, "text", t.text, true);
        append_json_str(out, "host", t.host, true);
        append_json_str(out, "url", t.url, true);
        append_json_str(out, "esim_iccid", t.esim_iccid, true);
        sbuf_printf(out, "\"esim_restore\":%s,\"next\":%lld,\"repeat_days\":%d,\"last\":%lld}",
                    t.esim_restore ? "true" : "false", (long long)t.next, (int)t.repeat_days,
                    (long long)t.last);
    }
    sbuf_append(out, "]}");
}

esp_err_t handler_tasks(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }

    if (req->method == HTTP_GET) {
        sbuf_t out;
        sbuf_init(&out);
        tasks_to_json(&out);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, sbuf_cstr(&out), out.len);
        sbuf_free(&out);
        return ESP_OK;
    }

    char *body = web_read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }

    char action[16] = {0};
    char idx_s[8] = {0};
    web_form_get(body, "action", action, sizeof(action));
    int index = -1;
    if (web_form_get(body, "index", idx_s, sizeof(idx_s))) {
        index = atoi(idx_s);
    }

    sbuf_t out;
    sbuf_init(&out);
    httpd_resp_set_type(req, "application/json; charset=utf-8");

    if (strcmp(action, "delete") == 0) {
        if (sched_task_delete(index)) {
            web_json_ok(&out, "任务已删除");
        } else {
            web_json_fail(&out, "任务不存在");
        }
        free(body);
        httpd_resp_send(req, sbuf_cstr(&out), out.len);
        sbuf_free(&out);
        return ESP_OK;
    }

    sched_task_t t;
    memset(&t, 0, sizeof(t));
    if (index >= 0 && !sched_task_get(index, &t)) {
        web_json_fail(&out, "任务不存在");
        free(body);
        httpd_resp_send(req, sbuf_cstr(&out), out.len);
        sbuf_free(&out);
        return ESP_OK;
    }

    char val[256] = {0};
    char en[8] = {0}, type_s[8] = {0}, next_s[24] = {0}, rep_s[12] = {0};
    web_form_get(body, "enabled", en, sizeof(en));
    web_form_get(body, "type", type_s, sizeof(type_s));
    web_form_get(body, "next", next_s, sizeof(next_s));
    web_form_get(body, "repeat", rep_s, sizeof(rep_s));
    t.enabled = (strcmp(en, "on") == 0 || strcmp(en, "1") == 0);
    t.type = (uint8_t)atoi(type_s);
    t.next = (int64_t)strtoll(next_s, NULL, 10);
    t.repeat_days = (int32_t)atoi(rep_s);
    if (t.repeat_days < 0) {
        t.repeat_days = 0;
    }
    if (t.repeat_days > 365) {
        t.repeat_days = 365;
    }
    if (web_form_get(body, "phone", val, sizeof(val))) {
        snprintf(t.phone, sizeof(t.phone), "%s", val);
    }
    if (web_form_get(body, "text", val, sizeof(val))) {
        snprintf(t.text, sizeof(t.text), "%s", val);
    }
    if (web_form_get(body, "host", val, sizeof(val))) {
        snprintf(t.host, sizeof(t.host), "%s", val);
    }
    if (web_form_get(body, "url", val, sizeof(val))) {
        snprintf(t.url, sizeof(t.url), "%s", val);
    }
    if (web_form_get(body, "esim_iccid", val, sizeof(val))) {
        snprintf(t.esim_iccid, sizeof(t.esim_iccid), "%s", val);
    }
    char restore_s[8] = {0};
    if (web_form_get(body, "esim_restore", restore_s, sizeof(restore_s))) {
        t.esim_restore = (strcmp(restore_s, "on") == 0 || strcmp(restore_s, "1") == 0);
    } else if (index < 0 && t.esim_iccid[0]) {
        t.esim_restore = true; /* 新任务默认保号后切回原卡 */
    }

    bool esim_id_ok = true;
    for (const char *p = t.esim_iccid; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'))) {
            esim_id_ok = false;
        }
    }
    if (t.esim_iccid[0] && strlen(t.esim_iccid) < 10) {
        esim_id_ok = false;
    }

    if (t.type != SCHED_TYPE_SMS && t.type != SCHED_TYPE_PING && t.type != SCHED_TYPE_HTTP) {
        web_json_fail(&out, "任务类型无效");
    } else if (!esim_id_ok) {
        web_json_fail(&out, "eSIM 配置 ICCID 无效");
    } else if (t.next <= 0) {
        web_json_fail(&out, "请选择执行时间");
    } else if (t.type == SCHED_TYPE_SMS && (!t.phone[0] || !t.text[0])) {
        web_json_fail(&out, "短信任务需要填写号码和内容");
    } else if (t.type == SCHED_TYPE_HTTP && strncmp(t.url, "http", 4) != 0) {
        web_json_fail(&out, "网址需以 http:// 或 https:// 开头");
    } else {
        if (t.type == SCHED_TYPE_PING && !t.host[0]) {
            snprintf(t.host, sizeof(t.host), "qq.com");
        }
        int slot = sched_task_save(index, &t);
        if (slot < 0) {
            web_json_fail(&out, "任务数量已达上限（%d 个）", SCHED_MAX_TASKS);
        } else {
            time_t now = time(NULL);
            if (t.next <= (int64_t)now) {
                web_json_ok(&out, "任务已保存：时间已过，会在下一轮检查时立即补执行");
            } else {
                web_json_ok(&out, "任务已保存");
            }
            log_line("网页端保存定时任务 #%d（类型 %u，%s）", slot + 1, (unsigned)t.type,
                     t.repeat_days > 0 ? "循环" : "单次");
        }
    }

    free(body);
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ESP_OK;
}
