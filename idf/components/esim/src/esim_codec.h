#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* eSIM 侧的纯工具：十六进制/BCD/文本互转 + 极简 DER（只认定长 tag + 长度 + 值）。
 * 不碰 AT 指令，也不持有状态，便于单独测试。 */

int esim_hex_val(char c);
void esim_bin_to_hex(const uint8_t *in, size_t len, char *out, size_t cap);
int esim_hex_to_bin_n(const char *hex, size_t len, uint8_t *out, size_t cap);
int esim_hex_to_bin(const char *hex, uint8_t *out, size_t cap);
void esim_copy_text(char *dst, size_t cap, const uint8_t *src, size_t len);
void esim_bcd_to_iccid(char *out, size_t cap, const uint8_t *bcd, size_t len);
int esim_iccid_to_bcd(const char *iccid, uint8_t out[10]);

typedef struct {
    uint32_t tag;
    const uint8_t *val;
    size_t len;
    size_t next;
} esim_tlv_t;

bool esim_tlv_read(const uint8_t *buf, size_t total, size_t off, esim_tlv_t *out);
bool esim_tlv_find(const uint8_t *buf, size_t total, uint32_t tag, esim_tlv_t *out);
size_t esim_der_len(uint8_t *p, size_t len);
size_t esim_der_put(uint8_t *buf, size_t cap, size_t pos, uint32_t tag, const uint8_t *val,
                    size_t len);
