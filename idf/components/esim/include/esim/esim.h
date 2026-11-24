/**
 * @file esim.h
 * @brief eSIM（eUICC）管理：读 EID、列出 profile、切换启用中的 profile。
 *
 * 通过模组的 ETSI TS 102 221 逻辑通道指令（AT+CCHO / AT+CGLA / AT+CCHC）
 * 把 GSMA SGP.22 的 ES10b/ES10c 请求原样送进 eUICC，再解析 DER 应答。
 *
 * 设计边界（详见 docs/esim-plan.md）：
 *  - 只做「读列表 + 切卡」，**不提供删除**（用户明确要求：测试期间只允许切换）
 *  - 下载新 profile（SGP.22 RSP 的 ES9+/ES10b 全流程）不在设备端跑：
 *    C3 空闲堆只有几十 KB，BPP 动辄几十上百 KB，且需要连 SM-DP+ 的 TLS；
 *    改为暴露原始 APDU 透传接口，由 PC 上的 lpac + tools/esim_bridge.py 完成
 *  - 所有操作串行化：同一时刻只允许一个 eSIM 事务，避免逻辑通道互相踩
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESIM_MAX_PROFILES 10
#define ESIM_ICCID_MAX 24
#define ESIM_AID_MAX 36
#define ESIM_NAME_MAX 64

/** 一条 eSIM 配置（profile）。 */
typedef struct {
    char iccid[ESIM_ICCID_MAX];   /**< 20 位 ICCID（BCD 解出来的数字串） */
    char aid[ESIM_AID_MAX];       /**< ISD-P AID（32 个十六进制字符） */
    char name[ESIM_NAME_MAX];     /**< 展示名：profileName > nickname > 运营商名 */
    char nickname[ESIM_NAME_MAX];
    char provider[ESIM_NAME_MAX]; /**< 运营商名（serviceProviderName） */
    int state;                    /**< 1 已启用 / 0 未启用 / -1 未知 */
    int profile_class;            /**< 0 测试 / 1 预置 / 2 运营 / -1 未知 */
} esim_profile_t;

typedef enum {
    ESIM_OK = 0,
    ESIM_ERR_UNSUPPORTED = -1, /**< 模组不支持 CCHO/CGLA，或没插 eUICC */
    ESIM_ERR_AT = -2,          /**< AT 交互失败 / 超时 */
    ESIM_ERR_SW = -3,          /**< APDU 状态字非 90 00 */
    ESIM_ERR_FORMAT = -4,      /**< DER 应答解析失败 */
    ESIM_ERR_NOMEM = -5,
    ESIM_ERR_ARG = -6,
    ESIM_ERR_BUSY = -7,        /**< 已有 eSIM 事务在执行 */
    ESIM_ERR_CARD = -8,        /**< eUICC 返回业务错误码 */
} esim_err_t;

/** 创建互斥锁；探测推迟到第一次真正用到时进行（开机时模组还没就绪）。 */
esp_err_t esim_init(void);

/** 最近一次能力探测的结论（未探测过返回 false）。 */
bool esim_supported(void);

/** 是否有 eSIM 事务正在执行（切卡/读列表期间为 true）。 */
bool esim_busy(void);

/** 最近一次失败的原因，给页面和日志直接显示。 */
const char *esim_last_error(void);

/** 读 EID（32 位十六进制）。 */
esim_err_t esim_get_eid(char *out, size_t cap);

/**
 * 列出全部 profile（含已启用与未启用）。
 * @return 数量（>=0）；负数见 esim_err_t。
 */
int esim_list_profiles(esim_profile_t *out, int max);

/**
 * 一次性读 EID + 配置列表（只开一次逻辑通道）。
 * 这个模组每开/关一次通道都要多等几秒，分开读容易踩到上一条命令的残留应答。
 * @param count 返回配置条数（<0 表示列表读取失败）
 * @return 第一个失败的错误码；EID 失败但列表成功时仍返回列表条数
 */
esim_err_t esim_read_all(char *eid, size_t eid_cap, esim_profile_t *out, int max, int *count);

/**
 * 启用指定 profile（ICCID 或 16 字节 AID 的十六进制）。
 * 不删除任何配置；eUICC 会自行停用当前生效的那条。
 * @param refresh  true 让 eUICC 主动发 REFRESH（部分模组会返回 CAT busy，调用方可降级重试）
 * @param ecc      业务错误码（0 成功；其它见 SGP.22 ProfileInfoListError/EnableProfile 结果）
 */
esim_err_t esim_enable_profile(const char *id, bool refresh, int *ecc);
/** 删除一条**未启用**的配置（BF33）。使用中的配置会被拒绝，避免把正在用的卡删掉。 */
esim_err_t esim_delete_profile(const char *id);

/* ---- 原始 APDU 透传（给 PC 侧 lpac 桥接用，见 tools/esim_bridge.py） ---- */

/** 打开逻辑通道（AID 为十六进制字符串），返回通道号（1..）。 */
esim_err_t esim_channel_open(const char *aid_hex, int *channel);

/** 关闭逻辑通道。 */
void esim_channel_close(int channel);

/**
 * 通过指定通道发一条 APDU。
 * @param tx_hex 十六进制命令
 * @param rx_hex 输出十六进制应答（含状态字），cap 建议 >= 2*255+8
 * @param sw     状态字（0x9000 表示成功），可为 NULL
 */
esim_err_t esim_transmit(int channel, const char *tx_hex, char *rx_hex, size_t cap, int *sw);

/** 最近一次成功打开的通道号（0 表示当前没有），给透传接口复用。 */
int esim_last_channel(void);
