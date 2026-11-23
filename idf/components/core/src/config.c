#include "core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "sms_config"

static app_config_t s_config;
static nvs_handle_t s_nvs;
static nvs_handle_t s_state_nvs;
static bool s_nvs_ready;
static bool s_state_ready;
static SemaphoreHandle_t s_mutex;

static void load_str(const char *key, char *dst, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(s_nvs, key, dst, &len) != ESP_OK) {
        snprintf(dst, cap, "%s", def ? def : "");
        return;
    }
    dst[cap - 1] = '\0';
}

static void load_channel(int i)
{
    push_channel_t *ch = &s_config.channels[i];
    char key[16];

    snprintf(key, sizeof(key), "push%dname", i);
    load_str(key, ch->name, sizeof(ch->name), "");
    if (ch->name[0] == '\0') {
        snprintf(ch->name, sizeof(ch->name), "未命名通道");
    }

    snprintf(key, sizeof(key), "push%dtype", i);
    uint8_t type = PUSH_TYPE_POST_JSON;
    nvs_get_u8(s_nvs, key, &type);
    ch->type = type;

    snprintf(key, sizeof(key), "push%den", i);
    uint8_t en = 0;
    nvs_get_u8(s_nvs, key, &en);
    ch->enabled = en != 0;

    snprintf(key, sizeof(key), "push%durl", i);
    load_str(key, ch->url, sizeof(ch->url), "");
    snprintf(key, sizeof(key), "push%dk1", i);
    load_str(key, ch->key1, sizeof(ch->key1), "");
    snprintf(key, sizeof(key), "push%dk2", i);
    load_str(key, ch->key2, sizeof(ch->key2), "");
    snprintf(key, sizeof(key), "push%dbody", i);
    load_str(key, ch->custom_body, sizeof(ch->custom_body), "");
}

static void load_all(void)
{
    memset(&s_config, 0, sizeof(s_config));

    load_str("smtpServer", s_config.smtp_server, sizeof(s_config.smtp_server), "");
    s_config.smtp_port = 465;
    uint16_t port = 0;
    if (nvs_get_u16(s_nvs, "smtpPort", &port) == ESP_OK && port != 0) {
        s_config.smtp_port = port;
    }
    /* 旧固件用 putInt 存端口，兼容读取 */
    int32_t port_i = 0;
    if (nvs_get_i32(s_nvs, "smtpPort", &port_i) == ESP_OK && port_i > 0) {
        s_config.smtp_port = (uint16_t)port_i;
    }
    load_str("smtpUser", s_config.smtp_user, sizeof(s_config.smtp_user), "");
    load_str("smtpPass", s_config.smtp_pass, sizeof(s_config.smtp_pass), "");
    load_str("smtpSendTo", s_config.smtp_to, sizeof(s_config.smtp_to), "");
    /* 邮件开关：新键 mailEnable；老配置没有该键，缺省开启以保持原行为 */
    uint8_t mail_en = 1;
    nvs_get_u8(s_nvs, "mailEnable", &mail_en);
    s_config.mail_enabled = (mail_en != 0);
    load_str("adminPhone", s_config.admin_phone, sizeof(s_config.admin_phone), "");
    load_str("webUser", s_config.web_user, sizeof(s_config.web_user), DEFAULT_WEB_USER);
    load_str("webPass", s_config.web_pass, sizeof(s_config.web_pass), DEFAULT_WEB_PASS);
    load_str("numBlkList", s_config.blacklist, sizeof(s_config.blacklist), "");

    /* 选网：operatorMode/operatorCode 沿用 Arduino 版键名，operatorAct 为新增 */
    uint8_t op_mode = OPERATOR_MODE_AUTO;
    nvs_get_u8(s_nvs, "operatorMode", &op_mode);
    s_config.operator_mode = (op_mode == OPERATOR_MODE_MANUAL) ? OPERATOR_MODE_MANUAL : OPERATOR_MODE_AUTO;
    uint8_t op_act = OPERATOR_ACT_DEFAULT;
    if (nvs_get_u8(s_nvs, "operatorAct", &op_act) != ESP_OK ||
        (op_act != OPERATOR_ACT_2G && op_act != OPERATOR_ACT_3G && op_act != OPERATOR_ACT_LTE)) {
        op_act = OPERATOR_ACT_DEFAULT;
    }
    s_config.operator_act = op_act;
    load_str("operatorCode", s_config.operator_code, sizeof(s_config.operator_code), "");

    uint8_t cnt = 0;
    bool has_cnt = nvs_get_u8(s_nvs, "pushCount", &cnt) == ESP_OK &&
                   cnt >= PUSH_CHANNEL_MIN && cnt <= PUSH_CHANNEL_MAX;
    if (!has_cnt) {
        int last = -1;
        for (int i = 0; i < PUSH_CHANNEL_MAX; i++) {
            char k[16];
            uint8_t v = 0;
            snprintf(k, sizeof(k), "push%den", i);
            if (nvs_get_u8(s_nvs, k, &v) == ESP_OK) {
                last = i;
            }
        }
        cnt = (last >= 0) ? (uint8_t)(last + 1) : PUSH_CHANNEL_DEFAULT;
    }
    s_config.channel_count = cnt;
    s_config.channels = calloc(cnt, sizeof(push_channel_t));
    if (!s_config.channels) {
        s_config.channel_count = 0;
    }
    for (int i = 0; i < s_config.channel_count; i++) {
        load_channel(i);
    }

    /* 兼容更早版本的单一 httpUrl 配置 */
    char legacy_url[CFG_URL_LEN] = {0};
    size_t len = sizeof(legacy_url);
    if (nvs_get_str(s_nvs, "httpUrl", legacy_url, &len) == ESP_OK && legacy_url[0] &&
        !s_config.channels[0].enabled) {
        s_config.channels[0].enabled = true;
        snprintf(s_config.channels[0].url, sizeof(s_config.channels[0].url), "%s", legacy_url);
        snprintf(s_config.channels[0].name, sizeof(s_config.channels[0].name), "迁移通道");
        uint8_t bark = 0;
        nvs_get_u8(s_nvs, "barkMode", &bark);
        s_config.channels[0].type = bark ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
        log_line("已迁移旧 HTTP 配置到推送通道 1");
    }
}

