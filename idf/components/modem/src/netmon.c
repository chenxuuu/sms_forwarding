/**
 * @file netmon.c
 * @brief 蜂窝网络守护：定期确认 SIM 卡是否还注册在基站上，掉线自动分级自愈。
 *
 * 检测方式全部是 AT 信令查询（AT+CREG? / AT+CEREG? / AT+CSQ / AT+CPIN?），
 * 不上网、不发短信，因此不产生流量与话费；也不需要 SIM 卡支持任何增值业务。
 *
 * 自愈阶梯（仅在连续 2 次确认掉线后触发）：
 *   1. 射频重启：AT+CFUN=4 → AT+CFUN=1 → AT+COPS=0,0（重新搜网，最轻量）
 *   2. 软重启：AT+CFUN=1,1，等模组自己重启后重新初始化
 *   3. 硬重启：EN 引脚断电，彻底复位模组
 * 三级都失败 → 5 分钟后从第 1 级重新开始，避免无 SIM 卡时无休止地断电。
 *
 * 通知策略：日常掉线→重连只写日志；只有「一整轮（射频+软+硬重启）都失败」时
 * 才回调上层发一次通知（含掉线起始时间），恢复后再发一条恢复通知。
 */
#include "modem/modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "core/log.h"
#include "core/util.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem/modem_hw.h"

#define NETMON_FIRST_DELAY_MS 30000    /* 启动后先等初始化，再开始检测 */
#define NETMON_INIT_WAIT_MS 300000     /* 等初始化结束的上限 */
#define NETMON_INTERVAL_MS 60000       /* 在线时的检测间隔 */
#define NETMON_CONFIRM 2               /* 连续失败多少次才判定掉线 */
#define NETMON_REG_WAIT_MS 90000       /* 每级自愈后等待注册的时间 */
#define NETMON_STEP_GAP_MS 4000        /* 一级失败到下一级的间隔 */
#define NETMON_ROUND_BACKOFF_MS 300000 /* 三级都失败后的冷却时间 */
#define NETMON_RESET_TIMEOUT_MS 240000 /* 软/硬重启作业的等待上限 */

static modem_net_cb_t s_cb;
static volatile bool s_registered = true; /* 首次检测前不改变 modem_ready() 的判断 */
static modem_reg_t s_last = {.stat = -1, .csq = -1};

static void emit(modem_net_event_t ev, const char *detail)
{
    if (s_cb) {
        s_cb(ev, detail);
    }
}

static const char *reg_text(int stat)
{
    switch (stat) {
    case 0: return "未注册，未搜索";
    case 1: return "已注册，本地网络";
    case 2: return "未注册，正在搜索";
    case 3: return "注册被拒绝";
    case 4: return "注册状态未知";
    case 5: return "已注册，漫游";
    default: return "查询失败";
    }
}

static void csq_text(int csq, char *out, size_t cap)
{
    if (csq < 0) {
        snprintf(out, cap, "信号未知");
    } else if (csq == 99) {
        snprintf(out, cap, "无信号");
    } else {
        snprintf(out, cap, "信号 %d", csq);
    }
}

/** 解析 +CREG/+CEREG 查询应答中的 <stat>（格式 <n>,<stat>[,...]）。 */
static int parse_reg_stat(const char *resp, const char *prefix)
{
    const char *line = resp ? strstr(resp, prefix) : NULL;
    if (!line) {
        return -1;
    }
    const char *p = strchr(line, ':');
    if (!p) {
        return -1;
    }
    p++;
    char buf[96];
    size_t n = 0;
    while (*p && *p != '\r' && *p != '\n' && n + 1 < sizeof(buf)) {
        buf[n++] = *p++;
    }
    buf[n] = '\0';

    int fields[4] = {-1, -1, -1, -1};
    int cnt = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && cnt < 4; tok = strtok_r(NULL, ",", &save)) {
        util_trim(tok);
        fields[cnt++] = atoi(tok);
    }
    if (cnt >= 2) {
        return fields[1];
    }
    return cnt == 1 ? fields[0] : -1;
}

static int query_reg(const char *cmd, const char *prefix)
{
    char *resp = modem_at(cmd, 4000);
    int stat = parse_reg_stat(resp, prefix);
    free(resp);
    return stat;
}

static bool at_ok(const char *cmd, uint32_t timeout_ms)
{
    char *resp = modem_at(cmd, timeout_ms);
    bool ok = resp != NULL && strstr(resp, "OK") != NULL;
    free(resp);
    return ok;
}

