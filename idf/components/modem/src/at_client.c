#include "modem/at_client.h"

#include <stdio.h>
#include <string.h>

#include "core/board.h"
#include "core/log.h"
#include "esp_check.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AT_LINE_MAX 512
#define AT_UART_RX_BUF 2048
#define AT_UART_TX_BUF 1024

#define AT_URC_QUEUE_DEPTH 8
#define AT_URC_LINE_MAX 400

typedef struct {
    char line[AT_URC_LINE_MAX];
} at_urc_item_t;

static SemaphoreHandle_t s_lock;
static at_urc_cb_t s_urc_cb;
static char s_line[AT_LINE_MAX];
static size_t s_line_len;
static QueueHandle_t s_urc_queue;

static void pump_available(void);

/**
 * 事务期间读到的非应答行不直接回调，而是排队交给 at_reader 统一解析。
 * 这样 URC 状态机永远只在一个任务里运行，不会因为事务上下文而错位或丢短信。
 */
static void urc_defer(const char *line)
{
    if (!s_urc_queue) {
        if (s_urc_cb) {
            s_urc_cb(line);
        }
        return;
    }
    at_urc_item_t item;
    snprintf(item.line, sizeof(item.line), "%s", line);
    if (xQueueSend(s_urc_queue, &item, 0) != pdTRUE) {
        /* 队列满：宁可同步处理，也不要丢 URC（很可能是短信） */
        if (s_urc_cb) {
            s_urc_cb(line);
        }
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/** 读取一整行（去掉 \r\n）。返回值：1=拿到一行，0=超时/无数据。 */
static int read_line(char *out, size_t cap, uint32_t timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        int64_t remain = deadline - now_ms();
        if (remain <= 0) {
            break;
        }
        uint8_t c = 0;
        int n = uart_read_bytes(BOARD_MODEM_UART_NUM, &c, 1,
                                pdMS_TO_TICKS(remain > 20 ? 20 : remain));
        if (n != 1) {
            continue;
        }
        if (c == '\n') {
            if (s_line_len > 0 && s_line[s_line_len - 1] == '\r') {
                s_line_len--;
            }
            s_line[s_line_len] = '\0';
            snprintf(out, cap, "%s", s_line);
            s_line_len = 0;
            return 1;
        }
        if (c == '\r' || c == 0) {
            continue;
        }
        if (s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = (char)c;
        } else {
            /* 超长行保护：丢弃并重新对齐 */
            s_line_len = 0;
        }
    }
    return 0;
}

static void pump_available(void)
{
    char line[AT_LINE_MAX];
    while (read_line(line, sizeof(line), 0)) {
        urc_defer(line);
    }
}

static void at_reader_task(void *arg)
{
    (void)arg;
    char line[AT_LINE_MAX];
    at_urc_item_t item;
    while (1) {
        if (xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            while (read_line(line, sizeof(line), 0)) {
                urc_defer(line);
            }
            xSemaphoreGiveRecursive(s_lock);
        }
        /* 统一的 URC 消费点（含事务期间积压的行） */
        while (xQueueReceive(s_urc_queue, &item, 0) == pdTRUE) {
            if (s_urc_cb) {
                s_urc_cb(item.line);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t at_client_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_urc_queue = xQueueCreate(AT_URC_QUEUE_DEPTH, sizeof(at_urc_item_t));
    if (!s_lock || !s_urc_queue) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t cfg = {
        .baud_rate = BOARD_MODEM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BOARD_MODEM_UART_NUM, AT_UART_RX_BUF,
                                           AT_UART_TX_BUF, 0, NULL, 0),
                        "at", "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BOARD_MODEM_UART_NUM, &cfg), "at", "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BOARD_MODEM_UART_NUM, BOARD_MODEM_TX_GPIO,
                                     BOARD_MODEM_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        "at", "uart_set_pin");

    BaseType_t ok = xTaskCreate(at_reader_task, "at_reader", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    log_line("AT 串口就绪（UART%d TX=%d RX=%d）", BOARD_MODEM_UART_NUM,
             BOARD_MODEM_TX_GPIO, BOARD_MODEM_RX_GPIO);
    return ESP_OK;
}

void at_set_urc_handler(at_urc_cb_t cb)
{
    s_urc_cb = cb;
}

void at_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void at_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void append_resp(char *resp, size_t cap, const char *line)
{
    if (!resp || cap == 0) {
        return;
    }
    size_t used = strlen(resp);
    if (used + strlen(line) + 2 >= cap) {
        return;
    }
    strcat(resp, line);
    strcat(resp, "\n");
}

/**
 * 把一次 AT 交互压成一行日志：AT> 命令 → 应答摘要。
 * 环形缓冲只有 120 行，逐应答行记录会把关键事件冲掉；这里每条命令固定一行，
 * 既保留完整交互又不会刷屏。URC（+CMT/+CEREG 等）单独由 at_reader 记录。
 */
static void at_log_xfer(const char *cmd, const char *resp, at_result_t result)
{
    const char *tail = "超时";
    if (result == AT_RESULT_OK) tail = "OK";
    else if (result == AT_RESULT_ERROR) tail = "ERROR";

    /* 应答去掉换行，压平到一行；过长截断，保留开头关键信息 */
    char flat[160] = {0};
    if (resp && resp[0]) {
        snprintf(flat, sizeof(flat), "%s", resp);
        for (char *p = flat; *p; p++) {
            if (*p == '\r' || *p == '\n') *p = ' ';
        }
        while (strlen(flat) > 2 && flat[strlen(flat) - 1] == ' ') {
            flat[strlen(flat) - 1] = '\0';
        }
        /* 去掉末尾与 tail 重复的 OK/ERROR，避免 "OK → OK" */
        size_t n = strlen(flat);
        size_t tl = strlen(tail);
        if (n >= tl && strcmp(flat + n - tl, tail) == 0 &&
            (n == tl || flat[n - tl - 1] == ' ')) {
            flat[n - tl] = '\0';
            while (n > 1 && flat[n - 2] == ' ') flat[--n - 1] = '\0';
        }
    }
    if (flat[0]) {
        log_line("AT> %s → %s [%s]", cmd, flat, tail);
    } else {
        log_line("AT> %s → [%s]", cmd, tail);
    }
}

at_result_t at_command(const char *cmd, char *resp, size_t resp_cap, uint32_t timeout_ms)
{
    if (resp && resp_cap) {
        resp[0] = '\0';
    }
    at_lock();

    pump_available();
    /* 注意：不重置 s_line_len，保留可能尚未收全的 URC 行，避免丢短信 */

    char tx[AT_LINE_MAX];
    int n = snprintf(tx, sizeof(tx), "%s\r", cmd);
    uart_write_bytes(BOARD_MODEM_UART_NUM, tx, n);

    int64_t deadline = now_ms() + timeout_ms;
    at_result_t result = AT_RESULT_TIMEOUT;
    char line[AT_LINE_MAX];

    while (now_ms() < deadline) {
        int64_t remain = deadline - now_ms();
        if (!read_line(line, sizeof(line), (uint32_t)remain)) {
            break;
        }
        append_resp(resp, resp_cap, line);

        if (strcmp(line, "OK") == 0) {
            result = AT_RESULT_OK;
            break;
        }
        if (strstr(line, "ERROR")) {
            result = AT_RESULT_ERROR;
            break;
        }
        /* 非应答行 = 主动上报（+CMT:、+CEREG: ...），排队交给 at_reader */
        urc_defer(line);
    }

    at_log_xfer(cmd, resp ? resp : "", result);
    at_unlock();
    return result;
}

bool at_command_ok(const char *cmd, uint32_t timeout_ms)
{
    return at_command(cmd, NULL, 0, timeout_ms) == AT_RESULT_OK;
}

bool at_command_wait_for(const char *cmd, const char *keyword, char *line_out, size_t cap,
                         uint32_t timeout_ms)
{
    if (line_out && cap) {
        line_out[0] = '\0';
    }
    at_lock();
    pump_available();

    char tx[AT_LINE_MAX];
    int n = snprintf(tx, sizeof(tx), "%s\r", cmd);
    uart_write_bytes(BOARD_MODEM_UART_NUM, tx, n);

    bool found = false;
    int64_t deadline = now_ms() + timeout_ms;
    char line[AT_LINE_MAX];
    char collected[160] = {0};
    while (now_ms() < deadline) {
        int64_t remain = deadline - now_ms();
        if (!read_line(line, sizeof(line), (uint32_t)remain)) {
            break;
        }
        if (collected[0] == '\0') {
            snprintf(collected, sizeof(collected), "%s", line);
        }
        if (strstr(line, keyword)) {
            if (line_out && cap) {
                snprintf(line_out, cap, "%s", line);
            }
            found = true;
            break;
        }
        if (strstr(line, "ERROR")) {
            if (line_out && cap) {
                snprintf(line_out, cap, "%s", line);
            }
            break;
        }
        urc_defer(line);
    }
    for (char *p = collected; *p; p++) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    log_line("AT> %s → %s [%s]", cmd, collected[0] ? collected : "(无应答)",
             found ? "匹配" : "未匹配");
    at_unlock();
    return found;
}

/* 有的模组（ML307Y）会先回 OK，再把 CCHO/CGLA 的数据行吐出来；收到 OK 后再等这么久 */
#define AT_PAYLOAD_GRACE_MS 3000

/** 整个字符串都是数字（允许前后空白） */
static bool line_is_number(const char *s)
{
    while (*s == ' ') s++;
    if (!*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        s++;
    }
    return true;
}

at_result_t at_command_capture_ex(const char *cmd, const char *prefix, bool numeric_line,
                                  char *out, size_t cap, uint32_t timeout_ms)
{
    if (!out || cap < 2) {
        return AT_RESULT_ERROR;
    }
    out[0] = '\0';

    at_lock();
    pump_available();

    const size_t cmd_len = strlen(cmd);
    if (cmd_len <= 72) {
        log_line("AT> %s", cmd);
    } else {
        log_line("AT> %.60s…（%u 字符）", cmd, (unsigned)cmd_len);
    }
    uart_write_bytes(BOARD_MODEM_UART_NUM, cmd, cmd_len);
    uart_write_bytes(BOARD_MODEM_UART_NUM, "\r", 1);

    size_t len = 0;
    bool overflow = false;
    bool captured = false;
    bool saw_ok = false;
    const bool expect_payload = (prefix && prefix[0]) || numeric_line;
    at_result_t result = AT_RESULT_TIMEOUT;
    int64_t deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        int64_t remain = deadline - now_ms();
        uint8_t c = 0;
        int n = uart_read_bytes(BOARD_MODEM_UART_NUM, &c, 1,
                                pdMS_TO_TICKS(remain > 20 ? 20 : (remain > 0 ? remain : 1)));
        if (n != 1) {
            continue;
        }
        if (c == '\r' || c == 0) {
            continue;
        }
        if (c == '\n') {
            out[len < cap ? len : cap - 1] = '\0';
            if (!overflow && prefix && prefix[0] && strstr(out, prefix)) {
                captured = true;
                result = AT_RESULT_OK;
                break;
            }
            if (!overflow && numeric_line && line_is_number(out)) {
                captured = true;
                result = AT_RESULT_OK;
                break;
            }
            if (!overflow && strcmp(out, "OK") == 0) {
                if (!expect_payload) {
                    result = AT_RESULT_OK;
                    break;
                }
                /* 要等数据行：ML307Y 偶尔先回 OK，随后才吐 +CGLA/+CCHO */
                if (!saw_ok) {
                    saw_ok = true;
                    int64_t grace = now_ms() + AT_PAYLOAD_GRACE_MS;
                    if (grace < deadline) {
                        deadline = grace;
                    }
                    log_line("AT 先返回 OK，继续等待数据行（最多 %d ms）", AT_PAYLOAD_GRACE_MS);
                }
                len = 0;
                overflow = false;
                out[0] = '\0';
                continue;
            }
            if (!overflow && strstr(out, "ERROR")) {
                /* 保留这一行，调用方要拿它当失败原因 */
                result = AT_RESULT_ERROR;
                break;
            }
            if (overflow) {
                log_line("AT 应答行过长（超过 %u 字节），已截断", (unsigned)cap);
            } else if (out[0]) {
                /* 事务期间的非应答行（+CMT 等）照常交给 URC 处理，不能丢短信 */
                urc_defer(out);
            }
            len = 0;
            overflow = false;
            out[0] = '\0';
            continue;
        }
        if (len + 1 < cap) {
            out[len++] = (char)c;
        } else {
            overflow = true;
        }
    }

    if (!captured) {
        if (saw_ok) {
            result = AT_RESULT_OK;
        }
        out[0] = '\0'; /* 只有最终 OK，没有匹配行 */
    }
    log_line("AT> %s → %s", cmd_len <= 20 ? cmd : "(长指令)",
             captured ? "已捕获应答" : (result == AT_RESULT_OK ? "无目标行" : "失败"));
    at_unlock();
    return result;
}

at_result_t at_command_capture(const char *cmd, const char *prefix, char *out, size_t cap,
                               uint32_t timeout_ms)
{
    return at_command_capture_ex(cmd, prefix, false, out, cap, timeout_ms);
}

bool at_send_pdu(const char *pdu_hex, int tpdu_len, uint32_t timeout_ms)
{
    at_lock();
    pump_available();

    char tx[64];
    int n = snprintf(tx, sizeof(tx), "AT+CMGS=%d\r", tpdu_len);
    log_line("AT> AT+CMGS=%d (PDU %d 字节)...", tpdu_len, tpdu_len);
    uart_write_bytes(BOARD_MODEM_UART_NUM, tx, n);

    /* 等待 '>' 提示符（不以换行结尾，必须按字节读取） */
    bool got_prompt = false;
    int64_t deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        uint8_t c = 0;
        if (uart_read_bytes(BOARD_MODEM_UART_NUM, &c, 1, pdMS_TO_TICKS(20)) != 1) {
            continue;
        }
        if (c == '>') {
            got_prompt = true;
            break;
        }
        if (c == '\n') {
            continue;
        }
    }
    if (!got_prompt) {
        log_line("未收到 > 提示符，短信发送失败");
        at_unlock();
        return false;
    }

    uart_write_bytes(BOARD_MODEM_UART_NUM, pdu_hex, strlen(pdu_hex));
    const uint8_t ctrl_z = 0x1A;
    uart_write_bytes(BOARD_MODEM_UART_NUM, (const char *)&ctrl_z, 1);

    bool success = false;
    deadline = now_ms() + (int64_t)timeout_ms;
    char line[AT_LINE_MAX];
    while (now_ms() < deadline) {
        int64_t remain = deadline - now_ms();
        if (!read_line(line, sizeof(line), (uint32_t)remain)) {
            break;
        }
        if (strcmp(line, "OK") == 0) {
            success = true;
            break;
        }
        if (strstr(line, "ERROR")) {
            success = false;
            break;
        }
        /* 非应答行（+CMT 等）排队交给 at_reader，避免等待窗口内吞掉新短信 */
        urc_defer(line);
    }

    at_unlock();
    return success;
}
