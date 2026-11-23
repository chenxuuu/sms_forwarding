#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem/modem.h"
#include "modem/pdu.h"
#include "net/wifi.h"
#include "web_internal.h"

/* ---- JSON 小工具 ---- */
static void json_ok(sbuf_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void json_fail(sbuf_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void json_result(sbuf_t *out, bool ok, const char *fmt, va_list ap)
{
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    sbuf_printf(out, "{\"success\":%s,\"message\":\"", ok ? "true" : "false");
    util_json_escape(out, msg);
    sbuf_append(out, "\"}");
}

static void json_ok(sbuf_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    json_result(out, true, fmt, ap);
    va_end(ap);
}

static void json_fail(sbuf_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    json_result(out, false, fmt, ap);
    va_end(ap);
}

static void table_row(sbuf_t *out, const char *name, const char *value)
{
    sbuf_printf(out, "<tr><td>%s</td><td>", name);
    sbuf_append(out, value);
    sbuf_append(out, "</td></tr>");
}

/* 在 AT 应答中找某行（含关键字），返回该行起始位置 */
static const char *find_line(const char *resp, const char *keyword)
{
    if (!resp) {
        return NULL;
    }
    const char *p = strstr(resp, keyword);
    return p;
}

/* 取关键字行冒号后的内容（到行尾），去掉引号 */
static void take_after(const char *line, char sep, char *out, size_t cap)
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
static bool take_field(const char *line, char sep, int index, char *out, size_t cap)
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

/* ---- 发送短信 ---- */
static const char SEND_RESULT_TMPL[] =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta http-equiv=\"refresh\" content=\"3;url=/sms\"><title>发送结果</title>"
    "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:100px;background:#f5f5f5}"
    ".result{padding:20px;border-radius:10px;display:inline-block}"
    ".success{background:#4CAF50;color:#fff}.error{background:#f44336;color:#fff}"
    "</style></head><body><div class=\"result %CLASS%\"><h2>%ICON% %MSG%</h2>"
    "<p>3秒后返回发送页面...</p></div></body></html>";

static void send_result_page(sbuf_t *out, bool ok, const char *msg)
{
    /* 简易模板替换 */
    const char *p = SEND_RESULT_TMPL;
    while (*p) {
        if (strncmp(p, "%CLASS%", 7) == 0) {
            sbuf_append(out, ok ? "success" : "error");
            p += 7;
        } else if (strncmp(p, "%ICON%", 6) == 0) {
            sbuf_append(out, ok ? "✅" : "❌");
            p += 6;
        } else if (strncmp(p, "%MSG%", 5) == 0) {
            sbuf_append(out, msg);
            p += 5;
        } else {
            sbuf_append_char(out, *p++);
        }
    }
}

static void sendsms_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    log_line("网页端发送短信请求");
    log_line("目标号码: %s", ctx->arg1);
    log_line("短信内容: %s", ctx->arg2);
    bool ok = modem_send_sms(ctx->arg1, ctx->arg2, 30000);
    if (util_equals(ctx->arg3, "json")) {
        *ctype = "application/json; charset=utf-8";
        if (ok) {
            json_ok(out, "短信已发送");
        } else {
            json_fail(out, "发送失败：请检查模组状态，或内容是否超过单条长度（70 个汉字）");
        }
        return;
    }
    *ctype = "text/html; charset=utf-8";
    send_result_page(out, ok, ok ? "短信发送成功！"
                                  : "短信发送失败：请检查模组状态，或内容是否超过单条长度（70 个汉字）");
}

esp_err_t handler_sendsms(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_read_body(req);
    char phone[32] = {0};
    char content[1024] = {0};
    if (body) {
        web_form_get(body, "phone", phone, sizeof(phone));
        web_form_get(body, "content", content, sizeof(content));
        free(body);
    }
    util_trim(phone);
    util_trim(content);
    if (!phone[0] || !content[0]) {
        sbuf_t out;
        sbuf_init(&out);
        send_result_page(&out, false, phone[0] ? "错误：请输入短信内容" : "错误：请输入目标号码");
        web_send_html_redirect(req, sbuf_cstr(&out));
        sbuf_free(&out);
        return ESP_OK;
    }
    char json_flag[8] = {0};
    bool want_json = web_query_get(req, "json", json_flag, sizeof(json_flag)) &&
                     strcmp(json_flag, "1") == 0;
    web_spawn_async3(req, sendsms_async, phone, content, want_json ? "json" : "");
    return ESP_OK;
}

/* ---- Ping ---- */
static void ping_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    (void)ctx;
    *ctype = "application/json; charset=utf-8";
    log_line("网页端发起 Ping 请求");
    modem_ping_result_t result = {0};
    bool ok = modem_ping(&result, 35000);
    if (ok) {
        json_ok(out, "%s", result.detail);
    } else {
        json_fail(out, "%s", result.detail[0] ? result.detail : "操作超时，未收到 Ping 结果");
    }
}

