#include "notify/email.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/config.h"
#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define SMTP_LINE 512

typedef struct {
    bool tls;
    esp_tls_t *tls_conn;
    int sock;
} smtp_conn_t;

static int conn_write(smtp_conn_t *c, const char *data, size_t len)
{
    if (c->tls) {
        return esp_tls_conn_write(c->tls_conn, data, len);
    }
    return send(c->sock, data, len, 0);
}

static int conn_read(smtp_conn_t *c, char *buf, size_t len)
{
    if (c->tls) {
        return esp_tls_conn_read(c->tls_conn, buf, len);
    }
    return recv(c->sock, buf, len, 0);
}

static void conn_close(smtp_conn_t *c)
{
    if (c->tls_conn) {
        esp_tls_conn_destroy(c->tls_conn);
        c->tls_conn = NULL;
    }
    if (c->sock >= 0) {
        close(c->sock);
        c->sock = -1;
    }
}

static bool resolve(const char *host, struct in_addr *out)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return false;
    }
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static bool conn_open(smtp_conn_t *c, const char *host, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->sock = -1;

    if (port == 465) {
        esp_tls_cfg_t cfg = {
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 15000,
        };
        c->tls_conn = esp_tls_init();
        if (!c->tls_conn) {
            return false;
        }
        if (esp_tls_conn_new_sync(host, strlen(host), port, &cfg, c->tls_conn) != 1) {
            /* 只打印「连接失败」没法定位：DNS/TCP 不通、证书校验失败、堆不够都会走到这里 */
            int tls_code = 0, tls_flags = 0;
            esp_tls_error_handle_t handle = NULL;
            if (esp_tls_get_error_handle(c->tls_conn, &handle) == ESP_OK && handle) {
                esp_tls_get_and_clear_last_error(handle, &tls_code, &tls_flags);
            }
            log_line("[邮件] TLS 握手失败 %s:%u（tls=0x%x flags=0x%x 可用堆 %u KB）", host,
                     (unsigned)port, (unsigned)tls_code, (unsigned)tls_flags,
                     (unsigned)(esp_get_free_heap_size() / 1024));
            conn_close(c);
            return false;
        }
        c->tls = true;
        return true;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!resolve(host, &addr.sin_addr)) {
        log_line("[邮件] 解析 %s 失败", host);
        return false;
    }
    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (c->sock < 0) {
        return false;
    }
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(c->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_line("[邮件] 连接 %s:%u 失败，errno=%d", host, (unsigned)port, errno);
        conn_close(c);
        return false;
    }
    return true;
}

/** 读取一条（可能多行的）SMTP 应答，返回状态码；-1 表示失败。 */
static int read_reply(smtp_conn_t *c, sbuf_t *text)
{
    char line[SMTP_LINE];
    size_t pos = 0;
    int code = -1;

    while (1) {
        char ch = 0;
        int n = conn_read(c, &ch, 1);
        if (n <= 0) {
            return -1;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch != '\n') {
            if (pos < sizeof(line) - 1) {
                line[pos++] = ch;
            }
            continue;
        }
        line[pos] = '\0';
        if (pos >= 3) {
            code = atoi(line);
            bool last = (pos == 3) || line[3] == ' ';
            if (text) {
                sbuf_append(text, line);
                sbuf_append_char(text, '\n');
            }
            if (last) {
                return code;
            }
        }
        pos = 0;
    }
}

static bool send_cmd(smtp_conn_t *c, const char *cmd, int expect, const char *what)
{
    sbuf_t line;
    sbuf_init(&line);
    sbuf_printf(&line, "%s\r\n", cmd);
    int n = conn_write(c, sbuf_cstr(&line), line.len);
    sbuf_free(&line);
    if (n <= 0) {
        log_line("[邮件] %s 发送失败", what);
        return false;
    }
    sbuf_t reply;
    sbuf_init(&reply);
    int code = read_reply(c, &reply);
    if (code != expect) {
        log_line("[邮件] %s 失败: %d %s", what, code, sbuf_cstr(&reply));
        sbuf_free(&reply);
        return false;
    }
    sbuf_free(&reply);
    return true;
}

static bool ascii_only(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return false;
        }
    }
    return true;
}

static void append_base64_wrapped(sbuf_t *out, const char *text)
{
    sbuf_t b64;
    sbuf_init(&b64);
    util_base64_encode_str(&b64, text);
    const char *p = sbuf_cstr(&b64);
    size_t len = b64.len;
    for (size_t i = 0; i < len; i += 76) {
        size_t chunk = len - i > 76 ? 76 : len - i;
        sbuf_append_len(out, p + i, chunk);
        sbuf_append(out, "\r\n");
    }
    sbuf_free(&b64);
}

static void build_message(sbuf_t *out, const char *user, const char *to,
                          const char *subject, const char *body)
{
    sbuf_printf(out, "From: SMS Forwarder <%s>\r\n", user);
    sbuf_printf(out, "To: <%s>\r\n", to);
    if (ascii_only(subject)) {
        sbuf_printf(out, "Subject: %s\r\n", subject);
    } else {
        sbuf_t b64;
        sbuf_init(&b64);
        util_base64_encode_str(&b64, subject);
        sbuf_printf(out, "Subject: =?UTF-8?B?%s?=\r\n", sbuf_cstr(&b64));
        sbuf_free(&b64);
    }
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm_utc);
    sbuf_printf(out, "Date: %s\r\n", date);
    sbuf_printf(out, "Message-ID: <%" PRId64 "@sms-forwarder>\r\n", util_utc_ms());
    sbuf_append(out, "MIME-Version: 1.0\r\n");
    sbuf_append(out, "Content-Type: text/plain; charset=UTF-8\r\n");
    sbuf_append(out, "Content-Transfer-Encoding: base64\r\n");
    sbuf_append(out, "\r\n");
    append_base64_wrapped(out, body);
}

