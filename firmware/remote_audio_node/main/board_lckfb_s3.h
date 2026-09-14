/**
 * board_lckfb_s3.h —— 立创实战派 ESP32-S3 (LCKFB SZPI) 引脚定义
 *
 * 板载核心外设：
 *   - ES7210  4-ch ADC（录音，I2C 控制 + I2S 数据）
 *   - ES8311  DAC（播放，本工程不实现）
 *   - GC0308  DVP 摄像头
 *   - 2.0" IPS LCD（SPI）
 *   - TF 卡（SDMMC 1-bit）
 *   - QMI8658 6 轴 IMU
 *   - USB-HUB
 *
 * 注：摄像头与音频 ES7210 存在部分 GPIO 冲突（GPIO13/14/38），
 *     本固件仅用于音频采集，不启用摄像头，无冲突。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c.h"

/* ---------------- ES7210 音频 ADC（I2C 控制 + I2S 数据输入） ---------------- */
#define BOARD_ES7210_I2C_PORT   I2C_NUM_0
#define BOARD_ES7210_I2C_SDA    GPIO_NUM_1
#define BOARD_ES7210_I2C_SCL    GPIO_NUM_2
/* 实战派 S3 板上 AD0 被拉高，实测芯片落在 0x41（不是 0x40）。
 * 这个值只作为"优先尝试"，驱动还会在 0x40~0x43 全范围探测兜底。 */
#define BOARD_ES7210_I2C_ADDR   0x41
#define BOARD_ES7210_MCLK       GPIO_NUM_38 /* 主时钟输出，256 × Fs = 4.096MHz @16kHz */

/* ES7210 I2S 数据引脚（I2S0 RX） */
#define BOARD_I2S_RX_SCK        GPIO_NUM_14 /* BCLK  位时钟 */
#define BOARD_I2S_RX_WS         GPIO_NUM_13 /* WS/LRCK 字选择 */
#define BOARD_I2S_RX_SD         GPIO_NUM_12 /* SDOUT 数据（ES7210→ESP32） */
#define BOARD_I2S_RX_PORT       I2S_NUM_0

/* ---------------- I2S TX 未使用（ES8311 DAC 播放功能未实现） ----------------
 * 这里曾经定义过一个 TX 占位通道：控制器用 I2S_NUM_1，而 BCLK/WS 复用了上面
 * I2S0 的 GPIO14/13。这是错的 —— 同一组 BCLK/WS 引脚不能让两个 I2S 控制器
 * 同时驱动，GPIO 矩阵只保留最后接上的一路，I2S0 的时钟会被从引脚上挤掉，
 * 录音当场失去时钟（现象：i2s_channel_read 每帧都等满超时）。
 * 要加播放，正确做法是把 TX 挂到 I2S_NUM_0 做全双工（TX/RX 两个句柄共用同一份
 * 时钟/引脚配置），再把 DOUT 接到 ES8311 的 DIN。
 * 所以本工程不定义任何 TX 引脚，从根上避免误用。 */

/* ---------------- MicroSD（SDMMC 1-bit 模式） ---------------- */
#define BOARD_SD_CLK            GPIO_NUM_47
#define BOARD_SD_CMD            GPIO_NUM_48
#define BOARD_SD_D0             GPIO_NUM_21

/* sd_storage.c 实际走 SDMMC（esp_vfs_fat_sdmmc_mount + SDMMC_HOST_DEFAULT），
 * 下面几个 SPI 宏名只是历史兼容保留，SDMMC 路径不会用到。 */
#define BOARD_SD_CS             GPIO_NUM_NC
#define BOARD_SD_MOSI           GPIO_NUM_NC
#define BOARD_SD_SCK            BOARD_SD_CLK
#define BOARD_SD_MISO           GPIO_NUM_NC

/* ---------------- OLED 不可用（GPIO41/42 被摄像头占用） ---------------- */
/* 实战派 S3 自带 2.0" IPS LCD，不支持外接 SSD1306 OLED。
 * oled.c 检测到 SDA/SCL 为 NC 会直接返回 ESP_ERR_NOT_FOUND，
 * 且 Kconfig 的 RAN_ENABLE_OLED 默认已关闭。 */
#define BOARD_OLED_SDA          GPIO_NUM_NC
#define BOARD_OLED_SCL          GPIO_NUM_NC
#define BOARD_OLED_I2C_PORT     I2C_NUM_1   /* 占位，实际不启用 */

/* ---------------- 录音开关按键（BOOT 键） ---------------- */
#define BOARD_BTN_REC           GPIO_NUM_0

/* ---------------- 板载 LED（状态指示） ---------------- */
#define BOARD_LED_GPIO          GPIO_NUM_39

/* ---------------- 音频常量 ---------------- */
#define AUDIO_SAMPLE_RATE_HZ    16000
#define AUDIO_FRAME_MS          20
#define AUDIO_FRAME_SAMPLES     320          /* 16000 * 20ms */
#define AUDIO_FRAME_PCM_BYTES   640          /* 320 samples * 2 bytes */
#define AUDIO_REC_BITRATE       32000        /* Opus 32kbps */
#define AUDIO_FILE_CHUNK        2048         /* 文件回传分块大小 */

/* ---------------- WS binary 帧头（协议 §5） ---------------- */
#define WS_FRAME_TYPE_LIVE_PCM  0x01
#define WS_FRAME_TYPE_FILE_BIN  0x02
#define WS_FRAME_HEADER_LEN     7            /* type(1) + req_id(4) + seq(2) */
#define WS_FRAME_SEQ_END        0xFFFF

#define SD_MOUNT_POINT          "/sdcard"
#define SD_RECORD_DIR           SD_MOUNT_POINT "/recordings"
#define SD_FREE_RESERVE_BYTES   (200ULL * 1024 * 1024)   /* 剩余 <200MB 触发清理 */
