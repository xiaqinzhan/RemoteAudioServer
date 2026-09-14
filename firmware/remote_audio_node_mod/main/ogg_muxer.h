/**
 * ogg_muxer.h —— 轻量 Ogg Opus 复用器（RFC 7845 / RFC 3533 子集）
 *
 * 文件结构：
 *   页0: BOS  (0x02)  OpusHead  (magic/version=1/channels=1/pre-skip=312/
 *                               input-sample-rate=16000/gain=0/mapping-family=0)
 *   页1:      (0x00)  OpusTags  (vendor 字符串 + 0 条用户注释)
 *   页2..:    音频页，每个 Opus 包(20ms)一页；granule_position 按 48kHz 采样累计
 *             （RFC 7845 规定 Ogg Opus 的 granule 恒为 48kHz 域，每包 +960）
 *   末页:     EOS  (0x04)，close 时回填到最后一个音频页
 * CRC：标准 Ogg CRC32，多项式 0x04c11db7，初值 0，不反射。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ogg_muxer_s ogg_muxer_t;

/** 在 abs_path 上创建 .tmp 录音并写入 BOS/Tags 两页 */
esp_err_t ogg_muxer_open(const char *abs_path, uint32_t serial, ogg_muxer_t **out);

/** 写入一个 Opus 数据包（20ms 帧），内部封装为一个 Ogg 页 */
esp_err_t ogg_muxer_write_packet(ogg_muxer_t *m, const uint8_t *pkg, int len);

/** 刷新 EOS 页并关闭文件；out_bytes 返回文件总大小（可为 NULL） */
esp_err_t ogg_muxer_close(ogg_muxer_t *m, uint32_t *out_bytes);

#ifdef __cplusplus
}
#endif
