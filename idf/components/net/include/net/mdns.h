/**
 * @file mdns.h
 * @brief mDNS 服务发布，让设备可以用固定域名访问（与 DHCP 地址无关）。
 *
 * 设备名默认取 MAC 后两字节（如 `sms-9e3c`），因此同一内网里多台设备天然不冲突；
 * 也可以在 menuconfig 里用 `CONFIG_MDNS_NAME` 指定，但此时需要自己保证唯一。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

/** 计算设备名（mDNS 主机名 / DHCP 主机名）。 */
void net_device_name(char *out, size_t cap);

/** 发布 mDNS 服务（需在 WiFi 连接成功后调用）。 */
esp_err_t net_mdns_start(void);
