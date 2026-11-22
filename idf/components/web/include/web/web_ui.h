#pragma once

#include <stddef.h>

#include "esp_err.h"

/**
 * 页面输出回调：每凑满一块就交给调用方写出（通常是 httpd_resp_send_chunk），
 * 避免在只有 ~300KB RAM 的 C3 上申请 40KB 连续缓冲区。
 */
typedef esp_err_t (*web_ui_sink_t)(void *user, const char *data, size_t len);

/** 渲染首页并以分块方式输出。 */
esp_err_t web_ui_render_to(web_ui_sink_t sink, void *user);
