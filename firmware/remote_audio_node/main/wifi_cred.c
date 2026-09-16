/**
 * wifi_cred.c —— WiFi 凭据持久化实现
 */
#include "wifi_cred.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "wifi_cred";

#define NVS_WIFI_NS    "wifi"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

static esp_err_t open_rw(nvs_handle_t *h)
{
    return nvs_open(NVS_WIFI_NS, NVS_READWRITE, h);
}

esp_err_t wifi_cred_load(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    if (!ssid || !pass || ssid_size == 0 || pass_size == 0) return ESP_ERR_INVALID_ARG;
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_WIFI_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* 首次上电还没有这个 namespace 是正常情况，不打错误日志 */
        return err;
    }

    size_t len = ssid_size;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        ssid[0] = '\0';
        return err;
    }

    len = pass_size;
    if (nvs_get_str(h, NVS_KEY_PASS, pass, &len) != ESP_OK) {
        pass[0] = '\0';   /* 开放网络或仅保存了 SSID */
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t wifi_cred_save(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= WIFI_CRED_SSID_MAX) return ESP_ERR_INVALID_SIZE;
    if (pass && strlen(pass) >= WIFI_CRED_PASS_MAX) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "已保存 WiFi 凭据：ssid=%s（密码 %u 字节）",
                 ssid, (unsigned)strlen(pass ? pass : ""));
    } else {
        ESP_LOGE(TAG, "保存 WiFi 凭据失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_cred_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, NVS_KEY_SSID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_PASS);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGW(TAG, "已清除保存的 WiFi 凭据");
    return err;
}

bool wifi_cred_exists(void)
{
    char ssid[WIFI_CRED_SSID_MAX];
    char pass[WIFI_CRED_PASS_MAX];
    return wifi_cred_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0] != '\0';
}
