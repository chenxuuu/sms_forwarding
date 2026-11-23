#include "ota/ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/log.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net/wifi.h"
#include "net/wifi_portal.h"
#include "notify/notify.h"
#include "nvs.h"

#define OTA_NVS_NS "sms_ota"
#define OTA_HEAD_MAX 4096        /* 首块缓存：够放 image header + app desc */
#define OTA_MIN_IMAGE 0x10000    /* 小于 64KB 肯定不是有效固件 */
#define OTA_MANIFEST_MAX 4096
#define OTA_HEAP_MIN (6 * 1024) /* 最大连续块低于这个值不做 TLS 握手（IN 缓冲已降到 4 KB） */
#define OTA_FIRST_CHECK_S 180 /* 等开机通知的 TLS 结束再查，否则抢不到连续内存 */
#define OTA_PERIOD_S (24 * 3600)
#define OTA_REFRESH_MIN_S (20 * 3600)
#define OTA_RETRY_S 3600
#define OTA_RETRY_MAX 3
#define OTA_CONFIRM_WAIT_MS 45000
#define OTA_CONFIRM_LIMIT_MS 120000

static SemaphoreHandle_t s_lock;
static ota_info_t s_info;

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static size_t s_size;
static size_t s_got;
static size_t s_head_len;
static uint8_t *s_head;
static bool s_started;
static char s_new_version[32];

static esp_timer_handle_t s_daily;
static esp_timer_handle_t s_retry;
static int s_retry_count;

static TaskHandle_t s_worker;
static SemaphoreHandle_t s_wake;
static volatile bool s_checking;

/* ---------- 小工具 ---------- */

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

bool ota_version_is_release(const char *version)
{
    if (!version || strlen(version) != 13) {
        return false;
    }
    for (int i = 0; i < 13; i++) {
        if (i == 8) {
            if (version[i] != '.') {
                return false;
            }
        } else if (version[i] < '0' || version[i] > '9') {
            return false;
        }
    }
    return true;
}

const char *ota_version(void)
{
    return s_info.current;
}

bool ota_busy(void)
{
    bool busy;
    lock();
    busy = s_info.state == OTA_STATE_RECEIVING || s_info.state == OTA_STATE_VERIFYING;
    unlock();
    return busy;
}

void ota_get_info(ota_info_t *out)
{
    if (!out) {
        return;
    }
    lock();
    *out = s_info;
    unlock();
}

static void set_message(const char *text)
{
    snprintf(s_info.message, sizeof(s_info.message), "%s", text ? text : "");
}

/* ---------- NVS 缓存 ---------- */

static void cache_save(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i64(h, "last", s_info.checked_at);
    nvs_set_str(h, "latest", s_info.latest);
    nvs_set_str(h, "url", s_info.url);
    nvs_set_str(h, "notes", s_info.notes);
    nvs_commit(h);
    nvs_close(h);
}

static void cache_load(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int64_t last = 0;
    if (nvs_get_i64(h, "last", &last) == ESP_OK) {
        s_info.checked_at = last;
    }
    size_t len = sizeof(s_info.latest);
    nvs_get_str(h, "latest", s_info.latest, &len);
    len = sizeof(s_info.url);
    nvs_get_str(h, "url", s_info.url, &len);
    len = sizeof(s_info.notes);
    nvs_get_str(h, "notes", s_info.notes, &len);
    nvs_close(h);

    if (ota_version_is_release(s_info.current) && ota_version_is_release(s_info.latest)) {
        s_info.has_update = strcmp(s_info.latest, s_info.current) > 0;
    }
}

/* ---------- manifest ---------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} fetch_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_DATA && ctx && ctx->len + (size_t)ev->data_len < ctx->cap) {
        memcpy(ctx->buf + ctx->len, ev->data, (size_t)ev->data_len);
        ctx->len += (size_t)ev->data_len;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

static bool fetch_manifest(char *buf, size_t cap)
{
    fetch_ctx_t ctx = {.buf = buf, .cap = cap, .len = 0};
    buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .event_handler = http_event,
        .user_data = &ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        log_line("版本检查：HTTP 客户端创建失败");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int sock_errno = esp_http_client_get_errno(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        log_line("版本检查请求失败: %s（errno=%d，可用内存 %u KB）", esp_err_to_name(err),
                 sock_errno, (unsigned)(esp_get_free_heap_size() / 1024));
        return false;
    }
    if (status != 200) {
        log_line("版本检查返回 HTTP %d", status);
        return false;
    }
    return ctx.len > 0;
}

/** 从扁平 JSON 里取一个字符串字段（值里只处理 \\ 和 \\" 两种转义）。 */
static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

/* ---------- 每日检查 ---------- */

