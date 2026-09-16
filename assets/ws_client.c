/**
 * ws_client.c —— WebSocket 客户端实现（命令分发 / 文件回传 / 心跳 / 重连）
 *
 * v2.5.2：连接状态一致性加固（修「开关 WiFi 后设备一直连不上」）
 *  - wifi_mgr 掉线回调调用 ws_client_notify_link_down()，本模块清状态、停客户端、等 WiFi 回来重连
 *  - ws_client_is_connected() 交叉核对 esp_websocket_client_is_connected()，不再单信本地状态位
 *  - 连通状态下 ping 连续 2 次发不出去 → 判链路已死
 *  - 连通状态下 180s 收不到服务端任何数据 → 判链路已死（服务端每 20s 一条下行心跳）
 *  - WiFi 没拿到 IP 时只等、不计失败（不再把"断网"算成"建连失败"）
 */
#include "ws_client.h"
#include "board.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "sdkconfig.h"
#if CONFIG_RAN_SERVER_USE_TLS
#include "esp_crt_bundle.h"          /* wss 证书校验用 IDF 内置 CA bundle */
#endif

#include "device_config.h"
#include "wifi_mgr.h"                /* wifi_mgr_time_synced()：wss 需先对时；v2.3 用 wifi_mgr_is_connected() 做建连前 IP 门 */
#include "sd_storage.h"
#include "audio_stream.h"
#include "recorder.h"
#include "oled.h"

static const char *TAG = "ws";

#define BIT_CONNECTED   (1 << 0)
#define PING_INTERVAL_MS  15000      /* 应用层 JSON ping（协议要求，保留） */

/* v2.2：协议级 PING（RFC6455 opcode 0x9）参数。
 * 实测事实（2026-09-14，从外网连 wss://remoteaudio.coze.site/ws/device/<id>）：
 *  - 不开 WS 协议级 ping 时，连接在 288 / 300.4 / 302.5 / 302.8 秒被平台接入层掐断，
 *    且客户端连 close 帧都收不到；应用层 JSON ping（15s）和 50Hz 音频数据都不算"活着"。
 *  - 开启协议级 ping（10~20s）后，同一连接活了 425s 以上，越过 5 分钟不再断。
 * 所以必须让 WS 客户端自己发协议级 ping 帧（原来只发应用层 JSON ping，平台不认）。*/
#define WS_PING_INTERVAL_SEC        10
/* pong 超时：框架拿到 PING 后等 PONG，超时即判死链并断开重连。
 * 取 3×ping 间隔 = 30s：容忍连丢 2 个 pong 不误杀，真空死链最迟约 40s 内被发现。
 * 依据：esp_websocket_client 的 `pingpong_timeout_sec`（见 esp_websocket_client.h
 * "Period before connection is aborted due to no PONGs received"）；
 * 不显式设置时框架默认值是 120s —— 太慢，掉线现场要多等 2 分钟。 */
#define WS_PINGPONG_TIMEOUT_SEC     30

/* v2.4：重连后"等服务端裁决"的最长等待窗口。
 * 断线期间监听端可能已经全部离开，服务端当时下发的 stop_stream 又发不到我们（当时不在线）；
 * 只凭本地 s_want_stream 记忆就续流，会变成"对着空气推流"（647B/20ms ≈ 0.26 Mbps，一小时约 116MB）。
 * 所以重连并发 hello 后只等这么多毫秒：等到明确裁决才推，什么都等不到就不推（安全默认）。 */
#define WS_RESUME_ACK_TIMEOUT_MS    3000
/* v2.4：连续这么多次下行心跳都显示"没有监听端"就自动停流（心跳 20s 一次 → 60s 兜底）。 */
#define WS_HB_ZERO_STREAK_LIMIT     3

/* v2.2：重连提速（原为 起始 1000ms / 上限 30000ms）。
 * 目标：断线后 1 秒内自动重连并自动续流。300→600→1200→2400→3000 封顶。 */
#define RECONNECT_BACKOFF_MIN_MS    300
#define RECONNECT_BACKOFF_MAX_MS    3000

/* 取发送锁的最长等待：网络卡住时宁可丢这一帧，也绝不允许把调用任务
 * （audio_pipe / ws_mgr）无限挂死——真机上出现过"日志突然静止、服务端掉线"的现场。 */
#define SEND_LOCK_TIMEOUT_MS  500
/* 单次发送超过这个耗时就打警告：TLS 写被网络拖住的直接证据 */
#define SLOW_SEND_WARN_MS     300

static EventGroupHandle_t  s_evt;
static SemaphoreHandle_t   s_send_mtx;   /* 串行化所有 ws send（线程安全） */
static esp_websocket_client_handle_t s_client;

/* 文件回传任务控制（同一时间只回传一个文件） */
static TaskHandle_t        s_xfer_task;
static volatile uint32_t   s_xfer_req;  /* 当前回传请求 req_id，0=空闲；置新值即终止旧任务 */
static SemaphoreHandle_t   s_xfer_mtx;

/* "要不要实时流"的意图位（不是当前是否在推流）。
 * v2.4 起语义收敛为「服务端最近一次裁决」：收到 start_stream 置 true、stop_stream 置 false。
 * 重连后不再凭这个位无条件续流——先等 WS_RESUME_ACK_TIMEOUT_MS 内的服务端裁决，
 * 拿到 start_stream（或心跳里 listeners>0）才真的推流；一直没消息就按"没人监听"处理。
 * 这样即使断线期间监听端全走光、服务端的 stop_stream 丢了，也不会空推。 */
