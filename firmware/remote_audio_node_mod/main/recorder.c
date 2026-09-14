/**
 * recorder.c —— 录音状态机实现
 */
#include "recorder.h"
#include "board.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "audio_i2s.h"
#include "vad.h"
#include "opus_encoder.h"
#include "ogg_muxer.h"
#include "sd_storage.h"
#include "device_config.h"
#include "audio_stream.h"
#include "ws_client.h"

static const char *TAG = "recorder";

#define PIPE_TASK_STACK    8192     /* 含 i2s_read 的 1280B DMA 原始帧（320 帧 × 2 声道 × 2 字节）+ 前滚操作 */
#define ENC_TASK_STACK     32768    /* Opus 编码（libopus 浮点构建）需要大栈：16KB 会在第一次
                                     * opus_encode() 时栈溢出 panic（真机实测），与
                                     * s3_opus_rec_play 保持一致，别往下调 */
#define ENC_QUEUE_DEPTH    48       /* 48 帧 * 20ms = 960ms 积压容忍 */
#define PKT_CAP            512      /* 单 Opus 包上限（32kbps/20ms 约 80 字节） */

typedef enum {
    QMSG_START,     /* 开始新文件（携带 rel_path/abs_tmp，数据载荷为空） */
    QMSG_FRAME,     /* 一帧 PCM（320 samples） */
    QMSG_STOP,      /* 结束当前文件并落盘 */
} qmsg_type_t;

typedef struct {
    qmsg_type_t type;
    char        rel_path[160];   /* START: "YYYY-MM-DD/HH-MM-SS.opus" */
    char        abs_tmp[220];    /* START: 绝对 .tmp 路径 */
    char        abs_final[220];  /* START: 绝对 .opus 路径 */
    int16_t     pcm[AUDIO_FRAME_SAMPLES];
} enc_msg_t;

static QueueHandle_t s_enc_q;
static volatile bool s_recording;

/* ------------------------------------------------------------------ */
/* 工具：生成录音路径（时间已同步用本地时间；否则落 unsynced/ 目录）   */
/* ------------------------------------------------------------------ */
static bool build_recording_paths(char *rel, size_t rel_len,
                                  char *tmp, size_t tmp_len,
                                  char *fin, size_t fin_len)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    if (now > 1700000000) {   /* 时间已同步（2023-11 之后） */
        snprintf(rel, rel_len, "%04d-%02d-%02d/%02d-%02d-%02d.opus",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        snprintf(rel, rel_len, "unsynced/boot-%llu.opus",
                 (unsigned long long)(esp_timer_get_time() / 1000));
    }

    char date[32];
    snprintf(date, sizeof(date), "%s", rel);
    char *slash = strchr(date, '/');
    if (slash) *slash = '\0';

    char dir[200];
    snprintf(dir, sizeof(dir), "%s/%s", SD_RECORD_DIR, date);
    sd_storage_mkdir_p(dir);

    snprintf(fin, fin_len, "%s/%s", SD_RECORD_DIR, rel);
    /* .tmp 与最终名同目录，保证 rename 在同一文件系统内原子完成 */
    snprintf(tmp, tmp_len, "%s.tmp", fin);
    return true;
}

