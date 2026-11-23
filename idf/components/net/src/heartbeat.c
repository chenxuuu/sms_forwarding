#include "net/heartbeat.h"

#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "net/wifi.h"

#define HB_PROBE_TIMEOUT_MS 3000
#define HB_FAIL_RECONNECT 3    /* 外网连续失败多少次后主动重连 WiFi */
#define HB_FAIL_WARN 6         /* 之外网连续失败多少次后升级为告警日志 */
#define HB_SELF_FAIL_RESTART 2 /* 本机 Web 连续无响应多少次后重启 */
#define HB_FIRST_DELAY_MS 30000

static heartbeat_cb_t s_cb;
static char s_last_ip[16];
static int s_inet_fail;
static int s_self_fail;
static int s_ok_count;

/** 目标可达性探测：域名解析 + TCP 连接（带超时）。 */
static bool tcp_probe(const char *host, uint16_t port, int timeout_ms)
{
    if (!host || !host[0]) {
        return false;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        freeaddrinfo(res);
        return false;
    }

    struct sockaddr_in addr = *(struct sockaddr_in *)res->ai_addr;
    addr.sin_port = htons(port);
    freeaddrinfo(res);

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    bool ok = false;
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                ok = true;
            }
        }
    }
    close(sock);
    return ok;
}

/** 连自己的 Web 端口：用来判断"设备本身是否还能提供服务"（与外网无关）。 */
static bool probe_self_web(void)
{
    const char *ip = wifi_ip();
    if (!ip || !ip[0] || strcmp(ip, "0.0.0.0") == 0) {
        return false;
    }
    return tcp_probe(ip, 80, 2000);
}

static void report_ip(const char *ip)
{
    if (strcmp(ip, s_last_ip) == 0) {
        return;
    }
    bool first = s_last_ip[0] == '\0';
    snprintf(s_last_ip, sizeof(s_last_ip), "%s", ip);
    config_state_set_last_ip(ip);
    log_line("设备 IP: %s", ip);
    if (!first && s_cb) {
        s_cb(HB_EVENT_IP_CHANGED, ip);
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(HB_FIRST_DELAY_MS));

    while (1) {
        const char *ip = wifi_ip();
        bool wifi_up = wifi_connected();

        /* 1) 本机健康：自己的 HTTP 端口还能连上吗？连不上说明服务已挂，只能重启 */
        if (probe_self_web()) {
            if (s_self_fail > 0) {
                log_line("本机 Web 服务恢复正常");
            }
            s_self_fail = 0;
        } else {
            s_self_fail++;
            log_line("本机 Web 无响应 %d/%d", s_self_fail, HB_SELF_FAIL_RESTART);
            if (s_self_fail >= HB_SELF_FAIL_RESTART) {
                log_line("本机服务持续无响应，重启设备");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_HEARTBEAT_INTERVAL_S * 1000));
            continue;
        }

        /* 2) 外网健康：DNS + TCP 探测。外网断只影响通知，不重启设备 */
        bool inet_ok =
            wifi_up && tcp_probe(CONFIG_HEARTBEAT_HOST, CONFIG_HEARTBEAT_PORT, HB_PROBE_TIMEOUT_MS);
        if (inet_ok) {
            if (s_inet_fail > 0) {
                log_line("外网连接已恢复（此前失败 %d 次）", s_inet_fail);
                if (s_cb) {
                    s_cb(HB_EVENT_RECOVERED, ip);
                }
            }
            s_inet_fail = 0;
            report_ip(ip);
            if (++s_ok_count % (3600 / CONFIG_HEARTBEAT_INTERVAL_S) == 0) {
                log_line("心跳正常：IP %s，空闲堆 %u KB，已运行 %lld 分钟", ip,
                         (unsigned)(esp_get_free_heap_size() / 1024),
                         (long long)(esp_timer_get_time() / 60000000));
            }
        } else {
            s_inet_fail++;
            if (s_inet_fail == HB_FAIL_RECONNECT) {
                log_line("外网连续 %d 次不可达，主动重连 WiFi", s_inet_fail);
                wifi_kick();
            } else if (s_inet_fail % HB_FAIL_WARN == 0) {
                log_line("外网持续不可达（%d 次，WiFi %s），继续重试，不重启设备", s_inet_fail,
                         wifi_up ? "已连接" : "未连接");
                wifi_kick();
            } else {
                log_line("外网探测失败 %d 次（WiFi %s）", s_inet_fail,
                         wifi_up ? "已连接" : "未连接");
            }
        }

        vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_HEARTBEAT_INTERVAL_S * 1000));
    }
}

void heartbeat_start(heartbeat_cb_t cb, const char *current_ip)
{
    s_cb = cb;
    /* 基线由调用方给出（启动时的变化已在 app_main 里通知过，避免重复） */
    snprintf(s_last_ip, sizeof(s_last_ip), "%s", current_ip ? current_ip : "");
    config_state_set_last_ip(s_last_ip);
    s_inet_fail = 0;
    s_self_fail = 0;
    s_ok_count = 0;
    xTaskCreate(heartbeat_task, "heartbeat", 4096, NULL, 2, NULL);
    log_line("网络心跳已启动：每 %d 秒探测本机 Web 与 %s:%d", CONFIG_HEARTBEAT_INTERVAL_S,
             CONFIG_HEARTBEAT_HOST, CONFIG_HEARTBEAT_PORT);
}
