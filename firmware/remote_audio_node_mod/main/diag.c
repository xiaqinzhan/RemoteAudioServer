/**
 * diag.c —— 运行状态心跳（黑匣子日志）
 *
 * 为什么需要：真机上出现过"日志停在某一行、之后什么都没有、服务端显示离线"的现场，
 * 这种日志无法区分三种情况：任务挂死 / 芯片复位 / 只是断链。
 * 本任务不碰网络也不碰音频，只定期打一行快照，作为"CPU 是否还活着"的独立证据：
 *   - 心跳继续打   → CPU 活着，看 ws= 与发送统计判断卡在哪一层；
 *   - 心跳断掉、后面直接跟启动日志 → 复位/panic；
 *   - 心跳断掉且没有启动日志 → 有任务挂死了（看最后一行心跳的"慢发送/最慢"是否已到秒级）。
 */
#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "ws_client.h"
#include "audio_stream.h"

static const char *TAG = "diag";

#if CONFIG_RAN_DIAG_INTERVAL_MS > 0
static void diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RAN_DIAG_INTERVAL_MS));

        uint32_t fail = 0, slow = 0;
        int64_t  max_us = 0;
        ws_client_get_send_stats(&fail, &slow, &max_us);

        ESP_LOGI(TAG,
                 "心跳 %llds | ws=%d 推流=%d | 内部堆 %uK/最低 %uK | 发送失败 %u 慢发送 %u 最慢 %lldms 丢帧 %u",
                 (long long)(esp_timer_get_time() / 1000000),
                 ws_client_is_connected() ? 1 : 0,
                 audio_stream_active() ? 1 : 0,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)fail, (unsigned)slow,
                 (long long)(max_us / 1000),
                 (unsigned)audio_stream_drop_count());
    }
}
#endif

esp_err_t diag_start(void)
{
#if CONFIG_RAN_DIAG_INTERVAL_MS > 0
    if (xTaskCreatePinnedToCore(diag_task, "diag", 3072, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "运行状态心跳任务创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "运行状态心跳已启动（每 %d ms 一行）", CONFIG_RAN_DIAG_INTERVAL_MS);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "运行状态心跳已在 menuconfig 关闭（RAN_DIAG_INTERVAL_MS=0）");
    return ESP_OK;
#endif
}