/** 采集一次基站连接状态（4 条 AT，约 1~2 秒，不产生任何资费）。 */
static void net_check(modem_reg_t *r)
{
    memset(r, 0, sizeof(*r));
    r->stat = -1;
    r->csq = -1;

    char *resp = modem_at("AT", 4000);
    r->at_ok = resp != NULL;
    free(resp);
    if (!r->at_ok) {
        s_last = *r;
        return;
    }

    resp = modem_at("AT+CPIN?", 4000);
    r->sim_ready = resp && strstr(resp, "READY");
    free(resp);

    int creg = query_reg("AT+CREG?", "+CREG:");
    int cereg = query_reg("AT+CEREG?", "+CEREG:");
    r->stat = cereg >= 0 ? cereg : creg;
    r->registered = creg == 1 || creg == 5 || cereg == 1 || cereg == 5;

    resp = modem_at("AT+CSQ", 4000);
    if (resp) {
        const char *p = strstr(resp, "+CSQ:");
        if (p) {
            r->csq = atoi(p + 5);
        }
    }
    free(resp);

    /* 飞行模式是用户的明确选择（Web 开关），守护不能把它"修"回来 */
    resp = modem_at("AT+CFUN?", 4000);
    if (resp) {
        const char *p = strstr(resp, "+CFUN:");
        r->flight = p && atoi(p + 6) == 4;
    }
    free(resp);

    s_last = *r;
}

