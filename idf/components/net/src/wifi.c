#include "net/wifi.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_reconnect_timer;
static volatile bool s_connected;
static char s_ip[16] = "0.0.0.0";

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

static void load_credentials(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    snprintf(ssid, ssid_cap, "%s", CONFIG_WIFI_SSID);
    snprintf(pass, pass_cap, "%s", CONFIG_WIFI_PASSWORD);
}

bool wifi_configured(void)
{
    return CONFIG_WIFI_SSID[0] != '\0';
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
    char ssid[33];
    char pass[65];
    load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (strlen(pass) == 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), "wifi", "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), "wifi", "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), "wifi", "disable power save");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "wifi", "wifi start");

    log_line("连接 WiFi: %s", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
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
        snprintf(out->ssid, sizeof(out->ssid), "%s", CONFIG_WIFI_SSID);
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
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    log_line("正在重新连接 WiFi...");
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
