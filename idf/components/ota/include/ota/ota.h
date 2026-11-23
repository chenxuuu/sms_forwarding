/**
 * @file ota.h
 * @brief 固件升级：网页上传 `*-ota.bin` + 每天一次的版本检查。
 *
 * 设计边界（详见 docs/ota-plan.md）：
 *  - 只支持手动上传升级，不做自动下载、自动安装
 *  - 版本检查只拉几百字节的 manifest，命中新版本也只做提示
 *  - 写入的是非活动分区，失败/断电不影响当前固件
 *  - 新固件启动后需确认有效，否则下次复位自动回滚
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 版本清单地址；fork 的人改这里即可。 */
#define OTA_MANIFEST_URL \
    "https://github.com/chenxuuu/sms_forwarding/releases/latest/download/latest.json"

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
    char latest[32];       /**< 远端最新版本，空串表示未知 */
    char url[192];      /**< 下载地址（仅用于页面展示） */
    char notes[160];    /**< 版本说明 */
    int64_t checked_at; /**< 上次检查成功的 epoch，0 = 从未 */
    bool has_update;    /**< 远端版本比当前新 */
    bool checking;      /**< 正在检查远端版本 */
    ota_state_t state;
    int progress;       /**< 0-100 */
    char message[96];   /**< 最近一次结果/错误，给页面和日志看 */
} ota_info_t;

/** 初始化：读 NVS 缓存、启动检查定时器、必要时进入回滚确认流程。 */
void ota_init(void);

/** 当前运行版本（非发布格式时为开发版字符串）。 */
const char *ota_version(void);

/** 加锁拷贝一份状态。 */
void ota_get_info(ota_info_t *out);

/** 是否正在接收/写入固件，供其它写操作拒绝并发。 */
bool ota_busy(void);

/** 版本号是否为 `YYYYMMDD.HHMM` 的发布格式。 */
bool ota_version_is_release(const char *version);

/** 开始接收：image_size 为 Content-Length。会做尺寸预检，不擦 flash。 */
esp_err_t ota_begin(size_t image_size);

/** 收到一块数据就写一块；首块会先做镜像头校验再擦分区。 */
esp_err_t ota_write(const void *data, size_t len);

/** 收完：校验镜像、切启动分区、推送通知并安排重启。 */
esp_err_t ota_end(void);

/** 放弃本次升级（连接中断、校验失败都要调）。 */
void ota_abort(void);

/**
 * 请求检查远端版本：投递给内部工作任务（8 KB 栈），立即返回。
 * 结果通过 ota_get_info() 的 checked_at / has_update / checking 反映。
 * TLS 握手需要连续内存和足够的栈，不能在 esp_timer / httpd 任务里直接跑。
 */
esp_err_t ota_request_check(void);