void config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_mutex = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_mutex);

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        log_line("NVS 打开失败: %s", esp_err_to_name(err));
        s_nvs_ready = false;
        memset(&s_config, 0, sizeof(s_config));
        snprintf(s_config.web_user, sizeof(s_config.web_user), DEFAULT_WEB_USER);
        snprintf(s_config.web_pass, sizeof(s_config.web_pass), DEFAULT_WEB_PASS);
        s_config.smtp_port = 465;
        s_config.mail_enabled = true;
        s_config.operator_mode = OPERATOR_MODE_AUTO;
        s_config.operator_act = OPERATOR_ACT_DEFAULT;
        s_config.channel_count = PUSH_CHANNEL_DEFAULT;
        s_config.channels = calloc(PUSH_CHANNEL_DEFAULT, sizeof(push_channel_t));
        for (int i = 0; i < s_config.channel_count; i++) {
            snprintf(s_config.channels[i].name, sizeof(s_config.channels[i].name), "通道%d", i + 1);
            s_config.channels[i].type = PUSH_TYPE_POST_JSON;
        }
        return;
    }

    s_nvs_ready = true;
    if (nvs_open("sms_state", NVS_READWRITE, &s_state_nvs) == ESP_OK) {
        s_state_ready = true;
    }
    load_all();
    log_line("配置已加载（用户: %s, 推送通道: %d 个）", s_config.web_user, s_config.channel_count);
}

app_config_t *config_lock(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    return &s_config;
}

void config_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

void config_save_locked(void)
{
    if (!s_nvs_ready) {
        return;
    }
    nvs_set_str(s_nvs, "smtpServer", s_config.smtp_server);
    nvs_set_u16(s_nvs, "smtpPort", s_config.smtp_port);
    nvs_set_str(s_nvs, "smtpUser", s_config.smtp_user);
    nvs_set_str(s_nvs, "smtpPass", s_config.smtp_pass);
    nvs_set_str(s_nvs, "smtpSendTo", s_config.smtp_to);
    nvs_set_u8(s_nvs, "mailEnable", s_config.mail_enabled ? 1 : 0);
    nvs_set_str(s_nvs, "adminPhone", s_config.admin_phone);
    nvs_set_str(s_nvs, "webUser", s_config.web_user);
    nvs_set_str(s_nvs, "webPass", s_config.web_pass);
    nvs_set_str(s_nvs, "numBlkList", s_config.blacklist);

    nvs_set_u8(s_nvs, "operatorMode", s_config.operator_mode);
    nvs_set_u8(s_nvs, "operatorAct", s_config.operator_act);
    nvs_set_str(s_nvs, "operatorCode", s_config.operator_code);

    nvs_set_u8(s_nvs, "pushCount", (uint8_t)s_config.channel_count);
    for (int i = 0; i < s_config.channel_count; i++) {
        const push_channel_t *ch = &s_config.channels[i];
        char key[16];
        snprintf(key, sizeof(key), "push%den", i);
        nvs_set_u8(s_nvs, key, ch->enabled ? 1 : 0);
        snprintf(key, sizeof(key), "push%dtype", i);
        nvs_set_u8(s_nvs, key, ch->type);
        snprintf(key, sizeof(key), "push%durl", i);
        nvs_set_str(s_nvs, key, ch->url);
        snprintf(key, sizeof(key), "push%dname", i);
        nvs_set_str(s_nvs, key, ch->name);
        snprintf(key, sizeof(key), "push%dk1", i);
        nvs_set_str(s_nvs, key, ch->key1);
        snprintf(key, sizeof(key), "push%dk2", i);
        nvs_set_str(s_nvs, key, ch->key2);
        snprintf(key, sizeof(key), "push%dbody", i);
        nvs_set_str(s_nvs, key, ch->custom_body);
    }
    for (int i = s_config.channel_count; i < PUSH_CHANNEL_MAX; i++) {
        const char *fields[] = {"en", "type", "url", "name", "k1", "k2", "body"};
        for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
            char key[16];
            snprintf(key, sizeof(key), "push%d%s", i, fields[f]);
            nvs_erase_key(s_nvs, key);
        }
    }
    nvs_commit(s_nvs);
    log_line("配置已保存（推送通道 %d 个）", s_config.channel_count);
}

