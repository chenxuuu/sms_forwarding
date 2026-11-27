#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem/modem.h"
#include "web_internal.h"

/* 模组相关动作：保号 Ping/HTTP、飞行模式、软硬重启、网络自检（TLS 连通性）。 */
/* ---- 保号操作：Ping / HTTP 流量 ---- */
static void ping_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    const char *ip = (ctx->arg1[0] != '\0') ? ctx->arg1 : "qq.com";
    log_line("网页端发起 Ping: %s", ip);
    modem_ping_result_t result = {0};
    bool ok = modem_ping_ip(ip, &result, 35000);
    if (ok) {
        web_json_ok(out, "%s", result.detail);
    } else {
        web_json_fail(out, "%s", result.detail[0] ? result.detail : "操作超时，未收到 Ping 结果");
    }
}

esp_err_t handler_ping(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char ip[128] = {0};
    web_query_get(req, "ip", ip, sizeof(ip));
    web_spawn_async(req, ping_async, ip, "");
    return ESP_OK;
}

static void keepalive_http_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    log_line("网页端发起保号 HTTP 访问: %s", ctx->arg1);
    modem_http_result_t result = {0};
    bool ok = modem_http_get(ctx->arg1, &result, 45000);
    if (ok) {
        web_json_ok(out, "%s", result.detail);
    } else {
        web_json_fail(out, "%s", result.detail[0] ? result.detail : "访问失败，未收到模组应答");
    }
}

esp_err_t handler_keepalive_http(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char url[512] = {0};
    web_query_get(req, "url", url, sizeof(url));
    if (url[0] == '\0') {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"网址不能为空\"}");
        return ESP_OK;
    }
    web_spawn_async3_ex(req, keepalive_http_async, url, "", "", 8192); /* 可能是 https，栈留够 TLS */
    return ESP_OK;
}


/* ---- 飞行模式 ---- */
static void flight_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    const char *action = ctx->arg1;

    if (strcmp(action, "query") == 0) {
        log_line("网页端查询飞行模式: AT+CFUN?");
        char *resp = modem_at("AT+CFUN?", 3000);
        const char *line = web_at_find_line(resp, "+CFUN:");
        if (!line) {
            web_json_fail(out, "查询失败");
            free(resp);
            return;
        }
        char mode_field[16];
        web_at_take_after(line, ':', mode_field, sizeof(mode_field));
        int mode = atoi(mode_field);
        const char *desc;
        const char *icon;
        switch (mode) {
        case 0: desc = "最小功能模式（关机）"; icon = "🔴"; break;
        case 1: desc = "全功能模式（正常）"; icon = "🟢"; break;
        case 4: desc = "飞行模式（射频关闭）"; icon = "✈️"; break;
        default: desc = "未知模式"; icon = "❓"; break;
        }
        sbuf_t table;
        sbuf_init(&table);
        char status[96];
        snprintf(status, sizeof(status), "%s %s", icon, desc);
        web_json_table_row(&table, "当前状态", status);
        char mode_str[16];
        snprintf(mode_str, sizeof(mode_str), "%d", mode);
        web_json_table_row(&table, "CFUN值", mode_str);
        web_json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
        sbuf_free(&table);
        free(resp);
        return;
    }

    int target = 0;
    if (strcmp(action, "on") == 0) {
        target = 4;
    } else if (strcmp(action, "off") == 0) {
        target = 1;
    } else if (strcmp(action, "toggle") == 0) {
        char *resp = modem_at("AT+CFUN?", 3000);
        const char *line = web_at_find_line(resp, "+CFUN:");
        int current = 1;
        if (line) {
            char mode_field[16];
            web_at_take_after(line, ':', mode_field, sizeof(mode_field));
            current = atoi(mode_field);
        }
        free(resp);
        target = (current == 1) ? 4 : 1;
    } else {
        web_json_fail(out, "未知操作");
        return;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", target);
    log_line("切换飞行模式: %s", cmd);
    char *resp = modem_at(cmd, 8000);
    bool ok = resp && strstr(resp, "OK");
    if (ok) {
        /* 立即同步就绪状态：开飞行模式后射频关闭，首页不能再显示「已就绪」。
         * 关闭飞行模式时保持 false，等 netmon 检测到重新注册后再置 true。 */
        modem_set_ready(false);
        log_line("飞行模式已%s，模组就绪状态已同步为未就绪（等待网络守护重新确认）",
                 target == 4 ? "开启" : "关闭");
        web_json_ok(out, target == 4 ? "已开启飞行模式 ✈️<br>模组射频已关闭，无法收发短信"
                                 : "已关闭飞行模式 🟢<br>模组恢复正常工作，正在重新注册网络");
    } else {
        web_json_fail(out, "切换失败: %s", resp ? resp : "模组无响应");
    }
    free(resp);
}

static void flight_state_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    (void)ctx;
    *ctype = "application/json; charset=utf-8";
    char *resp = modem_at("AT+CFUN?", 3000);
    const char *line = web_at_find_line(resp, "+CFUN:");
    if (!line) {
        web_json_fail(out, "查询失败");
        free(resp);
        return;
    }
    char f[16];
    web_at_take_after(line, ':', f, sizeof(f));
    int mode = atoi(f);
    free(resp);
    sbuf_printf(out, "{\"success\":true,\"cfun\":%d,\"flight\":%s}", mode,
                mode == 4 ? "true" : "false");
}