/** 单次尝试，返回 0=成功，1=可重试失败，2=配置错误不重试 */
static int smtp_attempt(const char *host, uint16_t port, const char *user,
                        const char *pass, const char *to, const char *subject,
                        const char *body)
{
    smtp_conn_t conn;
    if (!conn_open(&conn, host, port)) {
        log_line("[邮件] 连接 %s:%u 失败", host, (unsigned)port);
        return 1;
    }

    sbuf_t reply;
    sbuf_init(&reply);
    int code = read_reply(&conn, &reply);
    if (code != 220) {
        log_line("[邮件] 服务端问候异常: %d %s", code, sbuf_cstr(&reply));
        sbuf_free(&reply);
        conn_close(&conn);
        return 1;
    }
    sbuf_reset(&reply);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "EHLO sms-forwarder");
    if (!send_cmd(&conn, cmd, 250, "EHLO")) {
        conn_close(&conn);
        return 1;
    }

    if (user[0] && pass[0]) {
        if (!send_cmd(&conn, "AUTH LOGIN", 334, "AUTH")) {
            conn_close(&conn);
            return 1;
        }
        sbuf_t b64;
        sbuf_init(&b64);
        util_base64_encode_str(&b64, user);
        snprintf(cmd, sizeof(cmd), "%s", sbuf_cstr(&b64));
        sbuf_free(&b64);
        if (!send_cmd(&conn, cmd, 334, "用户名")) {
            conn_close(&conn);
            return 2;
        }
        sbuf_init(&b64);
        util_base64_encode_str(&b64, pass);
        snprintf(cmd, sizeof(cmd), "%s", sbuf_cstr(&b64));
        sbuf_free(&b64);
        if (!send_cmd(&conn, cmd, 235, "密码")) {
            log_line("[邮件] 认证失败，请检查邮箱账号与 SMTP 授权码");
            conn_close(&conn);
            return 2;
        }
    }

    snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>", user);
    if (!send_cmd(&conn, cmd, 250, "MAIL FROM")) {
        conn_close(&conn);
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>", to);
    code = 0;
    {
        sbuf_t line;
        sbuf_init(&line);
        sbuf_printf(&line, "%s\r\n", cmd);
        conn_write(&conn, sbuf_cstr(&line), line.len);
        sbuf_free(&line);
        sbuf_t r;
        sbuf_init(&r);
        code = read_reply(&conn, &r);
        if (code != 250 && code != 251) {
            log_line("[邮件] RCPT TO 失败: %d %s", code, sbuf_cstr(&r));
            sbuf_free(&r);
            conn_close(&conn);
            return 1;
        }
        sbuf_free(&r);
    }

    if (!send_cmd(&conn, "DATA", 354, "DATA")) {
        conn_close(&conn);
        return 1;
    }

    sbuf_t msg;
    sbuf_init(&msg);
    build_message(&msg, user, to, subject, body);
    sbuf_append(&msg, ".\r\n");
    int n = conn_write(&conn, sbuf_cstr(&msg), msg.len);
    sbuf_free(&msg);
    if (n <= 0) {
        log_line("[邮件] 正文发送失败");
        conn_close(&conn);
        return 1;
    }

    sbuf_init(&reply);
    code = read_reply(&conn, &reply);
    sbuf_free(&reply);
    conn_close(&conn);
    if (code != 250) {
        log_line("[邮件] 服务端拒收: %d", code);
        return 1;
    }
    return 0;
}

bool email_send(const char *subject, const char *body)
{
    char host[CFG_MED_LEN];
    char user[96];
    char pass[96];
    char to[96];
    uint16_t port;

    app_config_t *cfg = config_lock();
    if (!config_email_valid(cfg)) {
        config_unlock();
        log_line("邮件配置不完整，跳过发送");
        return false;
    }
    snprintf(host, sizeof(host), "%s", cfg->smtp_server);
    snprintf(user, sizeof(user), "%s", cfg->smtp_user);
    snprintf(pass, sizeof(pass), "%s", cfg->smtp_pass);
    snprintf(to, sizeof(to), "%s", cfg->smtp_to);
    port = cfg->smtp_port;
    config_unlock();

    return email_send_direct(host, port, user, pass, to, subject, body);
}

bool email_send_direct(const char *host, uint16_t port, const char *user, const char *pass,
                       const char *to, const char *subject, const char *body)
{
    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (attempt > 1) {
            vTaskDelay(pdMS_TO_TICKS(1000 * (attempt - 1)));
            log_line("[邮件] 重试 (%d/%d)...", attempt, max_attempts);
        }
        int rc = smtp_attempt(host, port, user, pass, to, subject, body);
        if (rc == 0) {
            log_line("[邮件] 发送成功（第 %d/%d 次尝试）", attempt, max_attempts);
            return true;
        }
        if (rc == 2) {
            return false;
        }
    }
    log_line("邮件多次重试后仍失败，本次通知已丢弃");
    return false;
}