static volatile bool       s_want_stream;
/* v2.4：已发 hello，正在等服务端裁决（期间不推流） */
static volatile bool       s_await_ack;
static volatile TickType_t s_ack_deadline;
/* v2.4：最近一次下行心跳里的监听端数量（-1 = 未知，例如服务端还没带这个字段） */
static volatile int        s_last_listeners = -1;
static volatile uint32_t   s_hb_zero_streak;   /* 连续"0 人监听"的心跳次数 */

/* v2.3：链路诊断计数 + 硬复位保护 */
static volatile uint32_t   s_reconn_cnt;      /* 重连次数 */
static volatile uint32_t   s_conn_fail_cnt;   /* 建连失败次数 */
static volatile uint32_t   s_hard_reset_cnt;  /* 销毁重建客户端次数 */
static volatile bool       s_resetting;       /* 复位中：期间拒绝发送、对外视为未连接 */
static esp_websocket_client_config_t s_cfg;   /* v2.3：配置持久化（硬复位时重新 init 用） */

/* v2.5.2：「假在线」防护。
 * 现场（2026-09-16）：开/关 WiFi 之后设备一直连不上服务端，但 diag 一路显示 ws=1、
 * 重连/建连失败计数完全冻结、服务端设备列表里根本没有这台设备。原因是底层 socket 早就
 * 死了，而本模块的 BIT_CONNECTED 还留着 —— ws_mgr 以为"已连接"，于是永不重连；
 * send_text() 又因为组件状态为未连接而静默 return false（不计数），现场看不出任何异常。
 * 四处兜底：①wifi_mgr 掉线回调置 s_link_down_req，ws_mgr 据此清状态、停客户端；
 * ②ws_client_is_connected() 交叉核对组件真实状态；③连通时 ping 连续发不出去判死链；
 * ④长时间收不到服务端任何数据（服务端 20s 一条下行心跳）判死链。 */
static volatile bool     s_link_down_req;      /* WiFi 链路已断，待 ws_mgr 处理 */
static volatile int64_t  s_last_rx_ms;         /* 最后一次收到服务端数据的时刻（ms） */
static volatile uint32_t s_ping_fail_streak;   /* 连续发不出去的应用层 ping 次数 */

#define PING_FAIL_STREAK_LIMIT  2              /* 连续 2 次 ping 发不出去 → 判链路已死 */
#define LINK_RX_SILENCE_MS      180000         /* 180s 收不到任何下行数据 → 判链路已死 */

/* ------------------------------------------------------------------ */
/* 基础发送                                                            */
/* ------------------------------------------------------------------ */
static volatile uint32_t s_send_fail;      /* 取锁超时 + 发送失败 */
static volatile uint32_t s_slow_send;      /* 单次发送耗时 > SLOW_SEND_WARN_MS */
static volatile int64_t  s_max_send_us;

void ws_client_get_send_stats(uint32_t *fail, uint32_t *slow, int64_t *max_us)
{
    if (fail)   *fail   = s_send_fail;
    if (slow)   *slow   = s_slow_send;
    if (max_us) *max_us = s_max_send_us;
}

void ws_client_get_link_stats(uint32_t *reconn, uint32_t *conn_fail, uint32_t *hard_reset)
{
    if (reconn)     *reconn     = s_reconn_cnt;
    if (conn_fail)  *conn_fail  = s_conn_fail_cnt;
    if (hard_reset) *hard_reset = s_hard_reset_cnt;
}

/* v2.4：最近一次服务端心跳里报告的监听端数量；-1 = 未知（服务端还没带这个字段） */
int ws_client_get_listeners(void)
{
    return s_last_listeners;
}

static void send_note(int64_t dt_us, bool ok)
{
    if (dt_us > s_max_send_us) s_max_send_us = dt_us;
    if (dt_us / 1000 > SLOW_SEND_WARN_MS) {
        s_slow_send++;
        if (s_slow_send <= 20 || (s_slow_send % 20) == 0) {
            ESP_LOGW(TAG, "慢发送: 单次耗时 %lld ms（累计 %u 次）",
                     (long long)(dt_us / 1000), (unsigned)s_slow_send);
        }
    }
    if (!ok) s_send_fail++;
}

