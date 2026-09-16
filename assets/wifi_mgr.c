/**
 * wifi_mgr.c —— WiFi STA + SNTP + 「连不上自动转网页配网」
 *
 * v2.3：断线重连不再在事件回调里 vTaskDelay(2000) —— 那会把默认事件循环（所有事件
 * 回调串行跑在同一任务上）整体阻塞 2 秒，掉线期间 IP、SNTP、其它回调全部顺延。
 * 改成一次性 esp_timer 延时 2s 再 esp_wifi_connect()，事件循环立即返回。
 *
 * v2.5：新增配网联动（用户需求：没配过 WiFi 就起热点，让手机打开网页填 SSID/密码）
 *  - 凭据来源优先级：NVS（配网页保存的） > 编译期默认值（Kconfig，仍为占位值 "myssid" 视为没配）
 *  - 两条都没有 → 开机直接进配网模式
 *  - 有凭据但开机 CONFIG_RAN_PROV_FALLBACK_S 秒内没拿到 IP → 自动进配网
 *  - 配网页提交后由 wifi_mgr_connect_with() 配置 STA 并等待结果（失败原因回给页面）
 *  - 【重要】进配网不删除 NVS 里的旧凭据；只有用户保存新网络时才覆盖
 *  - 配网模式下 STA 仍在后台重连：路由器一旦恢复就自动连上并关掉热点
 */
#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "wifi_cred.h"
#include "wifi_prov.h"
#include "ws_client.h"

static const char *TAG = "wifi_mgr";

#define BIT_CONNECTED  BIT0

/* 掉线后等多久再 esp_wifi_connect()（毫秒）。给 AP 一点时间收尾，
 * 也避免"一断就抢连"把路由器拉黑。 */
#define WIFI_RECONNECT_DELAY_MS  2000
/* 配网模式下还没连上过时，重连慢一点：日志干净，也少抢信道影响用户手机操作 */
#define WIFI_RECONNECT_DELAY_PROV_MS  10000

/* Kconfig 里未改动的占位 SSID：视为"没配过 WiFi" */
#define WIFI_SSID_PLACEHOLDER  "myssid"

/* 原因码 210（找到同名 AP 但加密方式不兼容）在 IDF v4.4+ 才有符号名，
 * 各版本名字略有出入，这里直接用数值，避免换 IDF 版本时编译不过。 */
#define PROV_REASON_NO_AP_COMPAT_SECURITY  210

static EventGroupHandle_t s_evt;
static char s_ip_str[16] = "0.0.0.0";
static char s_ssid[WIFI_CRED_SSID_MAX];
static char s_err_text[160];
static volatile bool s_time_synced;
static volatile bool s_ever_connected;
static volatile bool s_prov_triggered;
static volatile int  s_last_disc_reason;
static esp_timer_handle_t s_reconn_timer;
static esp_timer_handle_t s_link_timer;   /* 开机连不上 → 转配网的兜底计时器 */

static void reconn_timer_cb(void *arg)
{
    (void)arg;
    if (!wifi_mgr_is_connected()) esp_wifi_connect();
}

/* 开机 CONFIG_RAN_PROV_FALLBACK_S 秒还没拿到 IP：开热点让用户重配。
 * 回调里只置标志 + 唤醒配网任务，WiFi 重活交给配网任务做。 */
static void link_timer_cb(void *arg)
{
    (void)arg;
    if (wifi_mgr_is_connected() || s_prov_triggered) return;
    s_prov_triggered = true;
    ESP_LOGW(TAG, "开机 %d 秒仍未连上 WiFi（最后原因码 %d）→ 自动开启配网热点",
             CONFIG_RAN_PROV_FALLBACK_S, s_last_disc_reason);
    wifi_prov_request_start();
}

