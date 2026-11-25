#include "modem/pdu.h"

#include <stdio.h>
#include <string.h>

#include "core/util.h"

/* ---- GSM 03.38 默认字母表（0x00-0x7F）---- */
static const uint16_t GSM7_DEFAULT[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

/* GSM 7bit 扩展表（0x1B 转义后的第二个字节） */
static const struct {
    uint8_t code;
    uint16_t cp;
} GSM7_EXT[] = {
    {0x0A, 0x000C}, {0x14, 0x005E}, {0x28, 0x007B}, {0x29, 0x007D},
    {0x2F, 0x005C}, {0x3C, 0x005B}, {0x3D, 0x007E}, {0x3E, 0x005D},
    {0x40, 0x007C}, {0x65, 0x20AC},
};

#define GSM7_EXT_COUNT (sizeof(GSM7_EXT) / sizeof(GSM7_EXT[0]))

static int gsm7_encode_char(uint32_t cp, uint8_t *out)
{
    for (int i = 0; i < 128; i++) {
        if (GSM7_DEFAULT[i] == cp && i != 0x1B) {
            out[0] = (uint8_t)i;
            return 1;
        }
    }
    for (size_t i = 0; i < GSM7_EXT_COUNT; i++) {
        if (GSM7_EXT[i].cp == cp) {
            out[0] = 0x1B;
            out[1] = GSM7_EXT[i].code;
            return 2;
        }
    }
    return 0;
}

static uint16_t gsm7_decode_char(uint8_t code)
{
    return GSM7_DEFAULT[code & 0x7F];
}

/* ---- UTF-8 ---- */

static int utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) {
        return 0;
    }
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static int utf8_emit(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/** UTF-8 -> UCS2 大端字节流，返回字节数；-1 表示缓冲区不足 */
static int utf8_to_ucs2(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (*s) {
        uint32_t cp = 0;
        int used = utf8_next(s, &cp);
        if (used == 0) {
            break;
        }
        s += used;
        if (cp > 0x10FFFF) {
            return -1; /* 不是合法码点 */
        }
        if (cp > 0xFFFF) {
            /* 非 BMP（emoji 等）：按 UTF-16 代理对拆成两个 16 位单元，
             * 收到的一端才能合回一个字符 */
            if (n + 4 > cap) {
                return -1;
            }
            uint32_t v = cp - 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (v >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + (v & 0x3FF));
            out[n++] = (uint8_t)(hi >> 8);
            out[n++] = (uint8_t)(hi & 0xFF);
            out[n++] = (uint8_t)(lo >> 8);
            out[n++] = (uint8_t)(lo & 0xFF);
            continue;
        }
        if (n + 2 > cap) {
            return -1;
        }
        out[n++] = (uint8_t)(cp >> 8);
        out[n++] = (uint8_t)(cp & 0xFF);
    }
    return n;
}

/** UCS2 大端字节流 -> UTF-8，返回写入长度 */
static int ucs2_to_utf8(const uint8_t *in, int len, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i + 1 < len; i += 2) {
        uint32_t cp = ((uint32_t)in[i] << 8) | in[i + 1];
        if (cp == 0) {
            continue;
        }
        /* UTF-16 代理对：emoji 等非 BMP 字符在 UCS2 里是两个 16 位单元。
         * 不合并的话会各按普通码点编码成 3 字节（CESU-8），既显示成乱码，
         * 又因为 0xED 开头的序列不是合法 UTF-8，让 /log 的 JSON 直接解析失败。 */
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
            uint32_t lo = ((uint32_t)in[i + 2] << 8) | in[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD; /* 落单的代理项：换成替换符，不能当普通码点编码 */
        }
        char tmp[4];
        int used = utf8_emit(cp, tmp);
        if (n + used >= cap) {
            break;
        }
        memcpy(out + n, tmp, used);
        n += used;
    }
    out[n] = '\0';
    return n;
}

/* ---- 7bit 打包/解包 ---- */

static int gsm7_pack(const uint8_t *septets, int count, uint8_t *out, int cap)
{
    memset(out, 0, cap);
    int bit = 0;
    for (int i = 0; i < count; i++) {
        int byte = bit >> 3;
        int shift = bit & 7;
        if (byte >= cap) {
            return -1;
        }
        out[byte] |= (uint8_t)((septets[i] & 0x7F) << shift);
        if (shift > 1) {
            if (byte + 1 >= cap) {
                return -1;
            }
            out[byte + 1] |= (uint8_t)((septets[i] & 0x7F) >> (8 - shift));
        }
        bit += 7;
    }
    return (bit + 7) / 8;
}

