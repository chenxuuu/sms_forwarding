/**
 * @file wifi.h
 * @brief WiFi STA 管理：连接、自动重连、状态查询。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char mac[18];
    char bssid[18];
    int rssi;
    int channel;
} wifi_info_t;

/** 是否已在编译期配置了 WiFi 账号（menuconfig / sdkconfig）。 */
bool wifi_configured(void);

/** 初始化并连接 WiFi（阻塞等待，超时返回 ESP_ERR_TIMEOUT）。 */
esp_err_t wifi_start(uint32_t timeout_ms);

bool wifi_connected(void);
const char *wifi_ip(void);

/** 读取完整状态（供 Web 页面展示）。 */
void wifi_get_info(wifi_info_t *out);

/** 断开并重新连接（供 Web 端“重启 WiFi”使用）。 */
esp_err_t wifi_reconnect(uint32_t timeout_ms);