static bool send_text(const char *text)
{
    if (!s_client || s_resetting) return false;
    if (!esp_websocket_client_is_connected(s_client)) return false;
    if (xSemaphoreTake(s_send_mtx, pdMS_TO_TICKS(SEND_LOCK_TIMEOUT_MS)) != pdTRUE) {
        send_note(0, false);
        ESP_LOGW(TAG, "取发送锁超时(%d ms)，丢弃文本帧", SEND_LOCK_TIMEOUT_MS);
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    int n = esp_websocket_client_send_text(s_client, text, (int)strlen(text),
                                           pdMS_TO_TICKS(3000));
    int64_t dt = esp_timer_get_time() - t0;
    xSemaphoreGive(s_send_mtx);
    send_note(dt, n >= 0);
    if (n < 0) {
        if (s_send_fail <= 10 || (s_send_fail % 100) == 0) {
            ESP_LOGW(TAG, "send_text 失败: %d（累计 %u 次）", n, (unsigned)s_send_fail);
        }
        return false;
    }
    return true;
}

esp_err_t ws_client_send_text(const char *text, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return send_text(text) ? ESP_OK : ESP_FAIL;
}

esp_err_t ws_client_send_bin(const uint8_t *data, int len, uint32_t timeout_ms)
{
    if (!s_client || s_resetting) return ESP_FAIL;
    if (!esp_websocket_client_is_connected(s_client)) return ESP_FAIL;
    if (xSemaphoreTake(s_send_mtx, pdMS_TO_TICKS(SEND_LOCK_TIMEOUT_MS)) != pdTRUE) {
        send_note(0, false);      /* 丢帧由调用方计数（audio_stream 的丢帧计数） */
        return ESP_FAIL;
    }
    int64_t t0 = esp_timer_get_time();
    int n = esp_websocket_client_send_bin(s_client, (const char *)data, len, pdMS_TO_TICKS(timeout_ms));
    int64_t dt = esp_timer_get_time() - t0;
    xSemaphoreGive(s_send_mtx);
    send_note(dt, n == len);
    return (n == len) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* 事件上报（各 response 帧在对应 cmd 处理函数内直接构造）             */
/* ------------------------------------------------------------------ */

/* 事件上报 */
void ws_client_send_recording_saved(const char *rel_path, uint32_t size, uint32_t duration_ms)
{
    if (!ws_client_is_connected()) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", "recording_saved");
    cJSON_AddStringToObject(root, "file", rel_path);
    cJSON_AddNumberToObject(root, "size", (double)size);
    cJSON_AddNumberToObject(root, "duration_ms", (double)duration_ms);
    cJSON_AddNumberToObject(root, "duration", (double)(duration_ms / 1000));
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) { ESP_LOGI(TAG, "上报 recording_saved: %s", s); send_text(s); free(s); }
}

void ws_client_send_recording_state(bool enabled)
{
    if (!ws_client_is_connected()) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", "recording_state");
    cJSON_AddBoolToObject(root, "enabled", enabled);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) { send_text(s); free(s); }
}

static void send_hello(void)
{
    const ran_config_t *cfg = device_config_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", cfg->device_id);
    cJSON_AddStringToObject(root, "fw", CONFIG_RAN_FW_VERSION);
    cJSON_AddBoolToObject(root, "recording_enabled", cfg->recording_enabled);
    cJSON_AddBoolToObject(root, "sd_ok", sd_storage_ready());
    cJSON *jc = cJSON_AddObjectToObject(root, "config");
    cJSON_AddNumberToObject(jc, "vad_threshold", (double)cfg->vad_threshold);
    cJSON_AddNumberToObject(jc, "retention_days", (double)cfg->retention_days);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) { ESP_LOGI(TAG, "hello -> %s", s); send_text(s); free(s); }
}

/* ------------------------------------------------------------------ */
/* 命令处理                                                            */
/* ------------------------------------------------------------------ */
static void cmd_list_recordings(int64_t req_id, const cJSON *root)
{
    cJSON *jdate = cJSON_GetObjectItem(root, "date");
    const char *date_filter = (cJSON_IsString(jdate)) ? jdate->valuestring : NULL;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddNumberToObject(resp, "req_id", (double)req_id);

    if (!sd_storage_ready()) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "sd_card_not_ready");
        char *s = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (s) { send_text(s); free(s); }
        return;
    }

    sd_rec_file_t *files = calloc(200, sizeof(sd_rec_file_t));
    if (!files) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "no_mem");
        char *s = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (s) { send_text(s); free(s); }
        return;
    }
    int n = sd_storage_list_recordings(files, 200, date_filter);
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON *jarr = cJSON_AddArrayToObject(resp, "files");
    for (int i = 0; i < n; i++) {
        cJSON *jf = cJSON_CreateObject();
        cJSON_AddStringToObject(jf, "file", files[i].rel_path);
        cJSON_AddStringToObject(jf, "name", files[i].rel_path);
        cJSON_AddStringToObject(jf, "date", files[i].date);
        cJSON_AddNumberToObject(jf, "size", (double)files[i].size);
        cJSON_AddNumberToObject(jf, "mtime", (double)files[i].mtime);
        cJSON_AddNumberToObject(jf, "duration", (double)files[i].duration);
        cJSON_AddItemToArray(jarr, jf);
    }
    free(files);

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (s) { ESP_LOGI(TAG, "list_recordings 响应 %d 个文件", n); send_text(s); free(s); }
}

/* 文件回传任务：分块 2048B，帧头 0x02 + req_id(BE) + seq(BE, 末帧 0xFFFF)
 * 同一时间仅允许一个回传任务；新 play_file 会通过 s_xfer_req 变化终止旧任务 */
typedef struct {
    uint32_t req_id;
    char     abs_path[256];
} xfer_arg_t;

