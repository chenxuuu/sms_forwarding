/**
 * @file at_client.h
 * @brief AT 指令通道：UART 读写串行化 + 主动上报（URC）分发。
 *
 * 设计要点：
 *  - 所有 AT 交互都经过同一把递归锁，天然串行，避免多个任务抢串口；
 *  - 事务等待期间收到的非应答行（如 +CMT:）会转交 URC 回调，不会丢短信；
 *  - 空闲时由独立读任务负责把 URC 分发给上层。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AT_RESULT_OK = 0,
    AT_RESULT_ERROR,
    AT_RESULT_TIMEOUT,
} at_result_t;

typedef void (*at_urc_cb_t)(const char *line);

/** 安装 UART 驱动并启动 URC 读任务。 */
esp_err_t at_client_init(void);

/** 执行一条 AT 指令，收集应答到 resp（可为 NULL），直到 OK/ERROR/超时。 */
at_result_t at_command(const char *cmd, char *resp, size_t resp_cap, uint32_t timeout_ms);

/** 便捷封装：只关心是否返回 OK。 */
bool at_command_ok(const char *cmd, uint32_t timeout_ms);

/**
 * PDU 模式发送短信：AT+CMGS=<tpdu_len> → 等待 '>' → 发送 PDU + Ctrl-Z。
 * @param pdu_hex  十六进制 PDU（不含 SMSC 前缀的 TPDU 也行，由上层决定）
 * @param tpdu_len TPDU 字节数（即 AT+CMGS 的参数）
 */
bool at_send_pdu(const char *pdu_hex, int tpdu_len, uint32_t timeout_ms);

/**
 * 发送指令并等待“包含 keyword 的行”，把该行拷到 line_out。
 * 期间收到的不相关行继续交给 URC 回调（例如 ping 等待 +MPING 上报）。
 */
bool at_command_wait_for(const char *cmd, const char *keyword, char *line_out, size_t cap,
                         uint32_t timeout_ms);

void at_set_urc_handler(at_urc_cb_t cb);

/** 多步操作期间独占串口（递归锁，可嵌套）。 */
void at_lock(void);
void at_unlock(void);
