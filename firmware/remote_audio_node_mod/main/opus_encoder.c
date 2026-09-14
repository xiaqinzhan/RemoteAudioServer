/**
 * opus_encoder.c —— Opus 薄封装实现
 *
 * 依赖组件：78/esp-opus（libopus 官方移植，提供标准 opus.h，见 idf_component.yml）。
 * 本文件是全工程唯一 #include "opus.h" 的地方，替换组件时无需修改其它代码。
 */
#include "opus_encoder.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "opus.h"   /* 标准 libopus 头文件 */

static const char *TAG = "opus_enc";

struct opus_enc_s {
    OpusEncoder *enc;
};

esp_err_t opus_encoder_open(opus_enc_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    opus_enc_t *h = calloc(1, sizeof(opus_enc_t));
    if (!h) {
        ESP_LOGE(TAG, "封装结构分配失败");
        return ESP_ERR_NO_MEM;
    }

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(AUDIO_SAMPLE_RATE_HZ, 1,
                                           OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) {
        ESP_LOGE(TAG, "opus_encoder_create 失败: %d (%s)",
                 err, opus_strerror(err));
        free(h);
        return ESP_FAIL;
    }

    /* 32kbps CBR，低复杂度（适合 ESP32-S3 单核实时），关闭 VBR/DTX 保证时长一致 */
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(AUDIO_REC_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_20_MS));

    h->enc = enc;
    *out = h;
    ESP_LOGI(TAG, "Opus 编码器就绪: %dHz mono, %dbps CBR, 20ms/frame",
             AUDIO_SAMPLE_RATE_HZ, AUDIO_REC_BITRATE);
    return ESP_OK;
}

int opus_encoder_encode(opus_enc_t *h, const int16_t *pcm320, uint8_t *out_pkg, int out_cap)
{
    if (!h || !h->enc || !pcm320 || !out_pkg || out_cap < 1) return -1;

    int n = opus_encode(h->enc, pcm320, AUDIO_FRAME_SAMPLES, out_pkg, out_cap);
    if (n < 0) {
        ESP_LOGE(TAG, "opus_encode 失败: %d (%s)", n, opus_strerror(n));
        return n;
    }
    return n;
}

void opus_encoder_close(opus_enc_t *h)
{
    if (!h) return;
    if (h->enc) opus_encoder_destroy(h->enc);
    free(h);
}
