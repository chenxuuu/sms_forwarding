#include "modem/modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/util.h"
#include "esp_check.h"
#include "esp_timer.h"
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
    JOB_HTTP_GET,
} modem_job_type_t;

typedef struct {
    modem_job_type_t type;
    char arg[MODEM_JOB_ARG_MAX];
    char text[MODEM_JOB_TEXT_MAX];
    uint32_t timeout_ms;
    bool ok;
    char *result;
    modem_ping_result_t ping;
    modem_http_result_t http;
    SemaphoreHandle_t done;
    volatile bool abandoned; /* 调用方等待超时：作业仍归 worker，由 worker 释放 */
} modem_job_t;

/* 队列元素是作业指针：工作线程改写同一块内存，调用方才能拿到 ok/result */
static QueueHandle_t s_queue;
static modem_job_t *job_new(modem_job_type_t type);
static volatile bool s_ready;
static volatile bool s_init_done;
static char s_model[MODEM_MODEL_MAX];
static char s_version[MODEM_VERSION_MAX];

/* ---- URC：短信上报 ---- */
#define PDU_WAIT_MAX_LINES 16
#define PDU_WAIT_MAX_MS 5000

static bool s_expect_pdu;
static volatile bool s_sms_cfg_dirty = true; /* 需要复核 +CNMI（SIM 重新初始化后会丢） */
static int s_pdu_wait_lines;  /* +CMT 之后夹进来的非 PDU 行数 */
static int64_t s_pdu_wait_ms; /* 开始等 PDU 的时刻 */
static int s_last_urc_stat = -1;

/** PDU 是纯十六进制、够长；用来和夹在中间的 AT 应答区分开 */
static bool looks_like_pdu(const char *line)
{
    size_t n = strlen(line);
    return n >= 20 && (n % 2) == 0 && util_is_hex_string(line);
}

