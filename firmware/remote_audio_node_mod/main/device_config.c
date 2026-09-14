/**
 * device_config.c —— 设备配置实现
 */
#include "device_config.h"

#include <string.h>
#include <stdio.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "device_cfg";

#define NVS_NAMESPACE  "ran"
#define NVS_KEY_REC    "rec_en"
#define NVS_KEY_VAD    "vad_th"
#define NVS_KEY_RET    "ret_day"

static ran_config_t s_cfg;

/** 由 MAC 后 3 字节派生默认 device_id：esp32-aabbcc */
static void derive_device_id(void)
{
    const char *over = CONFIG_RAN_DEVICE_ID_OVERRIDE;
    if (over && over[0] != '\0') {
        snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "%s", over);
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_cfg.device_id, sizeof(s_cfg.device_id),
             "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

esp_err_t device_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区不兼容，擦除后重新初始化 (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* Kconfig 默认值 */
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.recording_enabled = true;
    s_cfg.vad_threshold     = (uint32_t)CONFIG_RAN_VAD_THRESHOLD;
    s_cfg.retention_days    = (uint32_t)CONFIG_RAN_RETENTION_DAYS;
    derive_device_id();

    /* NVS 覆盖 */
    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        uint8_t en = 1;
        int32_t v32 = 0;
        if (nvs_get_u8(h, NVS_KEY_REC, &en) == ESP_OK) {
            s_cfg.recording_enabled = (en != 0);
        }
        if (nvs_get_i32(h, NVS_KEY_VAD, &v32) == ESP_OK && v32 > 0) {
            s_cfg.vad_threshold = (uint32_t)v32;
        }
        if (nvs_get_i32(h, NVS_KEY_RET, &v32) == ESP_OK && v32 >= 0) {
            s_cfg.retention_days = (uint32_t)v32;
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "NVS 打开失败，使用默认配置: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "device_id=%s rec=%d vad=%lu ret=%lu",
             s_cfg.device_id, s_cfg.recording_enabled,
             (unsigned long)s_cfg.vad_threshold,
             (unsigned long)s_cfg.retention_days);
    return ESP_OK;
}

const ran_config_t *device_config_get(void)
{
    return &s_cfg;
}

static esp_err_t nvs_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_i32(const char *key, int32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t device_config_set_recording_enabled(bool enabled)
{
    s_cfg.recording_enabled = enabled;
    esp_err_t err = nvs_u8(NVS_KEY_REC, enabled ? 1 : 0);
    if (err != ESP_OK) ESP_LOGE(TAG, "持久化录音开关失败: %s", esp_err_to_name(err));
    device_config_notify_changed();
    return err;
}

esp_err_t device_config_set_vad_threshold(uint32_t v)
{
    if (v == 0) return ESP_ERR_INVALID_ARG;
    s_cfg.vad_threshold = v;
    esp_err_t err = nvs_i32(NVS_KEY_VAD, (int32_t)v);
    if (err != ESP_OK) ESP_LOGE(TAG, "持久化 VAD 阈值失败: %s", esp_err_to_name(err));
    device_config_notify_changed();
    return err;
}

esp_err_t device_config_set_retention_days(uint32_t days)
{
    s_cfg.retention_days = days;
    esp_err_t err = nvs_i32(NVS_KEY_RET, (int32_t)days);
    if (err != ESP_OK) ESP_LOGE(TAG, "持久化保留天数失败: %s", esp_err_to_name(err));
    device_config_notify_changed();
    return err;
}

/* 配置变化的弱回调：ws_client 模块注册后用于刷新 hello/心跳之外的状态展示，
   这里保留统一入口，oled 等模块自行轮询 device_config_get() 即可。 */
void __attribute__((weak)) device_config_notify_changed(void)
{
}
