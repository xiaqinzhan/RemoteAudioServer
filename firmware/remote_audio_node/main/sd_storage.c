/**
 * sd_storage.c —— MicroSD FATFS 实现
 * 立创实战派 S3：TF 卡使用 SDMMC 1-bit 模式
 */
#include "sd_storage.h"
#include "board.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sdkconfig.h"

static const char *TAG = "sd";

static sdmmc_card_t      *s_card;
static SemaphoreHandle_t  s_mtx;
static volatile bool      s_ready;

/* ---------------- Ogg 时长解析（声明为内部使用） ---------------- */
static uint32_t parse_ogg_duration_sec(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long fsize = ftell(f);
    if (fsize < 27) return 0;

    /* 从文件尾部向前扫描最后一个 OggS 页（Opus EOS 页 granule 即总采样数） */
    uint8_t *tail = malloc(8192);
    if (!tail) return 0;
    long scan_len = fsize < 8192 ? fsize : 8192;
    if (fseek(f, fsize - scan_len, SEEK_SET) != 0) { free(tail); return 0; }
    size_t rd = fread(tail, 1, scan_len, f);

    long last_page = -1;
    for (size_t i = 0; i + 27 <= rd; i++) {
        if (tail[i] == 'O' && tail[i+1] == 'g' && tail[i+2] == 'g' && tail[i+3] == 'S') {
            last_page = (long)i;
        }
    }
    if (last_page < 0) { free(tail); return 0; }
    const uint8_t *p = tail + last_page;
    uint64_t granule = (uint64_t)p[6]
                     | ((uint64_t)p[7]  << 8)
                     | ((uint64_t)p[8]  << 16)
                     | ((uint64_t)p[9]  << 24)
                     | ((uint64_t)p[10] << 32)
                     | ((uint64_t)p[11] << 40)
                     | ((uint64_t)p[12] << 48)
                     | ((uint64_t)p[13] << 56);
    free(tail);
    /* Ogg Opus 的 granule_position 恒为 48kHz 域采样计数（RFC 7845） */
    if (granule == 0 || granule > 48000ULL * 3600 * 6) return 0;
    return (uint32_t)(granule / 48000ULL);
}

uint32_t sd_storage_ogg_duration_sec(const char *abs_path)
{
    FILE *f = fopen(abs_path, "rb");
    if (!f) return 0;
    uint32_t sec = parse_ogg_duration_sec(f);
    fclose(f);
    return sec;
}

/* ---------------- 互斥锁 ---------------- */
void sd_storage_lock(void)
{
    if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
}
void sd_storage_unlock(void)
{
    if (s_mtx) xSemaphoreGiveRecursive(s_mtx);
}

/* ---------------- 挂载（SDMMC 1-bit 模式） ---------------- */
esp_err_t sd_storage_init(void)
{
    s_mtx = xSemaphoreCreateRecursiveMutex();
    if (!s_mtx) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    /* SDMMC 主机：默认配置 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    /* 1-bit 模式引脚映射（实战派 S3） */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;                 /* 1-bit 模式 */
    slot.clk = BOARD_SD_CLK;       /* GPIO47 */
    slot.cmd = BOARD_SD_CMD;       /* GPIO48 */
    slot.d0  = BOARD_SD_D0;        /* GPIO21 */
    slot.d1  = -1;                  /* 未使用 */
    slot.d2  = -1;                  /* 未使用 */
    slot.d3  = -1;                  /* 未使用 */
    slot.cd  = SDMMC_SLOT_NO_CD;   /* 无卡检测引脚 */
    slot.wp  = SDMMC_SLOT_NO_WP;   /* 无写保护引脚 */

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡挂载失败（未插卡？）: %s —— 录音功能停用，实时推流不受影响",
                 esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    s_ready = true;
    sdmmc_card_print_info(stdout, s_card);

    /* 创建 recordings 根目录 */
    sd_storage_lock();
    mkdir(SD_RECORD_DIR, 0775);
    sd_storage_unlock();

    /* 容量信息 */
    uint64_t total = 0, used = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SD 容量: %llu MB, 已用 %llu MB, 剩余 %llu MB",
                 (unsigned long long)(total / (1024*1024)),
                 (unsigned long long)(used / (1024*1024)),
                 (unsigned long long)((total - used) / (1024*1024)));
    }
    return ESP_OK;
}

