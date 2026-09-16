/**
 * audio_i2s.h —— I2S 采集接口（实战派 S3 + ES7210 音频 ADC）
 *
 * 初始化时自动配置 ES7210（I2C）+ I2S RX 通道。
 * 读取帧返回 16bit 单声道 PCM（取自 MIC1 左声道）。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ES7210 + I2S RX 通道（16kHz/16bit/立体声槽位，取左声道） */
esp_err_t audio_i2s_init(void);

/**
 * 阻塞读取一帧 20ms PCM（单声道 int16，320 samples / 640 字节）。
 * 内部从 ES7210 的立体声 I2S 数据提取左声道（MIC1）。
 * 返回 ESP_OK / ESP_ERR_INVALID_STATE（未初始化）。
 */
esp_err_t audio_i2s_read_frame(int16_t *pcm16_out);

/** 最近一帧的电平 0..32767（供 OLED 电平条显示） */
int audio_i2s_last_level(void);

#ifdef __cplusplus
}
#endif
