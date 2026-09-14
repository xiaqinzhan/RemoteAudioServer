/**
 * ws_client.c —— WebSocket 客户端实现（命令分发 / 文件回传 / 心跳 / 重连）
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
#include "wifi_mgr.h"                /* wifi_mgr_time_synced()：wss 需先对时 */
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

/* v2.2：用户/服务端"要不要实时流"的意图（不是当前是否在推流）。
 * 收到 start_stream 置 true、stop_stream 置 false，与连接状态无关地一直保留。
 * 断线时必须停推流（断开期间不发），但意图保留；重连成功并重发 hello 后，
 * 若意图仍为 true 就自动恢复推流，用户不必重新点"开始监听"。 */
static volatile bool       s_want_stream;

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

    if (cmd) {
        if (strcmp(cmd, "start_stream") == 0) {
            s_want_stream = true;               /* v2.2：记住意图，供重连后续流 */
            audio_stream_set_active(true);
        } else if (strcmp(cmd, "stop_stream") == 0) {
            s_want_stream = false;              /* v2.2：用户明确停流，重连后不再自动续 */
            audio_stream_set_active(false);
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
        send_hello();
        /* v2.2：重连后自动续流。DISCONNECTED 分支在断开时把推流关掉（断开期间不发），
         * 这里若"要流"的意图仍成立，就在 hello 之后自动恢复，用户不用重新点。
         * 幂等：audio_stream_set_active(true) 只是置一个状态位（重复调用无副作用）；
         * 服务端在设备上线时若紧接着再下发 start_stream，也只是再置一次同一个位，
         * 不会重复开启、不冲突。 */
        if (s_want_stream) {
            ESP_LOGI(TAG, "重连成功：断线前的推流意图仍为开，自动恢复实时推流");
            audio_stream_set_active(true);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        xEventGroupClearBits(s_evt, BIT_CONNECTED);
        audio_stream_set_active(false);
        break;
    case WEBSOCKET_EVENT_DATA:
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
/* 管理任务：建连 / 心跳 / 指数退避重连                                 */
/* ------------------------------------------------------------------ */
static void ws_task(void *arg)
{
    (void)arg;
    /* 默认指向扣子云上部署的 remoteAudioServer（wss over TLS，443）。
     * 想连局域网里自己跑的 uvicorn 就把 menuconfig 里 RAN_SERVER_HOST 改成电脑 IP、
     * PORT 改成 8000，并把 RAN_SERVER_USE_TLS 关掉（走明文 ws://）。 */
#if CONFIG_RAN_SERVER_USE_TLS
    const char *scheme = "wss";
#else
    const char *scheme = "ws";
#endif
    char url[160];
    snprintf(url, sizeof(url), "%s://%s:%d/ws/device/%s",
             scheme, CONFIG_RAN_SERVER_HOST, CONFIG_RAN_SERVER_PORT,
             device_config_get()->device_id);

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .reconnect_timeout_ms = 0,        /* 自己管理重连（指数退避 + 重发 hello） */
        .network_timeout_ms = 8000,
        .task_stack = 8192,               /* wss 的 TLS 握手吃栈，6KB 偏紧 */
        .buffer_size = 4096,
        /* v2.2：开启 WS 协议级心跳。应用层 JSON ping 平台不认，只有协议级 PING 帧
         * 才能让平台接入层不把这条长连接当空闲连接掐掉（实测 300s → >425s）。*/
        .ping_interval_sec = WS_PING_INTERVAL_SEC,
        .pingpong_timeout_sec = WS_PINGPONG_TIMEOUT_SEC,
#if CONFIG_RAN_SERVER_USE_TLS
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 用内置 CA bundle 校验服务端证书 */
#endif
    };
    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;
    uint32_t reconn_cnt = 0;              /* v2.2：重连次数，仅用于日志（现场排查） */
    TickType_t last_ping = 0;

    for (;;) {
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
            ESP_LOGI(TAG, "连接 %s ...", url);
            esp_websocket_client_start(s_client);

            /* 等待连接成功（最多 10s） */
            EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED,
                                                   pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(10000));
            if (bits & BIT_CONNECTED) {
                backoff_ms = RECONNECT_BACKOFF_MIN_MS;
                last_ping = xTaskGetTickCount();
            } else {
                /* 连接失败/超时：停掉内部任务，退避后下一轮重新 start */
                reconn_cnt++;
                ESP_LOGW(TAG, "连接超时/失败（第 %lu 次重连），退避 %lu ms 后重试",
                         (unsigned long)reconn_cnt, (unsigned long)backoff_ms);
                esp_websocket_client_stop(s_client);
                vTaskDelay(pdMS_TO_TICKS(500));
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = backoff_ms * 2;
                if (backoff_ms > RECONNECT_BACKOFF_MAX_MS) backoff_ms = RECONNECT_BACKOFF_MAX_MS;
                continue;
            }
        }

        /* 已连接：维持心跳，每 15s 发送应用层 ping */
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ws_client_is_connected()) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ping) * portTICK_PERIOD_MS >= PING_INTERVAL_MS) {
                last_ping = now;
                char ping[64];
                snprintf(ping, sizeof(ping),
                         "{\"type\":\"ping\",\"ts\":%lld}",
                         (long long)esp_timer_get_time() / 1000);
                send_text(ping);
            }
        } else {
            /* 连接中途断开：停掉内部任务，指数退避后重连（下一轮 start）。
             * v2.2：退避 300ms 起、3s 封顶；重连成功后由 CONNECTED 事件自动续流。 */
            reconn_cnt++;
            ESP_LOGW(TAG, "连接断开（第 %lu 次重连），退避 %lu ms 后重连",
                     (unsigned long)reconn_cnt, (unsigned long)backoff_ms);
            esp_websocket_client_stop(s_client);
            vTaskDelay(pdMS_TO_TICKS(500));
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2;
            if (backoff_ms > RECONNECT_BACKOFF_MAX_MS) backoff_ms = RECONNECT_BACKOFF_MAX_MS;
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
    return s_evt && (xEventGroupGetBits(s_evt) & BIT_CONNECTED);
}