esp_err_t handler_flight(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char action[16] = {0};
    web_query_get(req, "action", action, sizeof(action));
    if (!action[0]) {
        char *body = web_read_body(req);
        if (body) {
            web_form_get(body, "action", action, sizeof(action));
            free(body);
        }
    }
    if (strcmp(action, "state") == 0) {
        web_spawn_async(req, flight_state_async, "", "");
        return ESP_OK;
    }
    web_spawn_async(req, flight_async, action, "");
    return ESP_OK;
}


/* ---- 模组动作 ---- */
static void reset_task(void *arg)
{
    int mode = (int)(intptr_t)arg;
    if (mode == 0) {
        modem_soft_reset(90000);
    } else {
        modem_hard_reset(90000);
    }
    vTaskDelete(NULL);
}

static void modem_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    const char *action = ctx->arg1;

    if (strcmp(action, "signal") == 0) {
        char *resp = modem_at("AT+CSQ", 4000);
        const char *line = web_at_find_line(resp, "+CSQ:");
        char field[16];
        if (line && web_at_take_field(line, ':', 0, field, sizeof(field))) {
            int rssi = atoi(field);
            const char *quality = rssi >= 19 ? "优秀"
                                  : rssi >= 14 ? "良好"
                                  : rssi >= 10 ? "一般"
                                  : rssi >= 5  ? "较差"
                                               : "很差";
            int dbm = (rssi == 99) ? -999 : (-113 + rssi * 2);
            web_json_ok(out, "RSRP: %d dBm (%s), RSSI: %d", dbm, quality, rssi);
        } else {
            web_json_fail(out, "无法获取信号: %s", resp ? resp : "模组无响应");
        }
        free(resp);
        return;
    }

    if (strcmp(action, "operator") == 0) {
        char *resp = modem_at("AT+COPS?", 6000);
        char name[48] = {0};
        if (resp) {
            const char *q1 = strchr(resp, '"');
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && q2 > q1 + 1) {
                size_t n = (size_t)(q2 - q1 - 1);
                if (n >= sizeof(name)) {
                    n = sizeof(name) - 1;
                }
                memcpy(name, q1 + 1, n);
                name[n] = '\0';
            }
            if (name[0]) {
                web_json_ok(out, "%s", name);
            } else {
                web_json_fail(out, "无法获取运营商: %s", resp);
            }
            free(resp);
            return;
        }
        web_json_fail(out, "无法获取运营商");
        return;
    }

    if (strcmp(action, "imei") == 0) {
        char *resp = modem_at("AT+GSN", 4000);
        char imei[40] = {0};
        if (resp) {
            char *save = NULL;
            for (char *line = strtok_r(resp, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                util_trim(line);
                if (!line[0] || strcmp(line, "OK") == 0 || strstr(line, "ERROR") ||
                    strcmp(line, "AT+GSN") == 0) {
                    continue;
                }
                /* ML307Y 返回的是字母数字序列号，不一定纯数字 */
                snprintf(imei, sizeof(imei), "%s", line);
                break;
            }
            free(resp);
        }
        if (imei[0]) {
            web_json_ok(out, "%s", imei);
        } else {
            web_json_fail(out, "无法获取 IMEI/序列号");
        }
        return;
    }

    web_json_fail(out, "未知操作: %s", action);
}

esp_err_t handler_modem(httpd_req_t *req)
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
        log_line("网页端请求软重启模组...");
        web_send_json(req, true, "正在软重启模组，请等待约 20 秒后刷新页面");
        xTaskCreate(reset_task, "modem_reset", 6144, (void *)(intptr_t)0, 3, NULL);
        return ESP_OK;
    }
    if (strcmp(action, "hardreset") == 0) {
        log_line("网页端请求硬重启模组...");
        web_send_json(req, true, "正在硬重启模组，请等待约 20 秒后刷新页面");
        xTaskCreate(reset_task, "modem_reset", 6144, (void *)(intptr_t)1, 3, NULL);
        return ESP_OK;
    }
    web_spawn_async(req, modem_async, action, "");
    return ESP_OK;
}


/* ---- 网络自检：验证 HTTPS 客户端（CA bundle / TLS / DNS）---- */
static void selftest_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    const char *url = ctx->arg1[0] ? ctx->arg1 : "https://www.baidu.com/";
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        web_json_fail(out, "HTTP 客户端创建失败");
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        web_json_fail(out, "请求失败: %s（errno=%d，可用堆 %u KB）", esp_err_to_name(err),
                  esp_http_client_get_errno(client),
                  (unsigned)(esp_get_free_heap_size() / 1024));
    } else {
        web_json_ok(out, "HTTP %d（可用堆 %u KB）", esp_http_client_get_status_code(client),
                (unsigned)(esp_get_free_heap_size() / 1024));
    }
    esp_http_client_cleanup(client);
}

esp_err_t handler_selftest(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char url[256] = {0};
    web_query_get(req, "url", url, sizeof(url));
    web_spawn_async3_ex(req, selftest_async, url, "", "", 8192); /* 要跑 TLS 握手 */
    return ESP_OK;
}