bool sd_storage_ready(void)
{
    return s_ready;
}

esp_err_t sd_storage_mkdir_p(const char *path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0775);   /* 已存在则忽略 */
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0) {
        /* EEXIST 正常 */
    }
    return ESP_OK;
}

esp_err_t sd_storage_rename(const char *from, const char *to)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (rename(from, to) != 0) {
        ESP_LOGE(TAG, "rename 失败: %s -> %s", from, to);
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char *sd_storage_safe_abs(const char *rel_path, char *out, size_t out_len)
{
    if (!rel_path || !out || out_len < 32) return NULL;
    if (strstr(rel_path, "..") || strchr(rel_path, '\\') || rel_path[0] == '/') {
        ESP_LOGE(TAG, "非法文件路径: %s", rel_path);
        return NULL;
    }
    snprintf(out, out_len, "%s/%s", SD_RECORD_DIR, rel_path);
    return out;
}

/* ---------------- 目录遍历 ---------------- */
static int cmp_files_desc(const void *a, const void *b)
{
    const sd_rec_file_t *fa = (const sd_rec_file_t *)a;
    const sd_rec_file_t *fb = (const sd_rec_file_t *)b;
    int c = strcmp(fb->date, fa->date);
    if (c != 0) return c;
    return strcmp(fb->rel_path, fa->rel_path);
}

int sd_storage_list_recordings(sd_rec_file_t *out_files, int max_files, const char *date_filter)
{
    if (!s_ready || !out_files || max_files <= 0) return -1;

    int count = 0;
    sd_storage_lock();
    DIR *root = opendir(SD_RECORD_DIR);
    if (!root) {
        sd_storage_unlock();
        ESP_LOGW(TAG, "无法打开 %s", SD_RECORD_DIR);
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(root)) != NULL && count < max_files) {
        if (de->d_type != DT_DIR) continue;
        if (de->d_name[0] == '.') continue;
        if (date_filter && strcmp(de->d_name, date_filter) != 0) continue;

        char dir_path[200];
        snprintf(dir_path, sizeof(dir_path), "%s/%.180s", SD_RECORD_DIR, de->d_name);
        DIR *dd = opendir(dir_path);
        if (!dd) continue;
        struct dirent *fde;
        while ((fde = readdir(dd)) != NULL && count < max_files) {
            const char *nm = fde->d_name;
            size_t nl = strlen(nm);
            if (nl < 5 || strcmp(nm + nl - 5, ".opus") != 0) continue;

            char abs_path[512];
            snprintf(abs_path, sizeof(abs_path), "%s/%s", dir_path, nm);
            struct stat st;
            if (stat(abs_path, &st) != 0) continue;

            sd_rec_file_t *f = &out_files[count];
            memset(f, 0, sizeof(*f));
            snprintf(f->rel_path, sizeof(f->rel_path), "%.10s/%.140s", de->d_name, nm);
            snprintf(f->date, sizeof(f->date), "%.15s", de->d_name);
            f->size  = (uint32_t)st.st_size;
            f->mtime = (uint32_t)st.st_mtime;
            f->duration = sd_storage_ogg_duration_sec(abs_path);
            count++;
        }
        closedir(dd);
    }
    closedir(root);
    sd_storage_unlock();

    qsort(out_files, count, sizeof(sd_rec_file_t), cmp_files_desc);
    ESP_LOGI(TAG, "list_recordings: 找到 %d 个录音文件%s",
             count, date_filter ? "（按日期过滤）" : "");
    return count;
}

