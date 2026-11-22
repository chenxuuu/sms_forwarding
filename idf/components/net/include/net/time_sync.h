#pragma once

#include <stdbool.h>

/** 启动 SNTP 时间同步（异步，不阻塞）。 */
void time_sync_start(void);
/** 是否已拿到有效时间。 */
bool time_sync_ready(void);
