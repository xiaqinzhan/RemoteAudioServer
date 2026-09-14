/**
 * ogg_muxer.c —— Ogg Opus 复用实现
 */
#include "ogg_muxer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "ogg_mux";

#define OGG_CRC_POLY      0x04c11db7u
#define OGG_GRANULE_48K_PER_FRAME  960u   /* 20ms @48kHz */

static uint32_t s_crc_table[256];
static bool     s_crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int b = 0; b < 8; b++) {
            r = (r & 0x80000000u) ? (r << 1) ^ OGG_CRC_POLY : (r << 1);
        }
        s_crc_table[i] = r;
    }
    s_crc_ready = true;
}

static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ s_crc_table[((crc >> 24) & 0xFF) ^ data[i]];
    }
    return crc;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

struct ogg_muxer_s {
    FILE    *f;
    uint32_t serial;
    uint32_t page_seq;
    uint64_t granule;
    long     last_page_off;
    int      audio_pages;
};

/**
 * 组装一个 Ogg 页到 buf（容量 >= 27+255+pktlen），返回总长度。
 * 一个 packet 做 lacing 分段（本工程包远小于 255 字节，实际 1 段）。
 */
static int build_page(uint8_t *buf, uint8_t htype, uint64_t granule,
                      uint32_t serial, uint32_t page_seq,
                      const uint8_t *pkt, int pktlen)
{
    memcpy(buf, "OggS", 4);
    buf[4] = 0;                 /* version */
    buf[5] = htype;
    put_le64(buf + 6, granule);
    put_le32(buf + 14, serial);
    put_le32(buf + 18, page_seq);
    put_le32(buf + 22, 0);      /* CRC 先置 0 */

    int segtab = 27;
    int datalen = 0;
    int rem = pktlen;
    while (rem >= 255) {
        buf[segtab++] = 255;
        rem -= 255;
        datalen += 255;
    }
    buf[segtab++] = (uint8_t)rem;   /* 末段（<255，标记 packet 结束）；pktlen=0 时为 0 */
    datalen += rem;

    buf[26] = (uint8_t)(segtab - 27);
    if (pktlen > 0) {
        memcpy(buf + segtab, pkt, pktlen);
    }
    int total = segtab + datalen;

    uint32_t crc = ogg_crc(buf, total);
    put_le32(buf + 22, crc);
    return total;
}

static esp_err_t write_page(ogg_muxer_t *m, uint8_t htype, uint64_t granule,
                            const uint8_t *pkt, int pktlen)
{
    uint8_t buf[27 + 255 + 300];   /* 本工程 packet <255B，余量充足 */
    int total = build_page(buf, htype, granule, m->serial, m->page_seq, pkt, pktlen);
    m->last_page_off = ftell(m->f);
    size_t w = fwrite(buf, 1, total, m->f);
    m->page_seq++;
    if (w != (size_t)total) {
        ESP_LOGE(TAG, "Ogg 页写入失败 (w=%u/%u)", (unsigned)w, total);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ogg_muxer_open(const char *abs_path, uint32_t serial, ogg_muxer_t **out)
{
    if (!abs_path || !out) return ESP_ERR_INVALID_ARG;
    if (!s_crc_ready) crc_init();

    FILE *f = fopen(abs_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "无法创建录音文件: %s", abs_path);
        return ESP_FAIL;
    }

    ogg_muxer_t *m = calloc(1, sizeof(*m));
    if (!m) { fclose(f); return ESP_ERR_NO_MEM; }
    m->f = f;
    m->serial = serial;
    m->granule = 0;

    /* --- OpusHead (19 字节) --- */
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                  /* version */
    head[9] = 1;                  /* channels = mono */
    put_le16(head + 10, 312);     /* pre-skip */
    put_le32(head + 12, 16000);   /* 原始输入采样率（信息字段） */
    put_le16(head + 16, 0);       /* output gain */
    head[18] = 0;                 /* channel mapping family 0 */
    esp_err_t err = write_page(m, 0x02, 0, head, sizeof(head));  /* BOS */
    if (err != ESP_OK) goto fail;

    /* --- OpusTags --- */
    const char *vendor = "remote-audio-node esp32s3";
    uint8_t tags[8 + 4 + 40 + 4];
    int t = 0;
    memcpy(tags + t, "OpusTags", 8); t += 8;
    put_le32(tags + t, (uint32_t)strlen(vendor)); t += 4;
    memcpy(tags + t, vendor, strlen(vendor)); t += (int)strlen(vendor);
    put_le32(tags + t, 0); t += 4;   /* 0 条用户注释 */
    err = write_page(m, 0x00, 0, tags, t);
    if (err != ESP_OK) goto fail;

    ESP_LOGI(TAG, "Ogg 录音已创建: %s (serial=0x%08lx)", abs_path,
             (unsigned long)serial);
    *out = m;
    return ESP_OK;

fail:
    fclose(f);
    free(m);
    return err;
}

esp_err_t ogg_muxer_write_packet(ogg_muxer_t *m, const uint8_t *pkg, int len)
{
    if (!m || !pkg || len <= 0) return ESP_ERR_INVALID_ARG;
    m->granule += OGG_GRANULE_48K_PER_FRAME;   /* 20ms @48kHz */
    esp_err_t err = write_page(m, 0x00, m->granule, pkg, len);
    if (err == ESP_OK) m->audio_pages++;
    return err;
}

esp_err_t ogg_muxer_close(ogg_muxer_t *m, uint32_t *out_bytes)
{
    if (!m || !m->f) return ESP_ERR_INVALID_STATE;

    if (m->audio_pages > 0) {
        /* 回读最后一个音频页，置 EOS 标志并重算 CRC（保持 granule 不变） */
        if (fseek(m->f, m->last_page_off, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "seek 末页失败");
        } else {
            uint8_t hdr[27];
            if (fread(hdr, 1, 27, m->f) == 27 && memcmp(hdr, "OggS", 4) == 0) {
                int nseg = hdr[26];
                int datalen = 0;
                uint8_t segtab[255];
                if (fread(segtab, 1, nseg, m->f) == (size_t)nseg) {
                    for (int i = 0; i < nseg; i++) datalen += segtab[i];
                    int page_len = 27 + nseg + datalen;
                    uint8_t *page = malloc(page_len);
                    if (page) {
                        fseek(m->f, m->last_page_off, SEEK_SET);
                        if (fread(page, 1, page_len, m->f) == (size_t)page_len) {
                            page[5] |= 0x04;     /* EOS */
                            put_le32(page + 22, 0);
                            uint32_t crc = ogg_crc(page, page_len);
                            put_le32(page + 22, crc);
                            fseek(m->f, m->last_page_off, SEEK_SET);
                            fwrite(page, 1, page_len, m->f);
                        }
                        free(page);
                    }
                }
            }
        }
    } else {
        /* 无音频包：写一个 0 段 EOS 页 */
        write_page(m, 0x04, m->granule, NULL, 0);
    }

    fflush(m->f);
    long sz = ftell(m->f);
    fclose(m->f);
    if (out_bytes) *out_bytes = (sz > 0) ? (uint32_t)sz : 0;
    ESP_LOGI(TAG, "Ogg 录音关闭: %u 字节, %d 个音频包",
             (unsigned)(sz > 0 ? sz : 0), m->audio_pages);
    free(m);
    return ESP_OK;
}
