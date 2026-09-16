/**
 * wifi_prov.c —— 零配置入网实现
 *
 * 用户视角：
 *   设备第一次上电（或换了路由器连不上），起一个开放热点「RAN-xxxxxx」，
 *   手机连上后浏览器打开 192.168.4.1（内置 DNS 把任意域名都指到这里，
 *   所以多数手机会自动弹出配网页），扫描/选择 WiFi、填密码、保存 →
 *   设备立即尝试连接，成功就关热点回到正常采集，失败则留在页面让用户重试。
 *
 * 工程约束（延续本工程既有原则）：
 *  - 配网不改动已保存的旧凭据；保存新网络时才覆盖（用户明确要求）。
 *  - AP 与 STA 并存（WIFI_MODE_APSTA），配网期间录音/按键/OLED 不受影响。
 *  - 定时器回调只置标志 + 唤醒本模块任务；扫描/连接等待都在 httpd 任务里做，
 *    绝不在事件回调或定时器回调里阻塞。
 *  - 配网页面来自 EMBED_TXTFILES（见 main/CMakeLists.txt）。
 */
#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "lwip/sockets.h"

#include "device_config.h"
#include "wifi_cred.h"
#include "wifi_mgr.h"

static const char *TAG = "wifi_prov";

/* bool 型 Kconfig 关闭时宏不会定义，这里补成 0，方便直接当条件用 */
#ifndef CONFIG_RAN_PROV_ENABLE
#define CONFIG_RAN_PROV_ENABLE 0
#endif

extern const uint8_t wifi_prov_html_start[] asm("_binary_wifi_prov_html_start");

#define PROV_AP_CHANNEL      1
#define PROV_AP_MAX_STA      4
#define PROV_CONNECT_WAIT_MS (CONFIG_RAN_PROV_CONNECT_TIMEOUT_S * 1000)
#define PROV_STOP_DELAY_MS   1500   /* 连接成功后留出时间把响应发回浏览器再关热点 */
#define PROV_SCAN_MAX        20

static httpd_handle_t s_httpd;
static TaskHandle_t   s_task;
static TaskHandle_t   s_dns_task;
static esp_netif_t   *s_ap_netif;
static volatile bool  s_dns_run;
static volatile bool  s_start_req;
static volatile bool  s_stop_req;
static volatile wifi_prov_state_t s_state = WIFI_PROV_NONE;
static char s_ap_ssid[33];
static char s_last_err[128];
static uint8_t s_ap_ip[4] = {192, 168, 4, 1};
/* DNS 收发缓冲放静态区：prov_dns 任务栈本来就小，函数里再放 2 个 512B 局部数组会直接穿栈
   （v2.5.0 实机首烧即 "A stack overflow in task prov_dns has been detected" + 重启循环）。
   同一时刻只会有一个 dns_task（wifi_prov_start 里有 s_state 重入保护），静态缓冲安全。 */
static uint8_t s_dns_rx[512];
static uint8_t s_dns_tx[512];

