/**
 * wifi_mgr.h —— WiFi STA 连接管理（断线自动重连 + SNTP + 连不上自动转网页配网）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 netif、事件循环并按凭据启动 STA；无可用凭据时同步开启配网热点。
 *  非阻塞：正常路径连接在后台进行 */
esp_err_t wifi_mgr_init(void);

/** 是否已获取 IP */
bool wifi_mgr_is_connected(void);

/** 获取本机 IPv4 字符串（如 "192.168.1.23"），未连接返回 "0.0.0.0" */
const char *wifi_mgr_get_ip(void);

/** SNTP 是否已完成首次时间同步（time(NULL) 有效） */
bool wifi_mgr_time_synced(void);

/* ---------------- v2.5：网页配网相关 ---------------- */

/** 是否有可用 WiFi 凭据（NVS 或编译期默认值） */
bool wifi_mgr_has_credentials(void);

/** 是否正处于配网模式（热点与配网页开着、等用户设置） */
bool wifi_mgr_is_provisioning(void);

/** 本次开机是否连上过路由器 */
bool wifi_mgr_ever_connected(void);

/** 用指定凭据连接（配网页保存时调用）：保存到 NVS → 配置 STA → 连接 → 等 IP。
 *  成功返回 ESP_OK；超时返回 ESP_ERR_TIMEOUT，细节见 wifi_mgr_last_error_text() */
esp_err_t wifi_mgr_connect_with(const char *ssid, const char *pass, uint32_t timeout_ms);

/** 最近一次连接失败的说明文字（含原因码），供配网页与日志使用 */
const char *wifi_mgr_last_error_text(void);

/** 最近一次断开/失败的原因码（0 = 没收到路由器响应） */
int wifi_mgr_last_disconnect_reason(void);

#ifdef __cplusplus
}
#endif
