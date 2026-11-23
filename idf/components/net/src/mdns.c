#include "net/mdns.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"

void net_device_name(char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    if (CONFIG_MDNS_NAME[0] != 0) {
        snprintf(out, cap, "%s", CONFIG_MDNS_NAME);
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        snprintf(out, cap, "sms-forwarder");
        return;
    }
    /* 用 MAC 后两字节做后缀：内网多台同固件设备也不会撞名 */
    snprintf(out, cap, "sms-%02x%02x", mac[4], mac[5]);
}

esp_err_t net_mdns_start(void)
{
    char host[33];
    net_device_name(host, sizeof(host));

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_set_hostname(netif, host);
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        log_line("mDNS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set("SMS Forwarder");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    log_line("mDNS 已启动：http://%s.local/", host);
    return ESP_OK;
}
