#pragma once

#include <stdbool.h>

typedef void (*time_sync_cb_t)(void);

/** 启动 SNTP 时间同步（异步，不阻塞）。 */
void time_sync_start(void);
/** 是否已拿到有效时间。 */
bool time_sync_ready(void);
/** 注册一次性就绪回调：若已同步立即调用，否则在首次 NTP 同步完成时调用一次。 */
void time_sync_on_ready(time_sync_cb_t cb);
