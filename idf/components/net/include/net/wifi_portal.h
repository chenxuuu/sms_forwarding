/**
 * @file wifi_portal.h
 * @brief 首次配网：开热点 + 网页表单，把 WiFi 账号写进 NVS。
 *
 * 触发条件（由 app_main 判断）：
 *  - 没有任何 WiFi 账号（既没 menuconfig，也没配过网）
 *  - 连续多次启动都连不上已保存的账号（多半是路由器改密码了）
 *
 * 保存成功后立刻重启，用新账号连接。
 */
#pragma once

#include "esp_err.h"

/** 启动配网热点与 HTTP 服务（端口 80），成功后阻塞返回，调用方自行等待。 */
esp_err_t wifi_portal_start(void);

/** 当前配网热点的 SSID（未启动时为空串）。 */
const char *wifi_portal_ssid(void);
