#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/sbuf.h"
#include "core/util.h"
#include "ota/ota.h"
#include "web_internal.h"

/* 固件升级：状态查询与镜像上传（校验由 ota 组件负责）。 */
/* ---- 固件升级：状态 / 上传 ---- */

static void ota_to_json(sbuf_t *out)
{
    ota_info_t info;
    ota_get_info(&info);

    sbuf_append(out, "{\"success\":true,\"current\":\"");
    util_json_escape(out, info.current);
    sbuf_append(out, "\",\"target\":\"");
    util_json_escape(out, info.next_version);
    sbuf_printf(out, "\",\"state\":%d,\"progress\":%d,\"message\":\"", (int)info.state, info.progress);
    util_json_escape(out, info.message);
    sbuf_append(out, "\"}");
}

esp_err_t handler_ota(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    sbuf_t out;
    sbuf_init(&out);
    ota_to_json(&out);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ESP_OK;
}

static void ota_error_json(httpd_req_t *req, const char *status, const char *message)
{
    sbuf_t out;
    sbuf_init(&out);
    web_json_fail(&out, "%s", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
}

esp_err_t handler_ota_upload(httpd_req_t *req)
{
    if (!web_require_auth(req)) {
        return ESP_OK;
    }
    if (ota_busy()) {
        ota_error_json(req, "409 Conflict", "固件升级进行中，请等待本次结束");
        return ESP_OK;
    }

    char len_hdr[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Length", len_hdr, sizeof(len_hdr)) != ESP_OK) {
        ota_error_json(req, "411 Length Required", "缺少 Content-Length，请用上传按钮提交");
        return ESP_OK;
    }
    long long total = atoll(len_hdr);
    if (total <= 0 || total > 8 * 1024 * 1024) {
        ota_error_json(req, "413 Payload Too Large", "固件大小异常");
        return ESP_OK;
    }

    esp_err_t err = ota_begin((size_t)total);
    if (err != ESP_OK) {
        ota_info_t info;
        ota_get_info(&info);
        ota_error_json(req,
                       err == ESP_ERR_INVALID_SIZE ? "413 Payload Too Large" : "400 Bad Request",
                       info.message[0] ? info.message : "无法开始升级");
        log_line("网页上传固件被拒绝：%s", info.message);
        return ESP_OK;
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        ota_abort();
        ota_error_json(req, "500 Internal Server Error", "内存不足");
        return ESP_OK;
    }

    int remaining = (int)total;
    int timeouts = 0;
    const char *fail_msg = NULL;
    while (remaining > 0) {
        int want = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                fail_msg = "接收超时，上传中断";
                break;
            }
            continue;
        }
        if (r <= 0) {
            fail_msg = "连接中断，上传未完成";
            break;
        }
        if (ota_write(buf, (size_t)r) != ESP_OK) {
            ota_info_t info;
            ota_get_info(&info);
            fail_msg = info.message[0] ? info.message : "写入失败";
            break;
        }
        remaining -= r;
    }
    free(buf);

    if (remaining > 0 || fail_msg) {
        ota_abort();
        if (!fail_msg) {
            fail_msg = "上传未完成";
        }
        log_line("网页上传固件失败：%s", fail_msg);
        ota_error_json(req, "400 Bad Request", fail_msg);
        return ESP_OK;
    }

    err = ota_end();
    if (err != ESP_OK) {
        ota_info_t info;
        ota_get_info(&info);
        ota_error_json(req, "400 Bad Request", info.message[0] ? info.message : "固件校验失败");
        return ESP_OK;
    }

    ota_info_t info;
    ota_get_info(&info);
    sbuf_t out;
    sbuf_init(&out);
    sbuf_append(&out, "{\"success\":true,\"version\":\"");
    util_json_escape(&out, info.next_version);
    sbuf_append(&out, "\",\"message\":\"固件已写入，设备即将重启\"}");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ESP_OK;
}