static void handle_urc(const char *line)
{
    if (s_expect_pdu) {
        if (line[0] == '\0') {
            return; /* 空行忽略，继续等 PDU */
        }
        if (looks_like_pdu(line)) {
            s_expect_pdu = false;
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
        /* 模组会先把 +CMT 报上来，再回当前 AT 命令的应答（+CPIN: READY 等），
           PDU 夹在它后面。这里不能当成状态错位直接放弃，否则整条短信就丢了。 */
        if (s_pdu_wait_lines == 0) {
            s_pdu_wait_ms = esp_timer_get_time() / 1000;
        }
        s_pdu_wait_lines++;
        if (s_pdu_wait_lines > PDU_WAIT_MAX_LINES ||
            esp_timer_get_time() / 1000 - s_pdu_wait_ms > PDU_WAIT_MAX_MS) {
            s_expect_pdu = false;
            log_line("等待 PDU 超时（夹了 %d 行其它数据），该条短信已丢弃", s_pdu_wait_lines);
        }
        /* 这一行仍按普通 URC 继续解析（注册状态等） */
    }

    if (util_starts_with(line, "+CMT:")) {
        s_expect_pdu = true;
        s_pdu_wait_lines = 0;
        s_pdu_wait_ms = 0;
        return;
    }
    if (util_starts_with(line, "+CMTI:")) {
        log_line("收到 +CMTI（短信存储通知）: %s", line);
        return;
    }
    /* SIM 重新 READY（CFUN 循环、模组自重启）时，模组会把 +CNMI 恢复成默认的
     * 0,0,0,0,0——短信从此不再上报，日志里连 +CMT 都没有。标记一下让守护任务复核。 */
    if (util_starts_with(line, "+CPIN:")) {
        if (strstr(line, "READY")) {
            s_sms_cfg_dirty = true;
        }
        return;
    }
    /* 网络守护每分钟轮询的应答：不逐条记录，只在注册状态真正变化时打一行 */
    if (util_starts_with(line, "+CSQ:") || util_starts_with(line, "+CFUN:")) {
        return;
    }
    if (util_starts_with(line, "+CREG:") || util_starts_with(line, "+CEREG:") ||
        util_starts_with(line, "+CGREG:")) {
        int stat = -1;
        const char *p = strchr(line, ':');
        if (p) {
            const char *q = p + 1;
            while (*q == ' ') q++;
            char buf[64];
            size_t n = 0;
            while (*q && *q != '\r' && *q != '\n' && n + 1 < sizeof(buf)) {
                buf[n++] = *q++;
            }
            buf[n] = '\0';
            int first = -1, second = -1;
            char *save = NULL;
            char *tok = strtok_r(buf, ",", &save);
            if (tok) {
                util_trim(tok);
                first = atoi(tok);
                tok = strtok_r(NULL, ",", &save);
                if (tok) {
                    util_trim(tok);
                    second = atoi(tok);
                }
            }
            stat = second >= 0 ? second : first;
        }
        if (stat >= 0 && stat != s_last_urc_stat) {
            s_last_urc_stat = stat;
            log_line("模组注册状态变化: %s", line);
        }
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

/** 从应答中逐行解析注册状态，只认 +CEREG/+CREG 行的第 2 字段（stat 1=本地 / 5=漫游）。 */
static bool reg_registered(const char *resp)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", resp);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        util_trim(line);
        if (!util_starts_with(line, "+CEREG:") && !util_starts_with(line, "+CREG:")) {
            continue;
        }
        const char *colon = strchr(line, ':');
        if (!colon) continue;
        /* 查询应答格式 <n>,<stat>[,...]；URC 格式 <stat>[,...]（CREG=2 时 stat 后直接是 LAC） */
        int first = -1, second = -1;
        char fields[8][32] = {{0}};
        int cnt = 0;
        char f[128];
        snprintf(f, sizeof(f), "%s", colon + 1);
        char *fsave = NULL;
        char *tok = strtok_r(f, ",", &fsave);
        while (tok && cnt < 8) {
            util_trim(tok);
            snprintf(fields[cnt], sizeof(fields[cnt]), "%s", tok);
            if (cnt == 0) first = atoi(fields[cnt]);
            if (cnt == 1) second = atoi(fields[cnt]);
            cnt++;
            tok = strtok_r(NULL, ",", &fsave);
        }
        int stat = -1;
        if (cnt >= 2 && fields[1][0] >= '0' && fields[1][0] <= '9') {
            stat = second;           /* 查询应答：<n>,<stat> */
        } else if (cnt >= 1) {
            stat = first;            /* URC：<stat>,"<lac>",... */
        }
        if (stat == 1 || stat == 5) return true;
    }
    return false;
}

static bool wait_registered(void)
{
    for (int i = 0; i < 30; i++) {
        char resp[512] = {0};
        if (at_command("AT+CEREG?", resp, sizeof(resp), 2000) != AT_RESULT_TIMEOUT) {
            if (reg_registered(resp)) {
                return true;
            }
        }
        log_line("等待网络注册... (%d/30)", i + 1);
        led_blink(500);
    }
    return false;
}

/**
 * @param power_cycle true=硬复位（EN 断电）；false=仅软复位后的重新配置，
 *                    用于 AT+CFUN=1,1 之后的重新初始化。
 */
/** 按配置应用选网：自动 AT+COPS=0；手动 AT+COPS=1,2,"PLMN",<AcT>。 */
static void apply_operator_selection(void)
{
    uint8_t mode;
    uint8_t act;
    char plmn[sizeof(((app_config_t *)0)->operator_code)];
    app_config_t *cfg = config_lock();
    mode = cfg->operator_mode;
    act = cfg->operator_act;
    snprintf(plmn, sizeof(plmn), "%s", cfg->operator_code);
    config_unlock();

    if (mode == OPERATOR_MODE_MANUAL && plmn[0] != '\0') {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+COPS=1,2,\"%s\",%u", plmn, (unsigned)act);
        log_line("应用选网配置：手动 PLMN %s（%s）", plmn,
                 act == OPERATOR_ACT_2G ? "2G" : act == OPERATOR_ACT_3G ? "3G" : "LTE");
        if (!at_command_ok(cmd, 60000)) {
            log_line("手动选网命令未成功，等待网络自行注册");
        }
    } else {
        if (mode == OPERATOR_MODE_MANUAL) {
            log_line("手动选网模式但未填写 PLMN，回退为自动选网");
        }
        log_line("应用选网配置：自动选择（AT+COPS=0）");
        if (!at_command_ok("AT+COPS=0", 60000)) {
            log_line("自动选网命令未成功，继续等待注册");
        }
    }
}

/* 从 AT+CNUM 应答里取出本机号码；SIM 卡未存储号码时返回 false */
static bool query_self_phone(char *out, size_t cap)
{
    char resp[512] = {0};
    if (at_command("AT+CNUM", resp, sizeof(resp), 3000) != AT_RESULT_OK) {
        return false;
    }
    const char *line = strstr(resp, "+CNUM:");
    if (!line) {
        return false;
    }
    const char *quote = strstr(line, ",\"");
    if (!quote) {
        return false;
    }
    const char *begin = quote + 2;
    const char *end = strchr(begin, '"');
    if (!end || end == begin) {
        return false;
    }
    size_t n = (size_t)(end - begin);
    if (n >= cap) {
        return false;
    }
    memcpy(out, begin, n);
    out[n] = '\0';
    util_trim(out);
    return out[0] != '\0';
}

