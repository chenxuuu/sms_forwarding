#include "modem/modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "modem/at_client.h"
#include "modem/modem_hw.h"
#include "modem/pdu.h"
#include "modem/sms_assembler.h"

#define MODEM_QUEUE_DEPTH 4
#define MODEM_JOB_ARG_MAX 128
#define MODEM_JOB_TEXT_MAX 1024
#define MODEM_AT_RESP_MAX 4096

typedef enum {
    JOB_INIT = 0,
    JOB_SEND_SMS,
    JOB_HARD_RESET,
    JOB_SOFT_RESET,
    JOB_AT,
    JOB_PING,
} modem_job_type_t;

typedef struct {
    modem_job_type_t type;
    char arg[MODEM_JOB_ARG_MAX];
    char text[MODEM_JOB_TEXT_MAX];
    uint32_t timeout_ms;
    bool ok;
    char *result;
    modem_ping_result_t ping;
    SemaphoreHandle_t done;
    volatile bool abandoned; /* 调用方等待超时：作业仍归 worker，由 worker 释放 */
} modem_job_t;

/* 队列元素是作业指针：工作线程改写同一块内存，调用方才能拿到 ok/result */
static QueueHandle_t s_queue;
static modem_job_t *job_new(modem_job_type_t type);
static volatile bool s_ready;
static char s_model[MODEM_MODEL_MAX];
static char s_version[MODEM_VERSION_MAX];

/* ---- URC：短信上报 ---- */
static bool s_expect_pdu;

static void handle_urc(const char *line)
{
    if (s_expect_pdu) {
        if (line[0] == '\0') {
            return; /* 空行忽略，继续等 PDU */
        }
        s_expect_pdu = false;
        if (util_is_hex_string(line)) {
            log_line("收到 PDU: %s", line);
            sms_deliver_t *msg = calloc(1, sizeof(*msg));
            if (!msg) {
                log_line("内存不足，丢弃该条短信");
                return;
            }
            if (!pdu_decode_deliver(line, msg)) {
                log_line("PDU 解析失败: %s", line);
                free(msg);
                return;
            }
            log_line("收到短信 %s: %s（%s）", msg->sender, msg->text, msg->timestamp);
            sms_assembler_feed(msg);
            free(msg);
            return;
        }
        /* 状态错位（+CMT 与 PDU 之间夹了其它行）：复位后按普通 URC 继续解析 */
        log_line("期望 PDU 却收到: %s", line);
    }

    if (util_starts_with(line, "+CMT:")) {
        s_expect_pdu = true;
        return;
    }
    if (util_starts_with(line, "+CMTI:")) {
        log_line("收到 +CMTI（短信存储通知）: %s", line);
        return;
    }
    /* 只记录真正的主动上报，避免 AT 应答行被当成 URC 刷屏 */
    if (line[0] == '+' || util_starts_with(line, "RDY") || util_starts_with(line, "RING") ||
        util_starts_with(line, "NO CARRIER")) {
        log_line("模组> %s", line);
    }
}

/* ---- 初始化流程 ---- */
static bool wait_at_ready(void)
{
    for (int i = 0; i < 30; i++) {
        if (at_command_ok("AT", 1000)) {
            return true;
        }
        log_line("AT 未响应，重试...");
        led_blink(500);
    }
    return false;
}

static void parse_ati(const char *resp)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", resp);
    int line_no = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        util_trim(line);
        if (!line[0] || strcmp(line, "ATI") == 0 || strcmp(line, "OK") == 0) {
            continue;
        }
        line_no++;
        if (line_no == 1) {
            /* 制造商，忽略 */
        } else if (line_no == 2) {
            snprintf(s_model, sizeof(s_model), "%s", line);
        } else if (line_no == 3) {
            snprintf(s_version, sizeof(s_version), "%s", line);
        }
    }
}

