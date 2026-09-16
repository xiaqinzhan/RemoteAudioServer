/**
 * audio_stream.c —— 实时 PCM 广播帧（0x01）发送
 */
#include "audio_stream.h"
#include "board.h"
#include "ws_client.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/atomic.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "stream";

static volatile bool s_active;
static uint32_t s_drop_cnt;

/* 帧缓冲：静态预分配，不再每帧 malloc/free（50 Hz 反复分配会让内部 RAM 碎片化，
 * 而且分配失败就直接丢帧）。只有 audio_pipe 任务调用 audio_stream_send_frame()，
 * 单调用者，无需额外加锁。 */
static uint8_t s_frame[WS_FRAME_HEADER_LEN + AUDIO_FRAME_PCM_BYTES];

uint32_t audio_stream_drop_count(void)
{
    return s_drop_cnt;
}

bool audio_stream_active(void)
{
    return s_active;
}

void audio_stream_set_active(bool active)
{
    if (s_active != active) {
        ESP_LOGI(TAG, "实时推流 %s", active ? "开始" : "停止");
    }
    s_active = active;
}

void audio_stream_send_frame(const int16_t *pcm)
{
    if (!s_active || !pcm) return;
    if (!ws_client_is_connected()) return;

    /* 帧头 7 字节 + 640 字节 PCM，用静态缓冲直接整帧发送 */
    const int total = WS_FRAME_HEADER_LEN + AUDIO_FRAME_PCM_BYTES;
    uint8_t *buf = s_frame;
    buf[0] = WS_FRAME_TYPE_LIVE_PCM;          /* type = 0x01 */
    buf[1] = 0; buf[2] = 0; buf[3] = 0; buf[4] = 0;   /* req_id = 0（大端） */
    buf[5] = 0; buf[6] = 0;                          /* seq 忽略 */
    memcpy(buf + WS_FRAME_HEADER_LEN, pcm, AUDIO_FRAME_PCM_BYTES);

    esp_err_t err = ws_client_send_bin(buf, total, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_drop_cnt++;
        if (s_drop_cnt <= 10 || (s_drop_cnt % 100) == 0) {
            ESP_LOGW(TAG, "PCM 帧发送失败: %s（累计丢帧 %u）", esp_err_to_name(err),
                     (unsigned)s_drop_cnt);
        }
    }
}
