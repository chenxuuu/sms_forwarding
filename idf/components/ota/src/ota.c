#include "ota/ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net/wifi.h"
#include "net/wifi_portal.h"
#include "notify/notify.h"

#define OTA_HEAD_MAX 4096     /* 首块缓存：够放 image header + app desc */
#define OTA_MIN_IMAGE 0x10000 /* 小于 64KB 肯定不是有效固件 */
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
}