static void retry_cb(void *arg);

static void schedule_retry(void)
{
    if (!s_retry || s_retry_count >= OTA_RETRY_MAX) {
        if (s_retry_count >= OTA_RETRY_MAX) {
            log_line("版本检查今日重试已达 %d 次，明天再试", OTA_RETRY_MAX);
        }
        return;
    }
    s_retry_count++;
    esp_timer_stop(s_retry);
    esp_timer_start_once(s_retry, (uint64_t)OTA_RETRY_S * 1000000ULL);
    log_line("版本检查将在 %d 分钟后重试（第 %d 次）", OTA_RETRY_S / 60, s_retry_count);
}

static esp_err_t check_latest(bool allow_retry)
{
    if (!wifi_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    if (now < 1600000000) {
        log_line("系统时间未同步，跳过版本检查（证书校验需要正确时间）");
        return ESP_ERR_INVALID_STATE;
    }
    /* TLS 握手要 20 KB 以上的连续内存；开机通知的邮件/推送可能正在握手，
     * 这时等几秒让它释放，而不是直接报"内存不足"让用户重试。 */
    unsigned free_kb = 0;
    unsigned block_kb = 0;
    for (int i = 0; i < 15; i++) {
        free_kb = (unsigned)(esp_get_free_heap_size() / 1024);
        block_kb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= OTA_HEAP_MIN) {
            break;
        }
        if (i == 0) {
            log_line("版本检查等待内存释放（空闲 %u KB，最大连续 %u KB）", free_kb, block_kb);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < OTA_HEAP_MIN) {
        log_line("可用内存不足（空闲 %u KB，最大连续 %u KB），跳过版本检查", free_kb, block_kb);
        return ESP_ERR_NO_MEM;
    }
    log_line("版本检查：空闲 %u KB，最大连续 %u KB", free_kb, block_kb);

    char *buf = malloc(OTA_MANIFEST_MAX);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    char version[32] = {0};
    char url[192] = {0};
    char notes[160] = {0};

    if (!fetch_manifest(buf, OTA_MANIFEST_MAX)) {
        goto done;
    }
    if (!json_str(buf, "version", version, sizeof(version))) {
        log_line("版本检查：响应里没有 version 字段");
        goto done;
    }
    if (!ota_version_is_release(version)) {
        log_line("版本检查：远端版本号 %s 格式非法，忽略", version);
        goto done;
    }
    json_str(buf, "url", url, sizeof(url));
    json_str(buf, "notes", notes, sizeof(notes));

    lock();
    snprintf(s_info.latest, sizeof(s_info.latest), "%s", version);
    snprintf(s_info.url, sizeof(s_info.url), "%s", url);
    snprintf(s_info.notes, sizeof(s_info.notes), "%s", notes);
    s_info.checked_at = now;
    s_info.has_update = ota_version_is_release(s_info.current) &&
                        strcmp(version, s_info.current) > 0;
    cache_save();
    unlock();

    log_line("版本检查完成：当前 %s，最新 %s%s", s_info.current, version,
             s_info.has_update ? "（有新版本，可在升级页上传）" : "");
    s_retry_count = 0;
    result = ESP_OK;

done:
    free(buf);
    if (result != ESP_OK && allow_retry) {
        schedule_retry();
    }
    return result;
}

esp_err_t ota_request_check(void)
{
    if (ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_checking) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_wake) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreGive(s_wake) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/** 检查专用工作任务：esp_timer / httpd 的栈都不够跑 TLS 握手。 */
static void worker_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_wake, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_checking = true;
        lock();
        s_info.checking = true;
        unlock();

        check_latest(true);

        lock();
        s_info.checking = false;
        unlock();
        s_checking = false;
    }
}

static void daily_cb(void *arg)
{
    (void)arg;
    if (ota_busy()) {
        return;
    }
    time_t now = time(NULL);
    if (s_info.checked_at && now - s_info.checked_at < OTA_REFRESH_MIN_S) {
        return;
    }
    s_retry_count = 0; /* 新的一天，重试次数重新计数 */
    ota_request_check();
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (ota_busy() || !wifi_connected()) {
        return;
    }
    ota_request_check();
}

/* ---------- 写入 ---------- */

static void free_head(void)
{
    if (s_head) {
        free(s_head);
        s_head = NULL;
    }
    s_head_len = 0;
}

static void reset_write_state(void)
{
    free_head();
    s_handle = 0;
    s_started = false;
    s_size = 0;
    s_got = 0;
    s_part = NULL;
    s_new_version[0] = '\0';
}

