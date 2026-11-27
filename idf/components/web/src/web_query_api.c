#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "modem/modem.h"
#include "net/wifi.h"
#include "web_internal.h"

/* 前台「信息查询」：ATI / 信号 / SIM / 驻网 / WiFi 都拼成 HTML 表格返回。 */
/* ---- 信息查询 ---- */
static void query_ati(sbuf_t *out)
{
    char *resp = modem_at("ATI", 3000);
    if (!resp || !strstr(resp, "OK")) {
        web_json_fail(out, "查询失败: %s", resp ? resp : "模组无响应");
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
    web_json_table_row(&table, "制造商", manufacturer);
    web_json_table_row(&table, "模组型号", model);
    web_json_table_row(&table, "固件版本", version);
    web_json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
}

static void query_signal(sbuf_t *out)
{
    char *resp = modem_at("AT+CESQ", 3000);
    const char *line = web_at_find_line(resp, "+CESQ:");
    if (!line) {
        web_json_fail(out, "查询失败: %s", resp ? resp : "模组无响应");
        free(resp);
        return;
    }

    char rsrp_field[16] = "", rsrq_field[16] = "";
    web_at_take_field(line, ':', 5, rsrp_field, sizeof(rsrp_field));
    web_at_take_field(line, ':', 4, rsrq_field, sizeof(rsrq_field));

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
    web_at_take_after(line, ':', raw, sizeof(raw));

    sbuf_t table;
    sbuf_init(&table);
    web_json_table_row(&table, "信号强度 (RSRP)", rsrp_str);
    web_json_table_row(&table, "信号质量 (RSRQ)", rsrq_str);
    web_json_table_row(&table, "原始数据", raw);
    web_json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
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
    web_json_table_row(&table, "IMSI", imsi);

    /* ML307Y 不支持 AT+ICCID，退回到 AT+QCCID（移远/中移风格） */
    char iccid[40] = "";
    resp = modem_at("AT+ICCID", 3000);
    const char *line = web_at_find_line(resp, "+ICCID:");
    if (line) {
        web_at_take_after(line, ':', iccid, sizeof(iccid));
    }
    free(resp);
    if (!iccid[0]) {
        resp = modem_at("AT+QCCID", 3000);
        line = web_at_find_line(resp, "+QCCID:");
        if (line) {
            web_at_take_after(line, ':', iccid, sizeof(iccid));
        }
        free(resp);
    }
    web_json_table_row(&table, "ICCID", iccid[0] ? iccid : "未知");

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
    web_json_table_row(&table, "本机号码", phone);

    web_json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
    sbuf_free(&table);
}

static void query_network(sbuf_t *out)
{
    sbuf_t table;
    sbuf_init(&table);

    char *resp = modem_at("AT+CEREG?", 3000);
    char stat_field[8] = "";
    const char *line = web_at_find_line(resp, "+CEREG:");
    if (line) {
        web_at_take_field(line, ':', 1, stat_field, sizeof(stat_field));
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
    web_json_table_row(&table, "网络注册", reg);

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
    web_json_table_row(&table, "运营商", oper);

    resp = modem_at("AT+CGACT?", 3000);
    const char *pdp = (resp && strstr(resp, "+CGACT: 1,1")) ? "已激活" : "未激活";
    free(resp);
    web_json_table_row(&table, "数据连接", pdp);

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
    web_json_table_row(&table, "APN", apn);

    web_json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
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
    web_json_table_row(&table, "连接状态", info.connected ? "已连接" : "未连接");
    web_json_table_row(&table, "当前SSID", info.ssid[0] ? info.ssid : "未知");
    web_json_table_row(&table, "信号强度 (RSSI)", rssi);
    web_json_table_row(&table, "IP地址", info.ip);
    web_json_table_row(&table, "网关", info.gateway);
    web_json_table_row(&table, "子网掩码", info.netmask);
    web_json_table_row(&table, "DNS服务器", info.dns);
    web_json_table_row(&table, "MAC地址", info.mac);
    web_json_table_row(&table, "路由器BSSID", info.bssid);
    web_json_table_row(&table, "WiFi信道", channel);

    web_json_ok(out, "<table class='info-table'>%s</table>", sbuf_cstr(&table));
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
        web_json_fail(out, "未知的查询类型");
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


/* ---- 原始 AT 指令 ---- */
static void at_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    if (!ctx->arg1[0]) {
        web_json_fail(out, "错误：指令不能为空");
        return;
    }
    log_line("网页端发送AT指令: %s", ctx->arg1);
    char *resp = modem_at(ctx->arg1, 8000);
    if (resp && resp[0]) {
        log_line("模组响应: %s", resp);
        web_json_ok(out, "%s", resp);
    } else {
        web_json_fail(out, "超时或无响应");
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
