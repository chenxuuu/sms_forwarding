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

/**
 * 启动蜂窝网络守护（modem_start 内部已调用）。
 * 每 60 秒用 AT+CREG?/AT+CEREG? 检查是否还连在基站上——纯信令查询，不产生短信与流量费用；
 * 掉线后按「射频重启 → 软重启 → 硬重启」逐级自愈，全部失败则 5 分钟后重来。
 */
void modem_watchdog_start(void);

/** 模组是否可用：初始化完成且当前已注册基站（由网络守护维护）。 */
bool modem_ready(void);

/** 开机初始化流程是否已结束（无论成功失败）。 */
bool modem_init_done(void);

/** 供网络守护更新就绪状态（重连成功/确认掉线时调用）。 */
void modem_set_ready(bool ready);

/* ---- 基站连接状态（网络守护维护）---- */

typedef struct {
    bool at_ok;      /* 模组是否响应 AT */
    bool sim_ready;  /* AT+CPIN? 是否 READY */
    bool registered; /* CREG 或 CEREG 为 1(本地)/5(漫游) */
    bool flight;     /* 用户手动开启了飞行模式（CFUN=4），守护不介入 */
    int stat;        /* 注册状态码 0..5，-1 未知 */
    int csq;         /* 0..31 信号强度，99 无信号，-1 未知 */
} modem_reg_t;

/** 最近一次基站检测结果。 */
void modem_net_info(modem_reg_t *out);

/** 当前是否已注册基站（网络守护维护，未检测过时为 true）。 */
bool modem_registered(void);

typedef enum {
    MODEM_NET_FAILED = 0, /* 射频/软/硬重启多轮都救不回来：此时才通知用户 */
    MODEM_NET_RESTORED,   /* 发过失败通知后又自动恢复 */
} modem_net_event_t;

typedef void (*modem_net_cb_t)(modem_net_event_t ev, const char *detail);

/**
 * 注册网络事件回调。
 * 日常掉线→自动重连只写日志，不触发回调；回调仅在「多轮自愈失败」和
 * 「失败后恢复」时发生，避免频繁打扰。
 */
void modem_set_net_event_handler(modem_net_cb_t cb);

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

/**
 * 临时激活数据连接并 ping 指定目标，完成后自动关闭数据连接。
 * @param ip 目标 IP 或域名；为 NULL/空时默认 8.8.8.8。
 */
bool modem_ping_ip(const char *ip, modem_ping_result_t *out, uint32_t timeout_ms);

/** 兼容旧调用：ping 8.8.8.8。 */
bool modem_ping(modem_ping_result_t *out, uint32_t timeout_ms);

typedef struct {
    bool success;
    char detail[160];
    int bytes; /* 实际收到的 body 字节数，-1 表示未知 */
} modem_http_result_t;

/**
 * 主动访问 http/https 网址（保号 / 消耗少量流量）。
 * 用模组 MHTTP 指令发起 GET，统计响应字节数，完成后删除实例并关闭数据连接。
 * @param url 形如 https://host/path，path 支持 ?a=b 查询串。
 */
bool modem_http_get(const char *url, modem_http_result_t *out, uint32_t timeout_ms);

/**
 * 复核「短信直报」配置（+CNMI=2,2 / +CMGF=0），被模组清掉就重新下发。
 *
 * ML307Y 在 CFUN 复位后（SIM 重新 +CPIN: READY 前后）会把 +CNMI 恢复成默认的
 * 0,0,0,0,0，之后不再上报 +CMT，表现为「收不到短信且日志毫无动静」。网络守护
 * 每分钟调一次即可；配置正常时只是多一条 AT+CNMI? 查询。
 */
void modem_sms_config_check(void);