static void json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    if (!src) src = "";
    for (const char *p = src; *p && o + 2 < dst_size; ++p) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = *p;
        } else if ((unsigned char)*p < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

static void make_ap_ssid(void)
{
    if (s_ap_ssid[0]) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X",
             CONFIG_RAN_PROV_AP_PREFIX, mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/* 配网页 HTTP 接口                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static const char *state_str(void)
{
    switch (s_state) {
    case WIFI_PROV_ACTIVE:     return "active";
    case WIFI_PROV_CONNECTING: return "connecting";
    case WIFI_PROV_SUCCESS:    return "success";
    default:                   return "none";
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    char ap_esc[48], err_esc[160], body[384];
    json_escape(ap_esc, sizeof(ap_esc), s_ap_ssid);
    json_escape(err_esc, sizeof(err_esc), s_last_err);
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"ap_ssid\":\"%s\",\"device_id\":\"%s\",\"fw\":\"%s\","
             "\"ip\":\"%s\",\"err\":\"%s\"}",
             state_str(), ap_esc, device_config_get()->device_id,
             CONFIG_RAN_FW_VERSION, wifi_mgr_get_ip(), err_esc);
    return send_json(req, body);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(err));
        return send_json(req, "{\"aps\":[]}");
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 32) num = 32;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(num ? num : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        return send_json(req, "{\"aps\":[]}");
    }
    esp_wifi_scan_get_ap_records(&num, recs);

    static char body[3072];
    int o = snprintf(body, sizeof(body), "{\"aps\":[");
    int emitted = 0;
    /* 同名只留最强的那一个；按信号从强到弱冒泡排序（数量很小，够用） */
    for (int i = 0; i < (int)num; ++i) {
        for (int j = i + 1; j < (int)num; ++j) {
            if (recs[j].rssi > recs[i].rssi) {
                wifi_ap_record_t t = recs[i];
                recs[i] = recs[j];
                recs[j] = t;
            }
        }
    }
    for (int i = 0; i < (int)num && emitted < PROV_SCAN_MAX; ++i) {
        char ssid[40];
        json_escape(ssid, sizeof(ssid), (const char *)recs[i].ssid);
        if (ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < i; ++j) {
            if (strcmp((const char *)recs[j].ssid, (const char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        int need = snprintf(body + o, sizeof(body) - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           emitted ? "," : "", ssid, recs[i].rssi,
                           recs[i].authmode == WIFI_AUTH_OPEN ? 0 : 1);
        if (need <= 0 || o + need >= (int)sizeof(body) - 4) break;
        o += need;
        emitted++;
    }
    free(recs);
    snprintf(body + o, sizeof(body) - o, "]}");

    /* 扫描会把射频拉到各信道走一圈，连在热点上的手机会瞬断一下（正常现象）。
       STA 没连路由器时（配网期间的常态）把信道拉回热点信道，让手机尽快回来。 */
    if (!wifi_mgr_is_connected()) {
        esp_wifi_set_channel(PROV_AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
    return send_json(req, body);
}

/* 解析 application/x-www-form-urlencoded 里的一个字段（支持 %XX 与 +） */
static bool form_field(const char *body, const char *key, char *out, size_t out_size)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t nlen = (size_t)(eq - p);
        const char *amp = strchr(eq + 1, '&');
        size_t vlen = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);

        if (nlen == klen && strncmp(p, key, klen) == 0) {
            size_t o = 0;
            for (size_t i = 0; i < vlen && o + 1 < out_size; ++i) {
                char c = eq[1 + i];
                if (c == '+') {
                    out[o++] = ' ';
                } else if (c == '%' && i + 2 < vlen) {
                    char hex[3] = { eq[1 + i + 1], eq[1 + i + 2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    out[o++] = c;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (out_size) out[0] = '\0';
    return false;
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        return send_json(req, "{\"ok\":false,\"err\":\"表单内容异常\"}");
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            return send_json(req, "{\"ok\":false,\"err\":\"读取表单失败\"}");
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[WIFI_CRED_SSID_MAX] = {0};
    char pass[WIFI_CRED_PASS_MAX] = {0};
    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return send_json(req, "{\"ok\":false,\"err\":\"WiFi 名称不能为空\"}");
    }
    form_field(body, "password", pass, sizeof(pass));

    char ssid_esc[80];
    json_escape(ssid_esc, sizeof(ssid_esc), ssid);
    ESP_LOGI(TAG, "用户在配网页提交：ssid=%s，密码 %u 字节", ssid, (unsigned)strlen(pass));

    s_state = WIFI_PROV_CONNECTING;
    s_last_err[0] = '\0';

    esp_err_t err = wifi_mgr_connect_with(ssid, pass, PROV_CONNECT_WAIT_MS);
    if (err == ESP_OK) {
        s_state = WIFI_PROV_SUCCESS;
        s_last_err[0] = '\0';
        char resp[192];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\",\"ssid\":\"%s\"}",
                 wifi_mgr_get_ip(), ssid_esc);
        ESP_LOGI(TAG, "配网成功：已连上 %s，IP %s，热点即将关闭", ssid, wifi_mgr_get_ip());
        esp_err_t r = send_json(req, resp);
        s_stop_req = true;                       /* 由配网任务延迟关热点 */
        if (s_task) xTaskNotifyGive(s_task);
        return r;
    }

    s_state = WIFI_PROV_ACTIVE;
    snprintf(s_last_err, sizeof(s_last_err), "%s", wifi_mgr_last_error_text());
    char err_esc[200];
    json_escape(err_esc, sizeof(err_esc), s_last_err);
    ESP_LOGW(TAG, "配网失败：%s（热点保持开启，等用户重试）", s_last_err);
    char resp[288];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"%s\"}", err_esc);
    return send_json(req, resp);
}

static esp_err_t h_get(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)wifi_prov_html_start, HTTPD_RESP_USE_STRLEN);
    }
    if (strcmp(uri, "/status") == 0) return h_status(req);
    if (strcmp(uri, "/scan") == 0)   return h_scan(req);

    /* 其余任意路径（手机自带的 /generate_204、/hotspot-detect.html 等连通性探测）
       一律 302 到配网页，多数手机会据此自动弹出「登录网络」界面。 */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" CONFIG_RAN_PROV_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* DNS 劫持：把任意域名解析到配网热点网关                              */
/* ------------------------------------------------------------------ */

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t *buf  = s_dns_rx;   /* 缓冲在静态区，见文件头 s_dns_rx 说明 */
    uint8_t *resp = s_dns_tx;
    struct sockaddr_in from;
    socklen_t flen;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket 创建失败");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int bc = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS 端口 53 绑定失败（劫持关闭）");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS 劫持已就绪：任意域名 → %u.%u.%u.%u",
             s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);

    while (s_dns_run) {
        flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;

        /* 跳过 question 的域名，拿到 qtype */
        int q = 12;
        while (q < n && buf[q] != 0) {
            if ((buf[q] & 0xC0) == 0xC0) { q += 2; break; }
            q += buf[q] + 1;
        }
        if (q >= n) continue;
        int q_end = q + 1 + 4;
        if (q_end > n) continue;
        uint16_t qtype = (uint16_t)((buf[q + 1] << 8) | buf[q + 2]);

        memcpy(resp, buf, q_end);
        resp[2] = 0x81;   /* QR=1 RD=1 RA=1 */
        resp[3] = 0x80;
        int len = q_end;
        if (qtype == 1) {                                  /* A 查询 → 回配网页地址 */
            resp[6] = 0x00; resp[7] = 0x01;                /* ANCOUNT = 1 */
            resp[len++] = 0xC0; resp[len++] = 0x0C;
            resp[len++] = 0x00; resp[len++] = 0x01;        /* TYPE A */
            resp[len++] = 0x00; resp[len++] = 0x01;        /* CLASS IN */
            resp[len++] = 0x00; resp[len++] = 0x00;
            resp[len++] = 0x00; resp[len++] = 0x3C;        /* TTL 60s */
            resp[len++] = 0x00; resp[len++] = 0x04;        /* RDLENGTH 4 */
            resp[len++] = s_ap_ip[0]; resp[len++] = s_ap_ip[1];
            resp[len++] = s_ap_ip[2]; resp[len++] = s_ap_ip[3];
        } else {
            resp[6] = 0x00; resp[7] = 0x00;                /* AAAA 等：空应答，客户端回退 A */
        }
        sendto(sock, resp, len, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns_task = NULL;
    ESP_LOGI(TAG, "DNS 劫持已停止");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 启停                                                                */
/* ------------------------------------------------------------------ */

static void stop_httpd(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        ESP_LOGI(TAG, "配网页服务器已停止");
    }
}

static void stop_dns(void)
{
    if (s_dns_task) {
        s_dns_run = false;
        /* 任务自己 1 秒内退出并释放句柄 */
        for (int i = 0; i < 20 && s_dns_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static esp_err_t start_httpd(void)
{
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.stack_size       = 6144;      /* 扫描 + 连接等待都在这里跑，栈给足 */
    hc.lru_purge_enable = true;
    hc.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网页服务器启动失败: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }
    httpd_uri_t get_any = {
        .uri = "/*", .method = HTTP_GET, .handler = h_get, .user_ctx = NULL,
    };
    httpd_uri_t post_save = {
        .uri = "/save", .method = HTTP_POST, .handler = h_save, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &get_any);
    httpd_register_uri_handler(s_httpd, &post_save);
    return ESP_OK;
}

esp_err_t wifi_prov_start(void)
{
    if (!CONFIG_RAN_PROV_ENABLE) return ESP_ERR_NOT_SUPPORTED;
    if (s_state == WIFI_PROV_ACTIVE || s_state == WIFI_PROV_CONNECTING) return ESP_OK;

    make_ap_ssid();

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "创建 AP 网络接口失败");
            return ESP_FAIL;
        }
        /* DNS 劫持的目标地址取 AP 网卡的真实 IP（sdkconfig 若改过默认网关也不会指错） */
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            char ip_str[16] = {0};
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                s_ap_ip[0] = (uint8_t)a; s_ap_ip[1] = (uint8_t)b;
                s_ap_ip[2] = (uint8_t)c; s_ap_ip[3] = (uint8_t)d;
            }
            if (strcmp(ip_str, CONFIG_RAN_PROV_AP_IP) != 0) {
                ESP_LOGW(TAG, "热点实际地址 %s 与 RAN_PROV_AP_IP(%s) 不一致，请改成一致",
                         ip_str, CONFIG_RAN_PROV_AP_IP);
            }
        }
    }

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ap.ap.ssid_len        = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel         = PROV_AP_CHANNEL;
    ap.ap.max_connection  = PROV_AP_MAX_STA;
    ap.ap.beacon_interval = 200;
    ap.ap.pmf_cfg.required = false;
    const char *ap_pass = CONFIG_RAN_PROV_AP_PASSWORD;
    if (ap_pass && ap_pass[0]) {
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", ap_pass);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "开启配网热点：%s（%s）", s_ap_ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "开放，无密码" : "WPA2");

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换 APSTA 模式失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "下发 AP 配置失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        /* 无凭据路径下 WiFi 可能已经跑起来，这里只提示不致命 */
        ESP_LOGW(TAG, "esp_wifi_start: %s（若已运行可忽略）", esp_err_to_name(err));
    }

    if (start_httpd() != ESP_OK) {
        ESP_LOGW(TAG, "配网页不可用，但热点仍在，可重新上电重试");
    }
    s_dns_run = true;
    /* 绑到 core 1：本工程音频采集任务在 core 0，配网任务别去挤它 */
    if (xTaskCreatePinnedToCore(dns_task, "prov_dns", 4096, NULL, 4, &s_dns_task, 1) != pdPASS) {
        s_dns_task = NULL;
        s_dns_run = false;
        ESP_LOGW(TAG, "DNS 劫持任务创建失败（可手动访问 http://%s ）", CONFIG_RAN_PROV_AP_IP);
    }

    s_state = WIFI_PROV_ACTIVE;
    s_last_err[0] = '\0';
    ESP_LOGW(TAG, "==== 配网模式已开启 ====");
    ESP_LOGW(TAG, " 1) 手机连接 WiFi：%s （无密码）", s_ap_ssid);
    ESP_LOGW(TAG, " 2) 浏览器打开：http://%s （多数手机会自动弹出配网页）", CONFIG_RAN_PROV_AP_IP);
    ESP_LOGW(TAG, " 3) 选择网络 + 填密码 → 保存后设备立即连接");
    ESP_LOGW(TAG, " 旧 WiFi 记录不会被删除，只有保存新网络时才覆盖");
    return ESP_OK;
}

static void prov_stop_impl(void)
{
    stop_httpd();
    stop_dns();
    s_state = WIFI_PROV_NONE;

    /* 切回纯 STA：热点必须消失，STA 由 wifi_mgr 的既有重连逻辑接管 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切回 STA 模式失败: %s（设备可能仍需重启一次）", esp_err_to_name(err));
        return;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA) {
        /* 个别情况下 set_mode 没真正切过去，硬来一次：停掉再按 STA 起 */
        ESP_LOGW(TAG, "当前 WiFi 模式仍为 %d，重启 WiFi 驱动切 STA", (int)mode);
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    } else if (!wifi_mgr_is_connected()) {
        esp_wifi_connect();     /* 热点关掉后确保 STA 继续尝试连接 */
    }
    ESP_LOGI(TAG, "配网结束：热点已关闭，切回 STA 模式继续运行");
}