/* 连上之后打一行信号质量，现场排查"是不是离路由器太远/信道太挤"用 */
static void log_ap_rssi(const char *stage)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "%s：RSSI %d dBm，信道 %d", stage, ap.rssi, ap.primary);
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "获取 IP: %s（DNS 就绪，WS 可以开始建连）", s_ip_str);
    xEventGroupSetBits(s_evt, BIT_CONNECTED);
    s_ever_connected = true;
    if (s_link_timer) esp_timer_stop(s_link_timer);
    log_ap_rssi("连上路由器");

    /* 配网过程中路由器恢复了（比如本来就是路由器重启）：自动收摊关热点 */
    if (wifi_prov_is_active()) {
        ESP_LOGI(TAG, "配网期间 WiFi 已自行连上 → 关闭配网热点");
        wifi_prov_request_stop();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 启动，开始连接 %s ...", s_ssid);
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        int reason = d ? d->reason : -1;
        s_last_disc_reason = reason;
        xEventGroupClearBits(s_evt, BIT_CONNECTED);
        snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");

        bool waiting_user = (s_prov_triggered && !s_ever_connected);
        uint32_t delay_ms = waiting_user ? WIFI_RECONNECT_DELAY_PROV_MS : WIFI_RECONNECT_DELAY_MS;
        if (waiting_user) {
            ESP_LOGI(TAG, "WiFi 未连上 (reason=%d)，%u ms 后重试（配网模式中）",
                     reason, (unsigned)delay_ms);
        } else {
            ESP_LOGW(TAG, "WiFi 断开 (reason=%d)，%u ms 后重连", reason, (unsigned)delay_ms);
        }

        /* v2.5.2：WiFi 链路断了就立刻告诉 WS 模块 —— 否则底层 socket 已经死掉、
         * 而 WS 模块还留着"已连接"状态，于是永不重连（现场表现：开关 WiFi 后设备
         * 一直连不上，只有断电重启才能恢复）。这里只置标志，清状态/停客户端这类
         * 会阻塞的动作交给 ws_mgr 任务做，事件回调立即返回。 */
        ws_client_notify_link_down();

        /* v2.3：延时重连走定时器，不在回调里阻塞（见文件头说明） */
        if (s_reconn_timer) {
            esp_timer_stop(s_reconn_timer);
            esp_timer_start_once(s_reconn_timer, (uint64_t)delay_ms * 1000);
        } else {
            esp_wifi_connect();
        }

        /* 连续 CONFIG_RAN_PROV_FALLBACK_S 秒没有 IP（开机起算，或运行中断网起算）→ 转配网。
           路由器恢复、GOT_IP 时这个计时器会被停掉（on_got_ip）。 */
        if (CONFIG_RAN_PROV_FALLBACK_S > 0 && s_link_timer) {
            esp_timer_stop(s_link_timer);
            esp_timer_start_once(s_link_timer, (uint64_t)CONFIG_RAN_PROV_FALLBACK_S * 1000000ULL);
        }
    }
}

static void sntp_synced_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    ESP_LOGI(TAG, "SNTP 时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
}

static void start_sntp(void)
{
    setenv("TZ", "CST-8", 1);   /* 中国标准时间 UTC+8（录音目录按本地时间命名） */
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_set_sync_interval(3600 * 1000);
    esp_sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 已启动，等待时间同步...");
}

static void apply_sta_creds(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass ? pass : "");
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

esp_err_t wifi_mgr_init(void)
{
    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    const esp_timer_create_args_t reconn_timer_args = {
        .callback = reconn_timer_cb,
        .name = "wifi_reconn",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconn_timer_args, &s_reconn_timer));

    const esp_timer_create_args_t link_timer_args = {
        .callback = link_timer_cb,
        .name = "wifi_link_to",
    };
    ESP_ERROR_CHECK(esp_timer_create(&link_timer_args, &s_link_timer));

    /* 配网后台任务先建好：无论走哪条路，60s 超时那套都要用得上 */
    wifi_prov_task_start();

    /* ---- 凭据来源：NVS 优先，其次编译期默认值 ---- */
    char ssid[WIFI_CRED_SSID_MAX] = {0};
    char pass[WIFI_CRED_PASS_MAX] = {0};
    bool have = (wifi_cred_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) && ssid[0] != '\0';
    if (have) {
        ESP_LOGI(TAG, "使用已保存的 WiFi 凭据：%s", ssid);
    } else {
        const char *kc_ssid = CONFIG_RAN_WIFI_SSID;
        if (kc_ssid && kc_ssid[0] && strcmp(kc_ssid, WIFI_SSID_PLACEHOLDER) != 0) {
            snprintf(ssid, sizeof(ssid), "%s", kc_ssid);
            snprintf(pass, sizeof(pass), "%s", CONFIG_RAN_WIFI_PASSWORD);
            have = true;
            ESP_LOGI(TAG, "使用编译期默认 WiFi（Kconfig）：%s", ssid);
        }
    }

    if (have) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        apply_sta_creds(ssid, pass);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        /* 连不上 CONFIG_RAN_PROV_FALLBACK_S 秒 → 自动进配网（用户确认的行为）。
           运行中掉线也会由 on_wifi_event 重新武装这个计时器。 */
        if (CONFIG_RAN_PROV_FALLBACK_S > 0) {
            esp_timer_start_once(s_link_timer, (uint64_t)CONFIG_RAN_PROV_FALLBACK_S * 1000000ULL);
            ESP_LOGI(TAG, "STA 已启动，%d 秒内连不上会自动开启配网热点",
                     CONFIG_RAN_PROV_FALLBACK_S);
        } else {
            ESP_LOGI(TAG, "STA 已启动（自动转配网已关闭：RAN_PROV_FALLBACK_S=0）");
        }
    } else {
        ESP_LOGW(TAG, "没有可用的 WiFi 凭据 → 直接进入配网模式");
        s_prov_triggered = true;
        wifi_prov_start();     /* 同步起热点，保证 main 里判断状态时已生效 */
    }

    start_sntp();
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return s_evt && (xEventGroupGetBits(s_evt) & BIT_CONNECTED);
}

