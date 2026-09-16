/**
 * wifi_prov.h —— 零配置入网（AP 热点 + 手机网页配网）
 *
 * v2.5 新增。触发条件（用户确认）：
 *   ① NVS 里没有任何 WiFi 记录（且编译期默认值未改）；
 *   ② 有记录但开机后 1 分钟（CONFIG_RAN_PROV_FALLBACK_S）仍未连上路由器。
 * 进配网【不删除】旧凭据；只有用户在页面保存新 WiFi 时才覆盖 NVS 记录。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    WIFI_PROV_NONE = 0,   /* 未在配网 */
    WIFI_PROV_ACTIVE,     /* 热点已开，等用户设置 */
    WIFI_PROV_CONNECTING, /* 用户已提交，正在尝试连接 */
    WIFI_PROV_SUCCESS,    /* 已连上，热点即将关闭 */
} wifi_prov_state_t;

/** 立即启动配网（幂等；同步执行，可在 main 任务里直接调用） */
esp_err_t wifi_prov_start(void);

/** 异步请求启动配网（供 esp_timer 回调使用：不在定时器回调里做 WiFi 重活） */
void wifi_prov_request_start(void);

/** 异步请求关闭配网热点并切回 STA（连接成功后、或配网期间 STA 自行连上时用） */
void wifi_prov_request_stop(void);

/** 创建配网后台任务（进程内调用一次即可；处理异步请求与延迟关热点） */
void wifi_prov_task_start(void);

/** 配网模式是否正在进行（热点与页面仍开着） */
bool wifi_prov_is_active(void);

wifi_prov_state_t wifi_prov_state(void);

/** 当前热点名（形如 RAN-7A44A0）；未起热点时返回空串 */
const char *wifi_prov_ap_ssid(void);

/** 最近一次连接失败的说明文字（页面与日志共用） */
const char *wifi_prov_last_error(void);
