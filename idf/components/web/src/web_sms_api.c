#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "modem/modem.h"
#include "modem/pdu.h"
#include "web_internal.h"

/* 网页发短信 + PDU 解码诊断。 */
/* ---- 发送短信 ---- */
static const char SEND_RESULT_TMPL[] =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta http-equiv=\"refresh\" content=\"3;url=/sms\"><title>发送结果</title>"
    "<style>body{font-family:Arial,sans-serif;text-align:center;padding-top:100px;background:#f5f5f5}"
    ".result{padding:20px;border-radius:10px;display:inline-block}"
    ".success{background:#4CAF50;color:#fff}.error{background:#f44336;color:#fff}"
    "</style></head><body><div class=\"result %CLASS%\"><h2>%ICON% %MSG%</h2>"
    "<p>3秒后返回发送页面...</p></div></body></html>";

static void send_result_page(sbuf_t *out, bool ok, const char *msg)
{
    /* 简易模板替换 */
    const char *p = SEND_RESULT_TMPL;
    while (*p) {
        if (strncmp(p, "%CLASS%", 7) == 0) {
            sbuf_append(out, ok ? "success" : "error");
            p += 7;
        } else if (strncmp(p, "%ICON%", 6) == 0) {
            sbuf_append(out, ok ? "✅" : "❌");
            p += 6;
        } else if (strncmp(p, "%MSG%", 5) == 0) {
            sbuf_append(out, msg);
            p += 5;
        } else {
            sbuf_append_char(out, *p++);
        }
    }
}

static void sendsms_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    log_line("网页端发送短信请求");
    log_line("目标号码: %s", ctx->arg1);
    log_line("短信内容: %s", ctx->arg2);
    bool ok = modem_send_sms(ctx->arg1, ctx->arg2, 30000);
    if (util_equals(ctx->arg3, "json")) {
        *ctype = "application/json; charset=utf-8";
        if (ok) {
            web_json_ok(out, "短信已发送");
        } else {
            web_json_fail(out, "发送失败：请检查模组状态，或内容是否超过单条长度（70 个汉字）");
        }
        return;
    }
    *ctype = "text/html; charset=utf-8";
    send_result_page(out, ok, ok ? "短信发送成功！"
                                  : "短信发送失败：请检查模组状态，或内容是否超过单条长度（70 个汉字）");
}

esp_err_t handler_sendsms(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_read_body(req);
    char phone[32] = {0};
    char content[1024] = {0};
    if (body) {
        web_form_get(body, "phone", phone, sizeof(phone));
        web_form_get(body, "content", content, sizeof(content));
        free(body);
    }
    util_trim(phone);
    util_trim(content);
    if (!phone[0] || !content[0]) {
        sbuf_t out;
        sbuf_init(&out);
        send_result_page(&out, false, phone[0] ? "错误：请输入短信内容" : "错误：请输入目标号码");
        web_send_html_redirect(req, sbuf_cstr(&out));
        sbuf_free(&out);
        return ESP_OK;
    }
    char json_flag[8] = {0};
    bool want_json = web_query_get(req, "json", json_flag, sizeof(json_flag)) &&
                     strcmp(json_flag, "1") == 0;
    /* 组 PDU + 走 AT 发短信，栈留宽一点 */
    web_spawn_async3_ex(req, sendsms_async, phone, content, want_json ? "json" : "", 10240);
    return ESP_OK;
}


/* ---- PDU 解码诊断（便于验证协议实现，返回解析后的字段）---- */
static void decode_async(web_async_ctx_t *ctx, sbuf_t *out, const char **ctype)
{
    *ctype = "application/json; charset=utf-8";
    sms_deliver_t *msg = calloc(1, sizeof(*msg));
    if (!msg) {
        web_json_fail(out, "内存不足");
        return;
    }
    if (!pdu_decode_deliver(ctx->arg1, msg)) {
        web_json_fail(out, "PDU 解析失败");
        free(msg);
        return;
    }
    sbuf_t text;
    sbuf_init(&text);
    util_json_escape(&text, msg->text);
    sbuf_t sender;
    sbuf_init(&sender);
    util_json_escape(&sender, msg->sender);
    sbuf_printf(out,
                "{\"success\":true,\"sender\":\"%s\",\"timestamp\":\"%s\",\"text\":\"%s\","
                "\"dcs\":%d,\"concat\":{\"ref\":%d,\"part\":%d,\"total\":%d}}",
                sbuf_cstr(&sender), msg->timestamp, sbuf_cstr(&text), msg->dcs,
                msg->concat_ref, msg->concat_part, msg->concat_total);
    sbuf_free(&text);
    sbuf_free(&sender);
    free(msg);
}

esp_err_t handler_decode(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char pdu[512] = {0};
    web_query_get(req, "pdu", pdu, sizeof(pdu));
    web_spawn_async(req, decode_async, pdu, "");
    return ESP_OK;
}
