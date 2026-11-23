/**
 * @file config.h
 * @brief 配置模型与 NVS 持久化。
 *
 * 配置存放在 NVS namespace "sms_config" 中，键名与旧 Arduino 固件完全一致，
 * 因此从 Arduino 版本升级时已有配置可以直接沿用。
 *
 * 线程安全：所有访问都必须先 config_lock()，用完 config_unlock()。
 * 写配置后调用 config_save_locked()，否则重启丢失。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PUSH_CHANNEL_MAX 50
#define PUSH_CHANNEL_MIN 1
#define PUSH_CHANNEL_DEFAULT 5

#define CFG_SMALL_LEN 32
#define CFG_MED_LEN 64
#define CFG_URL_LEN 256
#define CFG_KEY_LEN 128
#define CFG_BODY_LEN 512
#define CFG_BLACKLIST_LEN 512

/** 推送通道类型（数值与旧固件 / Web UI 保持一致，勿改） */
typedef enum {
    PUSH_TYPE_NONE = 0,
    PUSH_TYPE_POST_JSON = 1,
    PUSH_TYPE_BARK = 2,
    PUSH_TYPE_GET = 3,
    PUSH_TYPE_DINGTALK = 4,
    PUSH_TYPE_PUSHPLUS = 5,
    PUSH_TYPE_SERVERCHAN = 6,
    PUSH_TYPE_CUSTOM = 7,
    PUSH_TYPE_FEISHU = 8,
    PUSH_TYPE_GOTIFY = 9,
    PUSH_TYPE_TELEGRAM = 10,
} push_type_t;

typedef struct {
    bool enabled;
    uint8_t type;
    char name[24];
    char url[160];
    char key1[80];
    char key2[80];
    char custom_body[256];
} push_channel_t;

typedef struct {
    /* 邮件通道开关：可关闭；NVS 缺少 mailEnable 键时视为开启，兼容老配置 */
    bool mail_enabled;
    char smtp_server[CFG_MED_LEN];
    uint16_t smtp_port;
    char smtp_user[96];
    char smtp_pass[96];
    char smtp_to[96];
    char admin_phone[CFG_SMALL_LEN];
    push_channel_t *channels; /* 动态通道数组（堆上分配） */
    int channel_count;
    char web_user[CFG_SMALL_LEN];
    char web_pass[CFG_SMALL_LEN];
    char blacklist[CFG_BLACKLIST_LEN];
    /* 选网（键名与 Arduino 版一致：operatorMode / operatorCode，新增 operatorAct） */
    uint8_t operator_mode; /* 0=自动，1=手动 */
    uint8_t operator_act;  /* 0=2G / 2=3G / 7=LTE */
    char operator_code[8]; /* PLMN，5~6 位数字，如 46000 */
} app_config_t;

/* ---- 选网配置 ---- */
/* <mode>：0=自动选网，1=手动指定 PLMN */
#define OPERATOR_MODE_AUTO 0
#define OPERATOR_MODE_MANUAL 1
/* <AcT>：3GPP TS 27.007 规定的接入技术码 */
#define OPERATOR_ACT_2G 0   /* GSM */
#define OPERATOR_ACT_3G 2   /* UTRAN */
#define OPERATOR_ACT_LTE 7  /* E-UTRAN */
#define OPERATOR_ACT_DEFAULT OPERATOR_ACT_LTE

#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

/** 初始化 NVS 并加载配置（需在 app_main 中尽早调用）。 */
void config_init(void);

/** 取出全局配置并加锁（递归锁，可嵌套）。 */
app_config_t *config_lock(void);
void config_unlock(void);

/** 在持锁状态下把当前配置写入 NVS。 */
void config_save_locked(void);

bool config_email_valid(const app_config_t *cfg);
/** 邮件通道真正可用：开关打开且 SMTP 配置完整。 */
bool config_mail_active(const app_config_t *cfg);
bool config_push_channel_valid(const push_channel_t *ch);
/** 调整通道数量（1..PUSH_CHANNEL_MAX），需在持锁状态下调用。 */
bool config_set_channel_count(int count);

/** 至少配置了邮件或任一推送通道。 */
bool config_any_valid(void);

/* ---- 设备运行状态（不随配置重置，用于"地址变化"提醒）---- */
/** 读取上次成功联网时记录的 IP，没有则返回 false。 */
bool config_state_get_last_ip(char *out, size_t cap);
void config_state_set_last_ip(const char *ip);

/** 发送者是否为管理员（忽略 +86 前缀）。 */
bool config_sender_is_admin(const char *sender);
/** 发送者是否在号码黑名单中。 */
bool config_sender_blocked(const char *sender);
