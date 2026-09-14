/**
 * ws_client.h —— WebSocket 客户端（连接服务端 remoteAudioServer）
 *
 * 职责：
 *  - 连接 ws://<HOST>:<PORT>/ws/device/<device_id>，指数退避重连（v2.2：300ms~3s）
 *  - 连接后发 hello（v2.2：若断线前在推流，hello 后自动续流，用户不用重新点）
 *  - v2.2：WS 协议级 ping 每 10s（ping_interval_sec），pong 超时 30s —— 应用层
 *    JSON ping 平台不认，长连接会在约 300s 被接入层掐断
 *  - 每 15s 发 {"type":"ping"}（服务端 45s 判离线）
 *  - 接收 JSON 命令并分发：start_stream/stop_stream/list_recordings/
 *    play_file/stop_file/set_recording/set_config
 *  - binary 帧：0x01 实时 PCM（audio_stream 模块使用）、0x02 文件块（内部回传任务）
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 WS 管理任务（内部自动重连） */
esp_err_t ws_client_start(void);

bool ws_client_is_connected(void);

/** 发送 binary 帧（任意 type；调用方自带 7 字节帧头） */
esp_err_t ws_client_send_bin(const uint8_t *data, int len, uint32_t timeout_ms);

/** 发送 JSON 文本帧 */
esp_err_t ws_client_send_text(const char *text, uint32_t timeout_ms);

/** 发送统计（诊断心跳用）：失败次数 / 慢发送次数 / 最长单次发送耗时(us) */
void ws_client_get_send_stats(uint32_t *fail, uint32_t *slow, int64_t *max_us);

/** 事件：录音文件已保存 -> {"type":"event","event":"recording_saved",...} */
void ws_client_send_recording_saved(const char *rel_path, uint32_t size, uint32_t duration_ms);

/** 事件：录音开关变化 -> {"type":"event","event":"recording_state","enabled":..} */
void ws_client_send_recording_state(bool enabled);

#ifdef __cplusplus
}
#endif