static bool wait_registered(void)
{
    for (int i = 0; i < 30; i++) {
        char resp[256] = {0};
        if (at_command("AT+CEREG?", resp, sizeof(resp), 2000) != AT_RESULT_TIMEOUT) {
            if (strstr(resp, ",1") || strstr(resp, ",5")) {
                return true;
            }
        }
        log_line("等待网络注册... (%d/30)", i + 1);
        led_blink(500);
    }
    return false;
}

static void do_init(void)
{
    s_ready = false;
    s_expect_pdu = false;
    sms_assembler_reset();

    modem_power_cycle();

    int cycles = 0;
    while (!wait_at_ready()) {
        if (++cycles >= 3) {
            log_line("模组连续 3 次无响应，放弃本次初始化（可稍后点“硬重启模组”）");
            return;
        }
        log_line("模组仍无响应，尝试重新上电");
        modem_power_cycle();
    }
    log_line("模组 AT 响应正常");

    char resp[512] = {0};
    at_command("ATI", resp, sizeof(resp), 2000);
    log_line("ATI 响应: %s", resp);
    parse_ati(resp);
    log_line("模组型号: %s，固件: %s", s_model[0] ? s_model : "未知",
             s_version[0] ? s_version : "未知");

    /* ML307Y 的 AT+CGACT 有缺陷，跳过 */
    if (strcmp(s_model, "ML307Y") != 0) {
        while (!at_command_ok("AT+CGACT=0,1", 5000)) {
            log_line("禁用数据连接失败，重试...");
            led_blink(500);
        }
        log_line("已禁用数据连接（AT+CGACT=0,1），避免消耗流量");
    } else {
        log_line("ML307Y 跳过 AT+CGACT=0,1");
    }

    while (!at_command_ok("AT+CNMI=2,2,0,0,0", 1000)) {
        log_line("设置短信上报失败，重试...");
        led_blink(300);
    }
    log_line("短信直报已开启（CNMI=2,2,0,0,0）");

    while (!at_command_ok("AT+CMGF=0", 1000)) {
        log_line("设置 PDU 模式失败，重试...");
        led_blink(300);
    }
    log_line("PDU 模式已设置（CMGF=0）");

    if (wait_registered()) {
        log_line("网络已注册，模组就绪");
        s_ready = true;
        led_set(false);
    } else {
        log_line("网络注册超时（无 SIM 卡或信号差），模组功能不可用");
        s_ready = false;
        led_set(true);
    }
}

/* 解析 +MPING: <result>,<ip>,<len>,<time>,<ttl> */
static bool parse_mping(const char *line, char *detail, size_t cap)
{
    const char *p = strchr(line, ':');
    if (!p) {
        snprintf(detail, cap, "Ping 响应格式异常");
        return false;
    }
    p++;
    while (*p == ' ') p++;

    char buf[192];
    snprintf(buf, sizeof(buf), "%s", p);
    char *fields[6] = {0};
    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && count < 6; tok = strtok_r(NULL, ",", &save)) {
        util_trim(tok);
        fields[count++] = tok;
    }
    if (count < 2) {
        snprintf(detail, cap, "Ping 响应不完整: %s", line);
        return false;
    }

    int result = atoi(fields[0]);
    if (count >= 4 && (result == 0 || result == 1)) {
        const char *ip = fields[1];
        const char *time_ms = fields[3];
        const char *ttl = count >= 5 ? fields[4] : "N/A";
        snprintf(detail, cap, "目标: %s, 延迟: %sms, TTL: %s", ip, time_ms, ttl);
        return true;
    }
    snprintf(detail, cap, "Ping 超时或目标不可达（错误码 %d）", result);
    return false;
}

