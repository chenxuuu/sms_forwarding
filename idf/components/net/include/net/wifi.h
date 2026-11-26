/**
 * @file wifi.h
 * @brief WiFi STA 管理：连接、自动重连、状态查询。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/** 是否有可用账号：网页配网写入的 NVS 优先，其次 menuconfig。 */
bool wifi_configured(void);

/** 当前生效的 SSID（未配置时为空串）。 */
const char *wifi_ssid(void);

/** 写入新账号到 NVS（配网页面 / 网页修改 WiFi 用），下次启动生效。 */
esp_err_t wifi_save_credentials(const char *ssid, const char *pass);

/** 连续启动连接失败次数：达到阈值就转入配网模式，避免永远连不上。 */
int wifi_boot_fails(void);

/** 初始化并连接 WiFi（阻塞等待，超时返回 ESP_ERR_TIMEOUT）。 */
esp_err_t wifi_start(uint32_t timeout_ms);

bool wifi_connected(void);
const char *wifi_ip(void);

/** 读取完整状态（供 Web 页面展示）。 */
void wifi_get_info(wifi_info_t *out);

/** 断开并重新连接（供 Web 端“重启 WiFi”使用）。 */
esp_err_t wifi_reconnect(uint32_t timeout_ms);

/** 立即踢一次重连（非阻塞，供心跳自愈使用）。 */
void wifi_kick(void);

/* ---- 静态 IP ---- */

/**
 * 校验固定 IP 参数：IPv4 格式、掩码连续、IP 与网关同网段、不是网络/广播地址。
 * @param dns 可留空（留空时用网关当 DNS）
 * @param err 失败原因写在这里（可为 NULL）
 */
bool wifi_net_config_check(const char *ip, const char *gw, const char *mask, const char *dns,
                           char *err, size_t errcap);

/** 把配置里的静态 IP 应用到 netif；DHCP 模式下会重新启动 DHCP 客户端。 */
void wifi_apply_net_config(void);

/**
 * 保存静态 IP 配置（写 NVS），下次启动或重连 WiFi 时生效。
 * @param use_static false 表示改回 DHCP（字段保留，方便以后切回来）
 */
esp_err_t wifi_save_net_config(bool use_static, const char *ip, const char *gw, const char *mask,
                               const char *dns, char *err, size_t errcap);