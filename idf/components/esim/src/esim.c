#include "esim/esim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esim_codec.h"
#include "modem/at_client.h"

/* eUICC 的 ISD-R 应用标识（GSMA SGP.22 固定值） */
#define ISD_R_AID_HEX "A0000005591010FFFFFFFF8900000100"

/* +CGLA 单行应答上限（十六进制字符数）：es10x 的应答缓冲是 512 字节，
 * 转成十六进制再加 "+CGLA: <通道>,<长度>,\"" 前缀，1280 足够。
 * 之前取 4096，碎片化严重时 malloc(4096) 会失败，页面就报「内存不足」。 */
#define ESIM_CAPTURE_MAX 1280
#define ESIM_RESP_MAX 2048    /* APDU 应答字节上限（eUICC 单次最多 256 字节，留足翻页余量） */
#define ESIM_LOCK_MS 20000
#define ESIM_AT_TIMEOUT 5000
#define ESIM_APDU_TIMEOUT 15000

static SemaphoreHandle_t s_lock;
static char s_err[128];
static bool s_probed;
static bool s_probe_ok;
static volatile bool s_busy;
static int s_last_channel;

/* ---------- 小工具 ---------- */

static esim_err_t fail(esim_err_t code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    return code;
}

/* ---------- AT + APDU 通道 ---------- */

static bool card_probe(void)
{
    if (s_probed) {
        return s_probe_ok;
    }
    s_probed = true;
    s_probe_ok = at_command_ok("AT+CCHO=?", 3000) && at_command_ok("AT+CGLA=?", 3000);
    if (!s_probe_ok) {
        fail(ESIM_ERR_UNSUPPORTED, "模组不支持 eUICC 指令（AT+CCHO / AT+CGLA），无法管理 eSIM");
        log_line("eSIM：模组不支持 AT+CCHO / AT+CGLA");
    }
    return s_probe_ok;
}

static esim_err_t lock_take(void)
{
    if (!s_lock) {
        return fail(ESIM_ERR_BUSY, "eSIM 模块未初始化");
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(ESIM_LOCK_MS)) != pdTRUE) {
        return fail(ESIM_ERR_BUSY, "eSIM 正忙，请稍后再试");
    }
    return ESIM_OK;
}

static void lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