void wifi_prov_request_start(void)
{
    s_start_req = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void wifi_prov_request_stop(void)
{
    s_stop_req = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void prov_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_start_req) {
            s_start_req = false;
            wifi_prov_start();
        }
        if (s_stop_req) {
            s_stop_req = false;
            vTaskDelay(pdMS_TO_TICKS(PROV_STOP_DELAY_MS));   /* 让 /save 的响应先发出去 */
            /* 关热点的前提：已经连上（用户刚保存成功、或配网期间 STA 自行连上）。
               若刚保存完就又掉线，说明这次连接并不稳，留着热点让用户重试更靠谱。 */
            bool can_stop = (s_state == WIFI_PROV_SUCCESS) ||
                            (s_state == WIFI_PROV_NONE) ||
                            wifi_mgr_is_connected();
            if (can_stop) {
                prov_stop_impl();
            } else {
                ESP_LOGW(TAG, "请求关闭配网，但当前仍无 WiFi 连接 → 保留热点等用户重试");
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    }
}

void wifi_prov_task_start(void)
{
    if (s_task) return;
    /* 绑到 core 1，原因同 DNS 任务 */
    if (xTaskCreatePinnedToCore(prov_task, "wifi_prov", 5120, NULL, 5, &s_task, 1) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "配网任务创建失败");
    }
}

bool wifi_prov_is_active(void)
{
    return s_state == WIFI_PROV_ACTIVE || s_state == WIFI_PROV_CONNECTING;
}

wifi_prov_state_t wifi_prov_state(void)
{
    return s_state;
}

const char *wifi_prov_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_prov_last_error(void)
{
    return s_last_err;
}
