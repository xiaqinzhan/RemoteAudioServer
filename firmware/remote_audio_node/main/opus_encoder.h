/**
 * opus_encoder.h —— Opus 编码器薄封装（唯一直接依赖 Opus 组件的模块）
 *
 * 标准 libopus API：opus_encoder_create / opus_encoder_ctl / opus_encode。
 * 参数固定为 16kHz / 单声道 / VOIP 应用 / 32kbps CBR / 20ms 帧(320 samples)。
 * 若更换 Opus 组件（78/esp-opus、esphome/micro-opus、官方 libopus 移植），
 * 仅需保证 opus.h 提供上述符号即可，本接口与其它模块代码不用改。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct opus_enc_s opus_enc_t;

/** 创建编码器（内部 RAM 分配，Opus fixed-point 栈需求由调用任务栈承载） */
esp_err_t opus_encoder_open(opus_enc_t **out);

/** 编码一帧 20ms PCM（320 samples int16）。
 *  out_pkg 输出 Opus 数据包，返回字节数；静音帧 libopus 可能返回极小包（DTX 关闭）。
 *  返回 <0 为错误码。 */
int opus_encoder_encode(opus_enc_t *enc, const int16_t *pcm320, uint8_t *out_pkg, int out_cap);

void opus_encoder_close(opus_enc_t *enc);

#ifdef __cplusplus
}
#endif
