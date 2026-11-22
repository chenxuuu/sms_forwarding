#include "net/time_sync.h"

#include <time.h>

#include "core/log.h"
#include "esp_sntp.h"

static volatile bool s_synced;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    log_line("NTP 时间同步成功，当前 UTC 时间戳: %lld", (long long)time(NULL));
}

void time_sync_start(void)
{
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
