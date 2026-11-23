#include "net/wifi_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/wifi.h"

#define PORTAL_AP_MAX 20
#define PORTAL_BODY_MAX 256

typedef struct {
    char ssid[33];
    int rssi;
} portal_ap_t;

static httpd_handle_t s_server;
static char s_ap_ssid[33];
static portal_ap_t s_aps[PORTAL_AP_MAX];
static int s_ap_count;

/* ---- 小工具 ---- */

static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < cap; p++) {
        switch (*p) {
        case '&': o += snprintf(out + o, cap - o, "&amp;"); break;
        case '<': o += snprintf(out + o, cap - o, "&lt;"); break;
        case '>': o += snprintf(out + o, cap - o, "&gt;"); break;
        case '"': o += snprintf(out + o, cap - o, "&quot;"); break;
        case '\'': o += snprintf(out + o, cap - o, "&#39;"); break;
        default: out[o++] = *p; break;
        }
    }
    out[o] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** 从 x-www-form-urlencoded 里取一个字段，做 URL 解码。 */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < cap) {
                int hi, lo;
                if (*v == '+') {
                    out[o++] = ' ';
                    v++;
                } else if (*v == '%' && (hi = hex_val(v[1])) >= 0 && (lo = hex_val(v[2])) >= 0) {
                    out[o++] = (char)(hi * 16 + lo);
                    v += 3;
                } else {
                    out[o++] = *v++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static void scan_nearby(void)
{
    s_ap_count = 0;
    wifi_scan_config_t cfg = {0};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        return;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return;
    }
    if (num > PORTAL_AP_MAX) {
        num = PORTAL_AP_MAX;
    }
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) {
        return;
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        for (int i = 0; i < (int)num && s_ap_count < PORTAL_AP_MAX; i++) {
            if (recs[i].ssid[0] == '\0') {
                continue;
            }
            bool dup = false;
            for (int j = 0; j < s_ap_count; j++) {
                if (strcmp(s_aps[j].ssid, (const char *)recs[i].ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            snprintf(s_aps[s_ap_count].ssid, sizeof(s_aps[s_ap_count].ssid), "%s",
                     (const char *)recs[i].ssid);
            s_aps[s_ap_count].rssi = recs[i].rssi;
            s_ap_count++;
        }
        /* 信号强的排前面，一眼就能认出家里的网络 */
        for (int i = 1; i < s_ap_count; i++) {
            portal_ap_t cur = s_aps[i];
            int j = i - 1;
            while (j >= 0 && s_aps[j].rssi < cur.rssi) {
                s_aps[j + 1] = s_aps[j];
                j--;
            }
            s_aps[j + 1] = cur;
        }
    }
    free(recs);
}

/* ---- 页面 ---- */

static const char *PAGE_HEAD =
    "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>短信转发器 · 配网</title><style>"
    "body{margin:0;background:#0f1116;font:15px/1.6 -apple-system,BlinkMacSystemFont,'Segoe UI',"
    "'PingFang SC','Microsoft YaHei',sans-serif;color:#e6e9f0;display:flex;justify-content:center;"
    "padding:28px 16px}"
    ".card{width:100%;max-width:420px;background:#171a22;border:1px solid #262b36;border-radius:16px;"
    "padding:22px 20px 18px;box-shadow:0 12px 40px rgba(0,0,0,.45)}"
    "h1{font-size:19px;margin:0 0 6px}p.sub{color:#8b95a8;font-size:13px;margin:0 0 18px}"
    "label{display:block;font-size:12px;color:#8b95a8;margin:14px 0 6px}"
    "input{width:100%;box-sizing:border-box;background:#0f1116;border:1px solid #2b3140;color:#e6e9f0;"
    "border-radius:10px;padding:11px 12px;font-size:15px;outline:none}"
    "input:focus{border-color:#5b8cff}"
    "button{width:100%;margin-top:20px;padding:12px;border:0;border-radius:10px;font-size:15px;"
    "font-weight:600;color:#fff;background:linear-gradient(120deg,#5b8cff,#8b5cf6);cursor:pointer}"
    "button:active{opacity:.85}"
    ".tip{margin-top:16px;font-size:12px;color:#6f7a8d;line-height:1.7}"
    ".id{font-family:ui-monospace,monospace;color:#8b95a8}"
    "</style></head><body><div class=card>"
    "<h1>连接 WiFi</h1>"
    "<p class=sub>选一个 2.4G 网络并输入密码，保存后设备会自动重启</p>"
    "<form method=post action='/save'>"
    "<label for=ssid>WiFi 名称（SSID）</label>"
    "<input id=ssid name=ssid list=aps autocomplete=off required placeholder='家里的 2.4G WiFi'>"
    "<datalist id=aps>";

static const char *PAGE_TAIL_FMT =
    "</datalist>"
    "<label for=pass>WiFi 密码</label>"
    "<input id=pass name=pass type=password autocomplete=off placeholder='开放网络留空'>"
    "<button type=submit>保存并重启</button>"
    "</form>"
    "<div class=tip>只支持 2.4G 网络；设备热点 %s 没有密码，配好网后会自动关闭。<br>"
    "当前设备 <span class=id>%s</span></div>"
    "</div></body></html>";

static esp_err_t send_page(httpd_req_t *req)
{
    size_t cap = 2048 + (size_t)s_ap_count * 128;
    char *page = malloc(cap);
    if (!page) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    size_t len = 0;
    len += snprintf(page + len, cap - len, "%s", PAGE_HEAD);
    for (int i = 0; i < s_ap_count; i++) {
        char esc[160];
        html_escape(s_aps[i].ssid, esc, sizeof(esc));
        int n = snprintf(page + len, cap - len, "<option value=\"%s\">", esc);
        if (n > 0) {
            len += (size_t)n;
        }
        if (len >= cap - 1) {
            len = cap - 1;
            break;
        }
    }
    snprintf(page + len, cap - len, PAGE_TAIL_FMT, s_ap_ssid, s_ap_ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static esp_err_t handler_index(httpd_req_t *req)
{
    scan_nearby();
    return send_page(req);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[PORTAL_BODY_MAX];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    body[total] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 WiFi 名称");
        return ESP_FAIL;
    }
    if (!form_field(body, "pass", pass, sizeof(pass))) {
        pass[0] = '\0';
    }

    esp_err_t err = wifi_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        log_line("配网保存失败: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_FAIL;
    }
    log_line("配网完成：SSID=%s，1.5 秒后重启", ssid);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<!doctype html><meta charset=utf-8>"
                            "<meta name=viewport content='width=device-width,initial-scale=1'>"
                            "<body style='background:#0f1116;color:#e6e9f0;font:15px/1.7 "
                            "-apple-system,sans-serif;padding:40px 20px;text-align:center'>"
                            "<h2 style='font-size:18px'>已保存</h2>"
                            "<p style='color:#8b95a8'>设备正在重启并连接 WiFi，稍后看路由器里分配的地址。</p>"
                            "</body>");

    xTaskCreate(reboot_task, "portal_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* 手机连上热点后会探测连通性，全部指回首页，省得手动输地址 */
static esp_err_t handler_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- 启动 ---- */

static esp_err_t portal_netif_up(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") && !esp_netif_create_default_wifi_ap()) {
        return ESP_FAIL;
    }
    /* 已经跑过 wifi_start（连不上已保存的账号时）就不用再 init 一遍 */
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t wifi_portal_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    esp_err_t err = portal_netif_up();
    if (err != ESP_OK) {
        log_line("配网模式初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SMS-Forwarder-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    /* 不设置 wildcard 匹配函数时，通配 URI 不生效，手机探测连通性会一直 404 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return ESP_FAIL;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handler_index};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = handler_save};
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = handler_catchall};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &any);

    log_line("配网模式：热点 %s（无密码），浏览器打开 http://192.168.4.1/", s_ap_ssid);
    return ESP_OK;
}

const char *wifi_portal_ssid(void)
{
    return s_ap_ssid;
}