/**
 * 本机号码：用户没填时尝试从 SIM 卡自动读取并落盘；已填写则保留用户值，
 * 仅在两者不一致时提示，避免换卡后悄悄改掉手填的号码。
 */
static void detect_self_phone(void)
{
    char num[24] = {0};
    if (!query_self_phone(num, sizeof(num))) {
        log_line("未能自动获取本机号码（AT+CNUM 无结果），可在后台「本机号码」手动填写");
        return;
    }

    char configured_num[sizeof(((app_config_t *)0)->self_phone)] = {0};
    app_config_t *cfg = config_lock();
    snprintf(configured_num, sizeof(configured_num), "%s", cfg->self_phone);
    bool configured = configured_num[0] != '\0';
    if (!configured) {
        snprintf(cfg->self_phone, sizeof(cfg->self_phone), "%s", num);
        config_save_locked();
    }
    config_unlock();

    if (!configured) {
        log_line("本机号码已自动获取: %s", num);
    } else if (strcmp(configured_num, num) != 0) {
        log_line("SIM 卡号码 %s 与配置的本机号码 %s 不同，保留配置值", num, configured_num);
    }
}

/* ---- 短信直报（+CNMI / +CMGF） ---- */

/* 读回 +CNMI，确认仍是「模式 2 + MT 直报(+CMT)」；0,0,0,0,0 就代表短信不再上报。 */
static bool sms_config_intact(void)
{
    char resp[128] = {0};
    if (at_command("AT+CNMI?", resp, sizeof(resp), 3000) != AT_RESULT_OK) {
        return false;
    }
    const char *p = strstr(resp, "+CNMI:");
    int mode = -1, mt = -1;
    if (!p || sscanf(p, "+CNMI: %d,%d", &mode, &mt) != 2) {
        return false;
    }
    return mode == 2 && mt == 2;
}

/** 下发短信直报配置（CNMI 直报 + PDU 模式）。 */
static void apply_sms_config(void)
{
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
    s_sms_cfg_dirty = false;
}

/**
 * 复核短信直报配置，被模组清掉就补上。
 *
 * 实测 ML307Y：AT+CFUN=1 之后约 8 秒（SIM 重新 +CPIN: READY 时）模组会把 +CNMI
 * 恢复成默认的 0,0,0,0,0，这条链路上从此不再报 +CMT——表现为「收不到短信，
 * 日志里一点动静都没有」。自愈流程在 CFUN=1 后 2 秒就补发 CNMI，正好被这次
 * 清零覆盖。所以除了 +CPIN READY 时打标记，还要定期读一次 +CNMI? 兜底。
 */
void modem_sms_config_check(void)
{
    static int tick;
    if (!s_init_done) {
        return;
    }
    /* 打标记（SIM 重新就绪）时立刻查；平时每 10 轮（约 10 分钟）查一次 */
    if (!s_sms_cfg_dirty && ++tick < 10) {
        return;
    }
    tick = 0;
    if (sms_config_intact()) {
        s_sms_cfg_dirty = false;
        return;
    }
    log_line("短信直报配置被模组复位（+CNMI 不是 2,2），重新开启");
    apply_sms_config();
}