/* ---------------- 保留策略 ---------------- */
static uint64_t free_space_bytes(void)
{
    /* ESP-IDF newlib 无 sys/statvfs.h，使用 FATFS VFS 自带容量接口 */
    uint64_t total = 0, used = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &used) != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info 失败，跳过空间检查");
        return UINT64_MAX;   /* 未知空间，按“充足”处理避免误删 */
    }
    return (total >= used) ? (total - used) : 0;
}

static void remove_dir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_dir_recursive(full);
        } else {
            unlink(full);
        }
    }
    closedir(d);
    rmdir(path);
}

void sd_storage_apply_retention(uint32_t retention_days)
{
    if (!s_ready) return;

    sd_storage_lock();

    /* 收集日期目录（"YYYY-MM-DD" 格式；unsynced 不参与年龄清理，参与空间清理放最后） */
    char dates[64][16];
    int  ndates = 0;
    bool has_unsynced = false;

    DIR *root = opendir(SD_RECORD_DIR);
    if (!root) { sd_storage_unlock(); return; }
    struct dirent *de;
    while ((de = readdir(root)) != NULL && ndates < 64) {
        if (de->d_type != DT_DIR || de->d_name[0] == '.') continue;
        if (strcmp(de->d_name, "unsynced") == 0) { has_unsynced = true; continue; }
        /* 仅接受 YYYY-MM-DD 形式，避免误删 */
        int y=0, m=0, dd2=0;
        if (sscanf(de->d_name, "%d-%d-%d", &y, &m, &dd2) == 3 && y >= 2020 && y < 2100) {
            snprintf(dates[ndates], sizeof(dates[0]), "%.15s", de->d_name);
            ndates++;
        }
    }
    closedir(root);

    /* 日期目录名按字典序 = 按时间顺序；升序排序，最旧在前 */
    for (int i = 0; i < ndates - 1; i++)
        for (int j = i + 1; j < ndates; j++)
            if (strcmp(dates[j], dates[i]) < 0) {
                char tmp[16];
                strcpy(tmp, dates[i]);
                strcpy(dates[i], dates[j]);
                strcpy(dates[j], tmp);
            }

    time_t now = time(NULL);
    bool need_space = free_space_bytes() < SD_FREE_RESERVE_BYTES;
    int  oldest = 0;

    while (oldest < ndates) {
        /* 年龄判断 */
        bool too_old = false;
        if (retention_days > 0 && now > 1700000000) {  /* 时间已同步 */
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", SD_RECORD_DIR, dates[oldest]);
            struct stat st;
            if (stat(path, &st) == 0) {
                double age_days = difftime(now, st.st_mtime) / 86400.0;
                too_old = age_days > (double)retention_days;
            }
        }
        if (!too_old && !need_space) break;   /* 最旧目录都不满足条件，停止 */

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", SD_RECORD_DIR, dates[oldest]);
        ESP_LOGW(TAG, "保留策略：删除日期目录 %s (space_need=%d too_old=%d)",
                 dates[oldest], need_space, too_old);
        remove_dir_recursive(path);
        oldest++;

        /* 每删一天重新评估空间；删到只剩最近一天也停止空间清理，避免清空全部 */
        if (need_space) {
            need_space = free_space_bytes() < SD_FREE_RESERVE_BYTES;
            if (ndates - oldest <= 1 && !too_old) {
                ESP_LOGW(TAG, "剩余录音不足，空间清理停止");
                break;
            }
        }
    }

    /* 空间仍不足且存在 unsynced 目录，最后再清理它 */
    if (has_unsynced && free_space_bytes() < SD_FREE_RESERVE_BYTES) {
        char path[256];
        snprintf(path, sizeof(path), "%s/unsynced", SD_RECORD_DIR);
        ESP_LOGW(TAG, "保留策略：空间不足，删除 unsynced 目录");
        remove_dir_recursive(path);
    }

    sd_storage_unlock();
}
