/**
 * diag.h —— 运行状态心跳（黑匣子日志）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动运行状态心跳任务（间隔 CONFIG_RAN_DIAG_INTERVAL_MS，0 = 不启动） */
esp_err_t diag_start(void);

#ifdef __cplusplus
}
#endif
