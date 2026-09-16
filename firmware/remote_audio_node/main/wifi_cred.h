/**
 * wifi_cred.h —— WiFi 凭据持久化（NVS，独立 namespace "wifi"）
 *
 * v2.5 新增。设计要点（对应用户明确要求）：
 *  - 进配网模式【不删除】旧凭据；只有用户在配网页面上保存新 WiFi 时才覆盖。
 *  - 读不到时返回 ESP_ERR_NVS_NOT_FOUND，由 wifi_mgr 决定回退编译期默认值还是进配网。
 *  - 与设备配置（namespace "ran"）分开存放，便于单独排查/覆盖。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* 802.11 SSID 上限 32 字节 + 结束符；WPA2 密码上限 63 字节 + 结束符 */
#define WIFI_CRED_SSID_MAX 33
#define WIFI_CRED_PASS_MAX 65

/** 读取已保存凭据；无记录返回 ESP_ERR_NVS_NOT_FOUND（ssid 置空串） */
esp_err_t wifi_cred_load(char *ssid, size_t ssid_size, char *pass, size_t pass_size);

/** 保存/覆盖凭据（用户提交新 WiFi 时调用） */
esp_err_t wifi_cred_save(const char *ssid, const char *pass);

/** 清除凭据（保留接口：当前流程不主动调用，避免误删用户记录） */
esp_err_t wifi_cred_erase(void);

/** 是否已有保存的凭据 */
bool wifi_cred_exists(void);
