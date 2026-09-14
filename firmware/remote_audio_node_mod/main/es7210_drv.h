/**
 * es7210_drv.h —— ES7210 四通道音频 ADC 驱动接口（本工程封装）
 *
 * 【为什么文件名是 es7210_drv.* 而不是 es7210.*】
 * 官方托管组件 espressif/es7210 的头文件名字就叫 es7210.h。
 * 如果我方也放一个 main/es7210.h，那么 main/ 目录会优先命中，
 * 把官方头文件整个遮住，es7210_new_codec / es7210_config_codec 这些声明
 * 就全都找不到。所以本工程统一用 es7210_drv.*，绝不与官方组件撞名。
 *
 * 本文件只做两件事：装 I2C 总线 + 自适应探测芯片地址；
 * 真正的寄存器配置交给官方组件。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ES7210 I2C 地址（由 AD0/AD1 两个引脚决定，四选一） */
#define ES7210_I2C_ADDR_00   0x40   /* AD0=GND, AD1=GND */
#define ES7210_I2C_ADDR_01   0x41   /* AD0=VDD, AD1=GND */
#define ES7210_I2C_ADDR_10   0x42   /* AD0=GND, AD1=VDD */
#define ES7210_I2C_ADDR_11   0x43   /* AD0=VDD, AD1=VDD */

/**
 * 初始化 ES7210。
 *
 * 流程：安装 I2C 总线 → 在 0x40~0x43 中探测芯片（addr_hint 优先试）
 *      → 调官方组件写寄存器（16kHz / 16bit / 标准 I2S / 非 TDM / MIC1+MIC2）。
 *
 * 必须在 I2S RX 初始化之前调用：芯片寄存器只走 I2C，不需要 MCLK 就能配好。
 *
 * @param i2c_port  I2C 端口号（I2C_NUM_0 / I2C_NUM_1）
 * @param sda_gpio  I2C SDA 引脚
 * @param scl_gpio  I2C SCL 引脚
 * @param addr_hint 优先尝试的 I2C 地址（传 0 表示直接按 0x40→0x43 顺序探测）
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示 0x40~0x43 全部无应答
 */
esp_err_t es7210_init(int i2c_port, int sda_gpio, int scl_gpio, uint8_t addr_hint);

/** 返回实际探测到的 I2C 地址（尚未成功初始化时返回 0） */
uint8_t es7210_i2c_addr(void);

#ifdef __cplusplus
}
#endif
