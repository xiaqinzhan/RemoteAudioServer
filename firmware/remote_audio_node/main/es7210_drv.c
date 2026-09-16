/**
 * es7210_drv.c —— ES7210 音频 ADC 初始化（基于官方 espressif/es7210 组件）
 *
 * 【为什么重写】
 * 上一版是手写的寄存器序列，作者自己在 MASTER_CTRL3 那一行注明
 * "寄存器值 = 512/2-1 = 255 → 但这里用简化设置"，也就是没经过真机验证；
 * 同时 I2C 地址硬编码 0x40，而立创实战派 S3 实测在 0x41。
 * 地址不匹配时每次写寄存器都会 NACK（只打一行错误日志），
 * 但 es7210_init 依然返回 ESP_OK —— 表现成"初始化成功却录不到声音"，极难排查。
 *
 * 现在改成：地址自适应探测 + 官方组件的寄存器配置，
 * 探测不到芯片就直接返回错误，不再假装成功。
 */
#include "es7210_drv.h"
#include "board.h"

#include <stdbool.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "es7210.h"   /* 官方托管组件 espressif/es7210（由 idf_component.yml 拉取） */

static const char *TAG = "es7210";

/* MCLK 与采样率的关系：MCLK = 256 × Fs = 4.096MHz @16kHz。
 * 必须与 audio_i2s.c 里 I2S_STD_CLK_DEFAULT_CONFIG 的 mclk_multiple
 * （I2S_MCLK_MULTIPLE_256）保持一致，否则采样率会整体偏移。 */
#define ES7210_MCLK_RATIO   256

static i2c_port_t          s_port  = I2C_NUM_0;
static uint8_t             s_addr  = 0;
static es7210_dev_handle_t s_codec = NULL;

/* ------------------------------------------------------------------ */
/* I2C 总线 + 地址探测                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t i2c_bus_install(int port, int sda_gpio, int scl_gpio)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = sda_gpio,
        .scl_io_num       = scl_gpio,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,   /* ES7210 支持 400k，100k 更稳 */
        .clk_flags        = 0,
    };

    esp_err_t err = i2c_param_config(port, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config(I2C%d) 失败: %s", port, esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(port, conf.mode, 0, 0, 0);
    /* 已经被别的模块装过了也算正常，不当作错误 */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install(I2C%d) 失败: %s", port, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C%d 就绪：SDA=IO%d SCL=IO%d 100kHz", port, sda_gpio, scl_gpio);
    return ESP_OK;
}

/** 发一个只含地址的写事务，器件有 ACK 就说明在线 */
static bool i2c_probe(uint8_t addr7)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK);
}

/** hint 优先，然后按 0x40→0x43 顺序兜底；都没应答返回 0 */
static uint8_t probe_addr(uint8_t hint)
{
    if (hint >= ES7210_I2C_ADDR_00 && hint <= ES7210_I2C_ADDR_11 && i2c_probe(hint)) {
        return hint;
    }
    for (uint8_t a = ES7210_I2C_ADDR_00; a <= ES7210_I2C_ADDR_11; a++) {
        if (i2c_probe(a)) return a;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                            */
/* ------------------------------------------------------------------ */
esp_err_t es7210_init(int i2c_port, int sda_gpio, int scl_gpio, uint8_t addr_hint)
{
    s_port = (i2c_port_t)i2c_port;

    esp_err_t err = i2c_bus_install(s_port, sda_gpio, scl_gpio);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t addr = probe_addr(addr_hint);
    if (addr == 0) {
        ESP_LOGE(TAG, "ES7210 在 0x40~0x43 均无应答：检查排线/供电，"
                      "或先用 i2c 扫描确认芯片地址");
        return ESP_ERR_NOT_FOUND;
    }
    s_addr = addr;
    ESP_LOGI(TAG, "ES7210 探测到 I2C 地址 0x%02X", addr);

    /* 官方组件会取用该 I2C 端口上已装好的总线，不需要我们传句柄 */
    es7210_i2c_config_t i2c_cfg = {
        .i2c_port = s_port,
        .i2c_addr = addr,
    };
    err = es7210_new_codec(&i2c_cfg, &s_codec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es7210_new_codec 失败: %s", esp_err_to_name(err));
        return err;
    }

    es7210_codec_config_t codec_cfg = {
        .sample_rate_hz   = AUDIO_SAMPLE_RATE_HZ,
        .mclk_ratio       = ES7210_MCLK_RATIO,
        .i2s_format       = ES7210_I2S_FMT_I2S,     /* 标准 I2S，不用 TDM */
        .bit_width        = ES7210_I2S_BITS_16B,
        .mic_bias         = ES7210_MIC_BIAS_2V87,
        .mic_gain         = ES7210_MIC_GAIN_24DB,
        .flags.tdm_enable = false,
    };
    err = es7210_config_codec(s_codec, &codec_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es7210_config_codec 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 0 = ADC 数字音量不衰减（最大）。要调小改这里。 */
    err = es7210_config_volume(s_codec, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es7210_config_volume 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ES7210 配置完成：%dHz / 16bit / 标准 I2S / 非 TDM，MIC1+MIC2，增益 24dB",
             AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

uint8_t es7210_i2c_addr(void)
{
    return s_addr;
}
