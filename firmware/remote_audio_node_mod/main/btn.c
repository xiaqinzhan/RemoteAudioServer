/**
 * btn.c —— GPIO0 按键轮询消抖
 */
#include "btn.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "recorder.h"

static const char *TAG = "btn";

#define POLL_PERIOD_MS   20
#define DEBOUNCE_MS      50
#define DEBOUNCE_COUNT   (DEBOUNCE_MS / POLL_PERIOD_MS)

static void btn_task(void *arg)
{
    (void)arg;
    int stable_low = 0;
    bool pressed_latched = false;

    for (;;) {
        int level = gpio_get_level(BOARD_BTN_REC);
        if (level == 0) {
            stable_low++;
            if (!pressed_latched && stable_low >= DEBOUNCE_COUNT) {
                pressed_latched = true;
                bool now_on = !recorder_is_enabled();
                ESP_LOGI(TAG, "按键：录音功能切换为 %s", now_on ? "开启" : "关闭");
                recorder_set_enabled(now_on);
            }
        } else {
            stable_low = 0;
            pressed_latched = false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t btn_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BTN_REC,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键 GPIO 配置失败: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(btn_task, "btn", 2560, NULL, 6, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "录音按键已启动 (GPIO%d, 低有效, 消抖 %dms)",
             BOARD_BTN_REC, DEBOUNCE_MS);
    return ESP_OK;
}
