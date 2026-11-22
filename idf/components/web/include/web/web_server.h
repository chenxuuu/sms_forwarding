/**
 * @file web_server.h
 * @brief 内嵌 Web 管理界面（esp_http_server）。
 */
#pragma once

#include "esp_err.h"

/** 启动 HTTP 服务（需在 WiFi 连接成功后调用）。 */
esp_err_t web_server_start(void);
