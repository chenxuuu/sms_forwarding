#include "forwarder/forwarder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/util.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem/modem.h"
#include "modem/sms_assembler.h"
#include "notify/notify.h"

#define FWD_QUEUE_DEPTH 4
#define FWD_TEXT_MAX 1600
#define FWD_SENDER_MAX 40
#define FWD_TIME_MAX 32

typedef struct {
    char sender[FWD_SENDER_MAX];
    char timestamp[FWD_TIME_MAX];
    char text[FWD_TEXT_MAX];
} sms_event_t;

static QueueHandle_t s_queue;

static void handle_admin_command(const char *sender, const char *text)
{
    (void)sender;
    char cmd[FWD_TEXT_MAX];
    snprintf(cmd, sizeof(cmd), "%s", text);
    util_trim(cmd);
    log_line("处理管理员命令: %s", cmd);

    if (util_starts_with(cmd, "SMS:")) {
        char *first = strchr(cmd, ':');
        char *second = first ? strchr(first + 1, ':') : NULL;
        if (!second) {
            log_line("SMS 命令格式错误，应为 SMS:号码:内容");
            notify_email("命令执行失败", "SMS 命令格式错误，正确格式: SMS:号码:内容");
            return;
        }
        *second = '\0';
        char *phone = first + 1;
        char *content = second + 1;
        util_trim(phone);
        util_trim(content);

        if (!phone[0] || !content[0]) {
            log_line("SMS 命令参数为空");
            notify_email("命令执行失败", "号码或内容为空");
            return;
        }

        log_line("目标号码: %s", phone);
        log_line("短信内容: %s", content);
        bool ok = modem_send_sms(phone, content, 30000);

        char body[768];
        snprintf(body, sizeof(body),
                 "管理员命令执行结果:\n命令: %.160s\n目标号码: %.32s\n短信内容: %.400s\n执行结果: %s",
                 cmd, phone, content, ok ? "成功" : "失败");
        notify_email(ok ? "短信发送成功" : "短信发送失败", body);
        return;
    }

    if (util_equals_ci(cmd, "RESET")) {
        log_line("执行 RESET 命令，重启模组与整机");
        notify_email("重启命令已执行", "收到 RESET 命令，设备即将重启...");
        notify_drain(15000);
        modem_hard_reset(60000);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    log_line("未知命令: %s", cmd);
}

static void forwarder_task(void *arg)
{
    (void)arg;
    sms_event_t *ev = malloc(sizeof(sms_event_t));
    if (!ev) {
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_queue, ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        log_line("=== 处理短信 ===");
        log_line("发送者: %s", ev->sender);
        log_line("时间戳: %s", ev->timestamp);
        log_line("内容: %s", ev->text);
        log_line("===============");

        if (config_sender_blocked(ev->sender)) {
            log_line("发送者在号码黑名单中，忽略该短信");
            continue;
        }

        if (config_sender_is_admin(ev->sender)) {
            char trimmed[FWD_TEXT_MAX];
            snprintf(trimmed, sizeof(trimmed), "%s", ev->text);
            util_trim(trimmed);
            if (util_starts_with(trimmed, "SMS:") || util_equals_ci(trimmed, "RESET")) {
                handle_admin_command(ev->sender, ev->text);
                continue;
            }
        }

        notify_sms(ev->sender, ev->text, ev->timestamp);
    }
}

static void on_sms_complete(const char *sender, const char *text, const char *timestamp)
{
    if (!s_queue) {
        return;
    }
    sms_event_t ev = {0};
    snprintf(ev.sender, sizeof(ev.sender), "%s", sender);
    snprintf(ev.timestamp, sizeof(ev.timestamp), "%s", timestamp);
    snprintf(ev.text, sizeof(ev.text), "%s", text);
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) {
        log_line("转发队列已满，丢弃短信");
    }
}

void forwarder_init(void)
{
    s_queue = xQueueCreate(FWD_QUEUE_DEPTH, sizeof(sms_event_t));
    if (!s_queue) {
        log_line("转发队列创建失败");
        return;
    }
    xTaskCreate(forwarder_task, "forwarder", 6144, NULL, 3, NULL);
    sms_assembler_init(on_sms_complete);
    log_line("短信转发策略已就绪");
}

void forwarder_notify_device(const char *title, const char *detail, const char *device_url)
{
    char body[256];
    snprintf(body, sizeof(body), "%s\n设备地址: %s", detail ? detail : "",
             device_url ? device_url : "");
    notify_email(title, body);
}
