#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "esim/esim.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "forwarder/forwarder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem/modem.h"
#include "net/heartbeat.h"
#include "net/mdns.h"
#include "net/time_sync.h"
#include "net/wifi.h"
#include "net/wifi_portal.h"
#include "notify/notify.h"
#include "ota/ota.h"
#include "sched/sched.h"
#include "web/web_server.h"

/**
 * 启动顺序（与旧 Arduino 固件保持一致，但 Web 更早可用）：
 *  1. 日志 + 配置（NVS）
 *  2. 通知任务 / 转发策略（先建好队列，短信不会丢）
 *  3. WiFi（20s 超时，失败重启重试）
 *  4. Web 服务（此时即可访问配置页面）
 *  5. NTP 时间同步（异步）
 *  6. 网络心跳（DHCP 换 IP / 断网自愈，并把新地址发邮件）
 *  7. 4G 模组初始化（慢，异步任务中执行）+ 蜂窝网络守护（掉线自动重连）
 *  8. 启动通知邮件
 */
static void on_net_event(heartbeat_event_t ev, const char *ip)
{
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", ip);
    if (ev == HB_EVENT_IP_CHANGED) {
        forwarder_notify_device("设备网络地址已变化", "DHCP 重新分配了地址，请使用新地址访问", url);
    } else if (ev == HB_EVENT_RECOVERED) {
        forwarder_notify_device("设备网络已恢复", "网络心跳连续失败后已自动恢复", url);
    }
}

/**
 * 模组自愈结果：只有「多轮重启仍连不上」和「之后恢复」才发通知，
 * 日常掉线重连不打扰用户（只在日志里记录）。
 */
static void on_modem_event(modem_net_event_t ev, const char *detail)
{
    char url[48];
    snprintf(url, sizeof(url), "http://%s/", wifi_ip());
    if (ev == MODEM_NET_FAILED) {
        forwarder_notify_device("4G 网络无法自动重连", detail, url);
    } else {
        forwarder_notify_device("4G 网络已恢复", detail, url);
    }
}

void app_main(void)
{
    log_init();
    log_line("=== 短信转发器启动（ESP-IDF）===");
    log_line("复位原因: %d，可用内存: %u KB", (int)esp_reset_reason(),
             (unsigned)(esp_get_free_heap_size() / 1024));

    config_init();
    notify_init();
    forwarder_init();
    sched_init();
    ota_init(); /* 版本号、回滚确认、每日版本检查都在这里起步 */

    esp_err_t err = wifi_start(20000);
    if (err != ESP_OK) {
        /* 没账号，或连续 3 次都连不上已保存的账号（多半是路由器改了密码）：
         * 开热点让人用手机配网，配好自动重启，避免设备永远失联。 */
        bool need_portal = !wifi_configured() || wifi_boot_fails() >= 3;
        if (need_portal) {
            if (!wifi_configured()) {
                log_line("还没有 WiFi 账号，进入配网模式");
            } else {
                log_line("连续 %d 次连不上 %s，进入配网模式重新设置", wifi_boot_fails(), wifi_ssid());
            }
            if (wifi_portal_start() == ESP_OK) {
                char hint[128];
                snprintf(hint, sizeof(hint), "手机连热点 %s（无密码），浏览器打开 http://192.168.4.1/",
                         wifi_portal_ssid());
                log_line("%s", hint);
                while (1) {
                    vTaskDelay(pdMS_TO_TICKS(60000));
                }
            }
            log_line("配网模式启动失败，保持控制台可读（60 秒后重试提示）");
            while (1) {
                log_line("设备未配置 WiFi，无法联网");
                vTaskDelay(pdMS_TO_TICKS(60000));
            }
        }
        log_line("WiFi 连接超时，10 秒后重启重试...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    web_server_start();
    time_sync_start();
    net_mdns_start();

    /* 重启后的地址变化也能发现：和上次记录的地址比一比 */
    const char *ip = wifi_ip();
    char last_ip[16];
    bool had_last = config_state_get_last_ip(last_ip, sizeof(last_ip));
    bool ip_changed = had_last && strcmp(last_ip, ip) != 0;

    heartbeat_start(on_net_event, ip);
    modem_set_net_event_handler(on_modem_event);
    modem_start();
    esim_init();

    char url[48];
    snprintf(url, sizeof(url), "http://%s/", ip);
    if (ip_changed) {
        char detail[96];
        snprintf(detail, sizeof(detail), "地址已变化：%s → %s", last_ip, ip);
        log_line("%s", detail);
        forwarder_notify_device("设备网络地址已变化", detail, url);
    } else if (config_any_valid()) {
        log_line("配置有效，发送启动通知，设备地址 %s", url);
        forwarder_notify_device("短信转发器已启动", "设备已启动", url);
    } else {
        log_line("尚未配置通知方式，请访问 %s 完成配置", url);
    }
}
