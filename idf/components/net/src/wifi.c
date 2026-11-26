#include "net/wifi.h"

#include <stdio.h>
#include <string.h>

#include "lwip/inet.h"

#include "core/config.h"
#include "core/log.h"
#include "net/mdns.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_reconnect_timer;
static volatile bool s_connected;
static char s_ip[16] = "0.0.0.0";

/* 账号优先级：NVS（网页配网 / 运行时修改） > menuconfig 编译期常量 */
#define WIFI_NVS_NS "sms_wifi"
#define WIFI_KEY_SSID "ssid"
#define WIFI_KEY_PASS "pass"
#define WIFI_KEY_FAILS "fails"
static char s_ssid[33];
static char s_pass[65];
static bool s_loaded;
static int s_fails;
static bool s_fails_loaded;

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            log_line("WiFi 已连上 AP，等待获取 IP...");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_ip[0] = '\0';
            snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
            if (s_events) {
                xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
            }
            log_line("WiFi 断开，3 秒后重连");
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, 3 * 1000 * 1000);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        log_line("WiFi 已连接，IP: %s", s_ip);
        if (s_events) {
            xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        }
    }
}

static void fails_load(void)
{
    if (s_fails_loaded) {
        return;
    }
    s_fails_loaded = true;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, WIFI_KEY_FAILS, &v) == ESP_OK && v > 0) {
            s_fails = (int)v;
        }
        nvs_close(h);
    }
}

static void fails_store(int value)
{
    s_fails = value;
    s_fails_loaded = true;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, WIFI_KEY_FAILS, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_credentials(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    bool from_nvs = false;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_ssid);
        if (nvs_get_str(h, WIFI_KEY_SSID, s_ssid, &len) == ESP_OK && s_ssid[0] != '\0') {
            from_nvs = true;
            len = sizeof(s_pass);
            if (nvs_get_str(h, WIFI_KEY_PASS, s_pass, &len) != ESP_OK) {
                s_pass[0] = '\0';
            }
        }
        nvs_close(h);
    }
    if (!from_nvs) {
        char ssid[33];
        char pass[65];
        snprintf(ssid, sizeof(ssid), "%s", CONFIG_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", CONFIG_WIFI_PASSWORD);
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", pass);
        /* 编译期账号（menuconfig）以前只在内存里用，从没落盘。
           一旦 OTA 到不带 CONFIG_WIFI_* 的固件（比如 CI 包），设备就会变回"未配置"
           并进配网模式——看起来就像配置全丢了。首次使用就写进 NVS，之后任何固件都能连上。 */
        if (ssid[0] != '\0') {
            if (wifi_save_credentials(ssid, pass) == ESP_OK) {
                log_line("WiFi 账号来自编译期配置，已写入 NVS 保存（%s）", ssid);
            } else {
                log_line("WiFi 账号来自编译期配置，写入 NVS 失败");
            }
        }
    }
}

bool wifi_configured(void)
{
    load_credentials();
    return s_ssid[0] != '\0';
}

const char *wifi_ssid(void)
{
    load_credentials();
    return s_ssid;
}

int wifi_boot_fails(void)
{
    fails_load();
    return s_fails;
}

esp_err_t wifi_save_credentials(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, WIFI_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, WIFI_KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(h, WIFI_KEY_FAILS, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
        s_loaded = true;
        s_fails = 0;
        s_fails_loaded = true;
    }
    return err;
}

/* ---- 静态 IP ---- */

static bool parse_ipv4(const char *s, uint32_t *out)
{
    if (!s || s[0] == '\0') {
        return false;
    }
    uint32_t v = esp_ip4addr_aton(s);
    if (v == 0) {
        return false; /* 0.0.0.0 或格式错误 */
    }
    *out = v;
    return true;
}