static void xfer_task2(void *arg)
{
    xfer_arg_t *xa = (xfer_arg_t *)arg;
    uint32_t my_req = xa->req_id;
    char path[256];
    snprintf(path, sizeof(path), "%s", xa->abs_path);
    free(xa);

    ESP_LOGI(TAG, "文件回传开始 req=%lu file=%s", (unsigned long)my_req, path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "response");
        cJSON_AddNumberToObject(r, "req_id", (double)my_req);
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "file_not_found");
        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (s) { send_text(s); free(s); }
        xSemaphoreTake(s_xfer_mtx, portMAX_DELAY);
        if (s_xfer_req == my_req) s_xfer_req = 0;
        xSemaphoreGive(s_xfer_mtx);
        vTaskDelete(NULL);
    }

    int hdr = WS_FRAME_HEADER_LEN;
    int cap = hdr + AUDIO_FILE_CHUNK;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap);

    bool aborted = false;
    bool sent_final = false;
    uint16_t seq = 0;
    if (buf) {
        for (;;) {
            xSemaphoreTake(s_xfer_mtx, portMAX_DELAY);
            bool still_mine = (s_xfer_req == my_req);
            xSemaphoreGive(s_xfer_mtx);
            if (!still_mine) { aborted = true; break; }

            sd_storage_lock();
            size_t rd = fread(buf + hdr, 1, AUDIO_FILE_CHUNK, f);
            sd_storage_unlock();

            if (rd == 0) {
                /* 读到末尾：发末帧 seq=0xFFFF，payload 为空 */
                buf[0] = WS_FRAME_TYPE_FILE_BIN;
                buf[1] = (my_req >> 24) & 0xFF;
                buf[2] = (my_req >> 16) & 0xFF;
                buf[3] = (my_req >> 8) & 0xFF;
                buf[4] = my_req & 0xFF;
                buf[5] = 0xFF; buf[6] = 0xFF;
                if (ws_client_send_bin(buf, hdr, 2000) == ESP_OK) sent_final = true;
                break;
            }

            buf[0] = WS_FRAME_TYPE_FILE_BIN;
            buf[1] = (my_req >> 24) & 0xFF;
            buf[2] = (my_req >> 16) & 0xFF;
            buf[3] = (my_req >> 8) & 0xFF;
            buf[4] = my_req & 0xFF;
            buf[5] = (seq >> 8) & 0xFF;
            buf[6] = seq & 0xFF;

            if (ws_client_send_bin(buf, hdr + (int)rd, 2000) != ESP_OK) {
                ESP_LOGW(TAG, "文件块发送失败 seq=%u，中止", seq);
                aborted = true;
                break;
            }
            seq++;
        }
        free(buf);
    } else {
        ESP_LOGE(TAG, "回传缓冲分配失败");
        aborted = true;
    }

    fclose(f);

    xSemaphoreTake(s_xfer_mtx, portMAX_DELAY);
    bool was_active = (s_xfer_req == my_req);
    if (was_active) s_xfer_req = 0;
    xSemaphoreGive(s_xfer_mtx);

    if (was_active && !aborted && sent_final) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "response");
        cJSON_AddNumberToObject(r, "req_id", (double)my_req);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "done", true);
        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (s) { ESP_LOGI(TAG, "play_file 完成 req=%lu", (unsigned long)my_req); send_text(s); free(s); }
    } else {
        ESP_LOGW(TAG, "play_file 中止 req=%lu (aborted=%d final=%d)",
                 (unsigned long)my_req, aborted, sent_final);
    }
    vTaskDelete(NULL);
}

static void cmd_play_file(int64_t req_id, const cJSON *root)
{
    cJSON *jf = cJSON_GetObjectItem(root, "file");
    if (!cJSON_IsString(jf)) return;

    char abs_path[256];
    if (!sd_storage_safe_abs(jf->valuestring, abs_path, sizeof(abs_path))) {
        return;
    }
    sd_storage_lock();
    bool exists = (access(abs_path, F_OK) == 0);
    sd_storage_unlock();
    if (!exists) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "response");
        cJSON_AddNumberToObject(r, "req_id", (double)req_id);
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "file_not_found");
        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (s) { send_text(s); free(s); }
        return;
    }

    /* 终止旧回传任务，启动新任务（旧任务发现 s_xfer_req 变化自行退出） */
    xSemaphoreTake(s_xfer_mtx, portMAX_DELAY);
    s_xfer_req = (uint32_t)req_id;
    xSemaphoreGive(s_xfer_mtx);

    xfer_arg_t *xa = malloc(sizeof(xfer_arg_t));
    if (!xa) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "response");
        cJSON_AddNumberToObject(r, "req_id", (double)req_id);
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "no_mem");
        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (s) { send_text(s); free(s); }
        return;
    }
    xa->req_id = (uint32_t)req_id;
    snprintf(xa->abs_path, sizeof(xa->abs_path), "%s", abs_path);

    BaseType_t ok = xTaskCreatePinnedToCore(xfer_task2, "file_xfer", 6144, xa,
                                            8, &s_xfer_task, 1);
    if (ok != pdPASS) {
        free(xa);
        ESP_LOGE(TAG, "回传任务创建失败");
    }
}

static void cmd_stop_file(int64_t req_id)
{
    xSemaphoreTake(s_xfer_mtx, portMAX_DELAY);
    s_xfer_req = 0;   /* 旧任务自行退出（不回 done） */
    xSemaphoreGive(s_xfer_mtx);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "response");
    cJSON_AddNumberToObject(r, "req_id", (double)req_id);
    cJSON_AddBoolToObject(r, "ok", true);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { send_text(s); free(s); }
}

static void cmd_set_recording(int64_t req_id, cJSON *root)
{
    cJSON *je = cJSON_GetObjectItem(root, "enabled");
    bool enabled = cJSON_IsTrue(je);
    recorder_set_enabled(enabled);
    /* recorder_set_enabled 内部已持久化并上报 recording_state 事件；此处回当前状态 */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "response");
    if (req_id >= 0) cJSON_AddNumberToObject(r, "req_id", (double)req_id);
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "recording_enabled", enabled);
    cJSON_AddBoolToObject(r, "enabled", enabled);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { send_text(s); free(s); }
}