/** 从任意 bit 偏移解出 septets，返回实际字符数 */
static int gsm7_unpack(const uint8_t *in, int in_len, int bit_off, int septet_count,
                       uint8_t *out)
{
    for (int i = 0; i < septet_count; i++) {
        int bit = bit_off + i * 7;
        int byte = bit >> 3;
        int shift = bit & 7;
        if (byte >= in_len) {
            return i;
        }
        uint16_t v = in[byte];
        if (shift > 1 && byte + 1 < in_len) {
            v |= (uint16_t)in[byte + 1] << 8;
        }
        out[i] = (uint8_t)((v >> shift) & 0x7F);
    }
    return septet_count;
}

static int gsm7_to_utf8(const uint8_t *septets, int count, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (septets[i] == 0x1B && i + 1 < count) {
            int mapped = -1;
            for (size_t k = 0; k < GSM7_EXT_COUNT; k++) {
                if (GSM7_EXT[k].code == septets[i + 1]) {
                    mapped = (int)GSM7_EXT[k].cp;
                    break;
                }
            }
            i++;
            if (mapped < 0) {
                continue;
            }
            char tmp[4];
            int used = utf8_emit((uint32_t)mapped, tmp);
            if (n + used >= cap) {
                break;
            }
            memcpy(out + n, tmp, used);
            n += used;
            continue;
        }
        char tmp[4];
        int used = utf8_emit(gsm7_decode_char(septets[i]), tmp);
        if (n + used >= cap) {
            break;
        }
        memcpy(out + n, tmp, used);
        n += used;
    }
    out[n] = '\0';
    return n;
}

/* ---- 号码 ---- */

static void normalize_number(const char *phone, char *digits, size_t cap, int *ton)
{
    char raw[32] = {0};
    size_t n = 0;
    for (const char *p = phone; *p && n < sizeof(raw) - 1; p++) {
        if (*p >= '0' && *p <= '9') {
            raw[n++] = *p;
        }
    }
    raw[n] = '\0';

    if (phone[0] == '+' || (n > 2 && strncmp(raw, "00", 2) == 0)) {
        /* 已是国际格式 */
        if (raw[0] == '0' && raw[1] == '0') {
            memmove(raw, raw + 2, n - 1);
        }
        snprintf(digits, cap, "%s", raw);
        *ton = 0x91;
        return;
    }
    /* 国内 11 位手机号补 86，转为国际格式，兼容性更好 */
    if (n == 11 && raw[0] == '1') {
        snprintf(digits, cap, "86%s", raw);
        *ton = 0x91;
        return;
    }
    snprintf(digits, cap, "%s", raw);
    *ton = 0x81;
}

