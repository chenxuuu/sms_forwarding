#include "modem/sms_assembler.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PART_TEXT_MAX 512

typedef struct {
    bool used;
    int ref;
    char sender[PDU_SENDER_MAX];
    char timestamp[PDU_TIME_MAX];
    int total;
    int received;
    int64_t first_ms;
    bool valid[SMS_CONCAT_MAX_PARTS];
    char parts[SMS_CONCAT_MAX_PARTS][PART_TEXT_MAX];
} concat_group_t;

static concat_group_t s_groups[SMS_CONCAT_MAX_GROUPS];
static sms_complete_cb_t s_cb;
static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_timer;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void group_clear(concat_group_t *g)
{
    g->used = false;
    g->received = 0;
    g->sender[0] = '\0';
    g->timestamp[0] = '\0';
    for (int i = 0; i < SMS_CONCAT_MAX_PARTS; i++) {
        g->valid[i] = false;
        g->parts[i][0] = '\0';
    }
}

static void deliver(concat_group_t *g)
{
    /* 合并结果最大 5KB，放堆上：这个函数可能运行在栈很小的 esp_timer 任务里 */
    char *merged = calloc(1, SMS_CONCAT_MAX_PARTS * PART_TEXT_MAX);
    if (!merged) {
        log_line("内存不足，无法合并长短信");
        return;
    }
    size_t merged_cap = SMS_CONCAT_MAX_PARTS * PART_TEXT_MAX;
    size_t used = 0;
    for (int i = 0; i < g->total; i++) {
        if (g->valid[i]) {
            size_t n = strlen(g->parts[i]);
            if (used + n + 1 < merged_cap) {
                memcpy(merged + used, g->parts[i], n);
                used += n;
                merged[used] = '\0';
            }
        } else {
            char miss[32];
            int n = snprintf(miss, sizeof(miss), "[缺失分段%d]", i + 1);
            if (used + (size_t)n + 1 < merged_cap) {
                memcpy(merged + used, miss, (size_t)n);
                used += (size_t)n;
                merged[used] = '\0';
            }
        }
    }
    if (s_cb) {
        s_cb(g->sender, merged, g->timestamp);
    }
    free(merged);
}

static void assembler_tick(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return;
    }
    int64_t now = now_ms();
    for (int i = 0; i < SMS_CONCAT_MAX_GROUPS; i++) {
        concat_group_t *g = &s_groups[i];
        if (g->used && now - g->first_ms >= SMS_CONCAT_TIMEOUT_MS) {
            log_line("长短信超时，强制转发不完整消息（ref=%d %d/%d）", g->ref,
                     g->received, g->total);
            deliver(g);
            group_clear(g);
        }
    }
    xSemaphoreGive(s_mutex);
}

void sms_assembler_init(sms_complete_cb_t cb)
{
    s_cb = cb;
    memset(s_groups, 0, sizeof(s_groups));
    s_mutex = xSemaphoreCreateMutex();
    const esp_timer_create_args_t args = {
        .callback = assembler_tick,
        .name = "sms_concat",
    };
    esp_timer_create(&args, &s_timer);
    esp_timer_start_periodic(s_timer, 1000 * 1000);
}

void sms_assembler_reset(void)
{
    for (int i = 0; i < SMS_CONCAT_MAX_GROUPS; i++) {
        group_clear(&s_groups[i]);
    }
}

static concat_group_t *group_for(int ref, const char *sender, int total)
{
    for (int i = 0; i < SMS_CONCAT_MAX_GROUPS; i++) {
        concat_group_t *g = &s_groups[i];
        if (g->used && g->ref == ref && strcmp(g->sender, sender) == 0) {
            return g;
        }
    }
    concat_group_t *slot = NULL;
    for (int i = 0; i < SMS_CONCAT_MAX_GROUPS; i++) {
        if (!s_groups[i].used) {
            slot = &s_groups[i];
            break;
        }
    }
    if (!slot) {
        log_line("长短信缓存已满，覆盖最老的槽位");
        slot = &s_groups[0];
        for (int i = 1; i < SMS_CONCAT_MAX_GROUPS; i++) {
            if (s_groups[i].first_ms < slot->first_ms) {
                slot = &s_groups[i];
            }
        }
    }
    group_clear(slot);
    slot->used = true;
    slot->ref = ref;
    snprintf(slot->sender, sizeof(slot->sender), "%s", sender);
    slot->total = total > SMS_CONCAT_MAX_PARTS ? SMS_CONCAT_MAX_PARTS : total;
    slot->first_ms = now_ms();
    return slot;
}

void sms_assembler_feed(const sms_deliver_t *msg)
{
    if (!msg->concat_total || msg->concat_total <= 1 || msg->concat_part <= 0) {
        if (s_cb) {
            s_cb(msg->sender, msg->text, msg->timestamp);
        }
        return;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    concat_group_t *g = group_for(msg->concat_ref, msg->sender, msg->concat_total);
    if (!g->timestamp[0]) {
        snprintf(g->timestamp, sizeof(g->timestamp), "%s", msg->timestamp);
    }

    int part = msg->concat_part - 1;
    if (part >= 0 && part < SMS_CONCAT_MAX_PARTS) {
        if (!g->valid[part]) {
            g->valid[part] = true;
            snprintf(g->parts[part], PART_TEXT_MAX, "%s", msg->text);
            g->received++;
            log_line("收到长短信分段 %d/%d（ref=%d，已收 %d）", msg->concat_part,
                     msg->concat_total, msg->concat_ref, g->received);
        } else {
            log_line("分段 %d 重复，忽略", msg->concat_part);
        }
    }

    if (g->received >= g->total) {
        log_line("长短信已收齐，开始合并转发");
        deliver(g);
        group_clear(g);
    }

    xSemaphoreGive(s_mutex);
}