static void cmd_set_config(int64_t req_id, cJSON *root)
{
    cJSON *jc = cJSON_GetObjectItem(root, "config");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    if (req_id >= 0) cJSON_AddNumberToObject(resp, "req_id", (double)req_id);
    bool ok = true;

    if (cJSON_IsObject(jc)) {
        cJSON *jv = cJSON_GetObjectItem(jc, "vad_threshold");
        if (cJSON_IsNumber(jv) && jv->valuedouble > 0) {
            device_config_set_vad_threshold((uint32_t)jv->valuedouble);
        }
        cJSON *jr = cJSON_GetObjectItem(jc, "retention_days");
        if (cJSON_IsNumber(jr) && jr->valuedouble >= 0) {
            device_config_set_retention_days((uint32_t)jr->valuedouble);
        }
    } else {
        ok = false;
    }
    cJSON_AddBoolToObject(resp, "ok", ok);
    const ran_config_t *cfg = device_config_get();
    cJSON *jnow = cJSON_AddObjectToObject(resp, "config");
    cJSON_AddNumberToObject(jnow, "vad_threshold", (double)cfg->vad_threshold);
    cJSON_AddNumberToObject(jnow, "retention_days", (double)cfg->retention_days);
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (s) { send_text(s); free(s); }
}

/* v2.4：推流状态的唯一写入口 —— "意图位"永远等于服务端最近一次裁决 */
static void stream_apply(bool on, const char *why)
{
    s_want_stream    = on;
    s_await_ack      = false;
    s_hb_zero_streak = 0;
    audio_stream_set_active(on);
    if (why) ESP_LOGI(TAG, "%s", why);
}

