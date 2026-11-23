#include "notify/notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/config.h"
#include "core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "notify/email.h"
#include "notify/push.h"

#define NOTIFY_QUEUE_DEPTH 4
#define NOTIFY_SUBJECT_MAX 200
#define NOTIFY_BODY_MAX 2048
#define NOTIFY_SENDER_MAX 64
#define NOTIFY_TIME_MAX 32
#define NOTIFY_MESSAGE_MAX 1600

typedef struct {
    bool do_email;
    bool do_push;
    bool is_notice; /* true=系统通知（title+body），false=短信（sender+message） */
    char sender[NOTIFY_SENDER_MAX];
    char timestamp[NOTIFY_TIME_MAX];
    char subject[NOTIFY_SUBJECT_MAX];
    char body[NOTIFY_BODY_MAX];
    char message[NOTIFY_MESSAGE_MAX];
} notify_job_t;

static QueueHandle_t s_queue;
static volatile int s_pending;

static void notify_worker(void *arg)
{
    (void)arg;
    notify_job_t *job = malloc(sizeof(notify_job_t));
    if (!job) {
        log_line("通知任务内存不足");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_queue, job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job->do_push) {
            push_send_all(job->sender, job->message, job->timestamp, job->is_notice);
        }
        if (job->do_email) {
            email_send(job->subject, job->body);
        }
        __sync_sub_and_fetch(&s_pending, 1);
    }
}

void notify_init(void)
{
    s_queue = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(notify_job_t));
    if (!s_queue) {
        log_line("通知队列创建失败");
        return;
    }
    xTaskCreate(notify_worker, "notify", 8192, NULL, 3, NULL);
    log_line("通知任务已启动");
}

static void enqueue(notify_job_t *job)
{
    if (!s_queue) {
        return;
    }
    __sync_add_and_fetch(&s_pending, 1);
    if (xQueueSend(s_queue, job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        __sync_sub_and_fetch(&s_pending, 1);
        log_line("通知队列已满，丢弃本次通知");
    }
}

void notify_sms(const char *sender, const char *message, const char *timestamp)
{
    app_config_t *cfg = config_lock();
    bool do_email = config_mail_active(cfg);
    bool do_push = false;
    for (int i = 0; i < cfg->channel_count && !do_push; i++) {
        do_push = config_push_channel_valid(&cfg->channels[i]);
    }
    config_unlock();

    if (!do_email && !do_push) {
        log_line("未配置邮件或推送通道，短信仅记录在日志中");
        return;
    }

    notify_job_t *job = calloc(1, sizeof(notify_job_t));
    if (!job) {
        log_line("通知内存不足，丢弃");
        return;
    }
    job->do_email = do_email;
    job->do_push = do_push;
    snprintf(job->sender, sizeof(job->sender), "%s", sender);
    snprintf(job->timestamp, sizeof(job->timestamp), "%s", timestamp);
    snprintf(job->message, sizeof(job->message), "%s", message);
    snprintf(job->subject, sizeof(job->subject), "短信%.40s,%.150s", sender, message);
    snprintf(job->body, sizeof(job->body), "来自：%.40s，时间：%.24s，内容：%.1900s", sender,
             timestamp, message);
    enqueue(job);
    free(job);
}

void notify_system(const char *title, const char *body)
{
    app_config_t *cfg = config_lock();
    bool do_email = config_mail_active(cfg);
    bool do_push = false;
    for (int i = 0; i < cfg->channel_count && !do_push; i++) {
        do_push = config_push_channel_valid(&cfg->channels[i]);
    }
    config_unlock();

    if (!do_email && !do_push) {
        log_line("未配置邮件或推送通道，系统通知仅记录在日志中");
        return;
    }

    notify_job_t *job = calloc(1, sizeof(notify_job_t));
    if (!job) {
        log_line("通知内存不足，丢弃");
        return;
    }
    job->do_email = do_email;
    job->do_push = do_push;
    job->is_notice = true;
    snprintf(job->sender, sizeof(job->sender), "%s", title);
    snprintf(job->subject, sizeof(job->subject), "%s", title);
    snprintf(job->body, sizeof(job->body), "%s", body);
    snprintf(job->message, sizeof(job->message), "%s", body);

    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(job->timestamp, sizeof(job->timestamp), "%y/%m/%d %H:%M:%S", &tm_local);

    enqueue(job);
    free(job);
}

void notify_email(const char *subject, const char *body)
{
    app_config_t *cfg = config_lock();
    bool do_email = config_mail_active(cfg);
    config_unlock();
    if (!do_email) {
        log_line("邮件配置不完整，跳过发送");
        return;
    }

    notify_job_t *job = calloc(1, sizeof(notify_job_t));
    if (!job) {
        return;
    }
    job->do_email = true;
    snprintf(job->subject, sizeof(job->subject), "%s", subject);
    snprintf(job->body, sizeof(job->body), "%s", body);
    enqueue(job);
    free(job);
}

bool notify_drain(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_pending > 0 && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_pending == 0;
}