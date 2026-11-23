#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SCHED_MAX_TASKS 8

#define SCHED_TYPE_SMS 1
#define SCHED_TYPE_PING 2
#define SCHED_TYPE_HTTP 3

typedef struct {
    bool enabled;
    uint8_t type;        /* SCHED_TYPE_* */
    char phone[24];      /* 短信目标号码 */
    char text[160];      /* 短信内容 */
    char host[64];       /* Ping 主机 */
    char url[160];       /* 访问网址 */
    int64_t next;        /* 下次执行时间（epoch 秒）；0=不再执行 */
    int32_t repeat_days; /* 0=不循环；N>0 表示执行后 N 天再执行 */
    int64_t last;        /* 上次执行时间（epoch 秒），0=从未执行 */
} sched_task_t;

/** 加载 NVS 中的任务并启动调度任务。 */
void sched_init(void);

/** 当前任务数量与快照（越界返回 false）。 */
int sched_task_count(void);
bool sched_task_get(int index, sched_task_t *out);

/** index<0 追加新任务，否则更新；返回新任务的下标，失败返回 -1。 */
int sched_task_save(int index, const sched_task_t *task);

/** 删除任务，返回是否成功。 */
bool sched_task_delete(int index);