esim_err_t esim_channel_open(const char *aid_hex, int *channel)
{
    if (!aid_hex || !channel) {
        return ESIM_ERR_ARG;
    }
    char cmd[72];
    snprintf(cmd, sizeof(cmd), "AT+CCHO=\"%s\"", aid_hex);
    char *line = malloc(160);
    if (!line) {
        return fail(ESIM_ERR_NOMEM, "内存不足");
    }
    /* 有的模组回 "+CCHO: 1"，有的（ML307Y）只回一个光秃秃的通道号 */
    int ch = -1;
    bool busy = false;
    bool no_sim = false;
    for (int attempt = 0; attempt < 2 && ch <= 0; attempt++) {
        if (attempt) {
            /* AT+CCHC 之后模组会补一条 "+CCHC" 主动上报，偶尔正好撞进下一次
             * CCHO 的应答窗口，导致拿不到通道号。隔 300 ms 再试一次即可。 */
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        at_result_t r = at_command_capture_ex(cmd, "+CCHO:", true, line, 160, ESIM_AT_TIMEOUT);
        if (r == AT_RESULT_OK) {
            const char *p = strstr(line, "+CCHO:");
            ch = p ? atoi(p + 6) : atoi(line);
        }
        if (strstr(line, "CME ERROR: 4")) {
            busy = true;
        }
        if (strstr(line, "CME ERROR: 10")) {
            no_sim = true;
        }
    }
    free(line);
    if (ch <= 0) {
        if (busy) {
            return fail(ESIM_ERR_UNSUPPORTED,
                        "逻辑通道被占用（上次操作没关干净），请到「设备」页软重启模组后重试");
        }
        if (no_sim) {
            return fail(ESIM_ERR_UNSUPPORTED, "模组读不到 SIM 卡，请检查卡片或重启模组");
        }
        return fail(ESIM_ERR_UNSUPPORTED,
                    "打不开 eUICC 的逻辑通道（可能没插 eSIM，或模组不支持该卡）");
    }
    *channel = ch;
    s_last_channel = ch;
    return ESIM_OK;
}

int esim_last_channel(void)
{
    return s_last_channel;
}

void esim_channel_close(int channel)
{
    if (channel <= 0) {
        return;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CCHC=%d", channel);
    at_command_ok(cmd, ESIM_AT_TIMEOUT);
    if (s_last_channel == channel) {
        s_last_channel = 0;
    }
}

static esim_err_t transmit(int channel, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t cap,
                           size_t *rx_len, int *sw)
{
    if (tx_len == 0 || tx_len > 255) {
        return fail(ESIM_ERR_ARG, "APDU 长度非法（%u 字节）", (unsigned)tx_len);
    }
    const size_t hex_len = tx_len * 2;
    char *cmd = malloc(hex_len + 40);
    char *line = malloc(ESIM_CAPTURE_MAX);
    if (!cmd || !line) {
        free(cmd);
        free(line);
        return fail(ESIM_ERR_NOMEM, "内存不足，无法发送 APDU（最大连续块 %u 字节）",
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    char *tx_hex = cmd + 32;
    esim_bin_to_hex(tx, tx_len, tx_hex, hex_len + 1);
    snprintf(cmd, 40, "AT+CGLA=%d,%u,\"", channel, (unsigned)hex_len);

    /* snprintf 只写了前缀，把十六进制和收尾引号接上去 */
    size_t prefix = strlen(cmd);
    memcpy(cmd + prefix, tx_hex, hex_len);
    cmd[prefix + hex_len] = '"';
    cmd[prefix + hex_len + 1] = '\0';

    at_result_t r = at_command_capture(cmd, "+CGLA: ", line, ESIM_CAPTURE_MAX, ESIM_APDU_TIMEOUT);
    free(cmd);

    if (r != AT_RESULT_OK) {
        esim_err_t err = fail(ESIM_ERR_AT, "APDU 传输失败：%s", line[0] ? line : "模组无应答");
        free(line);
        return err;
    }
    const char *q = strchr(line, '"');
    if (!q) {
        esim_err_t err = fail(ESIM_ERR_FORMAT, "APDU 应答格式异常：%.40s", line);
        free(line);
        return err;
    }
    const char *end = strrchr(q + 1, '"');
    size_t chars = end ? (size_t)(end - q - 1) : strlen(q + 1);
    if (chars < 4 || chars % 2 != 0) {
        free(line);
        return fail(ESIM_ERR_FORMAT, "APDU 应答长度异常（%u 字符）", (unsigned)chars);
    }
    int n = esim_hex_to_bin_n(q + 1, chars, rx, cap);
    free(line);
    if (n < 2) {
        return fail(ESIM_ERR_FORMAT, "APDU 应答不是合法十六进制");
    }
    *rx_len = (size_t)n;
    if (sw) {
        *sw = (rx[n - 2] << 8) | rx[n - 1];
    }
    if (n >= 2) {
        log_line("eSIM APDU %u→%u 字节，SW=%02X%02X", (unsigned)tx_len, (unsigned)n,
                 rx[n - 2], rx[n - 1]);
        if (((rx[n - 2] << 8) | rx[n - 1]) != 0x9000) {
            char hex[65];
            esim_bin_to_hex(rx, n > 32 ? 32 : (size_t)n, hex, sizeof(hex));
            log_line("eSIM APDU 非 9000 应答：%s", hex);
        }
    }
    return ESIM_OK;
}

esim_err_t esim_transmit(int channel, const char *tx_hex, char *rx_hex, size_t cap, int *sw)
{
    if (!tx_hex || !rx_hex || cap < 8) {
        return ESIM_ERR_ARG;
    }
    uint8_t tx[256];
    int tx_len = esim_hex_to_bin(tx_hex, tx, sizeof(tx));
    if (tx_len <= 0) {
        return fail(ESIM_ERR_ARG, "APDU 命令不是合法十六进制");
    }
    uint8_t *rx = malloc(ESIM_RESP_MAX);
    if (!rx) {
        return fail(ESIM_ERR_NOMEM, "内存不足");
    }
    size_t rx_len = 0;
    esim_err_t e = lock_take();
    if (e != ESIM_OK) {
        free(rx);
        return e;
    }
    s_busy = true;
    e = transmit(channel, tx, (size_t)tx_len, rx, ESIM_RESP_MAX, &rx_len, sw);
    s_busy = false;
    lock_give();
    if (e == ESIM_OK) {
        if (rx_len * 2 + 1 > cap) {
            e = fail(ESIM_ERR_FORMAT, "APDU 应答过长（%u 字节）", (unsigned)rx_len);
        } else {
            esim_bin_to_hex(rx, rx_len, rx_hex, cap);
        }
    }
    free(rx);
    return e;
}

/* ---------- ES10x ---------- */

/** 在已打开的通道上发一条 ES10x 请求，把全部数据段拼进 out。 */
static esim_err_t es10x_on(int channel, const uint8_t *req, size_t req_len, uint8_t *out, size_t cap,
                           size_t *out_len)
{
    if (req_len == 0 || req_len > 255) {
        return fail(ESIM_ERR_ARG, "ES10x 请求长度非法（%u）", (unsigned)req_len);
    }

    uint8_t apdu[260];
    size_t apdu_len = 5 + req_len;
    apdu[0] = (uint8_t)(0x80 | (channel & 0x0F));
    apdu[1] = 0xE2;
    apdu[2] = 0x91;
    apdu[3] = 0x00;
    apdu[4] = (uint8_t)req_len;
    memcpy(apdu + 5, req, req_len);

    uint8_t *rx = malloc(300);
    size_t total = 0;
    int sw = 0;
    bool skip_data = false;   /* 取主动命令的那一包不并入应答 */
    bool got_proactive = false;
    if (!rx) {
        return fail(ESIM_ERR_NOMEM, "内存不足");
    }
    esim_err_t e = ESIM_OK;

    for (int round = 0; round < 24; round++) {
        size_t rx_len = 0;
        e = transmit(channel, apdu, apdu_len, rx, 300, &rx_len, &sw);
        if (e != ESIM_OK) {
            break;
        }
        size_t data = rx_len >= 2 ? rx_len - 2 : 0;
        if (skip_data) {
            data = 0;
            skip_data = false;
        }
        if (total + data > cap) {
            data = cap - total;
        }
        memcpy(out + total, rx, data);
        total += data;

        uint8_t sw1 = (uint8_t)(sw >> 8);
        uint8_t sw2 = (uint8_t)(sw & 0xFF);
        if (sw1 == 0x91) {
            /* 卡里有主动命令（一般是 REFRESH）待取：用 GET RESPONSE 取走再丢掉，
               否则这条命令会一直挂着，后续 APDU 全返回 6985。 */
            got_proactive = true;
            apdu[0] = (uint8_t)(0x80 | (channel & 0x0F));
            apdu[1] = 0xC0;
            apdu[2] = 0x00;
            apdu[3] = 0x00;
            apdu[4] = sw2 ? sw2 : 0x00;
            apdu_len = 5;
            skip_data = true;
            continue;
        }
        if (sw1 == 0x61) {
            /* 还有数据：GET RESPONSE */
            apdu[0] = (uint8_t)(0x80 | (channel & 0x0F));
            apdu[1] = 0xC0;
            apdu[2] = 0x00;
            apdu[3] = 0x00;
            apdu[4] = sw2 ? sw2 : 0x00;
            apdu_len = 5;
            continue;
        }
        if (sw1 == 0x6C) {
            apdu[4] = sw2;
            continue;
        }
        if (sw1 == 0x90) {
            e = ESIM_OK;
        } else if (got_proactive && total > 0) {
            /* 已经拿到业务应答，主动命令取完后的状态字不影响结论 */
            log_line("eSIM：主动命令处理完成，忽略后续状态字 %02X%02X", sw1, sw2);
            e = ESIM_OK;
        } else {
            e = fail(ESIM_ERR_SW, "eUICC 返回状态字 %02X%02X", sw1, sw2);
        }
        break;
    }
    free(rx);
    if (e == ESIM_OK) {
        *out_len = total;
    }
    return e;
}

/** 打开通道 → 发一条 ES10x → 关通道 */
static esim_err_t es10x(const uint8_t *req, size_t req_len, uint8_t *out, size_t cap, size_t *out_len)
{
    int channel = 0;
    esim_err_t e = esim_channel_open(ISD_R_AID_HEX, &channel);
    if (e != ESIM_OK) {
        return e;
    }
    e = es10x_on(channel, req, req_len, out, cap, out_len);
    esim_channel_close(channel);
    return e;
}

static esim_err_t parse_eid(const uint8_t *resp, size_t len, char *out, size_t cap)
{
    esim_tlv_t top, eid;
    if (!esim_tlv_read(resp, len, 0, &top) || top.tag != 0xBF3E ||
        !esim_tlv_find(top.val, top.len, 0x5A, &eid)) {
        return fail(ESIM_ERR_FORMAT, "EID 应答解析失败");
    }
    esim_bin_to_hex(eid.val, eid.len, out, cap);
    return ESIM_OK;
}

static int parse_profiles(const uint8_t *resp, size_t len, esim_profile_t *out, int max)
{
    esim_tlv_t top, list;
    if (!esim_tlv_read(resp, len, 0, &top) || top.tag != 0xBF2D ||
        !esim_tlv_find(top.val, top.len, 0xA0, &list)) {
        return fail(ESIM_ERR_FORMAT, "eSIM 列表应答解析失败");
    }

    int count = 0;
    size_t pos = 0;
    esim_tlv_t node;
    while (count < max && esim_tlv_read(list.val, list.len, pos, &node)) {
        pos = node.next;
        /* E3 = ProfileInfo（部分 eUICC 用 BF25） */
        if (node.tag != 0xE3 && node.tag != 0xBF25) {
            continue;
        }
        esim_profile_t *p = &out[count];
        p->state = -1;
        p->profile_class = -1;
        size_t cpos = 0;
        esim_tlv_t f;
        while (esim_tlv_read(node.val, node.len, cpos, &f)) {
            cpos = f.next;
            switch (f.tag) {
            case 0x5A:
                esim_bcd_to_iccid(p->iccid, sizeof(p->iccid), f.val, f.len);
                break;
            case 0x4F:
                esim_bin_to_hex(f.val, f.len, p->aid, sizeof(p->aid));
                break;
            case 0x9F70: {
                int v = 0;
                for (size_t i = 0; i < f.len; i++) {
                    v = (v << 8) | f.val[i];
                }
                p->state = v == 0 ? 0 : (v == 1 ? 1 : -1);
                break;
            }
            case 0x90:
                esim_copy_text(p->nickname, sizeof(p->nickname), f.val, f.len);
                break;
            case 0x91:
                esim_copy_text(p->provider, sizeof(p->provider), f.val, f.len);
                break;
            case 0x92:
                esim_copy_text(p->name, sizeof(p->name), f.val, f.len);
                break;
            case 0x95: {
                int v = 0;
                for (size_t i = 0; i < f.len; i++) {
                    v = (v << 8) | f.val[i];
                }
                p->profile_class = (v >= 0 && v <= 2) ? v : -1;
                break;
            }
            default:
                break;
            }
        }
        if (!p->name[0]) {
            snprintf(p->name, sizeof(p->name), "%s", p->nickname[0] ? p->nickname : p->provider);
        }
        if (!p->name[0]) {
            snprintf(p->name, sizeof(p->name), "%s", p->iccid[0] ? p->iccid : "未命名配置");
        }
        count++;
    }
    return count;
}

static const uint8_t REQ_EID[] = {0xBF, 0x3E, 0x03, 0x5C, 0x01, 0x5A};
/* 只要 ICCID / AID / 状态 / 昵称 / 运营商 / 名称 / 类型 */
static const uint8_t REQ_LIST[] = {0xBF, 0x2D, 0x0A, 0x5C, 0x08, 0x5A,
                                   0x4F, 0x9F, 0x70, 0x90, 0x91, 0x92, 0x95};

esim_err_t esim_get_eid(char *out, size_t cap)
{
    if (!out || cap < 33) {
        return ESIM_ERR_ARG;
    }
    out[0] = '\0';
    esim_err_t e = lock_take();
    if (e != ESIM_OK) {
        return e;
    }
    s_busy = true;
    if (!card_probe()) {
        s_busy = false;
        lock_give();
        return ESIM_ERR_UNSUPPORTED;
    }

    uint8_t *resp = malloc(512);
    size_t len = 0;
    e = resp ? es10x(REQ_EID, sizeof(REQ_EID), resp, 512, &len) : fail(ESIM_ERR_NOMEM, "内存不足");
    if (e == ESIM_OK) {
        e = parse_eid(resp, len, out, cap);
    }
    free(resp);
    s_busy = false;
    lock_give();
    return e;
}

int esim_list_profiles(esim_profile_t *out, int max)
{
    if (!out || max <= 0) {
        return ESIM_ERR_ARG;
    }
    memset(out, 0, sizeof(*out) * (size_t)max);

    esim_err_t e = lock_take();
    if (e != ESIM_OK) {
        return e;
    }
    s_busy = true;
    if (!card_probe()) {
        s_busy = false;
        lock_give();
        return ESIM_ERR_UNSUPPORTED;
    }

    const size_t resp_cap = 4096;
    uint8_t *resp = malloc(resp_cap);
    size_t len = 0;
    if (!resp) {
        s_busy = false;
        lock_give();
        return fail(ESIM_ERR_NOMEM, "内存不足，无法读取 eSIM 列表");
    }
    e = es10x(REQ_LIST, sizeof(REQ_LIST), resp, resp_cap, &len);
    int count = (e == ESIM_OK) ? parse_profiles(resp, len, out, max) : (int)e;
    if (e != ESIM_OK) {
        count = (int)e;
    }
    free(resp);
    s_busy = false;
    lock_give();
    if (e == ESIM_OK) {
        log_line("eSIM：读取到 %d 条配置", count);
    }
    return count;
}

/**
 * 一次性读 EID + 配置列表。
 * 只开一次逻辑通道：这个模组每开/关一次通道都要多等几秒，分开读容易踩到残留应答。
 */
esim_err_t esim_read_all(char *eid, size_t eid_cap, esim_profile_t *out, int max, int *count)
{
    if (eid && eid_cap) {
        eid[0] = '\0';
    }
    if (count) {
        *count = -1;
    }
    if (!out || max <= 0) {
        return ESIM_ERR_ARG;
    }
    memset(out, 0, sizeof(*out) * (size_t)max);

    esim_err_t e = lock_take();
    if (e != ESIM_OK) {
        return e;
    }
    s_busy = true;
    if (!card_probe()) {
        s_busy = false;
        lock_give();
        return ESIM_ERR_UNSUPPORTED;
    }

    int channel = 0;
    e = esim_channel_open(ISD_R_AID_HEX, &channel);
    if (e != ESIM_OK) {
        s_busy = false;
        lock_give();
        return e;
    }

    const size_t resp_cap = 4096;
    uint8_t *resp = malloc(resp_cap);
    if (!resp) {
        esim_channel_close(channel);
        s_busy = false;
        lock_give();
        return fail(ESIM_ERR_NOMEM, "内存不足");
    }

    size_t len = 0;
    esim_err_t eid_err = es10x_on(channel, REQ_EID, sizeof(REQ_EID), resp, resp_cap, &len);
    if (eid_err == ESIM_OK && eid && eid_cap) {
        eid_err = parse_eid(resp, len, eid, eid_cap);
    } else if (eid_err == ESIM_OK) {
        eid_err = ESIM_ERR_ARG;
    }

    len = 0;
    esim_err_t list_err = es10x_on(channel, REQ_LIST, sizeof(REQ_LIST), resp, resp_cap, &len);
    int n = (list_err == ESIM_OK) ? parse_profiles(resp, len, out, max) : -1;

    free(resp);
    esim_channel_close(channel);
    s_busy = false;
    lock_give();

    if (count) {
        *count = n;
    }
    if (n >= 0) {
        log_line("eSIM：EID %s，读取到 %d 条配置", (eid && eid[0]) ? eid : "读取失败", n);
    } else {
        log_line("eSIM：读取列表失败：%s", esim_last_error());
    }
    if (list_err != ESIM_OK) {
        return list_err;
    }
    return eid_err;
}

/* ---------- 对外接口 ---------- */

esp_err_t esim_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool esim_supported(void)
{
    return s_probe_ok;
}

bool esim_busy(void)
{
    return s_busy;
}

const char *esim_last_error(void)
{
    return s_err[0] ? s_err : "未知错误";
}

static const char *enable_reason(int code)
{
    switch (code) {
    case 1:
        return "找不到该 ICCID / AID";
    case 2:
        return "该配置不是停用状态，无法启用";
    case 3:
        return "被 profile 策略禁止";
    case 4:
        return "错误的重新启用操作";
    case 5:
        return "终端忙（CAT busy），可尝试关闭刷新后重试";
    case 6:
        return "eUICC 内部错误";
    default:
        return "未知原因";
    }
}

static const char *delete_reason(int code)
{
    switch (code) {
    case 1:
        return "找不到该 ICCID / AID";
    case 2:
        return "该配置正在使用中，请先切换到其它配置";
    case 3:
        return "被 profile 策略禁止删除";
    case 4:
        return "eUICC 不支持删除该配置";
    case 5:
        return "终端忙（CAT busy），请稍后重试";
    default:
        return "未知原因";
    }
}

esim_err_t esim_delete_profile(const char *id)
{
    if (!id || !id[0]) {
        return fail(ESIM_ERR_ARG, "缺少 ICCID 或 AID");
    }

    char trimmed[40];
    snprintf(trimmed, sizeof(trimmed), "%s", id);
    util_trim(trimmed);

    /* 先确认不是当前启用中的配置：删掉正在用的卡会直接断网，这里直接拒绝。
     * 列表放堆上：10 条配置的数组约 2.6 KB，调用它的网页任务栈只有 4 KB。 */
    esim_profile_t *list = calloc(ESIM_MAX_PROFILES, sizeof(*list));
    if (!list) {
        return fail(ESIM_ERR_NOMEM, "内存不足");
    }
    int count = esim_list_profiles(list, ESIM_MAX_PROFILES);
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].iccid, trimmed) == 0 || strcmp(list[i].aid, trimmed) == 0) {
            if (list[i].state == 1) {
                esim_err_t active = fail(ESIM_ERR_CARD, "「%s」正在使用中，不能删除；请先切换到其它配置",
                                          list[i].name[0] ? list[i].name : trimmed);
                free(list);
                return active;
            }
            break;
        }
    }
    free(list);

    uint8_t ident_val[16];
    size_t ident_val_len;
    uint32_t ident_tag;
    if (strlen(trimmed) == 32) {
        int n = esim_hex_to_bin(trimmed, ident_val, sizeof(ident_val));
        if (n != 16) {
            return fail(ESIM_ERR_ARG, "AID 不是 32 位十六进制");
        }
        ident_tag = 0x4F;
        ident_val_len = (size_t)n;
    } else {
        int n = esim_iccid_to_bcd(trimmed, ident_val);
        if (n != 10) {
            return fail(ESIM_ERR_ARG, "ICCID 必须是 18-20 位数字，或 32 位十六进制 AID");
        }
        ident_tag = 0x5A;
        ident_val_len = (size_t)n;
    }

    esim_err_t e = lock_take();
    if (e != ESIM_OK) {
        return e;
    }
    s_busy = true;
    if (!card_probe()) {
        s_busy = false;
        lock_give();
        return ESIM_ERR_UNSUPPORTED;
    }

    /* DeleteProfileRequest ::= [51] { profileIdentifier }
     * 注意：CHOICE 直接放在 BF33 里，**不能再包一层 A0**——实测 9eSIM 收到
     * A0 包裹的形式会返回「找不到该 ICCID / AID」（结果码 1）。 */
    uint8_t ident[24];
    size_t ident_len = esim_der_put(ident, sizeof(ident), 0, ident_tag, ident_val, ident_val_len);

    uint8_t req[40];
    size_t req_len = esim_der_put(req, sizeof(req), 0, 0xBF33, ident, ident_len);
    if (!req_len) {
        s_busy = false;
        lock_give();
        return fail(ESIM_ERR_FORMAT, "构造删除请求失败");
    }

    uint8_t *resp = malloc(256);
    size_t len = 0;
    e = resp ? es10x(req, req_len, resp, 256, &len) : fail(ESIM_ERR_NOMEM, "内存不足");
    if (e == ESIM_OK) {
        esim_tlv_t top, code;
        if (!esim_tlv_read(resp, len, 0, &top) || top.tag != 0xBF33 ||
            !esim_tlv_find(top.val, top.len, 0x80, &code) || code.len < 1) {
            e = fail(ESIM_ERR_FORMAT, "删除应答解析失败");
        } else {
            int ret = 0;
            for (size_t i = 0; i < code.len; i++) {
                ret = (ret << 8) | code.val[i];
            }
            if (ret != 0) {
                e = fail(ESIM_ERR_CARD, "eUICC 拒绝删除：%s（结果码 %d）", delete_reason(ret), ret);
            } else {
                log_line("eSIM：已删除 %s（没有触碰当前启用的配置）", trimmed);
            }
        }
    }
    free(resp);
    s_busy = false;
    lock_give();
    return e;
}