static void dispatch_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "无法解析 JSON: %.*s", len > 128 ? 128 : len, data);
        return;
    }

    cJSON *jcmd  = cJSON_GetObjectItem(root, "cmd");
    cJSON *jtype = cJSON_GetObjectItem(root, "type");
    cJSON *jreq  = cJSON_GetObjectItem(root, "req_id");
    int64_t req_id = cJSON_IsNumber(jreq) ? (int64_t)jreq->valuedouble : -1;

    const char *cmd = cJSON_IsString(jcmd) ? jcmd->valuestring : NULL;
    if (!cmd && cJSON_IsString(jtype) && strcmp(jtype->valuestring, "request") == 0) {
        /* {type:"request", cmd:..} 形式 cmd 字段仍应存在，兜底： */
        cmd = NULL;
    }

    /* v2.4：下行心跳里带监听端数量（服务端 20s 一条）—— 兜底"空推流"看门狗。
     * 断线期间监听端全走光、服务端那条 stop_stream 又丢了的场景，最迟 3 条心跳（60s）收住。 */
    if (!cmd && cJSON_IsString(jtype) && strcmp(jtype->valuestring, "hb") == 0) {
        cJSON *jl = cJSON_GetObjectItem(root, "listeners");
        if (cJSON_IsNumber(jl)) {
            int n = jl->valueint;
            s_last_listeners = n;
            if (n > 0) {
                s_hb_zero_streak = 0;
                if (s_await_ack) stream_apply(true, "心跳显示有人在监听，恢复实时推流");
            } else if (s_want_stream || audio_stream_active()) {
                s_hb_zero_streak++;
                if (s_hb_zero_streak >= WS_HB_ZERO_STREAK_LIMIT) {
                    ESP_LOGW(TAG, "连续 %u 次心跳都显示没有监听端，停掉实时推流（避免空推）",
                             (unsigned)s_hb_zero_streak);
                    stream_apply(false, NULL);
                }
            } else {
                s_hb_zero_streak = 0;
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (cmd) {
        if (strcmp(cmd, "start_stream") == 0) {
            /* v2.4：服务端裁决"有人在听"——重连后的续流确认，或监听端刚连上 */
            stream_apply(true, s_await_ack ? "服务端确认有人监听，恢复实时推流"
                                           : "服务端要求开始实时推流");
        } else if (strcmp(cmd, "stop_stream") == 0) {
            /* v2.4：服务端裁决"没人听"（含设备重连后 hello 复盘下发的 stop_stream）*/
            stream_apply(false, "服务端要求停流（当前无人监听），停止实时推流");
        } else if (strcmp(cmd, "set_recording") == 0) {
            cmd_set_recording(req_id, root);
        } else if (strcmp(cmd, "set_config") == 0) {
            cmd_set_config(req_id, root);
        } else if (strcmp(cmd, "list_recordings") == 0) {
            cmd_list_recordings(req_id, root);
        } else if (strcmp(cmd, "play_file") == 0) {
            cmd_play_file(req_id, root);
        } else if (strcmp(cmd, "stop_file") == 0) {
            cmd_stop_file(req_id);
        } else {
            ESP_LOGW(TAG, "未知命令: %s", cmd);
        }
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* WS 事件回调                                                         */
/* ------------------------------------------------------------------ */
static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接");
        xEventGroupSetBits(s_evt, BIT_CONNECTED);
        s_last_rx_ms = esp_timer_get_time() / 1000;   /* v2.5.2：静默看门狗起算点 */
        s_ping_fail_streak = 0;
        send_hello();
        /* v2.4：不再凭本地记忆无条件续流。断线期间监听端可能已经全部离开，服务端那时
         * 下发的 stop_stream 也发不到我们（当时不在线）；先等 WS_RESUME_ACK_TIMEOUT_MS，
         * 收到 start_stream（或心跳里 listeners>0）才真推流，什么都没等到就不推。 */
        s_hb_zero_streak = 0;
        if (s_want_stream) {
            s_await_ack    = true;
            s_ack_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WS_RESUME_ACK_TIMEOUT_MS);
            ESP_LOGI(TAG, "断线前在推流：等 %d ms 看服务端是否还有人监听，再决定要不要续流",
                     WS_RESUME_ACK_TIMEOUT_MS);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        xEventGroupClearBits(s_evt, BIT_CONNECTED);
        audio_stream_set_active(false);
        s_await_ack      = false;      /* v2.4：重连后重新等一次服务端裁决 */
        s_hb_zero_streak = 0;
        s_last_listeners = -1;
        break;
    case WEBSOCKET_EVENT_DATA:
        s_last_rx_ms = esp_timer_get_time() / 1000;   /* v2.5.2：任一帧下行都算链路活跃 */
        if (e->op_code == 1 /* text */) {
            dispatch_text(e->data_ptr, e->data_len);
        } else if (e->op_code == 2 /* binary */) {
            /* 服务端不主动发 binary，忽略 */
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket 错误: type=%d tls_err=0x%x tls_stack=%d sock_errno=%d",
                 (int)e->error_handle.error_type,
                 (unsigned)e->error_handle.esp_tls_last_esp_err,
                 (int)e->error_handle.esp_tls_stack_err,
                 (int)e->error_handle.esp_transport_sock_errno);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* v2.3：客户端生命周期辅助（建连失败自愈）                             */
/* ------------------------------------------------------------------ */

/* 连续失败到这个次数（约 1 分钟）就销毁并重建客户端：把组件内部残留状态
 * （状态位、没退出的内部任务、半开 socket）一次性清干净，避免"再也连不上"。 */
#define WS_HARD_RESET_AFTER_FAILS  6

static esp_err_t ws_client_client_init(void)
{
    s_client = esp_websocket_client_init(&s_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "WS 客户端初始化失败");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    return ESP_OK;
}

static void ws_hard_reset(void)
{
    s_resetting = true;
    xEventGroupClearBits(s_evt, BIT_CONNECTED);
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_resetting = false;
    if (ws_client_client_init() == ESP_OK) {
        s_hard_reset_cnt++;
        ESP_LOGW(TAG, "已销毁并重建 WS 客户端（第 %u 次硬复位），用干净状态重新建连",
                 (unsigned)s_hard_reset_cnt);
    }
}

/* 一次建连/重连失败的统一收尾：计数 -> 到阈值就硬复位 -> 指数退避后重试 */
static void ws_on_failure(uint32_t *fail_streak, uint32_t *backoff_ms, const char *why)
{
    s_conn_fail_cnt++;
    (*fail_streak)++;
    s_reconn_cnt++;
    ESP_LOGW(TAG, "%s（累计建连失败 %u 次，重连第 %u 次），退避 %lu ms 后重试",
             why, (unsigned)s_conn_fail_cnt, (unsigned)s_reconn_cnt, (unsigned long)*backoff_ms);
    if (s_client) esp_websocket_client_stop(s_client);
    if (*fail_streak >= WS_HARD_RESET_AFTER_FAILS) {
        *fail_streak = 0;
        ws_hard_reset();
    } else {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(*backoff_ms));
    *backoff_ms = *backoff_ms * 2;
    if (*backoff_ms > RECONNECT_BACKOFF_MAX_MS) *backoff_ms = RECONNECT_BACKOFF_MAX_MS;
}

/* ------------------------------------------------------------------ */
/* 管理任务：建连 / 心跳 / 指数退避重连                                 */
/* ------------------------------------------------------------------ */
static void ws_task(void *arg)
{
    (void)arg;
    /* 默认指向扣子云上部署的 remoteAudioServer（wss over TLS，443）。
     * 想连局域网里自己跑的 uvicorn 就把 menuconfig 里 RAN_SERVER_HOST 改成电脑 IP、
     * PORT 改成 8000，并把 RAN_SERVER_USE_TLS 关掉（走明文 ws://）。 */
    static char url[160];        /* 组件长期持有 uri 指针，必须是静态生命周期 */
#if CONFIG_RAN_SERVER_USE_TLS
    const char *scheme = "wss";
#else
    const char *scheme = "ws";
#endif
    snprintf(url, sizeof(url), "%s://%s:%d/ws/device/%s",
             scheme, CONFIG_RAN_SERVER_HOST, CONFIG_RAN_SERVER_PORT,
             device_config_get()->device_id);

    s_cfg.uri = url;
    /* v2.3：重连只保留一条路径（本任务）。
     * 必须显式关掉组件自带的自动重连：reconnect_timeout_ms 在 esp_websocket_client
     * 1.x 各版本里的语义都是「自动重连等待时间」，传 0 只是回落默认 10s，并不等于禁用
     * （组件源码：if (!disable_auto_reconnect && reconnect_timeout_ms <= 0)
     *            wait_timeout_ms = 10000;  auto_reconnect 仍为 true）。
     * 所以 v2.1/v2.2 实际是「组件自己每 10s 重连」+「ws_task 停掉再 start」两套逻辑
     * 抢同一个客户端，现场日志里的 `Reconnect after 10000 ms` 就是组件打的。关掉它，
     * 重连节奏、退避、hello 重发才完全由我们控制。 */
    s_cfg.disable_auto_reconnect = true;
    s_cfg.reconnect_timeout_ms = 0;
    s_cfg.network_timeout_ms = 8000;
    s_cfg.task_stack = 8192;             /* wss 的 TLS 握手吃栈，6KB 偏紧 */
    s_cfg.buffer_size = 4096;
    /* v2.2 起：开启 WS 协议级心跳。应用层 JSON ping 平台不认，只有协议级 PING 帧
     * 才能让平台接入层不把这条长连接当空闲连接掐掉（实测 300s → >425s）。*/
    s_cfg.ping_interval_sec = WS_PING_INTERVAL_SEC;
    s_cfg.pingpong_timeout_sec = WS_PINGPONG_TIMEOUT_SEC;
#if CONFIG_RAN_SERVER_USE_TLS
    s_cfg.crt_bundle_attach = esp_crt_bundle_attach;   /* 用内置 CA bundle 校验服务端证书 */
#endif
    if (ws_client_client_init() != ESP_OK) {
        ESP_LOGE(TAG, "WS 客户端创建失败，管理任务退出");
        vTaskDelete(NULL);
    }

    uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;
    uint32_t fail_streak = 0;             /* v2.3：连续建连失败次数（触发硬复位） */
    TickType_t last_ping = 0;

    for (;;) {
        /* v2.5.2：WiFi 链路已断 —— wifi_mgr 的 STA_DISCONNECTED 回调置了这个标志。
         * 立刻清本地连接状态、停客户端，然后安静等 WiFi 回来再重连；
         * 不计失败、不进硬复位（WiFi 断了不是"建连失败"，计数只会污染现场日志）。 */
        if (s_link_down_req) {
            s_link_down_req = false;
            if (xEventGroupGetBits(s_evt) & BIT_CONNECTED) {
                ESP_LOGW(TAG, "WiFi 链路断开 → 清本地连接状态并停客户端，等 WiFi 回来再重连");
            }
            xEventGroupClearBits(s_evt, BIT_CONNECTED);
            s_await_ack = false;
            s_ping_fail_streak = 0;
            audio_stream_set_active(false);
            if (s_client) esp_websocket_client_stop(s_client);
            backoff_ms = RECONNECT_BACKOFF_MIN_MS;
            fail_streak = 0;
        }

        if (!ws_client_is_connected()) {
#if CONFIG_RAN_SERVER_USE_TLS
            /* TLS 证书有生效/过期时间，设备刚上电时系统时间是 1970 年，校验必然失败。
             * 所以 wss 之前先等 SNTP 对时（最多 10s）；等不到也照连一次，靠重连兜底。 */
            if (!wifi_mgr_time_synced()) {
                ESP_LOGI(TAG, "等待 SNTP 时间同步（wss 证书校验需要）...");
                for (int i = 0; i < 20 && !wifi_mgr_time_synced(); i++) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                if (!wifi_mgr_time_synced()) {
                    ESP_LOGW(TAG, "SNTP 仍未同步，先尝试连接（可能因证书时间校验失败而重试）");
                }
            }
#endif
            /* v2.3：建连前还必须确认 Wi-Fi 真拿到 IP。v2.2 只有 SNTP 门，SNTP 没同步时
             * 会直接在「掉线窗口」里硬连：TCP 层能试、DNS 解析不了，现场日志里的
             * getaddrinfo() returns 202 / EAI_FAIL 就是这么来的。 */
            if (!wifi_mgr_is_connected()) {
                ESP_LOGI(TAG, "Wi-Fi 未就绪（还没拿到 IP），等它起来再建连 ...");
                for (int i = 0; i < 30 && !wifi_mgr_is_connected() && !s_link_down_req; i++) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                if (!wifi_mgr_is_connected()) {
                    /* v2.5.2：WiFi 没 IP 只是"等"，不是"建连失败"——不计失败、不硬复位。
                     * 旧逻辑在这里计一次失败，长时间断网会把"重连/建连失败/硬复位"刷到几百次
                     * （现场就是 203/203/32），既污染判读，也让客户端被无意义地反复销毁重建。 */
                    continue;
                }
            }
            /* v2.5.2：本地状态位与组件真实状态不一致（假在线）时先修正，否则 start()
             * 会被组件以"The client has started"拒绝，白等一轮退避。 */
            if (xEventGroupGetBits(s_evt) & BIT_CONNECTED) {
                ESP_LOGW(TAG, "本地连接状态与底层不一致（假在线）→ 清状态并停客户端后重连");
                xEventGroupClearBits(s_evt, BIT_CONNECTED);
                s_await_ack = false;
                audio_stream_set_active(false);
                if (s_client) esp_websocket_client_stop(s_client);
            }

            ESP_LOGI(TAG, "连接 %s ...", url);
            esp_err_t serr = esp_websocket_client_start(s_client);
            if (serr != ESP_OK) {
                /* 组件在 state>=INIT 时会直接拒绝 start（"The client has started"）。
                 * 以前忽略返回值 → 这句错误被吞掉、客户端状态又没被清掉，会一直连不上。
                 * 现在：记一次失败、必要时硬复位、退避重来。 */
                char why[96];
                snprintf(why, sizeof(why), "启动 WS 客户端失败: %s", esp_err_to_name(serr));
                ws_on_failure(&fail_streak, &backoff_ms, why);
                continue;
            }

            /* 等待连接成功（最多 10s） */
            EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED,
                                                   pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(10000));
            if (bits & BIT_CONNECTED) {
                backoff_ms = RECONNECT_BACKOFF_MIN_MS;
                fail_streak = 0;
                last_ping = xTaskGetTickCount();
            } else {
                /* 连接失败/超时：停掉内部任务，退避后下一轮重新 start */
                ws_on_failure(&fail_streak, &backoff_ms, "连接超时/失败");
                continue;
            }
        }

        /* 已连接：维持心跳，每 15s 发送应用层 ping */
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ws_client_is_connected()) {
            /* v2.4：重连后的推流裁决超时 —— 窗口内服务端什么都没说，就当"没人监听" */
            if (s_await_ack && (int32_t)(xTaskGetTickCount() - s_ack_deadline) >= 0) {
                if (s_want_stream) {
                    ESP_LOGW(TAG, "重连后 %d ms 内没收到服务端的推流裁决，按「没人监听」处理，暂不推流"
                                  "（服务端随时可用 start_stream 唤醒）", WS_RESUME_ACK_TIMEOUT_MS);
                    stream_apply(false, NULL);
                } else {
                    s_await_ack = false;
                }
            }
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ping) * portTICK_PERIOD_MS >= PING_INTERVAL_MS) {
                last_ping = now;
                char ping[64];
                snprintf(ping, sizeof(ping),
                         "{\"type\":\"ping\",\"ts\":%lld}",
                         (long long)esp_timer_get_time() / 1000);
                if (send_text(ping)) {
                    s_ping_fail_streak = 0;
                } else {
                    /* v2.5.2：连通状态下 ping 发不出去 = 这条链路已经死了（现场就是
                     * "本地还显示已连接、服务端早就看不到设备"）。连续 2 次即清状态重连。 */
                    s_ping_fail_streak++;
                    ESP_LOGW(TAG, "应用层 ping 发不出去（连续 %u 次）", (unsigned)s_ping_fail_streak);
                    if (s_ping_fail_streak >= PING_FAIL_STREAK_LIMIT) {
                        ESP_LOGW(TAG, "连续 %u 次 ping 发不出去 → 判定链路已死，清状态准备重连",
                                 (unsigned)s_ping_fail_streak);
                        s_ping_fail_streak = 0;
                        xEventGroupClearBits(s_evt, BIT_CONNECTED);
                        s_await_ack = false;
                        audio_stream_set_active(false);
                    }
                }
            }

            /* v2.5.2：静默看门狗 —— 服务端每 20s 一条下行心跳，LINK_RX_SILENCE_MS 内一帧都
             * 收不到，说明这条链路"发得出去、对方收不到"，必须重建连接。 */
            if ((esp_timer_get_time() / 1000 - s_last_rx_ms) > LINK_RX_SILENCE_MS) {
                ESP_LOGW(TAG, "已超过 %d ms 没收到服务端任何数据（下行心跳 20s 一条）→ 判定链路已死，重建连接",
                         LINK_RX_SILENCE_MS);
                s_last_rx_ms = esp_timer_get_time() / 1000;   /* 先复位，避免下一轮立刻二次触发 */
                xEventGroupClearBits(s_evt, BIT_CONNECTED);
                s_await_ack = false;
                audio_stream_set_active(false);
            }
        } else {
            /* 连接中途断开：退避后重连（下一轮 start）。
             * v2.2：退避 300ms 起、3s 封顶；重连成功后由 CONNECTED 事件自动续流。
             * v2.5.2：顺手清掉可能残留的"假在线"状态位，否则下一轮 start() 会被组件拒绝。 */
            if (xEventGroupGetBits(s_evt) & BIT_CONNECTED) {
                ESP_LOGW(TAG, "连接已中断（本地状态位残留）→ 清状态后重连");
                xEventGroupClearBits(s_evt, BIT_CONNECTED);
                audio_stream_set_active(false);
            }
            s_await_ack = false;
            s_ping_fail_streak = 0;
            ws_on_failure(&fail_streak, &backoff_ms, "连接断开");
        }
    }
}

