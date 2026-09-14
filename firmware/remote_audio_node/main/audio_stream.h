/**
 * audio_stream.h —— 实时帧分发（WS binary 0x01 PCM 广播帧）
 * 录音/推流状态机在 recorder.c 中；本模块只负责把 20ms PCM 帧
 * 封装成协议帧并通过 ws_client 发送。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 是否有监听者正在拉流（服务端下发 start_stream 置位） */
bool audio_stream_active(void);

/** 设置实时推流开关（收到 start_stream/stop_stream 命令时调用） */
void audio_stream_set_active(bool active);

/**
 * 发送一帧实时 PCM：封装 7 字节帧头（type=0x01, req_id=0, seq=0）+ 640 字节 PCM。
 * 未连接/未在推流时直接返回。失败仅计数日志，不阻塞采集链路。
 */
void audio_stream_send_frame(const int16_t *pcm);

/** 累计丢帧数（分配/发送失败；诊断心跳会展示） */
uint32_t audio_stream_drop_count(void);

#ifdef __cplusplus
}
#endif