bool config_state_get_last_ip(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (!s_state_ready) {
        return false;
    }
    size_t len = cap;
    if (nvs_get_str(s_state_nvs, "lastIp", out, &len) != ESP_OK || out[0] == '\0') {
        out[0] = '\0';
        return false;
    }
    out[cap - 1] = '\0';
    return true;
}

void config_state_set_last_ip(const char *ip)
{
    if (!s_state_ready || !ip) {
        return;
    }
    nvs_set_str(s_state_nvs, "lastIp", ip);
    nvs_commit(s_state_nvs);
}

bool config_set_channel_count(int count)
{
    if (count < PUSH_CHANNEL_MIN) {
        count = PUSH_CHANNEL_MIN;
    }
    if (count > PUSH_CHANNEL_MAX) {
        count = PUSH_CHANNEL_MAX;
    }
    if (count == s_config.channel_count) {
        return true;
    }
    push_channel_t *arr = calloc((size_t)count, sizeof(push_channel_t));
    if (!arr) {
        return false;
    }
    int copy = count < s_config.channel_count ? count : s_config.channel_count;
    if (s_config.channels && copy > 0) {
        memcpy(arr, s_config.channels, (size_t)copy * sizeof(push_channel_t));
    }
    for (int i = copy; i < count; i++) {
        snprintf(arr[i].name, sizeof(arr[i].name), "未命名通道");
        arr[i].type = PUSH_TYPE_POST_JSON;
    }
    free(s_config.channels);
    s_config.channels = arr;
    s_config.channel_count = count;
    return true;
}

bool config_email_valid(const app_config_t *cfg)
{
    return cfg->smtp_server[0] && cfg->smtp_user[0] && cfg->smtp_pass[0] && cfg->smtp_to[0];
}

bool config_mail_active(const app_config_t *cfg)
{
    return cfg->mail_enabled && config_email_valid(cfg);
}

bool config_push_channel_valid(const push_channel_t *ch)
{
    if (!ch->enabled) {
        return false;
    }
    switch (ch->type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_CUSTOM:
        return ch->url[0] != '\0';
    case PUSH_TYPE_PUSHPLUS:
    case PUSH_TYPE_SERVERCHAN:
        return ch->key1[0] != '\0';
    case PUSH_TYPE_GOTIFY:
        return ch->url[0] != '\0' && ch->key1[0] != '\0';
    case PUSH_TYPE_TELEGRAM:
        return ch->key1[0] != '\0' && ch->key2[0] != '\0';
    default:
        return false;
    }
}

bool config_any_valid(void)
{
    app_config_t *cfg = config_lock();
    bool ok = config_mail_active(cfg);
    for (int i = 0; !ok && i < cfg->channel_count; i++) {
        ok = config_push_channel_valid(&cfg->channels[i]);
    }
    config_unlock();
    return ok;
}

/* 去掉 +86 前缀，便于号码比较 */
static const char *strip_86(const char *src)
{
    return util_starts_with(src, "+86") ? src + 3 : src;
}

bool config_sender_is_admin(const char *sender)
{
    app_config_t *cfg = config_lock();
    bool result = false;
    if (cfg->admin_phone[0] && sender && sender[0]) {
        result = util_equals(strip_86(sender), strip_86(cfg->admin_phone));
    }
    config_unlock();
    return result;
}

bool config_sender_blocked(const char *sender)
{
    if (!sender) {
        return false;
    }
    app_config_t *cfg = config_lock();
    bool blocked = false;
    if (cfg->blacklist[0]) {
        const char *stripped = util_starts_with(sender, "+86") ? sender + 3 : NULL;
        char line[64];
        const char *p = cfg->blacklist;
        while (*p && !blocked) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (n >= sizeof(line)) {
                n = sizeof(line) - 1;
            }
            memcpy(line, p, n);
            line[n] = '\0';
            util_trim(line);
            if (line[0] && (util_equals(line, sender) ||
                            (stripped && util_equals(line, stripped)))) {
                blocked = true;
            }
            p = nl ? nl + 1 : p + n;
        }
    }
    config_unlock();
    return blocked;
}