/* 掩码必须「前面全 1、后面全 0」：按网络字节序从最高位看，不能出现 0 之后又有 1。 */
static bool mask_contiguous(uint32_t m)
{
    /* esp_ip4addr_aton 返回的是网络字节序，按位判断连续性前要转成本机序 */
    uint32_t v = ntohl(m);
    if (v == 0 || v == 0xFFFFFFFFu) {
        return false;
    }
    bool zero_seen = false;
    for (int i = 0; i < 32; i++) {
        bool bit = (v >> (31 - i)) & 1u;
        if (!bit) {
            zero_seen = true;
        } else if (zero_seen) {
            return false;
        }
    }
    return true;
}

bool wifi_net_config_check(const char *ip, const char *gw, const char *mask, const char *dns,
                           char *err, size_t errcap)
{
    if (err && errcap) {
        err[0] = '\0';
    }
    uint32_t a = 0, g = 0, m = 0, d = 0;
    if (!parse_ipv4(ip, &a)) {
        snprintf(err, errcap, "IP 地址格式不正确");
        return false;
    }
    if (!parse_ipv4(gw, &g)) {
        snprintf(err, errcap, "默认网关格式不正确");
        return false;
    }
    if (!parse_ipv4(mask, &m)) {
        snprintf(err, errcap, "子网掩码格式不正确");
        return false;
    }
    if (!mask_contiguous(m)) {
        snprintf(err, errcap, "子网掩码不连续（例如 255.255.255.0）");
        return false;
    }
    if ((a & m) != (g & m)) {
        snprintf(err, errcap, "IP 与网关不在同一网段");
        return false;
    }
    if (a == g) {
        snprintf(err, errcap, "IP 不能与网关相同");
        return false;
    }
    uint32_t host = a & ~m;
    if (host == 0) {
        snprintf(err, errcap, "IP 不能是网络地址");
        return false;
    }
    if (host == ~m) {
        snprintf(err, errcap, "IP 不能是广播地址");
        return false;
    }
    if (dns && dns[0] && !parse_ipv4(dns, &d)) {
        snprintf(err, errcap, "DNS 地址格式不正确");
        return false;
    }
    return true;
}

void wifi_apply_net_config(void)
{
    if (!s_netif) {
        return;
    }
    bool use_static;
    char ip[16], gw[16], mask[16], dns[16];
    app_config_t *cfg = config_lock();
    use_static = cfg->net_static;
    snprintf(ip, sizeof(ip), "%s", cfg->net_ip);
    snprintf(gw, sizeof(gw), "%s", cfg->net_gw);
    snprintf(mask, sizeof(mask), "%s", cfg->net_mask);
    snprintf(dns, sizeof(dns), "%s", cfg->net_dns);
    config_unlock();

    if (!use_static) {
        esp_err_t err = esp_netif_dhcpc_start(s_netif);
        if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            log_line("WiFi 地址由 DHCP 自动获取");
        } else {
            log_line("启用 DHCP 失败: %s", esp_err_to_name(err));
        }
        return;
    }

    char reason[96] = {0};
    if (!wifi_net_config_check(ip, gw, mask, dns, reason, sizeof(reason))) {
        log_line("静态 IP 配置无效（%s），改用 DHCP", reason);
        esp_netif_dhcpc_start(s_netif);
        return;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        log_line("停止 DHCP 失败: %s，改用 DHCP", esp_err_to_name(err));
        esp_netif_dhcpc_start(s_netif);
        return;
    }

    esp_netif_ip_info_t info = {0};
    info.ip.addr = esp_ip4addr_aton(ip);
    info.gw.addr = esp_ip4addr_aton(gw);
    info.netmask.addr = esp_ip4addr_aton(mask);
    err = esp_netif_set_ip_info(s_netif, &info);
    if (err != ESP_OK) {
        log_line("设置静态 IP 失败: %s，改用 DHCP", esp_err_to_name(err));
        esp_netif_dhcpc_start(s_netif);
        return;
    }

    /* DHCP 已停用，DNS 不会自动下发；没填 DNS 就用网关（绝大多数路由器都做 DNS 转发） */
    esp_netif_dns_info_t dinfo = {0};
    dinfo.ip.type = ESP_IPADDR_TYPE_V4;
    dinfo.ip.u_addr.ip4.addr = esp_ip4addr_aton((dns[0] != '\0') ? dns : gw);
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dinfo);
    log_line("使用静态 IP %s，网关 %s，掩码 %s，DNS %s", ip, gw, mask,
             (dns[0] != '\0') ? dns : gw);
}

