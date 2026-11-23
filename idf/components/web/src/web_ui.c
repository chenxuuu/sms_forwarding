#include "web/web_ui.h"

#include <stddef.h>

#include "core/sbuf.h"

/* 由 EMBED_TXTFILES 生成 */
extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];

#define UI_FLUSH_THRESHOLD 2048

esp_err_t web_ui_render_to(web_ui_sink_t sink, void *user)
{
    const char *tmpl = _binary_index_html_start;
    size_t len = (size_t)(_binary_index_html_end - _binary_index_html_start);
    while (len > 0) {
        char c = tmpl[len - 1];
        if (c != 0 && c != 10 && c != 13) {
            break;
        }
        len--;
    }

    sbuf_t out;
    sbuf_init(&out);
    esp_err_t err = ESP_OK;

    for (size_t i = 0; i < len && err == ESP_OK; i++) {
        sbuf_append_char(&out, tmpl[i]);
        if (out.len >= UI_FLUSH_THRESHOLD) {
            err = sink(user, sbuf_cstr(&out), out.len);
            sbuf_reset(&out);
        }
    }
    if (err == ESP_OK && out.len > 0) {
        err = sink(user, sbuf_cstr(&out), out.len);
    }

    sbuf_free(&out);
    return err;
}
