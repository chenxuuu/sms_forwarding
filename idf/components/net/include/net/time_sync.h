#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 启动 SNTP 时间同步（异步，不阻塞）。 */
void time_sync_start(void);
/** 是否已拿到有效时间。 */
bool time_sync_ready(void);
/** 等待时间同步完成，最多等 timeout_ms。返回是否已就绪（超时为 false）。 */
bool time_sync_wait(uint32_t timeout_ms);
