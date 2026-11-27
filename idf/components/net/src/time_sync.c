#include "net/time_sync.h"

#include <stdlib.h>
#include <time.h>

#include "core/log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 设备面向国内使用；显式设置时区，否则日志/通知里的时间会是 UTC（差 8 小时） */
#define DEVICE_TZ "CST-8"

static volatile bool s_synced;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    log_line("NTP 时间同步成功，当前 UTC 时间戳: %lld", (long long)time(NULL));
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

bool time_sync_wait(uint32_t timeout_ms)
{
    /* 只给启动通知这类"必须先有时间"的场景用；200ms 轮询足够，不必再加信号量 */
    for (uint32_t waited = 0; !time_sync_ready() && waited < timeout_ms; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return time_sync_ready();
}