static void do_init(bool power_cycle)
{
    s_ready = false;
    s_init_done = false;
    s_expect_pdu = false;
    sms_assembler_reset();

    if (power_cycle) {
        modem_power_cycle();
    }

    int cycles = 0;
    while (!wait_at_ready()) {
        if (++cycles >= 3) {
            log_line("模组连续 3 次无响应，放弃本次初始化（可稍后点“硬重启模组”）");
            s_init_done = true;
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

    /* 模组可能停在飞行模式（自愈的射频重启被复位打断会遗留 CFUN=4）：
     * 这里强制拉回正常射频，否则 netmon 会把它当成用户主动开飞行模式而一直暂停。 */
    char cfun[128] = {0};
    if (at_command("AT+CFUN?", cfun, sizeof(cfun), 3000) == AT_RESULT_OK &&
        strstr(cfun, "+CFUN: 4")) {
        log_line("模组停留在飞行模式，恢复为正常射频（AT+CFUN=1）");
        at_command_ok("AT+CFUN=1", 20000);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

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

    apply_sms_config();

    /* 本机号码：用户未配置时尝试自动读取，失败就保持为空 */
    detect_self_phone();

    /* 打开注册状态主动上报：小区切换/掉线会立刻在日志里出现 +CREG/+CEREG */
    at_command_ok("AT+CREG=2", 2000);
    at_command_ok("AT+CEREG=2", 2000);
    log_line("注册状态上报已开启（CREG=2 / CEREG=2）");

    /* 应用持久化的选网配置（默认自动选择） */
    apply_operator_selection();

    if (wait_registered()) {
        log_line("网络已注册，模组就绪");
        s_ready = true;
        led_set(false);
    } else {
        log_line("网络注册超时（无 SIM 卡或信号差），模组功能不可用");
        s_ready = false;
        led_set(true);
    }
    s_init_done = true;
    /* 注册/附着过程中模组可能又把 +CNMI 清回默认值，这里强制复核一次 */
    s_sms_cfg_dirty = true;
    modem_sms_config_check();
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
        char ip[64] = {0};
        snprintf(ip, sizeof(ip), "%s", fields[1]);
        size_t ilen = strlen(ip);
        if (ilen >= 2 && ip[0] == '"' && ip[ilen - 1] == '"') {
            memmove(ip, ip + 1, ilen - 2);
            ip[ilen - 2] = '\0';
        }
        const char *time_ms = fields[3];
        const char *ttl = count >= 5 ? fields[4] : "N/A";
        snprintf(detail, cap, "目标: %s, 延迟: %sms, TTL: %s", ip, time_ms, ttl);
        return true;
    }
    snprintf(detail, cap, "Ping 超时或目标不可达（错误码 %d）", result);
    return false;
}

/* ---- 保号 HTTP：URL 拆分与 MHTTP URC 解析 ---- */
static void hex_encode(const char *s, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = 0;
    while (*s && n + 2 < cap) {
        out[n++] = HEX[((unsigned char)*s >> 4) & 0xF];
        out[n++] = HEX[(unsigned char)*s & 0xF];
        s++;
    }
    out[n] = '\0';
}

/** 把 http(s)://host[:port]/path 拆成 scheme / host / path。 */
static bool split_url(const char *url, char *scheme, size_t sch_cap,
                      char *host, size_t host_cap, char *path, size_t path_cap)
{
    const char *p = url ? url : "";
    bool https = false;
    if (strncmp(p, "https://", 8) == 0) { https = true; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return false;

    snprintf(scheme, sch_cap, "%s", https ? "https" : "http");
    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= host_cap) return false;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    snprintf(path, path_cap, "%s", slash ? slash : "/");
    return true;
}

/**
 * 解析 +MHTTPURC: "content",<id>,<len>,... 或 "err",<id>,<code>。
 * @return true=收到内容（bytes 有效）；false=错误（err 有效）。
 */
static bool parse_mhttp_urc(const char *line, int *bytes, int *err)
{
    if (bytes) *bytes = -1;
    if (err) *err = -1;
    const char *p = strchr(line, ':');
    if (!p) return false;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", p + 1);
    char *fields[8] = {0};
    int cnt = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && cnt < 8;
         tok = strtok_r(NULL, ",", &save)) {
        util_trim(tok);
        fields[cnt++] = tok;
    }
    if (cnt < 3) return false;
    if (strstr(fields[0], "content")) {
        if (bytes) *bytes = atoi(fields[2]);
        return true;
    }
    if (strstr(fields[0], "err")) {
        if (err) *err = atoi(fields[2]);
        return false;
    }
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
            do_init(true);
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
            const char *ip = (job->arg[0] != '\0') ? job->arg : "qq.com";
            /* 不碰 AT+CGACT：ML307Y 执行 CGACT 会触发模组重启（初始化也跳过了），
             * 且重启后可能长时间搜不到网。模组注册后数据连接默认可用。 */
            log_line("Ping %s ...", ip);
            char line[256] = {0};
            char cmd[160] = {0};
            snprintf(cmd, sizeof(cmd), "AT+MPING=\"%s\",30,1", ip);
            bool got = at_command_wait_for(cmd, "+MPING:", line, sizeof(line), job->timeout_ms);
            if (got) {
                log_line("收到 MPING 结果: %s", line);
                job->ping.success = parse_mping(line, job->ping.detail, sizeof(job->ping.detail));
            } else {
                snprintf(job->ping.detail, sizeof(job->ping.detail), "Ping 超时或目标不可达");
            }
            job->ok = job->ping.success;
            break;
        }
        case JOB_HTTP_GET: {
            char scheme[8] = {0}, host[128] = {0}, path[256] = {0};
            char pathhex[512] = {0}, cmd[320] = {0};
            job->http.bytes = -1;
            if (!split_url(job->text, scheme, sizeof(scheme), host, sizeof(host),
                           path, sizeof(path))) {
                snprintf(job->http.detail, sizeof(job->http.detail),
                         "网址格式不正确，需以 http:// 或 https:// 开头");
                break;
            }
            hex_encode(path, pathhex, sizeof(pathhex));

            /* 不碰 AT+CGACT：ML307Y 执行 CGACT 会触发模组重启（初始化也跳过了） */
            log_line("保号访问 %s ...", job->text);

            at_command_ok("AT+MHTTPDEL=0", 3000);
            snprintf(cmd, sizeof(cmd), "AT+MHTTPCREATE=\"%s://%s\"", scheme, host);
            at_command_ok(cmd, 10000);
            if (strcmp(scheme, "https") == 0) {
                at_command_ok("AT+MHTTPCFG=\"ssl\",0,1,0", 3000);
            }
            at_command_ok("AT+MHTTPCFG=\"encoding\",0,0,0", 3000);
            at_command_ok("AT+MHTTPCFG=\"encoding\",0,1,1", 3000);

            snprintf(cmd, sizeof(cmd), "AT+MHTTPREQUEST=0,1,0,%s", pathhex);
            char urc[512] = {0};
            bool got = at_command_wait_for(cmd, "+MHTTPURC:", urc, sizeof(urc), job->timeout_ms);
            log_line("保号访问模组应答: %s", urc[0] ? urc : "（无）");

            int bytes = -1, err = -1;
            bool content = got && parse_mhttp_urc(urc, &bytes, &err);
            if (content) {
                job->http.bytes = bytes;
                job->http.success = true;
                snprintf(job->http.detail, sizeof(job->http.detail),
                         "访问成功，收到 %d 字节数据", bytes);
            } else if (err >= 0) {
                snprintf(job->http.detail, sizeof(job->http.detail),
                         "访问失败（模组错误码 %d），请检查网址是否可达", err);
            } else {
                snprintf(job->http.detail, sizeof(job->http.detail),
                         "访问超时或模组无应答，请检查网址与 SIM 数据连接");
            }

            at_command_ok("AT+MHTTPDEL=0", 3000);
            job->ok = job->http.success;
            break;
        }
        case JOB_HARD_RESET:
            log_line("正在硬重启模组（EN 断电重启）...");
            do_init(true);
            job->ok = s_ready;
            break;
        case JOB_SOFT_RESET: {
            log_line("正在软重启模组（AT+CFUN=1,1）...");
            bool cmd_ok = at_command_ok("AT+CFUN=1,1", 15000);
            if (!cmd_ok) {
                log_line("软重启指令无应答，直接改为硬重启");
            }
            /* 等模组自己重启；硬复位由 do_init(power_cycle) 负责，这里不再断电 */
            vTaskDelay(pdMS_TO_TICKS(cmd_ok ? 12000 : 2000));
            do_init(!cmd_ok);
            job->ok = s_ready;
            break;
        }
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

    modem_watchdog_start();
    return ESP_OK;
}

