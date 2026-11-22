#include "web/web_ui.h"

#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/util.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "modem/modem.h"
#include "net/wifi.h"

/* 由 EMBED_TXTFILES 生成 */
extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];

static void html_escape(sbuf_t *out, const char *s)
{
    if (!s) {
        return;
    }
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&': sbuf_append(out, "&amp;"); break;
        case '<': sbuf_append(out, "&lt;"); break;
        case '>': sbuf_append(out, "&gt;"); break;
        case '"': sbuf_append(out, "&quot;"); break;
        case '\'': sbuf_append(out, "&#39;"); break;
        default: sbuf_append_char(out, *p);
        }
    }
}

static void html_attr(sbuf_t *out, const char *s)
{
    html_escape(out, s);
}

static void render_channels(sbuf_t *out)
{
    app_config_t *cfg = config_lock();
    for (int i = 0; i < PUSH_CHANNEL_MAX; i++) {
        const push_channel_t *ch = &cfg->channels[i];
        sbuf_printf(out, "<div class=\"push-channel%s\" id=\"channel%d\">",
                    ch->enabled ? " enabled" : "", i);
        sbuf_append(out, "<div class=\"push-channel-header\">");
        sbuf_printf(out,
                    "<input type=\"checkbox\" name=\"push%den\" id=\"push%den\" onchange=\"toggleChannel(%d)\"%s>",
                    i, i, i, ch->enabled ? " checked" : "");
        sbuf_printf(out,
                    "<label for=\"push%den\" class=\"label-inline\">启用推送通道 %d</label></div>",
                    i, i + 1);
        sbuf_append(out, "<div class=\"push-channel-body\">");

        sbuf_append(out, "<div class=\"form-group\"><label>通道名称</label>");
        sbuf_printf(out, "<input type=\"text\" name=\"push%dname\" value=\"", i);
        html_attr(out, ch->name);
        sbuf_append(out, "\" placeholder=\"自定义名称\"></div>");

        sbuf_append(out, "<div class=\"form-group\"><label>推送方式</label>");
        sbuf_printf(out, "<select name=\"push%dtype\" id=\"push%dtype\" onchange=\"updateTypeHint(%d)\">",
                    i, i, i);
        static const struct {
            int type;
            const char *label;
        } options[] = {
            {PUSH_TYPE_POST_JSON, "POST JSON（通用格式）"},
            {PUSH_TYPE_BARK, "Bark（iOS推送）"},
            {PUSH_TYPE_GET, "GET请求（参数在URL中）"},
            {PUSH_TYPE_DINGTALK, "钉钉机器人"},
            {PUSH_TYPE_PUSHPLUS, "PushPlus"},
            {PUSH_TYPE_SERVERCHAN, "Server酱"},
            {PUSH_TYPE_CUSTOM, "自定义模板"},
            {PUSH_TYPE_FEISHU, "飞书机器人"},
            {PUSH_TYPE_GOTIFY, "Gotify"},
            {PUSH_TYPE_TELEGRAM, "Telegram Bot"},
        };
        for (size_t k = 0; k < sizeof(options) / sizeof(options[0]); k++) {
            sbuf_printf(out, "<option value=\"%d\"%s>%s</option>", options[k].type,
                        ch->type == options[k].type ? " selected" : "", options[k].label);
        }
        sbuf_append(out, "</select>");
        sbuf_printf(out, "<div class=\"push-type-hint\" id=\"hint%d\"></div></div>", i);

        sbuf_append(out, "<div class=\"form-group\"><label>推送URL/Webhook</label>");
        sbuf_printf(out, "<input type=\"text\" name=\"push%durl\" id=\"url%d\" value=\"", i, i);
        html_attr(out, ch->url);
        sbuf_append(out, "\" placeholder=\"http://your-server.com/api 或 webhook地址\"></div>");

        sbuf_printf(out, "<div id=\"extra%d\" style=\"display:none;\">", i);
        sbuf_append(out, "<div class=\"form-group\">");
        sbuf_printf(out, "<label id=\"key1label%d\">参数1</label>", i);
        sbuf_printf(out, "<input type=\"text\" name=\"push%dkey1\" id=\"key1%d\" value=\"", i, i);
        html_attr(out, ch->key1);
        sbuf_append(out, "\"></div>");
        sbuf_printf(out, "<div class=\"form-group\" id=\"key2group%d\">", i);
        sbuf_printf(out, "<label id=\"key2label%d\">参数2</label>", i);
        sbuf_printf(out, "<input type=\"text\" name=\"push%dkey2\" id=\"key2%d\" value=\"", i, i);
        html_attr(out, ch->key2);
        sbuf_append(out, "\"></div></div>");

        sbuf_printf(out, "<div id=\"custom%d\" style=\"display:none;\">", i);
        sbuf_append(out, "<div class=\"form-group\">");
        sbuf_append(out,
                    "<label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label>");
        sbuf_printf(out,
                    "<textarea name=\"push%dbody\" rows=\"4\" style=\"width:100%%;font-family:monospace;\">",
                    i);
        html_escape(out, ch->custom_body);
        sbuf_append(out, "</textarea></div></div>");

        sbuf_append(out, "</div></div>");
    }
    config_unlock();
}