esim_err_t esim_enable_profile(const char *id, bool refresh, int *ecc)
{
    if (!id || !id[0]) {
        return fail(ESIM_ERR_ARG, "缺少 ICCID 或 AID");
    }
    if (ecc) {
        *ecc = -1;
    }
    esim_err_t e = lock_take();
    if (e != ESIM_OK) {
        return e;
    }
    s_busy = true;
    if (!card_probe()) {
        s_busy = false;
        lock_give();
        return ESIM_ERR_UNSUPPORTED;
    }

    char trimmed[40];
    snprintf(trimmed, sizeof(trimmed), "%s", id);
    util_trim(trimmed);

    uint8_t ident_val[16];
    size_t ident_val_len;
    uint32_t ident_tag;
    if (strlen(trimmed) == 32) {
        int n = esim_hex_to_bin(trimmed, ident_val, sizeof(ident_val));
        if (n != 16) {
            s_busy = false;
            lock_give();
            return fail(ESIM_ERR_ARG, "AID 不是 32 位十六进制");
        }
        ident_tag = 0x4F;
        ident_val_len = (size_t)n;
    } else {
        int n = esim_iccid_to_bcd(trimmed, ident_val);
        if (n != 10) {
            s_busy = false;
            lock_give();
            return fail(ESIM_ERR_ARG, "ICCID 必须是 18-20 位数字，或 32 位十六进制 AID");
        }
        ident_tag = 0x5A;
        ident_val_len = (size_t)n;
    }

    /* EnableProfileRequest ::= [49] { A0 { profileIdentifier, 81 refreshFlag } } */
    uint8_t ident[24];
    size_t choice_len = esim_der_put(ident, sizeof(ident), 0, ident_tag, ident_val, ident_val_len);

    uint8_t choice[32];
    memcpy(choice, ident, choice_len);
    uint8_t refresh_val = refresh ? 0xFF : 0x00;
    choice_len += esim_der_put(choice, sizeof(choice), choice_len, 0x81, &refresh_val, 1);

    uint8_t inner[40];
    size_t inner_len = esim_der_put(inner, sizeof(inner), 0, 0xA0, choice, choice_len);

    uint8_t req[48];
    size_t req_len = esim_der_put(req, sizeof(req), 0, 0xBF31, inner, inner_len);
    if (!req_len) {
        s_busy = false;
        lock_give();
        return fail(ESIM_ERR_FORMAT, "构造启用请求失败");
    }

    uint8_t *resp = malloc(512);
    size_t len = 0;
    e = resp ? es10x(req, req_len, resp, 512, &len) : fail(ESIM_ERR_NOMEM, "内存不足");
    if (e == ESIM_OK) {
        esim_tlv_t top, code;
        if (!esim_tlv_read(resp, len, 0, &top) || top.tag != 0xBF31 ||
            !esim_tlv_find(top.val, top.len, 0x80, &code) || code.len < 1) {
            e = fail(ESIM_ERR_FORMAT, "启用应答解析失败");
        } else {
            int ret = 0;
            for (size_t i = 0; i < code.len; i++) {
                ret = (ret << 8) | code.val[i];
            }
            if (ecc) {
                *ecc = ret;
            }
            if (ret != 0) {
                e = fail(ESIM_ERR_CARD, "eUICC 拒绝启用：%s（结果码 %d）", enable_reason(ret), ret);
            } else {
                log_line("eSIM：已启用 %s（refresh=%s）", trimmed, refresh ? "是" : "否");
            }
        }
    }
    free(resp);
    s_busy = false;
    lock_give();
    return e;
}
