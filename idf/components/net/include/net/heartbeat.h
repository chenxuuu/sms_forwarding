/**
 * @file heartbeat.h
 * @brief 网络心跳与自愈。
 *
 * 每 N 秒检查一次"WiFi 已连接 + 能访问公网"（DNS + TCP 探测）：
 *  - 连续 3 次失败 → 主动重连 WiFi；
 *  - 连续 6 次失败 → 重启设备（模拟断电重启，最后一招）；
 *  - 恢复时 / IP 变化时通过回调通知上层（用于发邮件告知新地址）。
 */
#pragma once

#include <stdbool.h>

typedef enum {
    HB_EVENT_IP_CHANGED = 0, /* IP 发生变化（启动后首次拿到 IP 不算） */
    HB_EVENT_RECOVERED,      /* 之前自检失败，现在恢复 */
} heartbeat_event_t;

typedef void (*heartbeat_cb_t)(heartbeat_event_t ev, const char *ip);

/**
 * 启动心跳任务（WiFi 连上后调用）。
 * @param current_ip 当前地址，作为"变化检测"的基线；启动时的地址变化由调用方负责通知。
 */
void heartbeat_start(heartbeat_cb_t cb, const char *current_ip);
