#include "net/time_sync.h"

#include <stdlib.h>
#include <time.h>

#include "core/log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"

/* 设备面向国内使用；显式设置时区，否则日志/通知里的时间会是 UTC（差 8 小时） */
#define DEVICE_TZ "CST-8"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_synced;
static time_sync_cb_t s_on_ready;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    time_sync_cb_t cb = NULL;
    portENTER_CRITICAL(&s_mux);
    s_synced = true;
    cb = s_on_ready;
    s_on_ready = NULL;
    portEXIT_CRITICAL(&s_mux);

    log_line("NTP 时间同步成功，当前 UTC 时间戳: %lld", (long long)time(NULL));
    if (cb) {
        cb();
    }
}

void time_sync_start(void)
{
    setenv("TZ", DEVICE_TZ, 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.ntsc.ac.cn");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();
    log_line("正在同步 NTP 时间...");
}

bool time_sync_ready(void)
{
    return s_synced || time(NULL) > 100000;
}

void time_sync_on_ready(time_sync_cb_t cb)
{
    if (!cb) {
        return;
    }
    bool already = time(NULL) > 100000;
    bool call_now = false;
    portENTER_CRITICAL(&s_mux);
    if (s_synced || already) {
        call_now = true;
    } else {
        s_on_ready = cb;
    }
    portEXIT_CRITICAL(&s_mux);

    if (call_now) {
        cb();
    }
}
