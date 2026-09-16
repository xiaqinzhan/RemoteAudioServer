/**
 * audio_i2s.c —— I2S RX 实现（立创实战派 S3 + ES7210 音频 ADC）
 *
 * ES7210 通过 I2C 配置完成后，在 I2S 标准模式下输出 16bit 立体声 PCM。
 * ESP32 作为 I2S Master 提供 MCLK/BCLK/WS 时钟。
 * 录音取左声道（MIC1），右声道（MIC2）保留供后续扩展。
 *
 * 采集通道是 simplex（i2s_new_channel 只传 RX 句柄，不带 TX）：
 * RX 自己就是时钟发生器，不需要 TX 帮忙提供 BCLK/WS。
 *
 * ⚠️ 这里原本还有一个 I2S TX 占位通道（ES8311 播放），已经删掉。原因：
 * 它用的是另一个控制器 I2S_NUM_1，BCLK/WS 却复用了 I2S0 的 GPIO14/13。
 * 同一组引脚被两个控制器同时驱动时，GPIO 矩阵只会保留最后接上的那一路，
 * I2S0 的时钟就被从引脚上挤掉了 —— 录音当场失去时钟，现象是
 * i2s_channel_read 每帧都等满超时、录音慢几十倍。
 * 以后要加播放，正确做法是把 TX 挂到 I2S_NUM_0 做全双工（两个句柄共用
 * 同一份时钟/引脚配置），并且必须先 enable TX 再 enable RX，
 * 否则 RX 同样拿不到时钟（I2S 全双工下时钟发生器在 TX 侧）。
 */
#include "audio_i2s.h"
#include "board.h"
#include "es7210_drv.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"

static const char *TAG = "audio_i2s";

/* 一帧只有 20ms，读取超时给 100ms 余量。
 * 注意 i2s_channel_read 的 timeout 单位是**毫秒**，不是 tick，
 * 不要传 pdMS_TO_TICKS（FREERTOS_HZ 不是 1000 时会算错）。 */
#define AUDIO_I2S_READ_TIMEOUT_MS   100

static i2s_chan_handle_t s_rx_chan;
static volatile int      s_last_level;

esp_err_t audio_i2s_init(void)
{
    /* ========== 1. 先初始化 ES7210 ADC（只走 I2C，不需要时钟） ========== */
    ESP_LOGI(TAG, "初始化 ES7210 ADC (I2C%d, SDA=IO%d, SCL=IO%d)",
             BOARD_ES7210_I2C_PORT, BOARD_ES7210_I2C_SDA, BOARD_ES7210_I2C_SCL);
    esp_err_t err = es7210_init(
        BOARD_ES7210_I2C_PORT,
        BOARD_ES7210_I2C_SDA,
        BOARD_ES7210_I2C_SCL,
        BOARD_ES7210_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 初始化失败: %s（I2C 地址或排线问题）", esp_err_to_name(err));
        return err;
    }

    /* ========== 2. 创建 I2S RX 通道（ES7210 数据输入） ========== */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_RX_PORT, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num  = 6;
    /* 一个描述符正好装一帧（320 帧 = 20ms），总缓冲 6 × 20ms = 120ms。
     * 原来写的 64 帧/描述符只有 24ms 总缓冲，Opus 编码或 SD 卡写稍微卡顿
     * 一下 DMA 就被覆盖，会直接丢音频。 */
    rx_chan_cfg.dma_frame_num = AUDIO_FRAME_SAMPLES;

    err = i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(RX) 失败: %s", esp_err_to_name(err));
        s_rx_chan = NULL;
        return err;
    }

    /* ES7210 输出 16bit 数据 → 直接匹配 16bit slot，无需 INMP441 那种位移缩放 */
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_ES7210_MCLK,
            .bclk = BOARD_I2S_RX_SCK,
            .ws   = BOARD_I2S_RX_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = BOARD_I2S_RX_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    /* MCLK = 256 × Fs（16kHz → 4.096MHz），必须与 es7210_drv.c 的
     * ES7210_MCLK_RATIO 一致，否则采样率整体偏移。 */
    rx_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s_rx_chan, &rx_std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(RX) 失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2s_channel_enable(RX) 失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    ESP_LOGI(TAG, "I2S RX 已启动：%dHz / 16bit 立体声槽位（取左声道 MIC1）",
             AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_i2s_read_frame(int16_t *pcm16_out)
{
    if (!s_rx_chan || !pcm16_out) return ESP_ERR_INVALID_STATE;

    /* DMA 数据：16bit × 2 声道 × 320 帧 = 1280 字节 */
    int16_t raw[AUDIO_FRAME_SAMPLES * 2];
    size_t  bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, raw, sizeof(raw),
                                     &bytes_read, AUDIO_I2S_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_read: %s", esp_err_to_name(err));
        return err;
    }

    int got = (int)(bytes_read / sizeof(int16_t));   /* 实际拿到的采样数（左右交织） */
    if (got > AUDIO_FRAME_SAMPLES * 2) {
        got = AUDIO_FRAME_SAMPLES * 2;               /* 防御：越界就截断 */
    }
    if (got < AUDIO_FRAME_SAMPLES * 2) {
        /* 短读：尾巴补 0，避免把上一次的残留数据当成本帧音频 */
        ESP_LOGW(TAG, "I2S 短读: %u/%u 字节", (unsigned)bytes_read, (unsigned)sizeof(raw));
        memset(&raw[got], 0, sizeof(raw) - (size_t)got * sizeof(int16_t));
    }

    /* ES7210 输出标准 16bit PCM，无需位移缩放。
     * 立体声交织：[L0, R0, L1, R1, ...]
     * 取左声道（MIC1）作为单声道输出 */
    int peak = 0;
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        int16_t s = raw[2 * i];  /* 左声道 = MIC1 */
        pcm16_out[i] = s;
        int a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    s_last_level = peak;
    return ESP_OK;
}

int audio_i2s_last_level(void)
{
    return s_last_level;
}
