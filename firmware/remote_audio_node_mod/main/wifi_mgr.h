/**
 * wifi_mgr.h —— WiFi STA 连接管理（含断线自动重连 + SNTP 时间同步）
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 NVS 依赖、netif、事件循环并启动 STA；非阻塞，连接在后台进行 */
esp_err_t wifi_mgr_init(void);

/** 是否已获取 IP */
bool wifi_mgr_is_connected(void);

/** 获取本机 IPv4 字符串（如 "192.168.1.23"），未连接返回 "0.0.0.0" */
const char *wifi_mgr_get_ip(void);

/** SNTP 是否已完成首次时间同步（time(NULL) 有效） */
bool wifi_mgr_time_synced(void);

#ifdef __cplusplus
}
#endif
