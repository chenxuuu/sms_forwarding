#include "esim_codec.h"

#include <stdio.h>
#include <string.h>

#include "core/util.h"

int esim_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void esim_bin_to_hex(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t n = 0;
    for (size_t i = 0; i < len && n + 2 < cap; i++) {
        out[n++] = digits[in[i] >> 4];
        out[n++] = digits[in[i] & 0x0F];
    }
    out[n] = '\0';
}

/** 十六进制串转字节（可指定字符数）；长度必须是偶数。返回字节数，-1 表示格式错。 */
int esim_hex_to_bin_n(const char *hex, size_t len, uint8_t *out, size_t cap)
{
    if (len % 2 != 0 || len / 2 > cap) {
        return -1;
    }
    for (size_t i = 0; i < len / 2; i++) {
        int hi = esim_hex_val(hex[i * 2]);
        int lo = esim_hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(len / 2);
}

int esim_hex_to_bin(const char *hex, uint8_t *out, size_t cap)
{
    return esim_hex_to_bin_n(hex, strlen(hex), out, cap);
}

/** 复制一段可能不是 NUL 结尾、可能带控制字符的文本字段。 */
void esim_copy_text(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        char c = (char)src[i];
        dst[n++] = (c < 0x20 || c == 0x7F) ? ' ' : c;
    }
    dst[n] = '\0';
    util_trim(dst);
}

void esim_bcd_to_iccid(char *out, size_t cap, const uint8_t *bcd, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        uint8_t lo = bcd[i] & 0x0F;
        uint8_t hi = (bcd[i] >> 4) & 0x0F;
        if (lo <= 9) out[n++] = (char)('0' + lo);
        if (hi <= 9 && n + 1 < cap) out[n++] = (char)('0' + hi);
    }
    out[n] = '\0';
}

/** ICCID 数字串转 GSM BCD（10 字节，奇数位补 F）。 */
int esim_iccid_to_bcd(const char *iccid, uint8_t out[10])
{
    char digits[21];
    int n = 0;
    for (const char *p = iccid; *p && n < 20; p++) {
        if (*p >= '0' && *p <= '9') {
            digits[n++] = *p;
        } else if (*p == ' ' || *p == '-') {
            continue;
        } else {
            return -1;
        }
    }
    if (n < 18) {
        return -1;
    }
    memset(out, 0xFF, 10);
    for (int i = 0; i < n; i += 2) {
        uint8_t lo = (uint8_t)(digits[i] - '0');
        uint8_t hi = (i + 1 < n) ? (uint8_t)(digits[i + 1] - '0') : 0x0F;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return 10;
}

/* ---------- DER（仅够用：定长 tag + 长度 + 值） ---------- */

bool esim_tlv_read(const uint8_t *buf, size_t total, size_t off, esim_tlv_t *out)
{
    if (!buf || off >= total) {
        return false;
    }
    size_t pos = off;
    uint32_t tag = buf[pos++];
    if ((tag & 0x1F) == 0x1F) {
        do {
            if (pos >= total) {
                return false;
            }
            tag = (tag << 8) | buf[pos];
        } while ((buf[pos++] & 0x80) != 0);
    }
    if (pos >= total) {
        return false;
    }
    uint8_t lb = buf[pos++];
    size_t len = 0;
    if ((lb & 0x80) == 0) {
        len = lb;
    } else {
        uint8_t n = lb & 0x7F;
        if (n == 0 || n > 4 || pos + n > total) {
            return false;
        }
        for (uint8_t i = 0; i < n; i++) {
            len = (len << 8) | buf[pos++];
        }
    }
    if (pos + len > total) {
        return false;
    }
    out->tag = tag;
    out->val = buf + pos;
    out->len = len;
    out->next = pos + len;
    return true;
}

bool esim_tlv_find(const uint8_t *buf, size_t total, uint32_t tag, esim_tlv_t *out)
{
    size_t pos = 0;
    esim_tlv_t n;
    while (esim_tlv_read(buf, total, pos, &n)) {
        if (n.tag == tag) {
            if (out) {
                *out = n;
            }
            return true;
        }
        pos = n.next;
    }
    return false;
}

size_t esim_der_len(uint8_t *p, size_t len)
{
    if (len < 0x80) {
        p[0] = (uint8_t)len;
        return 1;
    }
    if (len <= 0xFF) {
        p[0] = 0x81;
        p[1] = (uint8_t)len;
        return 2;
    }
    p[0] = 0x82;
    p[1] = (uint8_t)(len >> 8);
    p[2] = (uint8_t)len;
    return 3;
}

/** 追加一个 TLV，返回写入长度；放不下返回 0。 */
size_t esim_der_put(uint8_t *buf, size_t cap, size_t pos, uint32_t tag, const uint8_t *val, size_t len)
{
    uint8_t tagbuf[3];
    size_t taglen;
    if (tag > 0xFFFF) {
        tagbuf[0] = (uint8_t)(tag >> 16);
        tagbuf[1] = (uint8_t)(tag >> 8);
        tagbuf[2] = (uint8_t)tag;
        taglen = 3;
    } else if (tag > 0xFF) {
        tagbuf[0] = (uint8_t)(tag >> 8);
        tagbuf[1] = (uint8_t)tag;
        taglen = 2;
    } else {
        tagbuf[0] = (uint8_t)tag;
        taglen = 1;
    }
    uint8_t lenbuf[3];
    size_t lenlen = esim_der_len(lenbuf, len);
    if (pos + taglen + lenlen + len > cap) {
        return 0;
    }
    memcpy(buf + pos, tagbuf, taglen);
    memcpy(buf + pos + taglen, lenbuf, lenlen);
    if (len) {
        memcpy(buf + pos + taglen + lenlen, val, len);
    }
    return taglen + lenlen + len;
}