const char *wifi_mgr_get_ip(void)
{
    return s_ip_str;
}

bool wifi_mgr_time_synced(void)
{
    return s_time_synced;
}

bool wifi_mgr_has_credentials(void)
{
    if (wifi_cred_exists()) return true;
    const char *kc_ssid = CONFIG_RAN_WIFI_SSID;
    return kc_ssid && kc_ssid[0] && strcmp(kc_ssid, WIFI_SSID_PLACEHOLDER) != 0;
}

bool wifi_mgr_is_provisioning(void)
{
    return s_prov_triggered && wifi_prov_is_active();
}

bool wifi_mgr_ever_connected(void)
{
    return s_ever_connected;
}

esp_err_t wifi_mgr_connect_with(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_err_t err = wifi_cred_save(ssid, pass);   /* 新设置覆盖旧记录（用户确认的行为） */
    if (err != ESP_OK) {
        snprintf(s_err_text, sizeof(s_err_text), "保存 WiFi 记录失败：%s", esp_err_to_name(err));
        return err;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_last_disc_reason = 0;
    s_err_text[0] = '\0';
    xEventGroupClearBits(s_evt, BIT_CONNECTED);
    snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");

    apply_sta_creds(ssid, pass);

    esp_wifi_disconnect();      /* 清掉上一次尝试（未连接时返回错误，忽略即可） */
    vTaskDelay(pdMS_TO_TICKS(100));
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONNECTED) {
        s_ever_connected = true;
        if (s_link_timer) esp_timer_stop(s_link_timer);
        s_prov_triggered = false;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "按新凭据连接超时（%u ms，最后原因码 %d）",
             (unsigned)timeout_ms, s_last_disc_reason);
    return ESP_ERR_TIMEOUT;
}

const char *wifi_mgr_last_error_text(void)
{
    const char *hint;
    switch (s_last_disc_reason) {
    case WIFI_REASON_NO_AP_FOUND:
        hint = "没找到这个名字的 WiFi（路由器不在范围内，或者名字打错了）";
        break;
    case WIFI_REASON_AUTH_FAIL:
        hint = "认证失败：密码不对";
        break;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        hint = "密码校验超时：多半是密码不对";
        break;
    case WIFI_REASON_AUTH_EXPIRE:
        hint = "路由器没有响应认证（密码错误或信号太差）";
        break;
    case WIFI_REASON_ASSOC_FAIL:
        hint = "路由器拒绝关联（信号太差或路由器有限制）";
        break;
    case WIFI_REASON_CONNECTION_FAIL:
        hint = "路由器拒绝连接";
        break;
    case PROV_REASON_NO_AP_COMPAT_SECURITY:
        hint = "周围有这个 WiFi，但加密方式不被支持（把路由器改成 WPA2 再试）";
        break;
    default:
        hint = s_last_disc_reason ? "连接失败" : "连接超时：没收到路由器响应";
        break;
    }
    snprintf(s_err_text, sizeof(s_err_text), "%s（原因码 %d）", hint, s_last_disc_reason);
    return s_err_text;
}

int wifi_mgr_last_disconnect_reason(void)
{
    return s_last_disc_reason;
}
