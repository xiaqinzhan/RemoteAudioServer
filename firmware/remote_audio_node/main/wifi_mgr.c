/**
 * wifi_mgr.c —— WiFi STA + SNTP
 */
#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_mgr";

#define BIT_CONNECTED  BIT0

static EventGroupHandle_t s_evt;
static char s_ip_str[16] = "0.0.0.0";
static volatile bool s_time_synced;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "获取 IP: %s", s_ip_str);
    xEventGroupSetBits(s_evt, BIT_CONNECTED);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 启动，开始连接 %s ...", CONFIG_RAN_WIFI_SSID);
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi 断开 (reason=%d)，2s 后重连", d ? d->reason : -1);
        xEventGroupClearBits(s_evt, BIT_CONNECTED);
        snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    }
}

static void sntp_synced_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    ESP_LOGI(TAG, "SNTP 时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
}

static void start_sntp(void)
{
    setenv("TZ", "CST-8", 1);   /* 中国标准时间 UTC+8（录音目录按本地时间命名） */
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_set_sync_interval(3600 * 1000);
    esp_sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 已启动，等待时间同步...");
}

esp_err_t wifi_mgr_init(void)
{
    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", CONFIG_RAN_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", CONFIG_RAN_WIFI_PASSWORD);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_sntp();
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return s_evt && (xEventGroupGetBits(s_evt) & BIT_CONNECTED);
}

const char *wifi_mgr_get_ip(void)
{
    return s_ip_str;
}

bool wifi_mgr_time_synced(void)
{
    return s_time_synced;
}