esp_err_t handler_ping(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    web_spawn_async(req, ping_async, "", "");
    return ESP_OK;
}

/* ---- 信息查询 ---- */
static void query_ati(sbuf_t *out)
{
    char *resp = modem_at("ATI", 3000);
    if (!resp || !strstr(resp, "OK")) {
        json_fail(out, "查询失败: %s", resp ? resp : "模组无响应");
        free(resp);
        return;
    }
    char manufacturer[64] = "未知", model[64] = "未知", version[64] = "未知";
    int line_no = 0;
    char *save = NULL;
    for (char *line = strtok_r(resp, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        util_trim(line);
        if (!line[0] || strcmp(line, "ATI") == 0 || strcmp(line, "OK") == 0) {
            continue;
        }
        line_no++;
        if (line_no == 1) snprintf(manufacturer, sizeof(manufacturer), "%s", line);
        else if (line_no == 2) snprintf(model, sizeof(model), "%s", line);
        else if (line_no == 3) snprintf(version, sizeof(version), "%s", line);
    }
    free(resp);

    sbuf_t table;
    sbuf_init(&table);
    table_row(&table, "制造商", manufacturer);
    table_row(&table, "模组型号", model);
    table_row(&table, "固件版本", version);
    json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
}

static void query_signal(sbuf_t *out)
{
    char *resp = modem_at("AT+CESQ", 3000);
    const char *line = find_line(resp, "+CESQ:");
    if (!line) {
        json_fail(out, "查询失败: %s", resp ? resp : "模组无响应");
        free(resp);
        return;
    }

    char rsrp_field[16] = "", rsrq_field[16] = "";
    take_field(line, ':', 5, rsrp_field, sizeof(rsrp_field));
    take_field(line, ':', 4, rsrq_field, sizeof(rsrq_field));

    char rsrp_str[64];
    int rsrp = atoi(rsrp_field);
    if (rsrp == 99 || rsrp == 255 || !rsrp_field[0]) {
        snprintf(rsrp_str, sizeof(rsrp_str), "未知");
    } else {
        int dbm = -140 + rsrp;
        const char *hint = dbm >= -80 ? "信号极好"
                           : dbm >= -90 ? "信号良好"
                           : dbm >= -100 ? "信号一般"
                           : dbm >= -110 ? "信号较弱"
                                        : "信号很差";
        snprintf(rsrp_str, sizeof(rsrp_str), "%d dBm (%s)", dbm, hint);
    }

    char rsrq_str[32];
    int rsrq = atoi(rsrq_field);
    if (rsrq == 99 || rsrq == 255 || !rsrq_field[0]) {
        snprintf(rsrq_str, sizeof(rsrq_str), "未知");
    } else {
        snprintf(rsrq_str, sizeof(rsrq_str), "%.1f dB", -19.5 + rsrq * 0.5);
    }

    char raw[128];
    take_after(line, ':', raw, sizeof(raw));

    sbuf_t table;
    sbuf_init(&table);
    table_row(&table, "信号强度 (RSRP)", rsrp_str);
    table_row(&table, "信号质量 (RSRQ)", rsrq_str);
    table_row(&table, "原始数据", raw);
    json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
    free(resp);
}

static void query_siminfo(sbuf_t *out)
{
    sbuf_t table;
    sbuf_init(&table);

    char *resp = modem_at("AT+CIMI", 3000);
    char imsi[32] = "未知";
    if (resp) {
        char *save = NULL;
        for (char *line = strtok_r(resp, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            util_trim(line);
            if (strlen(line) >= 10) {
                bool digits = true;
                for (const char *c = line; *c; c++) {
                    if (*c < '0' || *c > '9') {
                        digits = false;
                        break;
                    }
                }
                if (digits) {
                    snprintf(imsi, sizeof(imsi), "%s", line);
                    break;
                }
            }
        }
        free(resp);
    }
    table_row(&table, "IMSI", imsi);

    /* ML307Y 不支持 AT+ICCID，退回到 AT+QCCID（移远/中移风格） */
    char iccid[40] = "";
    resp = modem_at("AT+ICCID", 3000);
    const char *line = find_line(resp, "+ICCID:");
    if (line) {
        take_after(line, ':', iccid, sizeof(iccid));
    }
    free(resp);
    if (!iccid[0]) {
        resp = modem_at("AT+QCCID", 3000);
        line = find_line(resp, "+QCCID:");
        if (line) {
            take_after(line, ':', iccid, sizeof(iccid));
        }
        free(resp);
    }
    table_row(&table, "ICCID", iccid[0] ? iccid : "未知");

    resp = modem_at("AT+CNUM", 3000);
    char phone[32] = "未存储或不支持";
    if (resp) {
        const char *p = strstr(resp, ",\"");
        if (p) {
            const char *end = strchr(p + 2, '"');
            if (end && end > p + 2) {
                size_t n = (size_t)(end - (p + 2));
                if (n < sizeof(phone)) {
                    memcpy(phone, p + 2, n);
                    phone[n] = '\0';
                }
            }
        }
        free(resp);
    }
    table_row(&table, "本机号码", phone);

    json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
}

static void query_network(sbuf_t *out)
{
    sbuf_t table;
    sbuf_init(&table);

    char *resp = modem_at("AT+CEREG?", 3000);
    char stat_field[8] = "";
    const char *line = find_line(resp, "+CEREG:");
    if (line) {
        take_field(line, ':', 1, stat_field, sizeof(stat_field));
    }
    free(resp);
    const char *reg = "未知";
    switch (atoi(stat_field)) {
    case 0: reg = "未注册，未搜索"; break;
    case 1: reg = "已注册，本地网络"; break;
    case 2: reg = "未注册，正在搜索"; break;
    case 3: reg = "注册被拒绝"; break;
    case 5: reg = "已注册，漫游"; break;
    default: break;
    }
    table_row(&table, "网络注册", reg);

    resp = modem_at("AT+COPS?", 3000);
    char oper[48] = "未知";
    if (resp) {
        const char *q1 = strchr(resp, '"');
        const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (q1 && q2 && q2 > q1 + 1) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n < sizeof(oper)) {
                memcpy(oper, q1 + 1, n);
                oper[n] = '\0';
            }
        }
        free(resp);
    }
    table_row(&table, "运营商", oper);

    resp = modem_at("AT+CGACT?", 3000);
    const char *pdp = (resp && strstr(resp, "+CGACT: 1,1")) ? "已激活" : "未激活";
    free(resp);
    table_row(&table, "数据连接", pdp);

    resp = modem_at("AT+CGDCONT?", 3000);
    char apn[64] = "未知";
    if (resp) {
        const char *p = strstr(resp, ",\"");
        if (p) {
            p = strstr(p + 2, ",\"");
            if (p) {
                const char *end = strchr(p + 2, '"');
                if (end) {
                    size_t n = (size_t)(end - (p + 2));
                    if (n == 0) {
                        snprintf(apn, sizeof(apn), "(自动)");
                    } else if (n < sizeof(apn)) {
                        memcpy(apn, p + 2, n);
                        apn[n] = '\0';
                    }
                }
            }
        }
        free(resp);
    }
    table_row(&table, "APN", apn);

    json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
}