static bool wait_registered(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    while (esp_timer_get_time() / 1000 < deadline) {
        modem_reg_t r;
        net_check(&r);
        if (r.registered) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
    return false;
}

/** 第 1 级：射频重启 + 自动选网（最轻量，不重启模组）。 */
static bool rf_cycle(void)
{
    log_line("自愈 1/3：射频重启（CFUN=4 → CFUN=1）并重新自动选网");
    if (!at_ok("AT+CFUN=4", 8000)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!at_ok("AT+CFUN=1", 20000)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 恢复自动选网；部分运营商锁网/手动选网后不会自己回到自动模式 */
    char *resp = modem_at("AT+COPS=0,0", 30000);
    free(resp);

    /* CFUN 循环可能丢掉运行期设置：短信直报、PDU 模式、禁用数据连接 */
    at_ok("AT+CNMI=2,2,0,0,0", 3000);
    at_ok("AT+CMGF=0", 3000);
    if (strcmp(modem_model(), "ML307Y") != 0) {
        at_ok("AT+CGACT=0,1", 8000);
    }
    log_line("射频已重启，等待重新注册...");
    return true;
}

/** 掉线起点：优先用墙上时间（NTP 已同步），否则退回设备运行时长。 */
static void offline_stamp(const time_t *wall, int64_t up_s, char *out, size_t cap)
{
    if (wall && *wall > 1700000000) {
        struct tm tmv;
        if (localtime_r(wall, &tmv)) {
            strftime(out, cap, "%m-%d %H:%M", &tmv);
            return;
        }
    }
    snprintf(out, cap, "启动后 %lld 分钟", (long long)(up_s / 60));
}

static int offline_minutes(int64_t up_s)
{
    int64_t now_s = esp_timer_get_time() / 1000000;
    return (int)((now_s > up_s ? now_s - up_s : 0) / 60);
}

static void netmon_task(void *arg)
{
    (void)arg;
    log_line("蜂窝网络守护已启动：每 %d 秒查询一次基站注册（纯信令，不产生短信/流量）",
             NETMON_INTERVAL_MS / 1000);

    vTaskDelay(pdMS_TO_TICKS(NETMON_FIRST_DELAY_MS));
    for (int i = 0; i < NETMON_INIT_WAIT_MS / 2000 && !modem_init_done(); i++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    int fail = 0;
    int step = 0;
    int round = 0;
    int weak = 0;           /* 已注册但 CSQ=99 的连续次数 */
    bool flight_logged = false;
    bool alerted = false;   /* 已发过"无法重连"通知（避免重复打扰） */
    int64_t offline_up = 0; /* 掉线起点的 uptime（秒），0 表示当前在线 */
    time_t offline_wall = 0;

    while (1) {
        modem_reg_t r;
        net_check(&r);
        char csq[24];
        csq_text(r.csq, csq, sizeof(csq));

        if (r.flight) {
            if (!flight_logged) {
                log_line("飞行模式已开启，暂停基站检测（关闭飞行模式后自动恢复）");
                modem_set_ready(false); /* 射频关闭，首页不能显示「已就绪」 */
                flight_logged = true;
            }
            fail = 0;
            weak = 0;
            vTaskDelay(pdMS_TO_TICKS(NETMON_INTERVAL_MS));
            continue;
        }
        if (flight_logged) {
            log_line("飞行模式已关闭，恢复基站检测");
            flight_logged = false;
        }

        /* 已注册但连续读不到信号：注册状态是卡死前的残留值，需要重启射频 */
        if (r.registered && r.csq == 99) {
            if (++weak >= 3) {
                log_line("已注册但连续 %d 次读不到信号（CSQ 99），判定为射频卡死", weak);
                r.registered = false;
                weak = 0;
            }
        } else if (r.registered) {
            weak = 0;
        }

        if (r.registered) {
            if (offline_up > 0) {
                char stamp[40];
                offline_stamp(&offline_wall, offline_up, stamp, sizeof(stamp));
                int mins = offline_minutes(offline_up);
                log_line("基站注册已恢复（%s，掉线约 %d 分钟）", csq, mins);
                if (alerted) {
                    char detail[192];
                    snprintf(detail, sizeof(detail),
                             "自 %s 起掉线、多次重连失败，现已自动恢复，掉线约 %d 分钟", stamp,
                             mins);
                    emit(MODEM_NET_RESTORED, detail);
                    alerted = false;
                }
            }
            fail = 0;
            step = 0;
            round = 0;
            weak = 0;
            offline_up = 0;
            offline_wall = 0;
            s_registered = true;
            modem_set_ready(true);
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(NETMON_INTERVAL_MS));
            continue;
        }

        /* 两条注册查询都无应答（固件不支持）时不下结论，只记日志 */
        if (r.at_ok && r.stat < 0) {
            log_line("无法查询注册状态（CREG/CEREG 无应答），跳过本轮检测");
            vTaskDelay(pdMS_TO_TICKS(NETMON_INTERVAL_MS));
            continue;
        }

        fail++;
        log_line("基站检测异常 %d/%d：AT %s，SIM %s，%s，%s", fail, NETMON_CONFIRM,
                 r.at_ok ? "正常" : "无应答", r.sim_ready ? "就绪" : "未就绪",
                 reg_text(r.stat), csq);

        if (fail < NETMON_CONFIRM) {
            vTaskDelay(pdMS_TO_TICKS(NETMON_INTERVAL_MS));
            continue;
        }

        /* 已连续确认掉线：记录掉线起点，开始自愈（日常掉线只记日志，不打扰用户） */
        if (offline_up == 0) {
            offline_up = esp_timer_get_time() / 1000000;
            time_t now = time(NULL);
            offline_wall = now > 1700000000 ? now : 0;
            char stamp[40];
            offline_stamp(&offline_wall, offline_up, stamp, sizeof(stamp));
            log_line("确认掉线（自 %s 起），开始自动重连", stamp);
        }
        s_registered = false;
        modem_set_ready(false);
        led_set(true);

        step++;
        if (step > 3) {
            /* 一轮 = 射频重启 + 软重启 + 硬重启全部失败：此时才值得打扰用户 */
            step = 1;
            round++;
            char stamp[40];
            offline_stamp(&offline_wall, offline_up, stamp, sizeof(stamp));
            if (!alerted) {
                char detail[192];
                snprintf(detail, sizeof(detail),
                         "自 %s 起掉线，射频重启/软重启/硬重启共 %d 轮均未能恢复，仍在持续重试",
                         stamp, round);
                log_line("自愈 %d 轮均失败，发送一次通知：%s", round, detail);
                emit(MODEM_NET_FAILED, detail);
                alerted = true;
            } else {
                log_line("自愈第 %d 轮仍失败（已通知过，不再重复发送），%d 秒后重试", round,
                         NETMON_ROUND_BACKOFF_MS / 1000);
            }
            vTaskDelay(pdMS_TO_TICKS(NETMON_ROUND_BACKOFF_MS));
        }

        bool ok = false;
        if (step == 1) {
            ok = rf_cycle() && wait_registered(NETMON_REG_WAIT_MS);
        } else if (step == 2) {
            log_line("自愈 2/3：软重启模组（AT+CFUN=1,1）");
            ok = modem_soft_reset(NETMON_RESET_TIMEOUT_MS);
        } else {
            log_line("自愈 3/3：硬重启模组（EN 断电）");
            ok = modem_hard_reset(NETMON_RESET_TIMEOUT_MS);
        }

        if (ok) {
            log_line("基站重连成功（第 %d 级措施，掉线约 %d 分钟）", step,
                     offline_minutes(offline_up));
            fail = 0;
            step = 0;
            round = 0;
            s_registered = true;
            modem_set_ready(true);
            led_set(false);
        } else {
            log_line("第 %d 级基站重连失败，继续升级处理", step);
            vTaskDelay(pdMS_TO_TICKS(NETMON_STEP_GAP_MS));
        }
    }
}

void modem_set_net_event_handler(modem_net_cb_t cb)
{
    s_cb = cb;
}

void modem_net_info(modem_reg_t *out)
{
    if (out) {
        *out = s_last;
    }
}

bool modem_registered(void)
{
    return s_registered;
}

void modem_watchdog_start(void)
{
    if (xTaskCreate(netmon_task, "netmon", 4096, NULL, 3, NULL) != pdPASS) {
        log_line("蜂窝网络守护任务创建失败");
    }
}
