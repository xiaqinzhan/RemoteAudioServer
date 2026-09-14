/**
 * device_config.h —— 设备配置：device_id 派生、NVS 持久化（录音开关 / VAD 阈值 / 保留天数）
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RAN_DEVICE_ID_MAX   32

typedef struct {
    bool     recording_enabled;   /* 录音功能总开关（NVS 持久化） */
    uint32_t vad_threshold;       /* VAD 能量阈值（NVS 持久化，初值取 Kconfig） */
    uint32_t retention_days;      /* 录音保留天数（NVS 持久化，0=永久） */
    char     device_id[RAN_DEVICE_ID_MAX]; /* esp32-aabbcc 或 Kconfig 覆盖值 */
} ran_config_t;

/** 初始化 NVS（含擦除损坏分区重试）并加载配置；默认值来自 Kconfig */
esp_err_t device_config_init(void);

/** 取配置指针（只读使用；修改请走 set_* 接口以保证持久化） */
const ran_config_t *device_config_get(void);

/** 设置录音开关并持久化；返回新状态 */
esp_err_t device_config_set_recording_enabled(bool enabled);

/** 更新 VAD 阈值并持久化（0 表示忽略该字段） */
esp_err_t device_config_set_vad_threshold(uint32_t v);

/** 更新保留天数并持久化 */
esp_err_t device_config_set_retention_days(uint32_t days);

/** 配置变化后通知 OLED/hello 等模块刷新（由调用方触发） */
void device_config_notify_changed(void);

#ifdef __cplusplus
}
#endif
