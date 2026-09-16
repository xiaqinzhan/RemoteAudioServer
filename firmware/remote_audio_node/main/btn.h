/**
 * btn.h —— GPIO0 录音开关按键（软件消抖，短按切换录音功能开关）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动按键轮询任务（内部创建，按下低有效，50ms 消抖） */
esp_err_t btn_start(void);

#ifdef __cplusplus
}
#endif
