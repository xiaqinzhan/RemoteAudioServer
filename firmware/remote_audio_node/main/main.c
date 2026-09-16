/**
 * main.c —— 多设备音频采集节点（ESP32-S3）启动入口
 *
 * 初始化顺序：NVS/配置 -> WiFi+SNTP -> SD 卡 -> I2S -> 录音状态机 ->
 *             按键 -> OLED -> 诊断心跳 -> （如需配网则先等配网）-> WebSocket
 * 任一非关键模块失败（SD/OLED）仅告警，不阻塞其余功能。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"

#include "device_config.h"
#include "wifi_mgr.h"
#include "wifi_prov.h"
#include "sd_storage.h"
#include "audio_i2s.h"
#include "recorder.h"
#include "btn.h"
#include "oled.h"
#include "ws_client.h"
#include "diag.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== remote-audio-node fw %s 启动 (ESP32-S3) ===",
             CONFIG_RAN_FW_VERSION);

    /* 1. NVS + 设备配置（device_id、录音开关、VAD、保留天数） */
    ESP_ERROR_CHECK(device_config_init());
    const ran_config_t *cfg = device_config_get();
    ESP_LOGI(TAG, "device_id = %s", cfg->device_id);

    /* 2. WiFi STA + SNTP（非阻塞，后台连接） */
    ESP_ERROR_CHECK(wifi_mgr_init());

    /* 3. MicroSD（FATFS /sdcard）；失败不阻塞实时推流 */
    esp_err_t sd_err = sd_storage_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡不可用：录音/文件功能停用，实时监听仍可工作");
    } else {
        /* 启动时执行一次保留策略清理 */
        sd_storage_apply_retention(cfg->retention_days);
    }

    /* 4. ES7210 ADC + I2S RX（音频采集）
     *    失败不 abort：这样 WebSocket、文件列表、按键仍然可用，
     *    日志里会明确写出失败原因，比直接重启好排查。 */
    if (audio_i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频采集初始化失败：录音与实时推流不可用"
                      "（检查 ES7210 排线 / I2C 地址是否被探测到）");
    }

    /* 5. 录音状态机（VAD + Opus 编码 + Ogg 落盘任务） */
    ESP_ERROR_CHECK(recorder_start());

    /* 6. 录音开关按键（GPIO0） */
    if (btn_start() != ESP_OK) {
        ESP_LOGW(TAG, "按键模块启动失败（不影响其他功能）");
    }

    /* 7. OLED 状态显示（可选，失败不阻塞） */
    if (oled_start() != ESP_OK) {
        ESP_LOGW(TAG, "OLED 不可用（未接线或地址错误），继续运行");
    }

    /* 8. 运行状态心跳（诊断黑匣子；失败不影响功能）
     *    提前到 WebSocket 之前：配网模式下也要能看到设备在跑 */
    if (diag_start() != ESP_OK) {
        ESP_LOGW(TAG, "运行状态心跳未启动（不影响功能）");
    }

    /* 9. 配网门闸：处于配网模式时先等用户配好网，再启动 WebSocket。
     *    没有网络时启动 WS 只会反复重连刷日志，先把配网做完更干净。 */
    if (wifi_mgr_is_provisioning()) {
        ESP_LOGW(TAG, "进入配网模式：手机连热点「%s」→ 浏览器打开 http://%s",
                 wifi_prov_ap_ssid(), CONFIG_RAN_PROV_AP_IP);
        uint32_t waited_s = 0;
        while (!wifi_mgr_is_connected() && wifi_mgr_is_provisioning()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited_s++;
            if (waited_s % 30 == 0) {
                ESP_LOGW(TAG, "配网等待中（%u 秒）：热点「%s」· 配网页 http://%s",
                         (unsigned)waited_s, wifi_prov_ap_ssid(), CONFIG_RAN_PROV_AP_IP);
            }
        }
        ESP_LOGI(TAG, "配网流程结束（%s），继续启动 WebSocket",
                 wifi_mgr_is_connected() ? "已连上 WiFi" : "未连上，交给 WS 自身重试");
    }

    /* 10. WebSocket 客户端（自动重连，连上即发 hello） */
    ESP_ERROR_CHECK(ws_client_start());

    ESP_LOGI(TAG, "=== 初始化完成，WiFi=%s IP=%s，目标服务端 ws://%s:%d/ws/device/%s ===",
             wifi_mgr_is_connected() ? "已连接" : "未连接",
             wifi_mgr_get_ip(),
             CONFIG_RAN_SERVER_HOST, CONFIG_RAN_SERVER_PORT, cfg->device_id);
}
