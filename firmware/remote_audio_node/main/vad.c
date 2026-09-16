/**
 * vad.c —— 滑动窗能量 VAD
 */
#include "vad.h"
#include "board.h"

#include <string.h>

void vad_init(vad_t *v, uint32_t threshold, int window_frames)
{
    memset(v, 0, sizeof(*v));
    v->threshold = threshold;
    if (window_frames < 1) window_frames = 1;
    if (window_frames > 32) window_frames = 32;
    v->window_frames = window_frames;
}

void vad_set_threshold(vad_t *v, uint32_t threshold)
{
    if (threshold > 0) v->threshold = threshold;
}

bool vad_process_frame(vad_t *v, const int16_t *pcm, uint32_t *frame_rms)
{
    /* 本帧均方能量 */
    uint64_t sq = 0;
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        int32_t s = pcm[i];
        sq += (uint64_t)(s * s);
    }
    uint32_t rms = (uint32_t)(sq / AUDIO_FRAME_SAMPLES);   /* 均方（未开根，比较阈值平方量级） */

    /* 滚动窗：写入环形，减去最旧帧 */
    if (v->hist_len == v->window_frames) {
        v->sum_sq -= v->ring[0];
        memmove(v->ring, v->ring + 1, sizeof(v->ring[0]) * (v->window_frames - 1));
        v->hist_len = v->window_frames - 1;
    }
    v->ring[v->hist_len++] = rms;
    v->sum_sq += rms;

    uint32_t win_ms = (uint32_t)v->hist_len;
    uint32_t win_rms = v->sum_sq / win_ms;

    if (frame_rms) {
        /* 输出真实 RMS（开方），便于日志 */
        uint32_t lo = 0, hi = 32768;
        while (lo < hi) {
            uint32_t mid = (lo + hi + 1) / 2;
            if (mid * mid <= rms) lo = mid; else hi = mid - 1;
        }
        *frame_rms = lo;
    }

    /* 与阈值平方比较（阈值默认 1200，平方量级约 1.44e6） */
    uint64_t thr_sq = (uint64_t)v->threshold * v->threshold;
    return win_rms >= (uint32_t)(thr_sq);
}