esp_err_t ws_client_start(void)
{
    s_evt = xEventGroupCreate();
    s_send_mtx = xSemaphoreCreateMutex();
    s_xfer_mtx = xSemaphoreCreateMutex();
    if (!s_evt || !s_send_mtx || !s_xfer_mtx) return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(ws_task, "ws_mgr", 6144, NULL, 10, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "WS 任务创建失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ws_client_is_connected(void)
{
    if (!s_evt || s_resetting) return false;   /* 复位中视为未连接，调用方不会往空客户端写 */
    if (!(xEventGroupGetBits(s_evt) & BIT_CONNECTED)) return false;
    /* v2.5.2：本地状态位可能是"假在线"（WiFi 掉了、组件没上报断开）。
     * 必须交叉核对组件真实状态，否则上层会一直以为在线、ws_mgr 也永不重连。 */
    if (s_client && !esp_websocket_client_is_connected(s_client)) return false;
    return true;
}

void ws_client_notify_link_down(void)
{
    /* v2.5.2：由 wifi_mgr 的 STA_DISCONNECTED 回调调用。
     * 这里只置标志（回调跑在系统事件循环上，绝不能阻塞），真正清状态/停客户端的动作
     * 交给 ws_mgr 任务下一轮（≤1s）处理。 */
    s_link_down_req = true;
}