bool modem_ready(void)
{
    return s_ready;
}

bool modem_init_done(void)
{
    return s_init_done;
}

void modem_set_ready(bool ready)
{
    s_ready = ready;
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

bool modem_ping_ip(const char *ip, modem_ping_result_t *out, uint32_t timeout_ms)
{
    modem_job_t *job = job_new(JOB_PING);
    if (!job) {
        return false;
    }
    if (ip && *ip) {
        snprintf(job->arg, sizeof(job->arg), "%s", ip);
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

bool modem_ping(modem_ping_result_t *out, uint32_t timeout_ms)
{
    return modem_ping_ip(NULL, out, timeout_ms);
}

bool modem_http_get(const char *url, modem_http_result_t *out, uint32_t timeout_ms)
{
    modem_job_t *job = job_new(JOB_HTTP_GET);
    if (!job) {
        return false;
    }
    snprintf(job->text, sizeof(job->text), "%s", url ? url : "");
    job->timeout_ms = timeout_ms;
    submit_result_t rc = submit(job, timeout_ms + 20000);
    bool ok = rc == SUBMIT_OK && job->http.success;
    if (out) {
        if (rc == SUBMIT_OK) {
            *out = job->http;
        } else if (!out->detail[0]) {
            snprintf(out->detail, sizeof(out->detail), "操作超时");
        }
    }
    if (rc != SUBMIT_TIMEOUT) {
        free(job);
    }
    return ok;
}