static void query_wifi(sbuf_t *out)
{
    wifi_info_t info;
    wifi_get_info(&info);

    char rssi[64];
    const char *hint = info.rssi >= -50 ? "信号极好"
                       : info.rssi >= -60 ? "信号很好"
                       : info.rssi >= -70 ? "信号良好"
                       : info.rssi >= -80 ? "信号一般"
                       : info.rssi >= -90 ? "信号较弱"
                                          : "信号很差";
    snprintf(rssi, sizeof(rssi), "%d dBm (%s)", info.rssi, hint);

    char channel[16];
    snprintf(channel, sizeof(channel), "%d", info.channel);

    sbuf_t table;
    sbuf_init(&table);
    table_row(&table, "连接状态", info.connected ? "已连接" : "未连接");
    table_row(&table, "当前SSID", info.ssid[0] ? info.ssid : "未知");
    table_row(&table, "信号强度 (RSSI)", rssi);
    table_row(&table, "IP地址", info.ip);
    table_row(&table, "网关", info.gateway);
    table_row(&table, "子网掩码", info.netmask);
    table_row(&table, "DNS服务器", info.dns);
    table_row(&table, "MAC地址", info.mac);
    table_row(&table, "路由器BSSID", info.bssid);
    table_row(&table, "WiFi信道", channel);

    json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
}

