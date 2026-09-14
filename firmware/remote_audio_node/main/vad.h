/**
 * vad.h —— 能量法 VAD：滑动窗 RMS / 峰值检测
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t threshold;     /* RMS 阈值（按 16bit 满量程 32767） */
    int      window_frames; /* 滑动窗帧数（20ms/帧） */
    int      hist_len;      /* 实际已缓存帧数 */
    uint32_t sum_sq;        /* 窗内能量平方和（滚动） */
    uint32_t ring[32];      /* 每帧 RMS；窗口上限 32 帧(640ms) */
} vad_t;

void vad_init(vad_t *v, uint32_t threshold, int window_frames);

/** 更新阈值（服务端 set_config 后生效） */
void vad_set_threshold(vad_t *v, uint32_t threshold);

/**
 * 送入一帧 20ms PCM（320 samples int16），返回 true=该帧有声。
 * 判定：滑动窗 RMS 超阈值即有声（窗口可滤掉偶发点击噪声）。
 * *frame_rms 输出本帧 RMS（可用于日志/显示），可为 NULL。
 */
bool vad_process_frame(vad_t *v, const int16_t *pcm, uint32_t *frame_rms);

#ifdef __cplusplus
}
#endif
