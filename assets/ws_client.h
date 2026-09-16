/**
 * ws_client.h —— WebSocket 客户端（连接服务端 remoteAudioServer）
 *
 * 职责：
 *  - 连接 ws://<HOST>:<PORT>/ws/device/<device_id>，指数退避重连（v2.2：300ms~3s）
 *  - v2.3：显式关闭组件自带自动重连（disable_auto_reconnect），重连只由本任务一条
 *    路径负责；建连前等 Wi-Fi 真拿到 IP；连续失败 6 次（约 1 分钟）销毁重建客户端
 *  - v2.4：重连后不凭本地记忆无条件续流，先等最多 3s 的服务端裁决（start_stream =
 *    有人监听 → 续流；stop_stream / 一直没消息 = 没人监听 → 不推），避免"对着空气推流"；
 *    另外用下行心跳里的 listeners 做兜底看门狗（连续 3 条 0 人 → 自动停流）
 *  - 连接后发 hello
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

/** v2.5.2：通知本模块「WiFi 链路已断」（由 wifi_mgr 的 STA_DISCONNECTED 回调调用）。
 * 只置标志、不阻塞；ws_mgr 任务下一轮会清连接状态、停客户端，等 WiFi 回来再重连。
 * 作用：修掉"WiFi 掉了但本地仍认为已连接 → 永不重连"的假在线卡死。 */
void ws_client_notify_link_down(void);

/** 发送 binary 帧（任意 type；调用方自带 7 字节帧头） */
esp_err_t ws_client_send_bin(const uint8_t *data, int len, uint32_t timeout_ms);

/** 发送 JSON 文本帧 */
esp_err_t ws_client_send_text(const char *text, uint32_t timeout_ms);

/** 发送统计（诊断心跳用）：失败次数 / 慢发送次数 / 最长单次发送耗时(us) */
void ws_client_get_send_stats(uint32_t *fail, uint32_t *slow, int64_t *max_us);

/** v2.3 链路诊断：重连次数 / 建连失败次数 / 销毁重建（硬复位）次数 */
void ws_client_get_link_stats(uint32_t *reconn, uint32_t *conn_fail, uint32_t *hard_reset);

/** v2.4：最近一次服务端心跳报告的监听端数量；-1 = 未知（服务端未带该字段） */
int ws_client_get_listeners(void);

/** 事件：录音文件已保存 -> {"type":"event","event":"recording_saved",...} */
void ws_client_send_recording_saved(const char *rel_path, uint32_t size, uint32_t duration_ms);

/** 事件：录音开关变化 -> {"type":"event","event":"recording_state","enabled":..} */
void ws_client_send_recording_state(bool enabled);

#ifdef __cplusplus
}
#endif