/* ------------------------------------------------------------------ */
/* 编码任务（核1）：START 建文件 -> FRAME 编码写页 -> STOP 落盘上报    */
/* ------------------------------------------------------------------ */
static void encoder_task(void *arg)
{
    (void)arg;
    enc_msg_t msg;
    ogg_muxer_t *mux = NULL;
    opus_enc_t  *opu = NULL;
    uint8_t     *pkg = malloc(PKT_CAP);
    char         cur_rel[160];
    uint64_t     frames = 0;
    uint32_t     serial = 0;

    if (!pkg) { ESP_LOGE(TAG, "Opus 包缓冲分配失败，编码任务退出"); vTaskDelete(NULL); }

    for (;;) {
        if (xQueueReceive(s_enc_q, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.type == QMSG_START) {
            if (mux) { ogg_muxer_close(mux, NULL); mux = NULL; }
            if (!opu) {
                if (opus_encoder_open(&opu) != ESP_OK) {
                    ESP_LOGE(TAG, "Opus 编码器创建失败，放弃本次录音");
                    /* 排空队列直到下一个 START/STOP */
                    continue;
                }
            }
            serial = (uint32_t)esp_random();
            snprintf(cur_rel, sizeof(cur_rel), "%s", msg.rel_path);
            frames = 0;

            sd_storage_lock();
            esp_err_t err = ogg_muxer_open(msg.abs_tmp, serial, &mux);
            sd_storage_unlock();
            if (err != ESP_OK) {
                mux = NULL;
                ESP_LOGE(TAG, "录音文件创建失败: %s", msg.abs_tmp);
            } else {
                ESP_LOGI(TAG, "开始录音 -> %s", msg.rel_path);
            }
        } else if (msg.type == QMSG_FRAME) {
            if (mux && opu) {
                int n = opus_encoder_encode(opu, msg.pcm, pkg, PKT_CAP);
                if (n > 0) {
                    sd_storage_lock();
                    esp_err_t err = ogg_muxer_write_packet(mux, pkg, n);
                    sd_storage_unlock();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "写 Ogg 页失败，结束录音");
                        ogg_muxer_close(mux, NULL);
                        mux = NULL;
                    } else {
                        frames++;
                    }
                }
            }
        } else if (msg.type == QMSG_STOP) {
            if (mux) {
                uint32_t file_bytes = 0;
                uint64_t saved_frames = frames;
                char     rel[160];
                char     tmp[220];
                snprintf(rel, sizeof(rel), "%s", cur_rel);
                snprintf(tmp, sizeof(tmp), "%s/%s.tmp", SD_RECORD_DIR, rel);
                char fin[220];
                snprintf(fin, sizeof(fin), "%s/%s", SD_RECORD_DIR, rel);

                sd_storage_lock();
                ogg_muxer_close(mux, &file_bytes);
                mux = NULL;
                sd_storage_rename(tmp, fin);
                sd_storage_unlock();

                uint32_t duration_ms = (uint32_t)(saved_frames * AUDIO_FRAME_MS);
                ESP_LOGI(TAG, "录音保存: %s, %u 字节, %u ms",
                         rel, file_bytes, duration_ms);

                /* 上报 recording_saved 事件（服务端广播给监听端） */
                ws_client_send_recording_saved(rel, file_bytes, duration_ms);

                /* 每次录音结束检查保留策略 */
                const ran_config_t *cfg = device_config_get();
                sd_storage_apply_retention(cfg->retention_days);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 采集 pipeline 任务（核0）：I2S -> VAD -> 推流/录音状态机            */
/* ------------------------------------------------------------------ */
static void pipeline_task(void *arg)
{
    (void)arg;

    /* 前滚环形缓冲（默认 10 帧 = 200ms），内部 RAM */
    const int preroll = CONFIG_RAN_PREROLL_FRAMES;
    int16_t (*pre)[AUDIO_FRAME_SAMPLES] = calloc(preroll, sizeof(*pre));
    if (!pre) { ESP_LOGE(TAG, "前滚缓冲分配失败"); vTaskDelete(NULL); }
    int pre_head = 0, pre_count = 0;

    vad_t vad;
    vad_init(&vad, device_config_get()->vad_threshold, CONFIG_RAN_VAD_WIN_FRAMES);

    int16_t *pcm = malloc(AUDIO_FRAME_PCM_BYTES);
    if (!pcm) { ESP_LOGE(TAG, "PCM 帧缓冲分配失败"); vTaskDelete(NULL); }

    enum { ST_IDLE, ST_REC } state = ST_IDLE;
    int silence_frames = 0;
    uint32_t rec_frames = 0;

    for (;;) {
        if (audio_i2s_read_frame(pcm) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* 实时推流：与录音完全独立，任何状态下都照常工作 */
        audio_stream_send_frame(pcm);

        const ran_config_t *cfg = device_config_get();

        /* 维护前滚缓冲（录音功能开启时才需要） */
        if (cfg->recording_enabled) {
            memcpy(pre[pre_head], pcm, AUDIO_FRAME_PCM_BYTES);
            pre_head = (pre_head + 1) % preroll;
            if (pre_count < preroll) pre_count++;
        } else {
            pre_count = 0;
            pre_head = 0;
        }

        if (!cfg->recording_enabled || !sd_storage_ready()) {
            /* 录音功能关闭或无 SD 卡：不做 VAD、不写文件 */
            if (state == ST_REC) {
                enc_msg_t stop = { .type = QMSG_STOP };
                xQueueSend(s_enc_q, &stop, pdMS_TO_TICKS(100));
                state = ST_IDLE;
                s_recording = false;
            }
            continue;
        }

        vad_set_threshold(&vad, cfg->vad_threshold);
        uint32_t rms = 0;
        bool voiced = vad_process_frame(&vad, pcm, &rms);

        if (state == ST_IDLE) {
            if (voiced) {
                /* 生成路径并入队 START */
                enc_msg_t m;
                memset(&m, 0, sizeof(m));
                m.type = QMSG_START;
                build_recording_paths(m.rel_path, sizeof(m.rel_path),
                                      m.abs_tmp, sizeof(m.abs_tmp),
                                      m.abs_final, sizeof(m.abs_final));
                xQueueSend(s_enc_q, &m, pdMS_TO_TICKS(100));

                /* 前滚缓冲按时间顺序全部补入 */
                int start = pre_count < preroll ? 0 : pre_head;
                int n = pre_count;
                for (int i = 0; i < n; i++) {
                    int idx = (start + i) % preroll;
                    enc_msg_t fm = { .type = QMSG_FRAME };
                    memcpy(fm.pcm, pre[idx], AUDIO_FRAME_PCM_BYTES);
                    xQueueSend(s_enc_q, &fm, pdMS_TO_TICKS(50));
                }

                state = ST_REC;
                s_recording = true;
                silence_frames = 0;
                rec_frames = n;
                ESP_LOGI(TAG, "VAD 触发录音 (rms=%lu thr=%lu, 前滚 %d 帧)",
                         (unsigned long)rms, (unsigned long)cfg->vad_threshold, n);
            }
        } else {
            /* 录音中：当前帧入队 */
            enc_msg_t fm = { .type = QMSG_FRAME };
            memcpy(fm.pcm, pcm, AUDIO_FRAME_PCM_BYTES);
            xQueueSend(s_enc_q, &fm, pdMS_TO_TICKS(100));
            rec_frames++;

            if (voiced) {
                silence_frames = 0;
            } else {
                silence_frames++;
                int silence_limit = CONFIG_RAN_SILENCE_TIMEOUT_MS / AUDIO_FRAME_MS;
                uint32_t max_frames = (uint32_t)CONFIG_RAN_MAX_CLIP_SECONDS *
                                      1000 / AUDIO_FRAME_MS;
                if (silence_frames >= silence_limit) {
                    ESP_LOGI(TAG, "静音 %dms，结束录音",
                             silence_frames * AUDIO_FRAME_MS);
                    enc_msg_t stop = { .type = QMSG_STOP };
                    xQueueSend(s_enc_q, &stop, pdMS_TO_TICKS(100));
                    state = ST_IDLE;
                    s_recording = false;
                    pre_count = 0;
                } else if (rec_frames >= max_frames) {
                    /* 最长切片：结束后立即开新文件，避免长录音丢失 */
                    ESP_LOGI(TAG, "到达 %ds 切片上限，切分文件",
                             CONFIG_RAN_MAX_CLIP_SECONDS);
                    enc_msg_t stop = { .type = QMSG_STOP };
                    xQueueSend(s_enc_q, &stop, pdMS_TO_TICKS(100));
                    s_recording = false;

                    enc_msg_t m2;
                    memset(&m2, 0, sizeof(m2));
                    m2.type = QMSG_START;
                    build_recording_paths(m2.rel_path, sizeof(m2.rel_path),
                                          m2.abs_tmp, sizeof(m2.abs_tmp),
                                          m2.abs_final, sizeof(m2.abs_final));
                    xQueueSend(s_enc_q, &m2, pdMS_TO_TICKS(100));
                    s_recording = true;
                    rec_frames = 0;
                    silence_frames = 0;
                    state = ST_REC;
                }
            }
        }
    }
}

esp_err_t recorder_start(void)
{
    s_enc_q = xQueueCreate(ENC_QUEUE_DEPTH, sizeof(enc_msg_t));
    if (!s_enc_q) {
        ESP_LOGE(TAG, "编码队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t r1 = xTaskCreatePinnedToCore(pipeline_task, "audio_pipe",
                                            PIPE_TASK_STACK, NULL, 12, NULL, 0);
    BaseType_t r2 = xTaskCreatePinnedToCore(encoder_task, "opus_enc",
                                            ENC_TASK_STACK, NULL, 11, NULL, 1);
    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "录音任务创建失败 r1=%d r2=%d", r1, r2);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "录音模块已启动 (VAD 阈值=%lu, 静音超时=%dms, 最长切片=%ds)",
             (unsigned long)device_config_get()->vad_threshold,
             CONFIG_RAN_SILENCE_TIMEOUT_MS, CONFIG_RAN_MAX_CLIP_SECONDS);
    return ESP_OK;
}

bool recorder_is_recording(void)
{
    return s_recording;
}

bool recorder_is_enabled(void)
{
    return device_config_get()->recording_enabled;
}

void recorder_set_enabled(bool enabled)
{
    if (device_config_get()->recording_enabled == enabled) return;
    device_config_set_recording_enabled(enabled);
    ESP_LOGI(TAG, "录音功能已 %s", enabled ? "开启" : "关闭");
    /* 上报 recording_state 事件（pipeline 任务下一帧会据此停录） */
    ws_client_send_recording_state(enabled);
}
