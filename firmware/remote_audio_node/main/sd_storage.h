/**
 * sd_storage.h —— MicroSD (SPI2) FATFS 挂载、目录遍历、保留策略清理
 *
 * 所有对 FATFS 元数据/目录的操作（挂载、mkdir、rename、目录遍历、清理）
 * 都在 sd_storage_lock()/unlock() 保护下进行；
 * 录音写文件、play_file 读文件属于流式顺序 IO（FATFS 内部可重入），
 * 但在与 rename/清理交互时也应持锁，避免读到半改名的文件。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     rel_path[160];   /* "2026-09-09/10-20-30.opus" */
    char     date[16];        /* "2026-09-09" */
    uint32_t size;
    uint32_t mtime;           /* Unix 秒 */
    uint32_t duration;        /* 秒（由 Ogg granule_position 解析） */
} sd_rec_file_t;

/** 挂载 SD 卡到 /sdcard 并创建 recordings 目录；未插卡返回 ESP_FAIL 但不重启 */
esp_err_t sd_storage_init(void);

bool sd_storage_ready(void);

/** 递归创建目录（相对 /sdcard 绝对路径），已存在视为成功 */
esp_err_t sd_storage_mkdir_p(const char *path);

/** 原子落盘：rename .tmp -> .opus */
esp_err_t sd_storage_rename(const char *from, const char *to);

/** FATFS 互斥锁（递归锁），保护目录操作与 rename */
void sd_storage_lock(void);
void sd_storage_unlock(void);

/** 把 rel_path（"日期/文件名.opus"）拼成安全的绝对路径；含 ".." 等非法内容返回 NULL */
const char *sd_storage_safe_abs(const char *rel_path, char *out, size_t out_len);

/**
 * 遍历 /sdcard/recordings，收集所有 .opus 文件（按日期/文件名倒序，最新在前）。
 * out_files 由调用方分配，返回文件数；出错返回 -1。
 * date_filter 非 NULL 时只返回该日期目录（"2026-09-09"）。
 */
int sd_storage_list_recordings(sd_rec_file_t *out_files, int max_files, const char *date_filter);

/** 解析 Ogg Opus 文件时长（秒）：读取末页 granule_position / 采样率 */
uint32_t sd_storage_ogg_duration_sec(const char *abs_path);

/**
 * 保留策略：剩余空间 < 200MB 或文件年龄 > retention_days 时，删除最旧日期目录。
 * 启动时与每次录音结束后调用。retention_days=0 表示不按年龄清理（空间不足仍清理）。
 */
void sd_storage_apply_retention(uint32_t retention_days);

#ifdef __cplusplus
}
#endif
