/**
 * recorder.h —— 录音状态机 + VAD + Opus 编码 + Ogg 落盘
 *
 * 状态机（pipeline 任务，核0）：
 *   空闲 -> 检测到声音（携带前滚 200ms 预滚动）-> 录音中（Opus 写 .tmp）
 *        -> 静音 1.5s 超时 / 到达 10 分钟切片 / 功能关闭 -> flush Ogg EOS
 *        -> rename 为 .opus（原子落盘）-> 保留策略清理 -> 上报 recording_saved
 * 编码任务（核1）：从队列取 PCM 帧做 Opus 编码 + Ogg 写页，与采集任务解耦。
 * recording_enabled=false 时不做 VAD、不写文件；实时推流不受影响。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建队列与任务（I2S 初始化后调用） */
esp_err_t recorder_start(void);

/** 当前是否正在录音（OLED/状态查询） */
bool recorder_is_recording(void);

/** 录音功能开关（btn / set_recording 命令）；关闭时若正在录音则停止并保存 */
void recorder_set_enabled(bool enabled);
bool recorder_is_enabled(void);

#ifdef __cplusplus
}
#endif
