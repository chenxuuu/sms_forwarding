/**
 * @file ota.h
 * @brief 固件升级：网页上传 `*-ota.bin` 并写入非活动分区。
 *
 * 设计边界（详见 docs/ota-plan.md）：
 *  - 只支持手动上传升级，不做自动下载、自动安装
 *  - 版本检查完全在浏览器侧完成（GitHub API），设备不联网、不跑 TLS
 *  - 写入的是非活动分区，失败/断电不影响当前固件
 *  - 新固件启动后需确认有效，否则下次复位自动回滚
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE = 0,   /**< 空闲，可以上传 */
    OTA_STATE_RECEIVING,  /**< 正在接收并写入 */
    OTA_STATE_VERIFYING,  /**< 已收完，正在校验 */
    OTA_STATE_READY,      /**< 已写好，等待重启 */
    OTA_STATE_ERROR,      /**< 最近一次升级失败，可重试 */
} ota_state_t;

typedef struct {
    char current[32];      /**< 当前运行版本 */
    char next_version[32]; /**< 已写入、等待重启后生效的版本 */
    ota_state_t state;
    int progress;     /**< 0-100 */
    char message[96]; /**< 最近一次结果/错误，给页面和日志看 */
} ota_info_t;

/** 初始化：读取当前分区状态，必要时进入回滚确认流程。 */
void ota_init(void);

/** 当前运行版本（非发布格式时为开发版字符串）。 */
const char *ota_version(void);

/** 加锁拷贝一份状态。 */
void ota_get_info(ota_info_t *out);

/** 是否正在接收/写入固件，供其它写操作拒绝并发。 */
bool ota_busy(void);

/** 开始接收：image_size 为 Content-Length。会做尺寸预检，不擦 flash。 */
esp_err_t ota_begin(size_t image_size);

/** 收到一块数据就写一块；首块会先做镜像头校验再擦分区。 */
esp_err_t ota_write(const void *data, size_t len);

/** 收完：校验镜像、切启动分区、推送通知并安排重启。 */
esp_err_t ota_end(void);

/** 放弃本次升级（连接中断、校验失败都要调）。 */
void ota_abort(void);
