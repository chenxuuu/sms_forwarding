#include "sched/sched.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <nvs.h>

#include "core/log.h"
#include "esim/esim.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "modem/modem.h"
#include "notify/notify.h"

#include <stdlib.h>

#define SCHED_NS "sms_task"
#define SCHED_POLL_MS 20000
#define SCHED_RETRY_GAP_MS 30000
/* 切卡后等驻网的最长秒数：外卡漫游附着可能要几分钟（见 docs/esim-plan.md §10.5） */
#define SCHED_ESIM_REG_WAIT_S 180

static sched_task_t s_tasks[SCHED_MAX_TASKS];
static int s_count;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void fmt_time(int64_t epoch, char *out, size_t cap)
{
    time_t t = (time_t)epoch;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) {
        snprintf(out, cap, "--");
        return;
    }
    strftime(out, cap, "%m-%d %H:%M", &tmv);
}

static const char *type_name(uint8_t type)
{
    switch (type) {
    case SCHED_TYPE_SMS: return "发送短信";
    case SCHED_TYPE_PING: return "Ping";
    case SCHED_TYPE_HTTP: return "访问网址";
    default: return "未知任务";
    }
}

/* ---- NVS ---- */
static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    out[0] = '\0';
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static void load_tasks(void)
{
    nvs_handle_t h;
    s_count = 0;
    if (nvs_open(SCHED_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    nvs_get_u8(h, "taskCount", &n);
    if (n > SCHED_MAX_TASKS) {
        n = SCHED_MAX_TASKS;
    }
    for (int i = 0; i < n; i++) {
        sched_task_t *t = &s_tasks[i];
        char key[16];
        uint8_t v8 = 0;
        int32_t v32 = 0;
        int64_t v64 = 0;

        snprintf(key, sizeof(key), "t%don", i);
        nvs_get_u8(h, key, &v8);
        t->enabled = v8 != 0;
        snprintf(key, sizeof(key), "t%dty", i);
        v8 = 0;
        nvs_get_u8(h, key, &v8);
        t->type = v8;
        snprintf(key, sizeof(key), "t%dph", i);
        load_str(h, key, t->phone, sizeof(t->phone));
        snprintf(key, sizeof(key), "t%dtx", i);
        load_str(h, key, t->text, sizeof(t->text));
        snprintf(key, sizeof(key), "t%dho", i);
        load_str(h, key, t->host, sizeof(t->host));
        snprintf(key, sizeof(key), "t%dur", i);
        load_str(h, key, t->url, sizeof(t->url));
        snprintf(key, sizeof(key), "t%dic", i);
        load_str(h, key, t->esim_iccid, sizeof(t->esim_iccid));
        snprintf(key, sizeof(key), "t%drs", i);
        v8 = 1; /* 老任务没有这个键：默认切回原卡，与新任务默认行为一致 */
        nvs_get_u8(h, key, &v8);
        t->esim_restore = v8 != 0;
        snprintf(key, sizeof(key), "t%dnx", i);
        nvs_get_i64(h, key, &v64);
        t->next = v64;
        snprintf(key, sizeof(key), "t%drp", i);
        v32 = 0;
        nvs_get_i32(h, key, &v32);
        t->repeat_days = v32;
        snprintf(key, sizeof(key), "t%dla", i);
        v64 = 0;
        nvs_get_i64(h, key, &v64);
        t->last = v64;
        s_count = i + 1;
    }
    nvs_close(h);
}

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(SCHED_NS, NVS_READWRITE, &h) != ESP_OK) {
        log_line("定时任务保存失败：NVS 打开异常");
        return;
    }
    nvs_erase_all(h);
    nvs_set_u8(h, "taskCount", (uint8_t)s_count);
    for (int i = 0; i < s_count; i++) {
        const sched_task_t *t = &s_tasks[i];
        char key[16];
        snprintf(key, sizeof(key), "t%don", i);
        nvs_set_u8(h, key, t->enabled ? 1 : 0);
        snprintf(key, sizeof(key), "t%dty", i);
        nvs_set_u8(h, key, t->type);
        snprintf(key, sizeof(key), "t%dph", i);
        nvs_set_str(h, key, t->phone);
        snprintf(key, sizeof(key), "t%dtx", i);
        nvs_set_str(h, key, t->text);
        snprintf(key, sizeof(key), "t%dho", i);
        nvs_set_str(h, key, t->host);
        snprintf(key, sizeof(key), "t%dur", i);
        nvs_set_str(h, key, t->url);
        snprintf(key, sizeof(key), "t%dic", i);
        nvs_set_str(h, key, t->esim_iccid);
        snprintf(key, sizeof(key), "t%drs", i);
        nvs_set_u8(h, key, t->esim_restore ? 1 : 0);
        snprintf(key, sizeof(key), "t%dnx", i);
        nvs_set_i64(h, key, t->next);
        snprintf(key, sizeof(key), "t%drp", i);
        nvs_set_i32(h, key, t->repeat_days);
        snprintf(key, sizeof(key), "t%dla", i);
        nvs_set_i64(h, key, t->last);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ---- 对外接口 ---- */
int sched_task_count(void)
{
    lock();
    int n = s_count;
    unlock();
    return n;
}

bool sched_task_get(int index, sched_task_t *out)
{
    bool ok = false;
    lock();
    if (index >= 0 && index < s_count && out) {
        *out = s_tasks[index];
        ok = true;
    }
    unlock();
    return ok;
}

int sched_task_save(int index, const sched_task_t *task)
{
    if (!task) {
        return -1;
    }
    int slot = -1;
    lock();
    if (index < 0) {
        if (s_count < SCHED_MAX_TASKS) {
            slot = s_count++;
        }
    } else if (index < s_count) {
        slot = index;
    }
    if (slot >= 0) {
        s_tasks[slot] = *task;
        persist_locked();
    }
    unlock();
    return slot;
}

bool sched_task_delete(int index)
{
    bool ok = false;
    lock();
    if (index >= 0 && index < s_count) {
        for (int i = index; i < s_count - 1; i++) {
            s_tasks[i] = s_tasks[i + 1];
        }
        s_count--;
        ok = true;
        persist_locked();
    }
    unlock();
    return ok;
}

/* ---- 执行 ---- */
static bool run_once(const sched_task_t *t, char *detail, size_t cap)
{
    switch (t->type) {
    case SCHED_TYPE_SMS: {
        bool ok = modem_send_sms(t->phone, t->text, 30000);
        snprintf(detail, cap, "发送短信到 %s：%s", t->phone, ok ? "成功" : "失败");
        return ok;
    }
    case SCHED_TYPE_PING: {
        const char *host = t->host[0] ? t->host : "qq.com";
        modem_ping_result_t r = {0};
        bool ok = modem_ping_ip(host, &r, 25000);
        snprintf(detail, cap, "Ping %s：%s", host,
                 r.detail[0] ? r.detail : (ok ? "成功" : "失败"));
        return ok;
    }
    case SCHED_TYPE_HTTP: {
        modem_http_result_t r = {0};
        bool ok = modem_http_get(t->url, &r, 45000);
        snprintf(detail, cap, "访问 %s：%s", t->url,
                 r.detail[0] ? r.detail : (ok ? "成功" : "失败"));
        return ok;
    }
    default:
        snprintf(detail, cap, "未知任务类型 %u", (unsigned)t->type);
        return false;
    }
}

/* ---- eSIM 切卡执行 ---- */

/* 等模组重新驻网；软重启内部已等过一轮，这里给漫游卡留足附着时间 */
static bool wait_modem_ready(int timeout_s)
{
    for (int waited = 0; waited < timeout_s; waited += 5) {
        if (modem_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    return modem_ready();
}

/* 切换到指定 profile 并软重启模组等驻网；失败时 esim_last_error() 有原因 */
static bool esim_switch_to(const char *iccid)
{
    int ecc = -1;
    esim_err_t e = esim_enable_profile(iccid, true, &ecc);
    if (e == ESIM_ERR_CARD && ecc == 5) {
        log_line("eSIM 切换返回 CAT busy，改为不带刷新重试");
        e = esim_enable_profile(iccid, false, &ecc);
    }
    if (e != ESIM_OK) {
        return false;
    }
    modem_soft_reset(90000);
    return wait_modem_ready(SCHED_ESIM_REG_WAIT_S);
}

/**
 * 带 eSIM 切换的执行：先切到任务指定的配置，驻网成功后才做保号动作，
 * 结束后按任务设置切回原卡。任何一步失败都中止，绝不在错的卡上执行
 * （发短信/Ping/HTTP 都会花目标卡以外那张卡的钱、保错号）。
 */
static bool run_task(const sched_task_t *t, char *detail, size_t cap)
{
    if (!t->esim_iccid[0]) {
        return run_once(t, detail, cap);
    }

    esim_profile_t *list = calloc(ESIM_MAX_PROFILES, sizeof(*list));
    if (!list) {
        snprintf(detail, cap, "内存不足，无法读取 eSIM 配置列表，已中止");
        return false;
    }
    char eid[40] = {0};
    int count = -1;
    esim_read_all(eid, sizeof(eid), list, ESIM_MAX_PROFILES, &count);
    if (count < 0) {
        snprintf(detail, cap, "读取 eSIM 配置失败：%s，已中止（未在任何卡上执行）",
                 esim_last_error());
        free(list);
        return false;
    }

    char orig_iccid[ESIM_ICCID_MAX] = {0};
    char orig_name[ESIM_NAME_MAX] = {0};
    char target_name[ESIM_NAME_MAX] = {0};
    bool target_found = false;
    bool target_active = false;
    for (int i = 0; i < count; i++) {
        if (list[i].state == 1) {
            snprintf(orig_iccid, sizeof(orig_iccid), "%s", list[i].iccid);
            snprintf(orig_name, sizeof(orig_name), "%s", list[i].name);
        }
        if (strcmp(list[i].iccid, t->esim_iccid) == 0) {
            target_found = true;
            target_active = list[i].state == 1;
            snprintf(target_name, sizeof(target_name), "%s", list[i].name);
        }
    }
    free(list);

    if (!target_found) {
        snprintf(detail, cap, "目标 eSIM 配置（ICCID %s）不在卡内，已中止（未在任何卡上执行）",
                 t->esim_iccid);
        return false;
    }

    bool switched = false;
    if (!target_active) {
        log_line("定时任务：切换 eSIM 到「%s」（%s）后执行保号", target_name, t->esim_iccid);
        if (!esim_switch_to(t->esim_iccid)) {
            char why[128];
            snprintf(why, sizeof(why), "%s", esim_last_error());
            /* 切换/驻网失败时尽力切回原卡，避免设备停在一张没驻网的卡上收不到短信 */
            if (orig_iccid[0] && strcmp(orig_iccid, t->esim_iccid) != 0) {
                log_line("定时任务：切卡失败，尝试切回原卡「%s」", orig_name);
                esim_switch_to(orig_iccid);
            }
            snprintf(detail, cap, "切换到目标 eSIM「%s」失败：%s，已中止（未在错的卡上执行）",
                     target_name, why[0] ? why : "驻网超时");
            return false;
        }
        switched = true;
    } else {
        log_line("定时任务：目标 eSIM「%s」已是当前配置，直接执行保号", target_name);
    }

    char action[240] = {0};
    bool ok = run_once(t, action, sizeof(action));

    char tail[200] = {0};
    if (switched && t->esim_restore) {
        if (orig_iccid[0]) {
            if (esim_switch_to(orig_iccid)) {
                snprintf(tail, sizeof(tail), "；已切回原卡「%s」", orig_name);
            } else {
                snprintf(tail, sizeof(tail), "；警告：切回原卡「%s」失败，请到 eSIM 页手动切回",
                         orig_name);
                log_line("定时任务：切回原卡「%s」失败：%s", orig_name, esim_last_error());
            }
        } else {
            snprintf(tail, sizeof(tail), "；警告：执行前没有启用的配置，未能切回");
        }
    } else if (switched) {
        snprintf(tail, sizeof(tail), "；按任务设置留在目标卡「%s」", target_name);
    }
    snprintf(detail, cap, "eSIM「%s」：%s%s", target_name, action, tail);
    return ok;
}

static void sched_loop(void *arg)
{
    (void)arg;
    bool wait_logged = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SCHED_POLL_MS));

        time_t now = time(NULL);
        if (now < 1600000000) {
            if (!wait_logged) {
                log_line("定时任务等待系统时间同步（NTP）后再执行");
                wait_logged = true;
            }
            continue;
        }
        wait_logged = false;

        for (int i = 0; i < SCHED_MAX_TASKS; i++) {
            sched_task_t t;
            if (!sched_task_get(i, &t)) {
                break;
            }
            if (!t.enabled || t.next <= 0 || (int64_t)now < t.next) {
                continue;
            }

            int late_min = (int)(((int64_t)now - t.next) / 60);
            char due[32];
            fmt_time(t.next, due, sizeof(due));
            log_line("定时任务 #%d 到期（计划 %s，已超时 %d 分钟）%s", i + 1, due, late_min,
                     late_min >= 3 ? "，补执行" : "");

            if (!modem_ready()) {
                log_line("模组未就绪，任务 #%d 保持待执行，稍后再试", i + 1);
                continue;
            }

            char detail[420] = {0};
            bool ok = run_task(&t, detail, sizeof(detail));
            if (!ok && !t.esim_iccid[0]) {
                /* 涉及话费/流量，只谨慎重试一次；
                 * 带切卡的任务不盲重试（切卡+驻网耗时长，失败原因多为确定性的） */
                log_line("任务 #%d 失败，%d 秒后重试一次", i + 1, SCHED_RETRY_GAP_MS / 1000);
                vTaskDelay(pdMS_TO_TICKS(SCHED_RETRY_GAP_MS));
                ok = run_once(&t, detail, sizeof(detail));
            }

            time_t done = time(NULL);
            int64_t next_at = 0;
            lock();
            s_tasks[i].last = (int64_t)done;
            if (s_tasks[i].repeat_days > 0) {
                int64_t step = (int64_t)s_tasks[i].repeat_days * 86400;
                int64_t next = s_tasks[i].next + step;
                while (next <= (int64_t)done) {
                    next += step; /* 错过的周期不补跑，避免一次性连发 */
                }
                s_tasks[i].next = next;
                s_tasks[i].enabled = true;
                next_at = next;
            } else {
                s_tasks[i].next = 0;
                s_tasks[i].enabled = false;
            }
            persist_locked();
            unlock();

            char when[32];
            fmt_time((int64_t)done, when, sizeof(when));
            char body[720];
            if (next_at > 0) {
                char nx[32];
                fmt_time(next_at, nx, sizeof(nx));
                snprintf(body, sizeof(body),
                         "任务 #%d（%s）\n%s\n执行时间: %s\n下次执行: %s", i + 1,
                         type_name(t.type), detail, when, nx);
            } else {
                snprintf(body, sizeof(body), "任务 #%d（%s）\n%s\n执行时间: %s\n该任务已结束（不循环）",
                         i + 1, type_name(t.type), detail, when);
            }
            log_line("定时任务 #%d %s：%s", i + 1, ok ? "完成" : "失败", detail);
            notify_system(ok ? "定时任务已完成" : "定时任务失败", body);
        }
    }
}

void sched_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    load_tasks();
    log_line("定时任务已加载：%d 个（上限 %d）", s_count, SCHED_MAX_TASKS);
    /* 栈 8KB：带切卡的任务要叠加 eSIM 读列表/切换与软重启的调用帧 */
    if (xTaskCreate(sched_loop, "sched", 8192, NULL, 2, NULL) != pdPASS) {
        log_line("定时任务线程创建失败");
    }
}