esp_err_t ota_begin(size_t image_size)
{
    lock();
    if (s_info.state == OTA_STATE_RECEIVING || s_info.state == OTA_STATE_VERIFYING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        set_message("没有可用的 OTA 分区，固件可能未按两槽布局烧录");
        s_info.state = OTA_STATE_ERROR;
        unlock();
        log_line("固件升级：找不到 OTA 分区（分区表不是 app0/app1 布局？）");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size < OTA_MIN_IMAGE || image_size > part->size) {
        snprintf(s_info.message, sizeof(s_info.message), "固件大小 %u 字节超出分区容量（%u）",
                 (unsigned)image_size, (unsigned)part->size);
        s_info.state = OTA_STATE_ERROR;
        unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    reset_write_state();
    s_size = image_size;
    s_head = malloc(OTA_HEAD_MAX);
    if (!s_head) {
        set_message("内存不足，无法开始升级");
        s_info.state = OTA_STATE_ERROR;
        unlock();
        return ESP_ERR_NO_MEM;
    }
    s_info.state = OTA_STATE_RECEIVING;
    s_info.progress = 0;
    set_message("正在接收固件");
    unlock();

    log_line("固件升级：开始接收，共 %u 字节，写入分区 %s", (unsigned)image_size, part->label);
    return ESP_OK;
}

/** 首块数据齐了之后：校验镜像头，然后擦分区并开写。 */
static esp_err_t start_partition(void)
{
    if (s_head_len < 0x100) {
        set_message("固件数据太短");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_app_desc_t desc;
    memcpy(&desc, s_head + 0x20, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        set_message("这不是可用的 OTA 固件（可能是整包 merged.bin），请上传 *-ota.bin");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(desc.project_name, "sms_forwarder") != 0) {
        set_message("这是整包固件（含 bootloader），请上传 *-ota.bin");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (desc.version[0] == '\0') {
        set_message("固件版本号为空，请重新下载");
        return ESP_ERR_INVALID_RESPONSE;
    }
    snprintf(s_new_version, sizeof(s_new_version), "%s", desc.version);
    snprintf(s_info.next_version, sizeof(s_info.next_version), "%s", desc.version);

    s_part = esp_ota_get_next_update_partition(NULL);
    esp_err_t err = esp_ota_begin(s_part, s_size, &s_handle);
    if (err != ESP_OK) {
        snprintf(s_info.message, sizeof(s_info.message), "写入准备失败: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;

    err = esp_ota_write(s_handle, s_head, s_head_len);
    if (err != ESP_OK) {
        snprintf(s_info.message, sizeof(s_info.message), "写入失败: %s", esp_err_to_name(err));
        return err;
    }
    log_line("固件升级：镜像校验通过（%s），已擦除分区 %s 并开始写入", s_new_version, s_part->label);
    free_head();
    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_OK;
    }

    lock();
    if (s_info.state != OTA_STATE_RECEIVING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_got + len > s_size) {
        set_message("收到的数据超过声明的长度");
        s_info.state = OTA_STATE_ERROR;
        free_head();
        if (s_started) {
            esp_ota_abort(s_handle);
            s_started = false;
        }
        unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    esp_err_t err = ESP_OK;

    if (!s_started) {
        size_t room = OTA_HEAD_MAX - s_head_len;
        size_t take = left < room ? left : room;
        memcpy(s_head + s_head_len, p, take);
        s_head_len += take;
        p += take;
        left -= take;

        if (s_head_len >= OTA_HEAD_MAX || s_got + take == s_size) {
            err = start_partition();
            if (err != ESP_OK) {
                s_info.state = OTA_STATE_ERROR;
                free_head();
                unlock();
                log_line("固件升级失败：%s", s_info.message);
                return err;
            }
        }
    }

    if (left > 0 && s_started) {
        err = esp_ota_write(s_handle, p, left);
        if (err != ESP_OK) {
            snprintf(s_info.message, sizeof(s_info.message), "写入失败: %s", esp_err_to_name(err));
            s_info.state = OTA_STATE_ERROR;
            esp_ota_abort(s_handle);
            s_started = false;
            unlock();
            return err;
        }
    }

    s_got += len;
    s_info.progress = s_size ? (int)((uint64_t)s_got * 100 / s_size) : 0;
    unlock();
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200)); /* 先把 HTTP 响应发出去 */
    notify_drain(15000);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

esp_err_t ota_end(void)
{
    lock();
    if (s_info.state != OTA_STATE_RECEIVING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_started || s_got != s_size) {
        set_message("固件未接收完整，升级中止");
        s_info.state = OTA_STATE_ERROR;
        free_head();
        if (s_started) {
            esp_ota_abort(s_handle);
            s_started = false;
        }
        unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    s_info.state = OTA_STATE_VERIFYING;
    s_info.progress = 100;
    unlock();

    esp_err_t err = esp_ota_end(s_handle);
    s_started = false;
    if (err != ESP_OK) {
        lock();
        snprintf(s_info.message, sizeof(s_info.message), "镜像校验失败: %s", esp_err_to_name(err));
        s_info.state = OTA_STATE_ERROR;
        unlock();
        log_line("固件升级失败：镜像校验不通过（%s）", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        lock();
        snprintf(s_info.message, sizeof(s_info.message), "切换启动分区失败: %s", esp_err_to_name(err));
        s_info.state = OTA_STATE_ERROR;
        unlock();
        log_line("固件升级失败：切换启动分区失败（%s）", esp_err_to_name(err));
        return err;
    }

    char body[192];
    snprintf(body, sizeof(body), "固件升级完成：%s → %s，设备即将重启（约 20 秒）。",
             s_info.current, s_new_version);
    log_line("固件升级完成：%s → %s，即将重启", s_info.current, s_new_version);

    lock();
    snprintf(s_info.message, sizeof(s_info.message), "固件已写入，正在重启");
    s_info.state = OTA_STATE_READY;
    unlock();

    notify_system("固件升级完成", body);
    if (xTaskCreate(reboot_task, "ota_reboot", 3072, NULL, 4, NULL) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    return ESP_OK;
}

void ota_abort(void)
{
    lock();
    if (s_started) {
        esp_ota_abort(s_handle);
        s_started = false;
    }
    free_head();
    if (s_info.state == OTA_STATE_RECEIVING || s_info.state == OTA_STATE_VERIFYING) {
        set_message("上传中断，设备仍运行原固件");
        s_info.state = OTA_STATE_ERROR;
        log_line("固件升级：上传中断，已放弃本次写入");
    }
    unlock();
}

/* ---------- 回滚确认 ---------- */

static void confirm_task(void *arg)
{
    (void)arg;
    TickType_t step = pdMS_TO_TICKS(5000);
    TickType_t waited = 0;
    /* 先给 45 秒稳定窗口，再看网络；进配网热点也算可用（否则换路由器会被反复回滚） */
    while (waited < pdMS_TO_TICKS(OTA_CONFIRM_LIMIT_MS)) {
        vTaskDelay(step);
        waited += step;
        if (waited < pdMS_TO_TICKS(OTA_CONFIRM_WAIT_MS)) {
            continue;
        }
        if (wifi_connected() || wifi_portal_ssid()[0] != '\0') {
            break;
        }
    }

    if (!wifi_connected() && wifi_portal_ssid()[0] == '\0') {
        log_line("启动 %d 秒仍无网络，暂不确认新固件（下次复位会自动回滚）", OTA_CONFIRM_LIMIT_MS / 1000);
        vTaskDelete(NULL);
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            log_line("新固件 %s 已确认有效（%s）", s_info.current, running->label);
        } else {
            log_line("确认新固件失败，下次复位可能回滚");
        }
    }
    vTaskDelete(NULL);
}

/* ---------- 初始化 ---------- */

void ota_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_info, 0, sizeof(s_info));
    snprintf(s_info.current, sizeof(s_info.current), "%s", esp_app_get_description()->version);
    s_info.state = OTA_STATE_IDLE;

    cache_load();

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool pending = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending) {
        log_line("检测到待确认的新固件 %s（%s），联网稳定后确认", s_info.current, running->label);
        xTaskCreate(confirm_task, "ota_confirm", 3072, NULL, 3, NULL);
    } else {
        log_line("固件版本 %s（分区 %s）", s_info.current, running ? running->label : "?");
    }

    const esp_timer_create_args_t daily_args = {
        .callback = daily_cb,
        .name = "ota_daily",
    };
    if (esp_timer_create(&daily_args, &s_daily) == ESP_OK) {
        esp_timer_start_periodic(s_daily, (uint64_t)OTA_PERIOD_S * 1000000ULL);
    }
    const esp_timer_create_args_t retry_args = {
        .callback = retry_cb,
        .name = "ota_retry",
    };
    esp_timer_create(&retry_args, &s_retry);

    s_wake = xSemaphoreCreateBinary();
    if (s_wake) {
        xTaskCreate(worker_task, "ota_check", 8192, NULL, 3, &s_worker);
    }

    const esp_timer_create_args_t boot_args = {
        .callback = daily_cb,
        .name = "ota_boot",
    };
    esp_timer_handle_t boot_check = NULL;
    if (esp_timer_create(&boot_args, &boot_check) == ESP_OK) {
        esp_timer_start_once(boot_check, (uint64_t)OTA_FIRST_CHECK_S * 1000000ULL);
    }
}
