/**
 * @file modem.h
 * @brief 4G 模组服务：初始化、收发短信、AT 转发、软/硬重启。
 *
 * 所有耗时操作都投递到内部工作队列（modem 任务）执行，调用方同步等待结果，
 * 因此 HTTP 处理器和转发逻辑都不会直接和串口时序纠缠。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MODEM_MODEL_MAX 32
#define MODEM_VERSION_MAX 64

/** 启动模组服务（创建任务与队列，并投递初始化任务）。 */
esp_err_t modem_start(void);

/** 模组是否已完成初始化并注册上网络。 */
bool modem_ready(void);

const char *modem_model(void);

/** 发送短信（阻塞，直到模组返回 OK/ERROR 或超时）。 */
bool modem_send_sms(const char *phone, const char *text, uint32_t timeout_ms);

/** 执行任意 AT 指令，返回 malloc 的应答文本（调用者 free）。 */
char *modem_at(const char *cmd, uint32_t timeout_ms);

/** 硬重启（EN 断电）+ 重新初始化。 */
bool modem_hard_reset(uint32_t timeout_ms);
/** 软重启（AT+CFUN=1,1）+ 重新初始化。 */
bool modem_soft_reset(uint32_t timeout_ms);

/** 重新执行初始化流程（例如注册失败后）。 */
bool modem_reinit(uint32_t timeout_ms);

typedef struct {
    bool success;
    char detail[128];
} modem_ping_result_t;

/** 临时激活数据连接并 ping 8.8.8.8，完成后自动关闭数据连接。 */
bool modem_ping(modem_ping_result_t *out, uint32_t timeout_ms);