/* ---- 任务与作业 ---- */
static void modem_worker(void *arg)
{
    (void)arg;
    modem_job_t *job = NULL;
    while (1) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE || !job) {
            continue;
        }
        job->ok = false;
        job->result = NULL;
        switch (job->type) {
        case JOB_INIT:
            do_init();
            job->ok = s_ready;
            break;
        case JOB_SEND_SMS: {
            char pdu[1024];
            int tpdu_len = pdu_encode_submit(job->arg, job->text, pdu, sizeof(pdu));
            if (tpdu_len == -2) {
                log_line("短信内容过长：单条最多 70 个汉字（或 160 个英文字符）");
                break;
            }
            if (tpdu_len < 0) {
                log_line("PDU 编码失败（号码或内容非法）");
                break;
            }
            log_line("发送短信到 %s，TPDU %d 字节", job->arg, tpdu_len);
            job->ok = at_send_pdu(pdu, tpdu_len, job->timeout_ms);
            log_line(job->ok ? "短信发送成功" : "短信发送失败");
            break;
        }
        case JOB_AT:
            job->result = malloc(MODEM_AT_RESP_MAX);
            if (job->result) {
                at_command(job->arg, job->result, MODEM_AT_RESP_MAX, job->timeout_ms);
                job->ok = job->result[0] != '\0';
            }
            break;
        case JOB_PING: {
            log_line("激活数据连接（AT+CGACT=1,1）...");
            at_command_ok("AT+CGACT=1,1", 10000);
            vTaskDelay(pdMS_TO_TICKS(500));
            char line[256] = {0};
            bool got = at_command_wait_for("AT+MPING=\"8.8.8.8\",30,1", "+MPING:", line,
                                           sizeof(line), job->timeout_ms);
            if (got) {
                log_line("收到 MPING 结果: %s", line);
                job->ping.success = parse_mping(line, job->ping.detail, sizeof(job->ping.detail));
            } else {
                snprintf(job->ping.detail, sizeof(job->ping.detail), "Ping 超时或目标不可达");
            }
            log_line("关闭数据连接（AT+CGACT=0,1）...");
            at_command_ok("AT+CGACT=0,1", 5000);
            job->ok = job->ping.success;
            break;
        }
        case JOB_HARD_RESET:
            log_line("正在硬重启模组（EN 断电重启）...");
            do_init();
            job->ok = s_ready;
            break;
        case JOB_SOFT_RESET:
            log_line("正在软重启模组（AT+CFUN=1,1）...");
            job->ok = at_command_ok("AT+CFUN=1,1", 15000);
            if (!job->ok) {
                log_line("软重启失败，改为硬重启");
            }
            do_init();
            job->ok = s_ready;
            break;
        }
        if (!job->done || job->abandoned) {
            /* 无人等待 / 调用方已超时放弃：由 worker 释放，绝不回写已释放内存 */
            free(job->result);
            if (job->done) {
                vSemaphoreDelete(job->done);
            }
            free(job);
        } else {
            xSemaphoreGive(job->done);
        }
    }
}

esp_err_t modem_start(void)
{
    modem_hw_init();
    ESP_RETURN_ON_ERROR(at_client_init(), "modem", "AT 通道初始化失败");
    at_set_urc_handler(handle_urc);

    s_queue = xQueueCreate(MODEM_QUEUE_DEPTH, sizeof(modem_job_t *));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(modem_worker, "modem", 8192, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    modem_job_t *job = job_new(JOB_INIT);
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    xQueueSend(s_queue, &job, 0); /* done == NULL：worker 执行完后自行释放 */
    return ESP_OK;
}

bool modem_ready(void)
{
    return s_ready;
}

const char *modem_model(void)
{
    return s_model;
}

static modem_job_t *job_new(modem_job_type_t type)
{
    modem_job_t *job = calloc(1, sizeof(*job));
    if (job) {
        job->type = type;
    }
    return job;
}

typedef enum {
    SUBMIT_OK = 0,      /* 已完成，调用方读取结果后自行释放 */
    SUBMIT_TIMEOUT,     /* 等待超时，作业可能仍在队列/执行中：调用方不得再碰 job */
    SUBMIT_FAILED,      /* 未入队，调用方负责释放 */
} submit_result_t;

static submit_result_t submit(modem_job_t *job, uint32_t wait_ms)
{
    if (!s_queue || !job) {
        return SUBMIT_FAILED;
    }
    job->done = xSemaphoreCreateBinary();
    if (!job->done) {
        return SUBMIT_FAILED;
    }
    modem_job_t *ptr = job;
    if (xQueueSend(s_queue, &ptr, pdMS_TO_TICKS(2000)) != pdTRUE) {
        vSemaphoreDelete(job->done);
        job->done = NULL;
        return SUBMIT_FAILED;
    }
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        vSemaphoreDelete(job->done);
        job->done = NULL;
        return SUBMIT_OK;
    }

    /* 超时：作业可能仍在队列里或正在执行，所有权交给 worker，避免悬空指针 */
    job->abandoned = true;
    if (xSemaphoreTake(job->done, 0) == pdTRUE) {
        /* 极小概率：worker 恰在此刻完成，自己回收 */
        vSemaphoreDelete(job->done);
        job->done = NULL;
        job->abandoned = false;
        return SUBMIT_OK;
    }
    return SUBMIT_TIMEOUT;
}