esp_err_t wifi_save_net_config(bool use_static, const char *ip, const char *gw, const char *mask,
                               const char *dns, char *err, size_t errcap)
{
    if (use_static && !wifi_net_config_check(ip, gw, mask, dns, err, errcap)) {
        return ESP_ERR_INVALID_ARG;
    }
    app_config_t *cfg = config_lock();
    cfg->net_static = use_static;
    if (use_static) {
        snprintf(cfg->net_ip, sizeof(cfg->net_ip), "%s", ip ? ip : "");
        snprintf(cfg->net_gw, sizeof(cfg->net_gw), "%s", gw ? gw : "");
        snprintf(cfg->net_mask, sizeof(cfg->net_mask), "%s",
                 (mask && mask[0]) ? mask : "255.255.255.0");
        snprintf(cfg->net_dns, sizeof(cfg->net_dns), "%s", dns ? dns : "");
    }
    config_save_locked();
    config_unlock();
    return ESP_OK;
}

esp_err_t wifi_start(uint32_t timeout_ms)
{
    if (!wifi_configured()) {
        log_line("WiFi 未配置：请执行 idf.py menuconfig → 短信转发器配置 → WiFi SSID / 密码");
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), "wifi", "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), "wifi", "wifi init");

    /* DHCP 主机名与 mDNS 保持一致：路由器客户端列表里也能看到设备名 */
    {
        char host[33];
        net_device_name(host, sizeof(host));
        esp_netif_set_hostname(s_netif, host);
        log_line("设备名: %s（mDNS 地址 http://%s.local/）", host, host);
    }

    /* 固定 IP 要在 esp_wifi_start 之前设好，DHCP 客户端必须先停掉 */
    wifi_apply_net_config();

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            event_handler, NULL, NULL),
                        "wifi", "register wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            event_handler, NULL, NULL),
                        "wifi", "register ip event");

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), "wifi", "timer");

    wifi_config_t wifi_cfg = {0};
    load_credentials();
    strlcpy((char *)wifi_cfg.sta.ssid, s_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, s_pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (strlen(s_pass) == 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), "wifi", "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), "wifi", "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), "wifi", "disable power save");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "wifi", "wifi start");

    log_line("连接 WiFi: %s", s_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        if (wifi_boot_fails() != 0) {
            fails_store(0);
        }
        return ESP_OK;
    }
    fails_load();
    fails_store(s_fails + 1);
    log_line("WiFi 连接超时（连续第 %d 次）", s_fails);
    return ESP_ERR_TIMEOUT;
}

bool wifi_connected(void)
{
    return s_connected;
}

const char *wifi_ip(void)
{
    return s_ip;
}

void wifi_get_info(wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = s_connected;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(out->ssid, sizeof(out->ssid), "%s", (const char *)ap.ssid);
        snprintf(out->bssid, sizeof(out->bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        out->rssi = ap.rssi;
        out->channel = ap.primary;
    } else {
        snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    }

    esp_netif_ip_info_t ip = {0};
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        snprintf(out->gateway, sizeof(out->gateway), IPSTR, IP2STR(&ip.gw));
        snprintf(out->netmask, sizeof(out->netmask), IPSTR, IP2STR(&ip.netmask));
    }
    esp_netif_dns_info_t dns = {0};
    if (s_netif && esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        snprintf(out->dns, sizeof(out->dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    }
}

void wifi_kick(void)
{
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_connect();
}

esp_err_t wifi_reconnect(uint32_t timeout_ms)
{
    if (!s_events) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 网页上改了静态 IP / 切回 DHCP 后，重连时生效 */
    wifi_apply_net_config();

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    log_line("正在重新连接 WiFi...");
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
