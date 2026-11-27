#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "esim/esim.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem/modem.h"
#include "web_internal.h"

/* eSIM 管理：EID/配置列表、启用、删除、APDU 透传。 */
/* ---- eSIM 管理 ---- */

#define ESIM_APDU_HEX_MAX 520 /* 255 字节 APDU → 510 个十六进制字符 + NUL */

static void esim_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    log_line("eSIM 已切换，软重启模组以重新注册网络");
    modem_soft_reset(90000);
    vTaskDelete(NULL);
}

static void esim_json_fail(sbuf_t *out)
{
    sbuf_append(out, "{\"success\":false,\"message\":\"");
    util_json_escape(out, esim_last_error());
    sbuf_append(out, "\"}");
}

static void esim_async(web_async_ctx_t *ctx, sbuf_t *out, const char **content_type)
{
    (void)content_type;
    if (strcmp(ctx->arg3, "delete") == 0) {
        esim_err_t e = esim_delete_profile(ctx->arg1);
        if (e != ESIM_OK) {
            esim_json_fail(out);
            log_line("eSIM 删除失败：%s", esim_last_error());
            return;
        }
        sbuf_append(out, "{\"success\":true,\"message\":\"已删除该配置\"}");
        return;
    }
    if (strcmp(ctx->arg3, "enable") == 0) {
        int ecc = -1;
        /* 先按「带刷新」切；有的模组会回 CAT busy（5），再按不带刷新重试 */
        esim_err_t e = esim_enable_profile(ctx->arg1, true, &ecc);
        if (e == ESIM_ERR_CARD && ecc == 5) {
            log_line("eSIM 切换返回 CAT busy，改为不带刷新重试");
            e = esim_enable_profile(ctx->arg1, false, &ecc);
        }
        if (e != ESIM_OK) {
            esim_json_fail(out);
            log_line("eSIM 切换失败：%s", esim_last_error());
            return;
        }
        sbuf_append(out, "{\"success\":true,\"message\":\"已切换，模组正在重新注册网络（约 20 秒）\"}");
        if (xTaskCreate(esim_reset_task, "esim_reset", 3072, NULL, 4, NULL) != pdPASS) {
            log_line("eSIM 切换成功，但创建重启任务失败，请手动重启模组");
        }
        return;
    }

    char eid[40] = {0};
    esim_profile_t *list = calloc(ESIM_MAX_PROFILES, sizeof(*list));
    if (!list) {
        sbuf_append(out, "{\"success\":false,\"message\":\"内存不足，请稍后重试\"}");
        return;
    }
    int count = -1;
    esim_err_t eid_err = esim_read_all(eid, sizeof(eid), list, ESIM_MAX_PROFILES, &count);

    sbuf_printf(out, "{\"success\":true,\"supported\":%s,\"eid\":\"",
                esim_supported() ? "true" : "false");
    util_json_escape(out, eid);
    sbuf_printf(out, "\",\"busy\":%s,\"profiles\":[", esim_busy() ? "true" : "false");
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            if (i) {
                sbuf_append(out, ",");
            }
            sbuf_append(out, "{\"iccid\":\"");
            util_json_escape(out, list[i].iccid);
            sbuf_append(out, "\",\"aid\":\"");
            util_json_escape(out, list[i].aid);
            sbuf_append(out, "\",\"name\":\"");
            util_json_escape(out, list[i].name);
            sbuf_append(out, "\",\"provider\":\"");
            util_json_escape(out, list[i].provider);
            sbuf_printf(out, "\",\"state\":%d,\"class\":%d}", list[i].state, list[i].profile_class);
        }
    }
    sbuf_append(out, "],\"message\":\"");
    if (count < 0 || eid_err != ESIM_OK) {
        util_json_escape(out, esim_last_error());
    }
    sbuf_append(out, "\"}");
    free(list);
}

esp_err_t handler_esim(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    web_spawn_async3_ex(req, esim_async, "", "", "list", 4096);
    return ESP_OK;
}

esp_err_t handler_esim_enable(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_read_body(req);
    char id[48] = {0};
    char action[16] = {0};
    bool got = web_form_get(body, "id", id, sizeof(id));
    web_form_get(body, "action", action, sizeof(action));
    free(body);
    const bool del = strcmp(action, "delete") == 0;
    if (!got || !id[0]) {
        web_send_json(req, false, del ? "缺少要删除的 ICCID / AID" : "缺少要切换的 ICCID / AID");
        return ESP_OK;
    }
    if (esim_busy()) {
        web_send_json(req, false, "eSIM 正忙，请稍后再试");
        return ESP_OK;
    }
    web_spawn_async3_ex(req, esim_async, id, "", del ? "delete" : "enable", 4096);
    return ESP_OK;
}

/**
 * 原始 APDU 透传：给 PC 上的 lpac 用（tools/esim_bridge.py）。
 * 表单：func=connect|logic_channel_open|logic_channel_close|transmit|disconnect
 *       param=<AID 十六进制 | 通道号十六进制 | APDU 十六进制>
 * 返回：{"success":true,"ecode":<通道号/0>,"data":"<应答十六进制>"}
 */
esp_err_t handler_esim_apdu(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    char *body = web_read_body(req);
    char func[32] = {0};
    if (!body || !web_form_get(body, "func", func, sizeof(func))) {
        free(body);
        web_send_json(req, false, "缺少 func");
        return ESP_OK;
    }
    char *param = calloc(1, 600);
    if (!param) {
        free(body);
        web_send_json(req, false, "内存不足");
        return ESP_OK;
    }
    web_form_get(body, "param", param, 600);
    free(body);

    sbuf_t out;
    sbuf_init(&out);
    int ecode = 0;
    bool ok = true;
    char *data = NULL;

    if (strcmp(func, "connect") == 0 || strcmp(func, "disconnect") == 0) {
        /* 连接/断开只做状态检查；真正的逻辑通道由 logic_channel_open 建立 */
    } else if (strcmp(func, "logic_channel_open") == 0) {
        int ch = 0;
        if (esim_channel_open(param, &ch) == ESIM_OK) {
            ecode = ch;
        } else {
            ok = false;
        }
    } else if (strcmp(func, "logic_channel_close") == 0) {
        int ch = (int)strtol(param, NULL, 16);
        if (ch > 0) {
            esim_channel_close(ch);
        } else {
            ok = false;
        }
    } else if (strcmp(func, "transmit") == 0) {
        int ch = esim_last_channel();
        if (ch <= 0) {
            ok = false;
        } else {
            data = calloc(1, ESIM_APDU_HEX_MAX);
            if (!data || esim_transmit(ch, param, data, ESIM_APDU_HEX_MAX, NULL) != ESIM_OK) {
                ok = false;
            }
        }
    } else {
        ok = false;
    }

    if (!ok) {
        sbuf_append(&out, "{\"success\":false,\"message\":\"");
        util_json_escape(&out, esim_last_error());
        sbuf_append(&out, "\"}");
    } else {
        sbuf_printf(&out, "{\"success\":true,\"ecode\":%d", ecode);
        if (data) {
            sbuf_append(&out, ",\"data\":\"");
            util_json_escape(&out, data);
            sbuf_append(&out, "\"");
        }
        sbuf_append(&out, "}");
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    free(data);
    free(param);
    return ESP_OK;
}