bool modem_send_sms(const char *phone, const char *text, uint32_t timeout_ms)
{
    modem_job_t *job = job_new(JOB_SEND_SMS);
    if (!job) {
        return false;
    }
    job->timeout_ms = timeout_ms;
    snprintf(job->arg, sizeof(job->arg), "%s", phone ? phone : "");
    snprintf(job->text, sizeof(job->text), "%s", text ? text : "");
    submit_result_t rc = submit(job, timeout_ms + 8000);
    bool ok = (rc == SUBMIT_OK) && job->ok;
    if (rc != SUBMIT_TIMEOUT) {
        free(job); /* 超时时作业仍归 worker */
    }
    return ok;
}

char *modem_at(const char *cmd, uint32_t timeout_ms)
{
    modem_job_t *job = job_new(JOB_AT);
    if (!job) {
        return NULL;
    }
    job->timeout_ms = timeout_ms;
    snprintf(job->arg, sizeof(job->arg), "%s", cmd ? cmd : "");
    submit_result_t rc = submit(job, timeout_ms + 3000);
    if (rc == SUBMIT_TIMEOUT) {
        return NULL; /* 作业归 worker，结果丢弃 */
    }
    char *result = (rc == SUBMIT_OK) ? job->result : NULL;
    free(job);
    return result;
}

static bool reset_job(modem_job_type_t type, uint32_t wait_ms)
{
    modem_job_t *job = job_new(type);
    if (!job) {
        return false;
    }
    submit_result_t rc = submit(job, wait_ms);
    bool ok = rc == SUBMIT_OK && job->ok;
    if (rc != SUBMIT_TIMEOUT) {
        free(job);
    }
    return ok;
}

bool modem_hard_reset(uint32_t timeout_ms)
{
    return reset_job(JOB_HARD_RESET, timeout_ms);
}

bool modem_soft_reset(uint32_t timeout_ms)
{
    return reset_job(JOB_SOFT_RESET, timeout_ms);
}

bool modem_reinit(uint32_t timeout_ms)
{
    return reset_job(JOB_INIT, timeout_ms);
}

bool modem_ping(modem_ping_result_t *out, uint32_t timeout_ms)
{
    modem_job_t *job = job_new(JOB_PING);
    if (!job) {
        return false;
    }
    job->timeout_ms = timeout_ms;
    submit_result_t rc = submit(job, timeout_ms + 20000);
    bool ok = rc == SUBMIT_OK && job->ping.success;
    if (out) {
        if (rc == SUBMIT_OK) {
            *out = job->ping;
        } else if (!out->detail[0]) {
            snprintf(out->detail, sizeof(out->detail), "操作超时");
        }
    }
    if (rc != SUBMIT_TIMEOUT) {
        free(job);
    }
    return ok;
}