static void query_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    const char *type = ctx->arg1;
    if (strcmp(type, "ati") == 0) {
        query_ati(out);
    } else if (strcmp(type, "signal") == 0) {
        query_signal(out);
    } else if (strcmp(type, "siminfo") == 0) {
        query_siminfo(out);
    } else if (strcmp(type, "network") == 0) {
        query_network(out);
    } else if (strcmp(type, "wifi") == 0) {
        query_wifi(out);
    } else {
        json_fail(out, "未知的查询类型");
    }
}

esp_err_t handler_query(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char type[24] = {0};
    if (!web_query_get(req, "type", type, sizeof(type))) {
        char *body = web_read_body(req);
        if (body) {
            web_form_get(body, "type", type, sizeof(type));
            free(body);
        }
    }
    web_spawn_async(req, query_async, type, "");
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
        const char *line = find_line(resp, "+CFUN:");
        if (!line) {
            json_fail(out, "查询失败");
            free(resp);
            return;
        }
        char mode_field[16];
        take_after(line, ':', mode_field, sizeof(mode_field));
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
        table_row(&table, "当前状态", status);
        char mode_str[16];
        snprintf(mode_str, sizeof(mode_str), "%d", mode);
        table_row(&table, "CFUN值", mode_str);
        json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
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
        const char *line = find_line(resp, "+CFUN:");
        int current = 1;
        if (line) {
            char mode_field[16];
            take_after(line, ':', mode_field, sizeof(mode_field));
            current = atoi(mode_field);
        }
        free(resp);
        target = (current == 1) ? 4 : 1;
    } else {
        json_fail(out, "未知操作");
        return;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", target);
    log_line("切换飞行模式: %s", cmd);
    char *resp = modem_at(cmd, 8000);
    bool ok = resp && strstr(resp, "OK");
    if (ok) {
        json_ok(out, target == 4 ? "已开启飞行模式 ✈️<br>模组射频已关闭，无法收发短信"
                                 : "已关闭飞行模式 🟢<br>模组恢复正常工作");
    } else {
        json_fail(out, "切换失败: %s", resp ? resp : "模组无响应");
    }
    free(resp);
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
    web_spawn_async(req, flight_async, action, "");
    return ESP_OK;
}

