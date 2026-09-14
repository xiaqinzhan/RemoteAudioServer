/**
 * oled.h —— SSD1306 128x64 I2C 状态显示（最小实现）
 * 显示内容：设备 ID / IP 或 WiFi 状态 / 在线(WS)状态 / 推流&录音状态。
 * 初始化失败仅告警，不阻塞主流程。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 I2C + SSD1306 并启动 1Hz 刷新任务；失败返回错误码且不重启 */
esp_err_t oled_start(void);

#ifdef __cplusplus
}
#endif