static void format_uptime(char *out, size_t cap)
{
    int64_t sec = esp_timer_get_time() / 1000000;
    snprintf(out, cap, "%lld:%02lld:%02lld", (long long)(sec / 3600),
             (long long)((sec % 3600) / 60), (long long)(sec % 60));
}

/* 根据占位符输出对应值 */
static bool emit_placeholder(sbuf_t *out, const char *key, size_t key_len,
                             const char *channels_html)
{
    app_config_t *cfg = config_lock();
    bool handled = true;

    if (key_len == 2 && strncmp(key, "IP", 2) == 0) {
        sbuf_append(out, wifi_ip());
    } else if (key_len == 9 && strncmp(key, "WIFI_SSID", 9) == 0) {
        wifi_info_t info;
        wifi_get_info(&info);
        html_escape(out, info.ssid);
    } else if (key_len == 9 && strncmp(key, "FREE_HEAP", 9) == 0) {
        sbuf_printf(out, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024));
    } else if (key_len == 6 && strncmp(key, "UPTIME", 6) == 0) {
        char uptime[24];
        format_uptime(uptime, sizeof(uptime));
        sbuf_append(out, uptime);
    } else if (key_len == 8 && strncmp(key, "WEB_USER", 8) == 0) {
        html_attr(out, cfg->web_user);
    } else if (key_len == 8 && strncmp(key, "WEB_PASS", 8) == 0) {
        html_attr(out, cfg->web_pass);
    } else if (key_len == 11 && strncmp(key, "SMTP_SERVER", 11) == 0) {
        html_attr(out, cfg->smtp_server);
    } else if (key_len == 9 && strncmp(key, "SMTP_PORT", 9) == 0) {
        sbuf_printf(out, "%u", (unsigned)cfg->smtp_port);
    } else if (key_len == 9 && strncmp(key, "SMTP_USER", 9) == 0) {
        html_attr(out, cfg->smtp_user);
    } else if (key_len == 9 && strncmp(key, "SMTP_PASS", 9) == 0) {
        html_attr(out, cfg->smtp_pass);
    } else if (key_len == 12 && strncmp(key, "SMTP_SEND_TO", 12) == 0) {
        html_attr(out, cfg->smtp_to);
    } else if (key_len == 11 && strncmp(key, "ADMIN_PHONE", 11) == 0) {
        html_escape(out, cfg->admin_phone);
    } else if (key_len == 17 && strncmp(key, "NUMBER_BLACK_LIST", 17) == 0) {
        html_escape(out, cfg->blacklist);
    } else if (key_len == 10 && strncmp(key, "SMTP_CHECK", 10) == 0) {
        sbuf_append(out, config_email_valid(cfg) ? "已配置" : "未配置");
    } else if (key_len == 11 && strncmp(key, "MODEM_CHECK", 11) == 0) {
        sbuf_append(out, modem_ready() ? "已就绪" : "未就绪");
    } else if (key_len == 10 && strncmp(key, "PUSH_COUNT", 10) == 0) {
        int count = 0;
        for (int i = 0; i < PUSH_CHANNEL_MAX; i++) {
            if (cfg->channels[i].enabled) {
                count++;
            }
        }
        sbuf_printf(out, "%d", count);
    } else if (key_len == 13 && strncmp(key, "PUSH_CHANNELS", 13) == 0) {
        sbuf_append(out, channels_html);
    } else {
        handled = false;
    }

    config_unlock();
    return handled;
}

#define UI_FLUSH_THRESHOLD 2048

esp_err_t web_ui_render_to(web_ui_sink_t sink, void *user)
{
    sbuf_t channels;
    sbuf_init(&channels);
    render_channels(&channels);

    sbuf_t out;
    sbuf_init(&out);

    const char *tmpl_start = _binary_index_html_start;
    size_t tmpl_len = (size_t)(_binary_index_html_end - _binary_index_html_start);

    esp_err_t err = ESP_OK;
    /* 只识别 %KEY% 形式的已知占位符；CSS 里的 100% 等字面百分号原样输出 */
    size_t i = 0;
    while (i < tmpl_len && err == ESP_OK) {
        if (tmpl_start[i] != '%') {
            sbuf_append_char(&out, tmpl_start[i++]);
        } else {
            size_t j = i + 1;
            while (j < tmpl_len && j - i <= 24) {
                char c = tmpl_start[j];
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
                    j++;
                } else {
                    break;
                }
            }
            if (j > i + 1 && j < tmpl_len && tmpl_start[j] == '%' &&
                emit_placeholder(&out, tmpl_start + i + 1, j - i - 1, sbuf_cstr(&channels))) {
                i = j + 1;
            } else {
                sbuf_append_char(&out, '%');
                i++;
            }
        }
        if (out.len >= UI_FLUSH_THRESHOLD) {
            err = sink(user, sbuf_cstr(&out), out.len);
            sbuf_reset(&out);
        }
    }

    if (err == ESP_OK && out.len > 0) {
        err = sink(user, sbuf_cstr(&out), out.len);
    }

    sbuf_free(&out);
    sbuf_free(&channels);
    return err;
}