/* ---- 原始 AT 指令 ---- */
static void at_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    if (!ctx->arg1[0]) {
        json_fail(out, "错误：指令不能为空");
        return;
    }
    log_line("网页端发送AT指令: %s", ctx->arg1);
    char *resp = modem_at(ctx->arg1, 8000);
    if (resp && resp[0]) {
        log_line("模组响应: %s", resp);
        json_ok(out, "%s", resp);
    } else {
        json_fail(out, "超时或无响应");
    }
    free(resp);
}

esp_err_t handler_at(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char cmd[160] = {0};
    web_query_get(req, "cmd", cmd, sizeof(cmd));
    if (!cmd[0]) {
        char *body = web_read_body(req);
        if (body) {
            web_form_get(body, "cmd", cmd, sizeof(cmd));
            free(body);
        }
    }
    web_spawn_async(req, at_async, cmd, "");
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
        const char *line = find_line(resp, "+CSQ:");
        char field[16];
        if (line && take_field(line, ':', 0, field, sizeof(field))) {
            int rssi = atoi(field);
            const char *quality = rssi >= 19 ? "优秀"
                                  : rssi >= 14 ? "良好"
                                  : rssi >= 10 ? "一般"
                                  : rssi >= 5  ? "较差"
                                               : "很差";
            int dbm = (rssi == 99) ? -999 : (-113 + rssi * 2);
            json_ok(out, "RSRP: %d dBm (%s), RSSI: %d", dbm, quality, rssi);
        } else {
            json_fail(out, "无法获取信号: %s", resp ? resp : "模组无响应");
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
                json_ok(out, "%s", name);
            } else {
                json_fail(out, "无法获取运营商: %s", resp);
            }
            free(resp);
            return;
        }
        json_fail(out, "无法获取运营商");
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
            json_ok(out, "%s", imei);
        } else {
            json_fail(out, "无法获取 IMEI/序列号");
        }
        return;
    }

    json_fail(out, "未知操作: %s", action);
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

/* ---- PDU 解码诊断（便于验证协议实现，返回解析后的字段）---- */
static void decode_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    sms_deliver_t *msg = calloc(1, sizeof(*msg));
    if (!msg) {
        json_fail(out, "内存不足");
        return;
    }
    if (!pdu_decode_deliver(ctx->arg1, msg)) {
        json_fail(out, "PDU 解析失败");
        free(msg);
        return;
    }
    sbuf_t text;
    sbuf_init(&text);
    util_json_escape(&text, msg->text);
    sbuf_t sender;
    sbuf_init(&sender);
    util_json_escape(&sender, msg->sender);
    sbuf_printf(out,
                "{\"success\":true,\"sender\":\"%s\",\"timestamp\":\"%s\",\"text\":\"%s\","
                "\"dcs\":%d,\"concat\":{\"ref\":%d,\"part\":%d,\"total\":%d}}",
                sbuf_cstr(&sender), msg->timestamp, sbuf_cstr(&text), msg->dcs,
                msg->concat_ref, msg->concat_part, msg->concat_total);
    sbuf_free(&text);
    sbuf_free(&sender);
    free(msg);
}

esp_err_t handler_decode(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char pdu[512] = {0};
    web_query_get(req, "pdu", pdu, sizeof(pdu));
    web_spawn_async(req, decode_async, pdu, "");
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
        json_fail(out, "HTTP 客户端创建失败");
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        json_fail(out, "请求失败: %s（errno=%d，可用堆 %u KB）", esp_err_to_name(err),
                  esp_http_client_get_errno(client),
                  (unsigned)(esp_get_free_heap_size() / 1024));
    } else {
        json_ok(out, "HTTP %d（可用堆 %u KB）", esp_http_client_get_status_code(client),
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
    web_spawn_async(req, selftest_async, url, "");
    return ESP_OK;
}