static void bcd_encode(const char *digits, uint8_t *out, int *out_len)
{
    int len = (int)strlen(digits);
    int bytes = (len + 1) / 2;
    for (int i = 0; i < bytes; i++) {
        uint8_t lo = (uint8_t)(digits[i * 2] - '0');
        uint8_t hi = (i * 2 + 1 < len) ? (uint8_t)(digits[i * 2 + 1] - '0') : 0x0F;
        out[i] = (uint8_t)(lo | (hi << 4));
    }
    *out_len = bytes;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int cap)
{
    int n = 0;
    while (hex[0] && hex[1]) {
        int hi = util_hex_val(hex[0]);
        int lo = util_hex_val(hex[1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        if (n >= cap) {
            return -1;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

int pdu_encode_submit(const char *phone, const char *text, char *out_hex, size_t out_cap)
{
    char digits[32];
    int ton = 0x81;
    normalize_number(phone, digits, sizeof(digits), &ton);
    if (digits[0] == '\0') {
        return -1;
    }

    /* 判断能否用 GSM 7bit 编码 */
    bool gsm7_ok = true;
    const char *scan = text;
    while (*scan) {
        uint32_t cp = 0;
        int used = utf8_next(scan, &cp);
        if (used == 0) {
            break;
        }
        scan += used;
        uint8_t tmp[2];
        if (gsm7_encode_char(cp, tmp) == 0) {
            gsm7_ok = false;
            break;
        }
    }

    uint8_t tpdu[320];
    int n = 0;
    tpdu[n++] = 0x11; /* SMS-SUBMIT，带相对有效期 */
    tpdu[n++] = 0x00; /* TP-MR */

    uint8_t addr[16];
    int addr_len = 0;
    bcd_encode(digits, addr, &addr_len);
    tpdu[n++] = (uint8_t)strlen(digits);
    tpdu[n++] = (uint8_t)ton;
    memcpy(tpdu + n, addr, addr_len);
    n += addr_len;

    tpdu[n++] = 0x00; /* TP-PID */
    tpdu[n++] = gsm7_ok ? 0x00 : 0x08; /* TP-DCS */
    tpdu[n++] = 0xAA; /* TP-VP：4 天 */

    if (gsm7_ok) {
        uint8_t septets[320];
        int count = 0;
        scan = text;
        while (*scan) {
            uint32_t cp = 0;
            int used = utf8_next(scan, &cp);
            if (used == 0) {
                break;
            }
            scan += used;
            uint8_t tmp[2];
            int cnt = gsm7_encode_char(cp, tmp);
            for (int i = 0; i < cnt && count < (int)sizeof(septets); i++) {
                septets[count++] = tmp[i];
            }
        }
        uint8_t packed[320];
        int packed_len = gsm7_pack(septets, count, packed, sizeof(packed));
        if (packed_len < 0) {
            return -1;
        }
        if (packed_len > 140) {
            return -2; /* 超过单条短信容量（160 个 7bit 字符） */
        }
        tpdu[n++] = (uint8_t)count; /* UDL = septet 数 */
        memcpy(tpdu + n, packed, packed_len);
        n += packed_len;
    } else {
        uint8_t ucs2[300];
        int ucs2_len = utf8_to_ucs2(text, ucs2, sizeof(ucs2));
        if (ucs2_len < 0) {
            return -1;
        }
        if (ucs2_len > 140) {
            return -2; /* 超过单条短信容量（70 个 UCS2 字符） */
        }
        tpdu[n++] = (uint8_t)ucs2_len; /* UDL = 八位组数 */
        memcpy(tpdu + n, ucs2, ucs2_len);
        n += ucs2_len;
    }

    /* SMSC 长度 0 = 使用模组默认短信中心 */
    size_t pos = 0;
    if (out_cap < (size_t)(n * 2 + 3)) {
        return -1;
    }
    out_hex[pos++] = '0';
    out_hex[pos++] = '0';
    static const char hexchars[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        out_hex[pos++] = hexchars[tpdu[i] >> 4];
        out_hex[pos++] = hexchars[tpdu[i] & 0x0F];
    }
    out_hex[pos] = '\0';
    return n;
}

static bool decode_address(const uint8_t *buf, int len, int *idx, char *sender, size_t cap)
{
    if (*idx + 2 > len) {
        return false;
    }
    int addr_len = buf[(*idx)++];
    int ton = buf[(*idx)++];
    int bytes = (addr_len + 1) / 2;
    if (*idx + bytes > len) {
        return false;
    }
    const uint8_t *p = buf + *idx;
    *idx += bytes;

    if ((ton & 0x70) == 0x50) {
        /* 字母数字发送方：7bit 编码 */
        int septets = (addr_len * 4) / 7;
        uint8_t sept[32];
        if (septets > (int)sizeof(sept)) {
            septets = sizeof(sept);
        }
        gsm7_unpack(p, bytes, 0, septets, sept);
        gsm7_to_utf8(sept, septets, sender, (int)cap);
        return true;
    }

    char digits[40];
    int n = 0;
    for (int i = 0; i < addr_len && n < (int)sizeof(digits) - 1; i++) {
        uint8_t b = p[i / 2];
        uint8_t nib = (i % 2 == 0) ? (b & 0x0F) : (b >> 4);
        if (nib > 9) {
            break;
        }
        digits[n++] = (char)('0' + nib);
    }
    digits[n] = '\0';
    if ((ton & 0x70) == 0x10) {
        snprintf(sender, cap, "+%s", digits);
    } else {
        snprintf(sender, cap, "%s", digits);
    }
    return true;
}

/* SCTS 合法性：月份 1-12、日 1-31、时分秒范围。畸形 PDU 通常在这里就能筛掉 */
static bool scts_valid(const uint8_t *b)
{
    int mo = (b[1] & 0x0F) * 10 + (b[1] >> 4);
    int dd = (b[2] & 0x0F) * 10 + (b[2] >> 4);
    int hh = (b[3] & 0x0F) * 10 + (b[3] >> 4);
    int mi = (b[4] & 0x0F) * 10 + (b[4] >> 4);
    int ss = (b[5] & 0x0F) * 10 + (b[5] >> 4);
    return mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh <= 23 && mi <= 59 && ss <= 59;
}

static void decode_scts(const uint8_t *b, char *out, size_t cap)
{
    int yy = (b[0] & 0x0F) * 10 + (b[0] >> 4);
    int mo = (b[1] & 0x0F) * 10 + (b[1] >> 4);
    int dd = (b[2] & 0x0F) * 10 + (b[2] >> 4);
    int hh = (b[3] & 0x0F) * 10 + (b[3] >> 4);
    int mi = (b[4] & 0x0F) * 10 + (b[4] >> 4);
    int ss = (b[5] & 0x0F) * 10 + (b[5] >> 4);
    snprintf(out, cap, "%02d/%02d/%02d %02d:%02d:%02d", yy, mo, dd, hh, mi, ss);
}

bool pdu_decode_deliver(const char *hex, sms_deliver_t *out)
{
    uint8_t buf[600];
    int len = hex_to_bytes(hex, buf, sizeof(buf));
    if (len < 10) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    int idx = 0;
    int smsc_len = buf[idx++];
    if (idx + smsc_len >= len) {
        return false;
    }
    idx += smsc_len;

    uint8_t first_octet = buf[idx++];
    if ((first_octet & 0x03) != 0x00) {
        /* 只处理 SMS-DELIVER */
        return false;
    }
    bool has_udh = (first_octet & 0x40) != 0;

    if (!decode_address(buf, len, &idx, out->sender, sizeof(out->sender))) {
        return false;
    }
    if (idx + 2 + 7 + 1 > len) {
        return false;
    }
    idx++; /* TP-PID */
    out->dcs = buf[idx++];
    if (!scts_valid(buf + idx)) {
        return false; /* 时间戳非法，判定为畸形 PDU */
    }
    decode_scts(buf + idx, out->timestamp, sizeof(out->timestamp));
    idx += 7;

    if (out->sender[0] == '\0') {
        return false;
    }
    /* 只处理文本短信：8bit 数据短信（DCS 0x04~0x07）不是可读文本 */
    if ((out->dcs & 0x0C) == 0x04) {
        return false;
    }

    int udl = buf[idx++];
    const uint8_t *ud = buf + idx;
    int ud_bytes = len - idx;
    if (ud_bytes <= 0) {
        return false;
    }

    int udh_len = 0;
    if (has_udh) {
        if (ud[0] + 1 > ud_bytes) {
            return false;
        }
        udh_len = ud[0] + 1;
        int i = 1;
        while (i + 1 < udh_len) {
            int iei = ud[i];
            int iel = ud[i + 1];
            if (iei == 0x00 && iel == 3 && i + 4 < udh_len + 1) {
                out->concat_ref = ud[i + 2];
                out->concat_total = ud[i + 3];
                out->concat_part = ud[i + 4];
            } else if (iei == 0x08 && iel == 4 && i + 5 < udh_len + 1) {
                out->concat_ref = (ud[i + 2] << 8) | ud[i + 3];
                out->concat_total = ud[i + 4];
                out->concat_part = ud[i + 5];
            }
            i += 2 + iel;
        }
    }

    bool is_ucs2 = (out->dcs & 0x0C) == 0x08;
    if (is_ucs2) {
        /* UDL 必须与真实数据长度一致，否则说明 PDU 被截断或本身就是乱码 */
        if (udl < udh_len || ud_bytes < udl || ((udl - udh_len) & 1)) {
            return false;
        }
        ucs2_to_utf8(ud + udh_len, udl - udh_len, out->text, sizeof(out->text));
    } else {
        /* UDH 占 48 bit，但 7bit 数据必须从下一个整 septet 边界开始：
           6 字节 UDH = 7 个 septet（49 bit），第 49 位是填充位。
           按 48 位解会让每个字符错位 1 bit，解出来就是一堆 ¡Ñ§Ω 之类的乱码。 */
        int udh_septets = udh_len > 0 ? (udh_len * 8 + 6) / 7 : 0;
        if (udl < udh_septets) {
            return false;
        }
        int septets = udl - udh_septets;
        int bit_off = udh_septets * 7;
        /* 整条 UD 共 udl 个 septet，止于 bit udl*7 */
        if (ud_bytes < (udl * 7 + 7) / 8) {
            return false;
        }
        uint8_t sept[400];
        if (septets > (int)sizeof(sept)) {
            septets = sizeof(sept);
        }
        int got = gsm7_unpack(ud, ud_bytes, bit_off, septets, sept);
        gsm7_to_utf8(sept, got, out->text, sizeof(out->text));
    }
    return true;
}
