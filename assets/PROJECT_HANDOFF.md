# remote_audio_node - Project Handoff

本文档是给接手维护本固件的开发者（或另一位 Agent）阅读的完整交接说明。
阅读完本文件 + 项目源码，即可在不查阅任何外部对话历史的情况下理解、
修改、扩展本项目，并能与已完成的服务端 `remoteAudioServer` 联调。

---

## 1. 项目概述

- **项目名**：remote_audio_node
- **用途**：ESP32-S3 多设备音频采集系统**采集端固件**。与服务端
  [remoteAudioServer](../../remoteAudioServer) 通过 WebSocket 配合，
  实现：
  - 本地 SD 卡 Ogg Opus 录音（VAD 智能录音，可远程开关）
  - 实时 PCM 音频流推送到服务端（订阅驱动：有监听者才推流）
  - 服务端远程浏览/回放设备本地录音、调整录音开关与配置

> **2026-09-14 变更摘要**（目标是让固件能在实战派 S3 上真跑起来）：
> ① ES7210 驱动改用官方组件 `espressif/es7210`，地址 0x40 修正为 **0x41** 并做
> 0x40~0x43 自适应探测；② 删除会抢 I2S0 时钟的 TX 占位通道（`RAN_ENABLE_I2S_TX`）；
> ③ 本工程驱动改名 `es7210_drv.*` 避开官方同名头文件；④ I2S DMA 缓冲 24ms→120ms、
> 超时单位修正、短读补零；⑤ ES7210 初始化失败改为软失败，不再整机 abort。
> ⑥ 默认服务器改为扣子云上部署的 `remoteaudio.coze.site`（`wss://` + 443 + CA bundle
> 校验，建连前先等 SNTP 对时），局域网模式仍可通过 menuconfig 切回 `ws://ip:8000`。
> ⑦ 调大 WebSocket 握手响应头缓冲（`CONFIG_WS_BUFFER_SIZE=4096`）：Coze 网关的 101
> 响应头约 980 字节，超过 IDF 默认 1024 的 ws 传输缓冲，握手会报
> `transport_ws: Header size exceeded buffer size`。
> ⑧ `opus_enc` 任务栈 16KB→32KB（真机第一次 `opus_encode()` 就栈溢出重启）。
> ⑨ **v2.1 诊断加固**：新增独立 `diag` 心跳任务（每 10s 一行：连接/推流/堆/发送统计）；
> ws 发送全部改为**有界取锁**（500ms，超时丢帧并告警）、单次发送 >300ms 打 `慢发送`
> 警告、发送失败计数；`WEBSOCKET_EVENT_ERROR` 打出 TLS/errno 细节；PCM 帧缓冲改静态
> 预分配。起因是一次"服务端反复开关监听后设备掉线、串口日志突然静止"的现场无法判读
> （详见 10.20）。详见第 10 节各条。
> ⑩ **v2.2 长连接保活 + 重连提速**：① WS 客户端改为发 **协议级 PING 帧**
> （`ping_interval_sec=10`），修掉「外网长连接约 300s 被平台接入层掐断」（实测 300s → >425s）；
> ② 重连退避 1s/30s → **300ms/3s**，断线后 1 秒内重新连上；③ 新增 `s_want_stream` 意图，
> 重连成功重发 hello 后**自动续流**，用户不必重新点「开始监听」；④ 重连日志带「第 N 次 +
> 退避毫秒数」。协议、端口、帧布局一律未动（详见 10.21）。
> ⑪ **v2.3 连接可靠性加固（修「v2.2 连不上」）**：① 显式 `.disable_auto_reconnect = true`，
> 关掉组件自带自动重连（`reconnect_timeout_ms=0` 在 1.x 各版本都只是回落默认 10s，**不等于
> 禁用**），重连只留 `ws_task()` 一条路径；② 建连前加「Wi-Fi 已拿到 IP」门，消除
> `getaddrinfo() returns 202` / `EAI_FAIL` 抢跑；③ 检查 `esp_websocket_client_start()` 返回值
> （组件在 `state>=INIT` 时返回 `ESP_FAIL`，以前被静默忽略 → 一直连不上）；④ 连续失败 6 次
> 就 `destroy+init` 重建客户端（自愈）；⑤ `wifi_mgr.c` 断线回调不再 `vTaskDelay(2000)`
> 阻塞事件循环（改一次性 `esp_timer`），并加 RSSI/信道日志。协议、端口、帧布局依旧零改动
> （详见 10.22）。
> ⑫ **v2.4 空推流治理（别对着空气推流）**：① 重连后不再凭本地记忆续流，发完 hello 先等最多
> 3s 的服务端裁决（`start_stream`→续流 / `stop_stream`→不推 / 超时或没消息→**也不推**）；
> ② 读下行心跳的 `listeners`，连续 3 条为 0 且正在推流则自动停流（兜底看门狗）；
> ③ 服务端侧配套：hello 后按真实 `listeners` 复盘下发裁决、心跳带 `listeners` 字段。
> 协议帧与 JSON 字段依旧零改动（复用既有 `start_stream`/`stop_stream`/`hb`，详见 10.23）。
> ⑬ **v2.5 网页配网（零配置入网）**：① 新增 `wifi_cred.c/h`（NVS namespace `wifi` 存
> ssid/pass，独立于 `ran` 那份设备配置）；② 新增 `wifi_prov.c/h` + `wifi_prov.html`
> （`WIFI_MODE_APSTA` 开放热点 `RAN-<MAC后3>` + `esp_http_server` + DNS 劫持实现
> captive portal + 扫描/保存/同步连接等待）；③ `wifi_mgr.c` 改三级凭据优先级
> （NVS → Kconfig → 配网）并加 60s 连不上自动转配网、失败原因中文文案；
> ④ `main.c` 配网期间先等配网再启动 WS，`diag` 心跳提前到 WS 之前。
> **服务端协议零改动**，仍是既有 hello/ping/start_stream（详见 10.24、README 3.1）。

- **固件版本**：2.5.2（网页配网 + 连接状态一致性；协议字段与 2.0.0~2.4.0 完全一致）
  - 入网路径（2.5.0/2.5.1）：`wifi_cred.c` 存凭据、`wifi_prov.c` 起 APSTA 热点 + 配网页 +
    DNS 劫持、`wifi_mgr.c` 三级凭据优先级与 60s 兜底转配网，见 10.24
  - 连接状态一致性（2.5.2）：WiFi 掉线主动通知 WS 模块清状态；连接判定交叉核对组件真实状态；
    ping / 静默看门狗判死链。见 10.25
- **目标硬件**：立创实战派 ESP32-S3（LCKFB SZPI，模组 ESP32-S3-WROOM-1 N16R8，
  16MB Flash / 8MB 八线 PSRAM）。板载 ES7210 音频 ADC + TF 卡，无需外接麦克风。
  （早期版本目标是 DevKitC-1 + INMP441，已整体切换）
- **对应服务端版本**：remoteAudioServer v2.0.0（协议完全对齐）
- **开发框架**：ESP-IDF v5.1+（已在 v5.5.4 上验证编译通过）

---

## 2. 硬件接线（实战派 S3 板载）

音频与 TF 卡都是板载连接，正常使用**无需外接任何线**。

| 外设 | 信号 | ESP32-S3 GPIO | 备注 |
|---|---|---|---|
| **ES7210 音频 ADC**（I2S RX 输入） | I2C SDA | GPIO1 | I2C0，100kHz |
| | I2C SCL | GPIO2 | |
| | I2C 地址 | **0x41** | 板上 AD0 拉高；驱动在 0x40~0x43 自适应探测 |
| | MCLK | GPIO38 | 256 × 16kHz = 4.096MHz |
| | BCLK (SCK) | GPIO14 | I2S0 |
| | WS (LRCK) | GPIO13 | I2S0 |
| | SDOUT (SD) | GPIO12 | ES7210 → ESP32，16bit 立体声（L=MIC1, R=MIC2） |
| **TF 卡**（SDMMC 1-bit） | CLK | GPIO47 | 挂载点 `/sdcard` |
| | CMD | GPIO48 | |
| | D0 | GPIO21 | |
| **录音开关按钮** | 按键 | GPIO0（BOOT 键） | 低有效，按下录音开关切状态 |
| **状态 LED** | — | GPIO39 | |
| **OLED** | — | 未接线 | 板上无 SSD1306，`RAN_ENABLE_OLED` 默认关 |
| **播放输出** | — | 未启用 | 本固件不实现播放（见第 12 节） |

> 引脚全部定义在 `main/board_lckfb_s3.h`（`main/board.h` 是选板入口）。
> ES7210 与板载摄像头 GC0308 在 GPIO13/14/38 上冲突，本固件不启用摄像头。

---

## 3. 服务端协议对齐（最容易踩坑的部分）

协议与服务端 `remoteAudioServer` 严格逐字节对齐，**改动前务必先核对服务端协议**。

### 3.1 WebSocket 接入
- 地址：`ws://<SERVER_HOST>:<SERVER_PORT>/ws/device/<device_id>`
- device_id：默认 `esp32-` + MAC 最后 3 字节 hex（如 `esp32-aabbcc`），可 Kconfig 覆盖
- 重连：指数退避（v2.2：300ms → 3s 上限），重连后必须**重发 hello**；
  v2.2 起还会在 hello 之后按断线前的 `s_want_stream` 意图**自动恢复推流**
- 保活：v2.2 起由 WS 客户端发**协议级 PING**（每 10s；pong 超时 30s）。应用层 JSON ping
  与音频数据都不足以让平台接入层认为链路「活着」，长连接约 300s 会被掐断（见 10.21）

### 3.2 文本消息（JSON）

**设备 → 服务端**：

```jsonc
// 连接/重连成功后发送
{"type":"hello", "device_id":"esp32-aabbcc", "fw":"2.2.0",
 "recording_enabled":true, "sd_ok":true,
 "config":{"vad_threshold":1200, "retention_days":30}}

// 心跳（15 秒一次；服务端 45 秒超时判离线）
{"type":"ping", "ts":<epoch_seconds>}

// 事件上报
{"type":"event", "event":"recording_saved",
 "file":"2026-09-09/10-20-30.opus",  // 相对路径（SD 卡内的路径段）
 "size":12345, "duration_ms":8000, "duration":8}
{"type":"event", "event":"recording_state", "enabled":true}

// 命令响应
{"type":"response", "req_id":<int>, "ok":true, ...}   // 各命令返回各自字段
{"type":"response", "req_id":<int>, "ok":false, "error":"file_not_found"}
```

**服务端 → 设备**（JSON 命令）：

| cmd 字段 | 设备动作 |
|---|---|
| `{"cmd":"start_stream"}` | 开始发送 0x01 实时 PCM 帧 |
| `{"cmd":"stop_stream"}` | 停止发送 0x01 帧 |
| `{"type":"request","req_id":N,"cmd":"list_recordings"}` | 遍历 SD 录音回复 files[] |
| `{"type":"request","req_id":N,"cmd":"play_file","file":"..."}` | 打开文件分块发 0x02 帧 |
| `{"type":"request","req_id":N,"cmd":"stop_file"}` | 中止当前文件回传 |
| `{"cmd":"set_recording","enabled":true/false}` | 切录音开关并持久化 NVS |
| `{"cmd":"set_config","config":{...}}` | 更新 vad_threshold/retention_days 等并持久化 |

### 3.3 二进制帧（设备 → 服务端，固定字节布局）

```
Byte 0    : 帧类型（0x01 = 实时 PCM 广播，0x02 = 文件块）
Byte 1-4  : req_id，uint32 大端（实时帧固定 = 0；文件块 = 请求中的 req_id）
Byte 5-6  : seq，uint16 大端（文件块 seq 从 0 起递增；**末帧 seq = 0xFFFF**）
Byte 7+   : payload
            - 0x01 帧：payload 固定 640 字节（16kHz/16bit/mono/20ms 原始 PCM）
            - 0x02 帧：payload ≤ 2048 字节（Ogg Opus 文件的原始字节切片）
```

**字节布局必须与服务端 `server/protocol.py` 一致，否则文件回放失败**。

### 3.4 音频参数
- 采样率 16kHz、16bit、单声道
- 每帧 20ms = 320 samples = 640 字节 PCM
- 实时流：原 PCM 直出，**不编码**（保实时性）
- 录音：Opus 32kbps CBR → Ogg Opus 容器

---

## 4. 模块职责

| 文件 | 职责 | 关键函数 |
|---|---|---|
| `main.c` | 初始化顺序与主任务启动 | `app_main()` |
| `board.h` | 全部 GPIO 宏定义 | — |
| `device_config.c/h` | 设备 ID 生成、NVS 配置持久化 | `device_config_load/save/get_device_id()` |
| `wifi_mgr.c/h` | WiFi STA + 自动重连 + SNTP 校时 | `wifi_mgr_start()` |
| `ws_client.c/h` | WebSocket 客户端、命令分发、心跳、重连 | `ws_client_start()`；内部有文件回传任务 |
| `audio_i2s.c/h` | I2S RX 驱动（新版 i2s_std API） | `audio_i2s_init/read()` |
| `vad.c/h` | 滑动窗 RMS 能量 VAD | `vad_feed()` 返回 VOICED/SILENT |
| `opus_encoder.c/h` | Opus 薄封装（**全工程唯一 include opus.h 的文件**） | `opus_encoder_open/encode/close` |
| `ogg_muxer.c/h` | Ogg Opus 封装（RFC 7845） | `ogg_muxer_open/encode_opus_packet/close` |
| `recorder.c/h` | 录音状态机 + 编码任务 | `recorder_feed()` |
| `sd_storage.c/h` | FATFS 挂载、目录遍历、保留策略清理 | `sd_storage_mount/list/apply_retention()` |
| `audio_stream.c/h` | 实时 0x01 帧封装发送 | `audio_stream_push_pcm()` |
| `btn.c/h` | GPIO0 按键消抖 | `btn_task()` |
| `oled.c/h` | SSD1306 最小状态显示（v5.1/v5.2 驱动兼容） | `oled_init/show()` |

---

## 5. FreeRTOS 双核任务架构

**核 0 (PRO_CPU)**：
- `audio_i2s_read()` → `vad_feed()` → `recorder_feed()`（录音中才进 Opus 编码）
- I2S DMA 缓冲 320 samples × 2 = 1280 字节
- 录音状态机 + 编码线程（16KB 栈，Opus encoder 吃栈）

**核 1 (APP_CPU)**：
- WiFi、WebSocket（esp_websocket_client 自带任务）
- 文件回传任务：按 req_id 读 SD 分块发 0x02 帧，末帧 seq=0xFFFF
- 按键、OLED、SNTP

**线程安全**：
- SD 卡 FATFS 用 xSemaphore 互斥（读写同文件禁止并发）
- 实时推流与录音互不影响（两套队列独立）
- NVS 写操作用互斥锁

---

## 6. 录音状态机

```
IDLE ──(VAD: VOICED)──► RECORDING
  ▲                          │
  │                     ├─ 静音超时 1.5s → flush EOS → rename .tmp → IDLE
  │                     └─ 时长 ≥ 10min → 切片重开 RECORDING
  └──(recording_enabled=false)── 任意时刻都可切到 DISABLED 不录音
```

- 录音前滚缓冲：默认 10 帧（200ms），触发录音时把缓冲中的 PCM 一起送进编码器
- 先写 `.tmp`，录音完成后 `rename` 原子改名，避免断电阻损坏文件
- 路径：`/sdcard/recordings/YYYY-MM-DD/HH-MM-SS.opus`
- 保留策略（启动时 + 每次录音结束后执行）：
  - 剩余空间 < 200MB → 删最旧日期目录
  - 文件年龄 > retention_days（默认 30 天；0=永久） → 删对应目录
- recording_enabled=false 时：不做 VAD、不写文件，但实时推流照常工作

---

## 7. Ogg Opus Muxer 实现要点

`ogg_muxer.c` 是自研的轻量 Ogg 封装，不依赖任何第三方 Ogg 库。关键点：

- **页结构**（RFC 7845）：`OggS` magic + version=0 + header_type + granule_position(8B LE)
  + serial(4B LE) + page_seq(4B LE) + checksum(4B LE) + segment_table
- **两个头部页**：
  - OpusHead：`magic="OpusHead"`, version=1, channels=1, pre-skip=312,
    input_sample_rate=16000, gain=0, channel_mapping=0
  - OpusTags：`magic="OpusTags"`, vendor string + 0 条 user comments
- **数据页**：每个 Opus 包（20ms）封装一页；`granule_position` 按 PCM 采样数累计
  **注意：本实现使用 48kHz 单位**（每 20ms 包 +960），与 RFC7845 规范一致；
  如果服务端或播放器按 16kHz 解析时长可能偏差，验证过浏览器原生可播。
- **CRC32**：标准 Ogg CRC（多项式 0x04c11db7，查表实现）
- **EOS 页**：录音结束时写 header_type=0x04 的空包页
- 如需修改时长解析或换播放器兼容，**只改 `ogg_muxer.c` 即可**

---

## 8. 编译与依赖

### 必须安装的组件（组件管理器自动拉取）
- `78/esp-opus ^1.0.5` —— xiaozhi-esp32 作者维护的 libopus 移植
  **注意**：组件仓库里**没有** `espressif/esp-opus` 这个包（这是早期错误版本），
  包名必须写 `78/esp-opus`。
- `espressif/esp_websocket_client ^1.2.0` —— IDF v5.x 起从核心框架移到组件仓库
  **注意**：`PRIV_REQUIRES` 里必须显式依赖 `esp_websocket_client`，否则编译报
  "Failed to resolve component"。

### ES7210 音频 ADC 组件
- `espressif/es7210 ^1.0.0` —— 官方组件，只负责 I2C 写寄存器配置 ES7210，
  **I2S 数据通路仍由 `main/audio_i2s.c` 自管**（不引入 esp_codec_dev）。
- `PRIV_REQUIRES` 里要加组件名 `espressif__es7210`。
- 该组件内部用 `i2c_master_get_bus_handle()` 取用已安装的总线，因此本工程用
  传统 API（`i2c_param_config` + `i2c_driver_install`）装的总线它也能直接用。
- 本工程自己的封装叫 `main/es7210_drv.c/h`，**不能**改名成 `es7210.c/h`
  （会遮蔽官方同名头文件，见第 10.13 节）。

### I2S/GPIO/I2C/SPI 驱动组件
ESP-IDF v5.5 把新版驱动从 `driver` 拆出为独立组件（`esp_driver_i2s`/
`esp_driver_gpio`/`esp_driver_i2c`/`esp_driver_spi`），`main/CMakeLists.txt`
的 `PRIV_REQUIRES` 必须显式列出，否则新 API 头文件找不到。

### 编译步骤
```bash
cd remote_audio_node
idf.py set-target esp32s3
idf.py menuconfig     # 至少配 WiFi SSID/密码、Server host
idf.py build
idf.py -p COMx flash monitor   # Windows
```

### CMake 链接探测
`main/CMakeLists.txt` 用通配 `managed_components/*[Oo]pus*` 自动找实际组件目录名，
所以换 Opus 组件（如 `esphome/micro-opus`）**无需改 CMake**。

---

## 9. 配置项（Kconfig.projbuild）

| 宏 | 默认值 | 说明 |
|---|---|---|
| `RAN_FW_VERSION` | "2.5.2" | hello.fw 字段 |
| `RAN_WIFI_SSID` / `RAN_WIFI_PASSWORD` | myssid/mypassword | **编译期兜底值**：`RAN_WIFI_SSID` 保持 `myssid` = "没配"，设备开机直接进网页配网；填真实路由器信息则按老流程直连 |
| `RAN_PROV_ENABLE` | **y** | 网页配网总开关（关掉后无凭据时只能靠 Kconfig 硬编 WiFi） |
| `RAN_PROV_AP_PREFIX` | RAN | 热点名前缀，最终热点名 `RAN-7A44A0`（前缀 + MAC 后 3 字节） |
| `RAN_PROV_AP_PASSWORD` | 空 | 留空 = 开放热点（无密码）；填空 = WPA2 |
| `RAN_PROV_AP_IP` | 192.168.4.1 | 配网页地址，也是 DNS 劫持的指向；须与 esp_netif 默认 AP 网关一致 |
| `RAN_PROV_FALLBACK_S` | 60 | 连续这么久没拿到 IP → 自动进配网（开机起算，运行中掉线也在 `WIFI_EVENT_STA_DISCONNECTED` 里重新武装计时器，`GOT_IP` 停表；0 = 关） |
| `RAN_PROV_CONNECT_TIMEOUT_S` | 20 | 用户在页面保存后，等 IP 的上限；超时把失败原因回显到网页 |
| `RAN_SERVER_HOST` | remoteaudio.coze.site | 默认指向扣子云上部署的 remoteAudioServer（https://remoteaudio.coze.site）；连局域网自建服务端改电脑 IP |
| `RAN_SERVER_PORT` | 443 | 云端 443（wss）；局域网 uvicorn 用 8000 |
| `RAN_SERVER_USE_TLS` | **y** | 开=`wss://` + IDF 内置 CA bundle 校验证书；关=明文 `ws://`（仅局域网用） |
| `RAN_DEVICE_ID_OVERRIDE` | 空 | 空=自动生成 |
| `RAN_VAD_THRESHOLD` | 1200 | 运行时可被 set_config 覆盖并持久化到 NVS |
| `RAN_RETENTION_DAYS` | 30 | 同上 |
| `RAN_SILENCE_TIMEOUT_MS` | 1500 | 静音切段阈值 |
| `RAN_MAX_CLIP_SECONDS` | 600 | 单文件切片时长 |
| `RAN_PREROLL_FRAMES` | 10 | 前滚缓冲帧数 |
| `RAN_VAD_WIN_FRAMES` | 8 | VAD 滑动窗帧数 |
| `RAN_ENABLE_OLED` | **n** | 板上无 OLED，默认关闭（开启后仅占用 I2C0，不影响录音） |
| `RAN_OLED_ADDR` | 0x3C | 仅 `RAN_ENABLE_OLED=y` 时有效 |

> 已删除 `RAN_ENABLE_I2S_TX`（早期 I2S TX 占位通道，默认 y）。它用 I2S_NUM_1 却复用
> I2S0 的 BCLK/WS 引脚，会把 I2S0 的时钟挤掉导致录音失效，已连同占位代码一起删除。
> 详见第 10.12 节。

**NVS 持久化项**：`RAN_VAD_THRESHOLD`、`RAN_RETENTION_DAYS`、录音开关 `recording_enabled`
（namespace `ran`）；WiFi 凭据 `ssid`/`pass`（namespace `wifi`，v2.5 新增，由配网页写入）。
服务端 `set_config` / `set_recording` 会写入 NVS，重启后保留。
**配网不进 NVS**：进配网模式只读凭据、不删不改，只有用户在页面保存新网络才覆盖。

---

## 10. 已知坑点与踩过的坑

> 接手后修改任何代码前务必通读本节，避免重蹈覆辙。

### 10.1 Opus 组件命名空间
**问题**：早期 `idf_component.yml` 写错为 `espressif/esp-opus`，报
"no versions of espressif/esp-opus match ^1.0.0"。
**正确**：`78/esp-opus ^1.0.5`（xiaozhi-esp32 作者维护，下载量 37 万+）。

### 10.2 WebSocket 客户端从核心框架移出
**问题**：IDF v5.x 起 `esp_websocket_client` 不再属于核心框架，
`PRIV_REQUIRES` 里只写名字不够，必须在 `idf_component.yml` 显式声明依赖。
**症状**：`Failed to resolve component 'esp_websocket_client'`。

### 10.3 新版 I2S/GPIO/I2C/SPI 驱动拆分
IDF v5.5 把 i2s_std / i2c_master 等 API 拆到 `esp_driver_*` 组件，
`PRIV_REQUIRES` 必须显式列出这四个，否则新版 API 头文件找不到。

### 10.4 CMake Opus 链接探测
`main/CMakeLists.txt` 用通配 `*[Oo]pus*` 探测 managed_components 下的目录名，
所以 Opus 组件不管叫 `78__esp-opus` / `espressif__esp-opus` / `esphome__micro-opus`
都能自动链接。但如果目录名不含 "opus" 字样（如自定义组件）需要手动加。

### 10.5 Ogg granule_position 单位
RFC 7845 规定 Ogg Opus 的 granule_position 必须以 **48kHz 为单位**（每 20ms
Opus 包 +960），不是以输入采样率 16kHz 为单位。实现里已按此处理，
改时长解析逻辑时**不能**直接按 16kHz 算。

### 10.6 原子写入
录音先写 `.tmp` 再 rename。断电/崩溃时不会出现损坏的录音文件。
**不要**改成直接写 `.opus`。

### 10.7 文件块末帧标识
`play_file` 命令回传文件块时，**最后一个分块的 seq 必须是 0xFFFF**，
服务端靠这个判断文件结束。如果忘记置末帧，服务端会一直等后续帧直到超时。

### 10.8 FATFS 互斥
SD 卡读写必须互斥。当前实现：所有 sd_storage 访问走 xSemaphore。
新增文件操作时**务必**加锁。

### 10.9 PSRAM vs 内部 RAM
I2S DMA 缓冲必须分配在**内部 RAM**（`heap_caps_malloc(size, MALLOC_CAP_DMA)`）。
文件回传缓冲区、Ogg 页缓冲区可以用 PSRAM。

### 10.10 Windows 终端 Kconfig 中文乱码
Windows ESP-IDF 终端是 GBK，Kconfig 中文会乱码。本文件已改英文（Kconfig.projbuild
全部英文）。源码里的中文注释不影响 menuconfig。

### 10.11 ES7210 的 I2C 地址是 0x41，不是 0x40（严重）
**问题**：早期代码把 `BOARD_ES7210_I2C_ADDR` 写死成 0x40。实战派 S3 板上 AD0 被
拉高，芯片实际落在 **0x41**。所有寄存器写入都被 NACK，但 `es7210_init()` 只读一次
0x7A 打印出来、不校验结果，照样返回 `ESP_OK`。
**症状**：日志显示"ES7210 初始化成功"，但录出来的全是静音/极低噪声。
**修复**：地址改为 0x41，且驱动在 0x40~0x43 全范围探测（hint 优先），
一个都不应答时返回 `ESP_ERR_NOT_FOUND`，日志明确报错，不再假装成功。
**改动位置**：`main/board_lckfb_s3.h`、`main/es7210_drv.c`。

### 10.12 不要在 I2S0 之外再建 TX 通道去用同一组 BCLK/WS（严重）
**问题**：早期 `RAN_ENABLE_I2S_TX` 默认开，用 `I2S_NUM_1` 建了个 TX 占位通道，
但它的 `bclk/ws` 直接复用了 `BOARD_I2S_RX_SCK/WS`（GPIO14/13）。
**同一组引脚只能由一个 I2S 控制器驱动**，后 enable 的控制器会把 I2S0 的时钟挤掉。
**症状**：`i2s_channel_read()` 每帧都等满 timeout，录音慢几十倍（实测 1011ms/帧）。
**修复**：删除该 Kconfig 项与整段 TX 占位代码。
**以后要加播放**：改用 I2S0 全双工，`i2s_new_channel(&cfg, &tx, &rx)` 一次拿两个
句柄、各自 `i2s_channel_init_std_mode`（同一份 `i2s_std_config_t`，含
`mclk_multiple = I2S_MCLK_MULTIPLE_256`），并且**先 enable TX 再 enable RX**
——全双工下时钟发生器挂在 TX 侧，只 enable RX 一样没时钟。
另外 `auto_clear` 是 TX 专用字段，RX-only 通道别开。

### 10.13 `es7210.h` 文件名被官方组件占用（必看）
工程依赖官方托管组件 `espressif/es7210` 后，`managed_components/` 下也有一个
`es7210.h`。C 的引号 include 会**优先搜索 includer 所在目录**，所以只要本工程
`main/` 下存在 `es7210.h`，`#include "es7210.h"` 永远命中自己那份，官方
`es7210_new_codec` 等声明全部不可见 → 一堆 implicit declaration 报错。
**正确做法**：本工程自己的封装命名为 **`es7210_drv.c` / `es7210_drv.h`**，
只有它内部用 `#include "es7210.h"` 引官方头文件。
（旧的手写驱动已改名保留为 `main/es7210.c.bak`、`main/es7210.h.bak`，
不参与编译，确认无用后可删。）

### 10.14 `i2s_channel_read()` 的超时单位是毫秒
**问题**：旧代码写 `i2s_channel_read(..., pdMS_TO_TICKS(100))`，而该参数单位就是
毫秒。本工程 `CONFIG_FREERTOS_HZ=1000` 时数值正好相等，换配置（如 100Hz）就会
放大 10 倍等待。
**修复**：改用字面量 `AUDIO_I2S_READ_TIMEOUT_MS 100`。

### 10.15 DMA 缓冲要留足余量
**问题**：旧 `dma_frame_num = 64`，6 个描述符 → 总缓冲仅 6×64×4B = 1536B ≈ 24ms。
Opus 编码或 SD 写卡抖动超过 24ms 就丢音频。
**修复**：`dma_frame_num = AUDIO_FRAME_SAMPLES`（320 帧 = 20ms/描述符），
总缓冲 6×20ms = 120ms。

### 10.16 wss 必须先对时，否则证书校验必然失败
**问题**：ESP32 刚上电时系统时间是 1970 年，mbedTLS 校验服务端证书的生效/过期时间
会直接失败，表现为连不上云服务器、日志里 TLS handshake 报错。
**修复**：`ws_task()` 在每次建连前，若 `CONFIG_RAN_SERVER_USE_TLS=y` 且
`!wifi_mgr_time_synced()`，先等 SNTP 对时（最多 10s）再连；等不到也照连一次，靠
指数退避重连兜底。
**配套**：`.task_stack` 从 6144 提到 8192（TLS 握手吃栈），并在 `PRIV_REQUIRES`
补 `esp-tls` / `mbedtls`（`esp_crt_bundle.h` 来自 mbedtls 组件）。

### 10.17 换服务器地址后必须重新配置
`sdkconfig.defaults` 的新默认值对已存在的 `sdkconfig` **不生效**。改过 Server 菜单
或想用新默认值时，要么删掉 `sdkconfig` 重新 `idf.py set-target esp32s3`，要么
`idf.py menuconfig` 手动改。忘了这步的表现是：代码明明改了，设备还去连老地址。

### 10.18 WebSocket 握手响应头缓冲区要调大（v5.5 实测坑）
**问题**：Coze 网关（Tengine + 字节 FaaS 网关）回的 101 响应头约 **980 字节**——
`X-Tt-Trace-Host` 一条就 352 字节，另有 `X-Tt-Trace-Id` / `x-tt-logid` / `EagleId` /
`Via` / `X-Faas-Gateway-Instance-Name` 等。ESP-IDF 的 ws 传输层用**同一块
`CONFIG_WS_BUFFER_SIZE` 缓冲**先拼 Upgrade 请求、再累积读响应头（默认 1024）；
`transport_ws.c` 只在 header_len ≥ WS_BUFFER_SIZE-1 且缓冲里找不到头部结束的空行时
才报 `Header size exceeded buffer size`，所以一旦报这个错，就说明响应头比缓冲区大，
不必再怀疑别的。
**现象**：TLS 已经握手成功（日志有 `esp-x509-crt-bundle: Certificate validated`），
紧接着 `transport_ws: Header size exceeded buffer size` +
`esp_transport_connect() failed with -1, esp_ws_handshake_status_code=0`，然后无限
重连。
**修复**：`sdkconfig.defaults` 设 `CONFIG_WS_BUFFER_SIZE=4096`（菜单路径
Component config → TCP Transport → Websocket → Websocket transport buffer size；
`depends on WS_TRANSPORT`，后者默认 y）。


### 10.19 Opus 编码任务栈要 32KB
**问题**：`recorder.c` 里 `opus_enc` 任务原本 16KB 栈，真机上第一次 `opus_encode()`
就栈溢出：`***ERROR*** A stack overflow in task opus_enc has been detected`，随后
panic 重启（现象固定在日志打印"开始录音 -> …"之后不久）。libopus 的编码器（浮点
构建）很吃栈，同芯片的 `s3_opus_rec_play` 工程用的是 32768，一开始就该对齐。
**修复**：`ENC_TASK_STACK` 16384 → 32768。

### 10.20 「无声掉线」怎么判读：先看 diag 心跳，再看发送统计（v2.1 新增）
**现象**：2026-09-14 真机上，服务端反复开始/停止监听几次后设备掉线，串口日志停在最后
一条 `stream: 实时推流 停止` 之后**再无任何输出**——既没有 `WebSocket 断开`，也没有
panic / 重启 banner；服务端设备列表里该设备消失。
**服务端侧实测结论**（用 Python 假设备 + 手工 `Sec-WebSocket` 握手验证，服务端源码不在
本仓库）：
1. `/ws/listen/<id>` 的开关只驱动服务端向设备下发 `{"cmd":"start_stream"}` /
   `{"cmd":"stop_stream"}`，设备 WS 不会被踢；黑洞监听（连上后完全不读数据）也不会
   把设备链路拖死；
2. 服务端设备列表里的 `online` 就是"设备 WS 会话还在"——不看 `last_seen` 新鲜度
   （假设备 35s 不发心跳仍是在线），设备 socket 一断，记录几秒内消失；
3. 所以页面显示"离线"= 设备的 TCP 连接真的断了，问题在设备侧。
**为什么以前判不出来**：日志只说明"设备最后做了什么"，无法区分任务挂死 / 芯片复位 /
仅断链；而且所有发送都用 `xSemaphoreTake(s_send_mtx, portMAX_DELAY)`，一旦网络写卡住，
采集任务会被无限挂死且不打任何日志。
**v2.1 加固**：
1. 新增 `diag` 任务（`main/diag.c`，核 0、优先级 2、栈 3KB），每
   `CONFIG_RAN_DIAG_INTERVAL_MS`（默认 10000ms）打一行：
   `心跳 Ns | ws=0/1 推流=0/1 | 内部堆 xxxK/最低 xxxK | 发送失败 n 慢发送 n 最慢 nms 丢帧 n`。
   它不碰网络也不碰音频，是"CPU 是否还活着"的独立证据。
2. ws 发送全部改为有界取锁（`SEND_LOCK_TIMEOUT_MS=500`）：宁可丢这一帧，也不允许把
   `audio_pipe`/`ws_mgr` 挂死；取锁超时、单次发送 >300ms（`SLOW_SEND_WARN_MS`）、
   发送失败都会打日志并累加计数。
3. `WEBSOCKET_EVENT_ERROR` 打出 `error_type` / `esp_tls_last_esp_err` /
   `esp_tls_stack_err` / `esp_transport_sock_errno`。
4. PCM 帧缓冲改静态预分配（不再 50Hz `malloc`/`free`）。
5. 固件版本 2.0.0 → 2.1.0（hello 的 `fw` 字段即可确认板上跑的是哪一版）。
**现场判读方法**：
- 心跳继续打 → CPU 活着：看 `ws=` 与发送统计；
- 心跳断了、紧跟着启动日志 → 复位 / panic；
- 心跳断了且没有启动日志 → 有任务挂死，最后一条心跳里的 `慢发送/最慢` 是否已到秒级
  就是关键线索（秒级说明 TLS 写被网络卡住）。
### 10.21 外网长连接约 300s 被平台接入层掐断 + 重连太慢（v2.2 修复）
**现象 A（连接每 ~5 分钟被掐）**：从外网连 `wss://remoteaudio.coze.site/ws/device/<id>`，
连接在 **288 / 300.4 / 302.5 / 302.8 秒**被平台接入层掐断，客户端**连 close 帧都收不到**
（表现为服务端设备列表里设备消失、页面显示离线，设备侧随后自行重连）。
**实测结论（同一测试脚本，只改 ping 设置）**：
1. **不开 WS 协议级 ping**：288 / 300.4 / 302.5 / 302.8s 被掐断；
2. **应用层 JSON ping（15s）不算「活着」**：发得再勤，照样 ~300s 被掐；
3. **50Hz 音频 binary 数据也不算「活着」**：一直推着流也照样被掐；
4. **开启 WS 协议级 ping（10~20s）后**：同一连接活了 **425s 以上**，越过 5 分钟不再断。
→ 必须让 `esp_websocket_client` 自己发协议级 PING 帧（RFC6455 opcode 0x9）。

**修复 A**：`ws_task()` 的 `esp_websocket_client_config_t` 增加
`.ping_interval_sec = 10`（`WS_PING_INTERVAL_SEC`）与
`.pingpong_timeout_sec = 30`（`WS_PINGPONG_TIMEOUT_SEC`；3×ping 间隔，容忍连丢 2 个 pong
不误杀，真空死链最迟约 40s 内判死并重连）。
字段依据（已核对 ESP-IDF / 组件公开头文件 `esp_websocket_client.h`）：
`size_t ping_interval_sec;  /*!< Websocket ping interval, defaults to 10 seconds if not set */`、
`int pingpong_timeout_sec;  /*!< Period before connection is aborted due to no PONGs received */`；
组件实现里两者缺省值为 10s / **120s**（`WEBSOCKET_PINGPONG_TIMEOUT_SEC 120`），
120s 对现场排查太慢，故显式设 30s。本机（子 Agent 环境）没有 ESP-IDF 源码，
字段名/语义依据以上游公开头文件确认，**需用户本地编译验证**。

**现象 B（重连太慢 + 断线必须重新点监听）**：原来退避 1s 起、30s 封顶，服务端掐一次后要等
500ms+1s 才开始重连，连续失败时最坏 30s 一次；且 `DISCONNECTED` 会把推流关掉，
**用户必须重新点「开始监听」**。

**修复 B**：
1. 退避改 `RECONNECT_BACKOFF_MIN_MS = 300` / `RECONNECT_BACKOFF_MAX_MS = 3000`
   （300 → 600 → 1200 → 2400 → 3000 封顶），连接成功即重置回 300ms。两条路径都改：
   连接超时/失败路径、运行中断开路径。
2. 新增 **`s_want_stream`**（用户/服务端「要不要流」的意图，不是当前是否在推流）：
   `start_stream` 置 true、`stop_stream` 置 false；`WEBSOCKET_EVENT_DISCONNECTED` 照旧
   `audio_stream_set_active(false)`（断开期间不发），但**重连成功、重发 hello 之后，若
   `s_want_stream` 仍为 true 就自动 `audio_stream_set_active(true)`**。
   幂等性：该函数只是置一个 bool（`audio_stream.c` 内的 `s_active`）并打一行日志，
   重复调用无副作用；服务端在设备上线时若紧接着再下发 `start_stream`，也只是再置一次
   同一位，不会重复开启、不冲突。设备冷启动时 `s_want_stream` 为 false，
   仍然是「订阅驱动」（服务端下发才开始推流）。
3. 重连日志带次数与本次退避：`连接断开（第 N 次重连），退避 X ms 后重连` /
   `连接超时/失败（第 N 次重连），退避 X ms 后重试`。
4. 固件版本 2.1.0 → **2.2.0**（hello 的 `fw` 字段即可确认板上跑的是哪一版）。

**协议层零改动**：URL 路径、JSON 字段、二进制帧 0x01/0x02 布局、端口全部保持不变；
`start_stream` / `stop_stream` 语义不变（只是多记了一个意图位）。

**必须确认的依赖细节（风险）**：协议级 PING 是 `espressif/esp_websocket_client` 组件发的。
该组件 **1.6.0 之前**的实现会在每次收到数据后重置 `ping_tick_ms`（相当于「只有链路无流量时
才发 ping」），1.6.0 的 `fix PING timing - enable periodic PING during active traffic`
才改成周期性发。当服务端有零星下行数据（例如对 JSON ping 的回应）时，老版本可能**一直不
发协议 ping**，本修复即失效。`main/idf_component.yml` 现在写的是 `^1.2.0`（理论上会解析到
最新 1.x），但**请在本地编译后确认实际拉到的组件版本 ≥ 1.6.0**（看 `dependencies.lock` 或
build 日志里的 `espressif/esp_websocket_client`），否则把约束提到 `^1.6.0`。
（**v2.3 已把约束直接提到 `^1.6.0`**，见下。）

---

### 10.22 「v2.2 连不上、v2.1 能连」：三个真凶与修法（v2.3）

**现场**：同一块板、同一个路由器，v2.1 固件能连上服务端，v2.2 连不上。串口日志关键几行：
```
I (11366) ws: 连接 wss://remoteaudio.coze.site:443/ws/device/esp32-7a44a0 ...
E (11371) esp-tls: couldn't get hostname for :remoteaudio.coze.site: getaddrinfo() returns 202
E (11400) websocket_client: esp_transport_connect() failed with -1, ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
I (11425) websocket_client: Reconnect after 10000 ms
I (11379) wifi:state: assoc -> run (0x10)      <-- 注意：Wi-Fi 是这一刻才真正就绪
```
即：建连抢跑在「Wi-Fi 还没拿到 IP / DNS 不可用」的窗口里。

**真凶 1：`reconnect_timeout_ms = 0` 并不禁用自动重连（认知错误，1.x 各版本一致）**
上游 `esp_websocket_client_init()` 源码（1.2.3 与 master 同一段）：
```c
if (!config->disable_auto_reconnect && config->reconnect_timeout_ms <= 0) {
    client->wait_timeout_ms = WEBSOCKET_RECONNECT_TIMEOUT_MS;   /* 10*1000 */
    ESP_LOGW(TAG, "`reconnect_timeout_ms` is not set, or it is less than or equal to zero, ...");
} else {
    client->wait_timeout_ms = config->reconnect_timeout_ms;
}
```
`auto_reconnect` 始终为 true，`0` 只是让等待时间回落成默认 10s；`Reconnect after %d ms` 就是
`esp_websocket_client_abort_connection()` 在 `auto_reconnect == true` 时打的。**结论：v2.1 也会
打这行日志，它不是 v2.2 独有现象；v2.1/v2.2 实际是「组件自带 10s 自动重连」+「`ws_task`
`stop()` 后再 `start()`」两套逻辑在抢同一个客户端。**

**修法**：`s_cfg.disable_auto_reconnect = true`（保留 `reconnect_timeout_ms = 0` 无副作用），
重连只由 `ws_task()` 负责 —— 退避节奏、`stop()`/`start()` 时机、hello 重发都只有一条路径。

**真凶 2：建连前只等 SNTP，没等「Wi-Fi 拿到 IP」**
v2.2 的建连门只有 SNTP（`wifi_mgr_time_synced()`，最多 10s）。当 SNTP 因为还没联网而同步不上时，
代码会打「SNTP 仍未同步，先尝试连接」然后**立刻硬连**——而此刻 Wi-Fi 可能正处在
`run -> init`（reason=15/201）的掉线窗口里，于是 `getaddrinfo()` 必然失败。
**修法**：建连前加 IP 门。
```c
if (!wifi_mgr_is_connected()) {
    for (int i = 0; i < 30 && !wifi_mgr_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(500));
    if (!wifi_mgr_is_connected()) { ws_on_failure(&fail_streak, &backoff_ms, "Wi-Fi 一直没拿到 IP"); continue; }
}
```

**真凶 3：`esp_websocket_client_start()` 的返回值从来没被检查**
组件实现：`if (client->state >= WEBSOCKET_STATE_INIT) { ESP_LOGE(TAG, "The client has started"); return ESP_FAIL; }`
（1.2.3 与 master 均有）。旧代码忽略返回值 → 这次失败既没清客户端状态、也没退避，
下一轮 `start()` 继续被拒，**表现为「再也连不上」**。
**修法**：检查返回值，失败记一次 `ws_on_failure()`（停客户端 + 退避 + 必要时重建）。

**附带加固**
1. `ws_task()` 连续失败 `WS_HARD_RESET_AFTER_FAILS = 6` 次（约 1 分钟）就
   `esp_websocket_client_stop() → destroy() → init() → register_events()`（`ws_hard_reset()`），
   把组件内部一切残留状态清干净；失败序列在连接成功时归零。
2. `wifi_mgr.c` 的 `WIFI_EVENT_STA_DISCONNECTED` 回调原来 `vTaskDelay(pdMS_TO_TICKS(2000))`
   再 `esp_wifi_connect()`，会把**默认事件循环**（所有事件回调串行跑在同一个任务上）整体阻塞
   2 秒，掉线期间 IP 事件、SNTP、按键回调全部顺延。改一次性 `esp_timer`
   （`WIFI_RECONNECT_DELAY_MS = 2000`）延时触发，回调立即返回。
3. 新增 `log_ap_rssi()`：`on_got_ip` 里打一行 `连上路由器：RSSI -xx dBm，信道 n`，用于判断
   是真机信号问题还是软件问题。
4. 诊断心跳行追加链路计数：`重连 N 建连失败 N 硬复位 N`（`ws_client_get_link_stats()`）。
5. `ws_client.h` 里 `ws_client_is_connected()` 在硬复位期间返回 false，发送函数增加
   `s_client == NULL` / `s_resetting` 保护，避免重建窗口里写空句柄。

**排查顺序建议（下次再出现「连不上」）**
1. 看 diag 心跳是否还在打 → 判断是任务挂死还是网络问题；
2. 看心跳行的 `重连/建连失败/硬复位` 三个计数是否在涨 → 涨就是在重连但连不上；
3. 看 `ws: Wi-Fi 未就绪（还没拿到 IP）` / `getaddrinfo() returns 202` → 网络/路由器层；
4. 看 `启动 WS 客户端失败: The client has started` → 客户端状态问题（v2.3 已会自愈）。

**协议层零改动**：URL 路径、JSON 字段、二进制帧 0x01/0x02 布局、端口全部不变。

**依赖**：`main/idf_component.yml` 的 `espressif/esp_websocket_client` 约束为 `^1.6.0`
（协议级 PING 在 active traffic 下也要周期性发出，是 1.6.0 才修的）。若现象随时间/版本漂移，
可临时把约束改回 `^1.2.0` 重新解析做 A/B 对照，但最终应保持 `^1.6.0`。

---

### 10.23 断网期间监听端走了，重连后别对着空气推流（v2.4）

**现象**：设备推流中 Wi-Fi 断；断网期间监听端在网页上关掉监听（服务端 `listeners` 1→0，
这时会下发 `stop_stream`，但设备不在线，这条指令直接丢了）；设备网络恢复重连后，v2.2/v2.3
的实现会因为 `s_want_stream` 还是 true 而**自动续流** → 没有任何人在收。空推成本：
`647B/20ms` ≈ 32.35 KB/s ≈ **0.26 Mbps，一小时约 116MB**，还让 Wi-Fi 射频一直保持唤醒。

**根因**：状态归属错了。「现在有没有人在听」只有**服务端**知道（`listener_count`），
而 v2.2 起 `s_want_stream` 被当成了设备自己的意图，跨断线无条件复用。

**v2.4 的修法（服务端 + 固件两端一起改）**

固件侧（`main/ws_client.c`）：
1. `WEBSOCKET_EVENT_CONNECTED` 里 `send_hello()` 后**不再**立刻
   `audio_stream_set_active(true)`。若断线前在推流，改为置 `s_await_ack` 并设 3s 截止
   （`WS_RESUME_ACK_TIMEOUT_MS 3000`），等服务端裁决。
2. `dispatch_text()` 里 `start_stream` / `stop_stream` 统一走新增的 `stream_apply(bool on, why)`：
   它一次性写 `s_want_stream`、清 `s_await_ack`、清 `s_hb_zero_streak`、
   调 `audio_stream_set_active()`。收到任意一条即是裁决 → 不再等。
3. `ws_task()` 已连接分支里判裁决超时：超过 3s 仍 `s_await_ack` 且 `s_want_stream` 为真，
   就按「没人监听」处理（`stream_apply(false, ...)`），日志明确写「服务端随时可用
   `start_stream` 唤醒」。**默认不推**是安全方向：少推一次只丢一次实时流（录音不受影响），
   空推则是一直在烧流量。
4. 新增 `listeners` 看门狗：下行心跳（服务端每 20s 的 `{"type":"hb",...}`）里若带 `listeners`，
   存进 `s_last_listeners`；`listeners > 0` 时清零计数，如果正在等裁决则直接当裁决续流；
   `listeners == 0` 且当前在推流则累加 `s_hb_zero_streak`，连续 3 次
   （`WS_HB_ZERO_STREAK_LIMIT 3`，≈60s）就自动停流。这样即使服务端一条裁决都没发，也不会无限空推。
5. `WEBSOCKET_EVENT_DISCONNECTED` 里清 `s_await_ack` / `s_hb_zero_streak` / `s_last_listeners`。
6. 新增 `int ws_client_get_listeners(void)`；diag 心跳行加 `监听 N`（`-1` = 服务端没报该字段）。

服务端侧（remoteAudioServer，见对应项目记录）：
1. 设备 hello 上线后按真实监听者数量复盘一次：0 → 下发 `stop_stream`；>0 → 下发 `start_stream`；
   原来那条「45s 宽限内设备回来就自动 start_stream」改成**先看监听者数量**再决定。
2. 下行心跳加 `"listeners": <int>`。
3. 监听者 1→0 时**延迟 10s** 再停流，10s 内有人回来则取消（防网页刷新导致的瞬断误停）。

**关键点**
- 没有新增任何协议消息、字段或帧类型：复用既有的 `start_stream` / `stop_stream` / `hb`，
  老服务端对 `hb` 里多出来的 `listeners` 无感，新固件对不带 `listeners` 的老 `hb` 也能跑
  （只是看门狗不生效，改为靠 3s 超时兜底）。
- **停实时流不影响本地录音与回放**：VAD 触发照样落 SD 卡（`recording_saved` 照常上报），
  回放走 `/api/recordings`，都不依赖推流。
- 诊断心跳行现在是：
  `心跳 Ns | ws=1 推流=1 监听 N | 内部堆 ..K/最低 ..K | 发送失败 .. 慢发送 .. 最慢 ..ms 丢帧 .. | 重连 .. 建连失败 .. 硬复位 ..`
- 前提：v2.4 要和服务端新逻辑**一起**上线才完整生效；只换固件时表现为「重连后要先等一次
  服务端指令才续流」，仍然不会空推（超时即不推），只是续流可能晚 3s。

---

### 10.24 没配过 WiFi 就自己开热点让手机配（v2.5 网页配网）

**需求**（用户原话）：「加一个 wifi 注册页面，就是开始如果没有配置 wifi，那就自动转为热点模式，
开放一个 web 页面，让用户连接上去之后设置要连接的 wifi 热点和密码」。确认后的两条边界：
① 触发条件 = **没配过就进** + **连不上路由器 1 分钟后自动进**（不做长按按键强制进配网）；
② 热点 **开放、无密码**（配网越省事越好）；③ **以前的记录不要删除，除非用新的 wifi 设置去覆盖**。

**模块划分**

| 文件 | 职责 |
|---|---|
| `main/wifi_cred.c/h` | NVS namespace `"wifi"` 存 `ssid`/`pass`：`load/save/erase/exists`。只被"保存新网络"调用写入 |
| `main/wifi_prov.c/h` | APSTA 热点 + `esp_http_server`（`/*` GET 通配 + `/save` POST）+ DNS 劫持 + 扫描 + 延迟关热点 |
| `main/wifi_prov.html` | 配网页（暗色卡片风、移动端）：`/status` 轮询、`/scan` 列表、`/save` 提交、成功/失败提示。**用 `EMBED_TXTFILES` 编进固件** |
| `main/wifi_mgr.c/h` | 三级凭据优先级、`wifi_mgr_connect_with()`（保存+连+等 IP）、失败原因文案、60s 兜底转配网 |
| `main/main.c` | 配网期间**先不启动 WS**，等配网结束再启；`diag` 心跳提前到 WS 之前 |

**凭据优先级（向后兼容的关键）**
1. NVS `"wifi"` 有记录 → 用它（现场配过网，永远优先）；
2. 否则看 Kconfig `RAN_WIFI_SSID`：**非占位值**（`!= "myssid"`）→ 用它（老板子/Qt 改过 sdkconfig 的板子行为完全不变）；
3. 都没有 → `s_prov_triggered = true` 并**同步**调 `wifi_prov_start()`（同步是为了让 `main.c`
   紧接着的 `wifi_mgr_is_provisioning()` 判断立即成立）。

**热点与页面**
- `esp_netif_create_default_wifi_ap()`（自带 DHCP server，默认 192.168.4.1）+
  `esp_wifi_set_mode(WIFI_MODE_APSTA)`：**配网不影响 STA 侧**，路由器恢复就自动连上并关热点。
- 热点名 `RAN-7A44A0`（`CONFIG_RAN_PROV_AP_PREFIX` + MAC 后 3 字节），开放（`WIFI_AUTH_OPEN`）。
- HTTP：`httpd_config_t.uri_match_fn = httpd_uri_match_wildcard`，注册 `/*`(GET) 与 `/save`(POST)。
  页面路由：`/`、`/index.html` 出页面；`/status` 出 JSON 状态；`/scan` 出扫到的 AP 列表；
  **其它任意路径一律 302 到 `http://192.168.4.1/`** —— 手机自带的 `/generate_204`、
  `/hotspot-detect.html` 连通性探测因此拿到 302，多数手机会自动弹「登录网络」界面。
- DNS 劫持任务：lwIP UDP socket 绑 53 端口，解析 question 取 qtype，A 查询回 1 条
  A 记录指向热点网关（TTL 60），AAAA 等其它类型回空应答让客户端回退到 A。
  即使手机把域名解析走别的路径，302 兜底仍能把用户带到页面。

**保存流程（`/save`）**
`form_field()` 解析 `application/x-www-form-urlencoded`（支持 `%XX` 与 `+`）→ `ssid` 必填 →
`s_state = CONNECTING` → `wifi_mgr_connect_with(ssid, pass, 20s)`：
`wifi_cred_save()`（**这里才覆盖旧记录**）→ `apply_sta_creds()` → `esp_wifi_disconnect()`
→ `esp_wifi_connect()` → `xEventGroupWaitBits(BIT_CONNECTED, 20s)`。
成功 → `{"ok":true,"ip":...}`，置 `s_stop_req`，1.5s 后关热点切回 `WIFI_MODE_STA`；
失败 → `{"ok":false,"err":"<中文原因（原因码 N）>"}`，`s_state = ACTIVE`，**热点留着让用户重试**。
失败原因映射集中在 `wifi_mgr_last_error_text()`：
`201 没找到这个名字的 WiFi` / `202 认证失败：密码不对` / `204 密码校验超时` /
`210 加密方式不被支持（改成 WPA2）` / `203 路由器拒绝关联` / `0 连接超时：没收到路由器响应`。
（原因码 210 的符号名在 IDF 各版本叫法不同，代码里用数值 `PROV_REASON_NO_AP_COMPAT_SECURITY`，
避免换 IDF 版本编译不过。）

**并发/实时性约束（延续本工程既有原则）**
- 60s 兜底计时器回调**只**置标志 + `xTaskNotifyGive`，真正的 WiFi 重活交给 `prov_task`；
  `prov_task`（4096B 栈）与 `dns_task` 都 `xTaskCreatePinnedToCore(..., 1)` 绑 core 1，
  不去挤 core 0 上的音频采集任务。计时器在「开机」和「运行中掉线」两个时刻都会武装，
  `GOT_IP` 时停表 —— 所以「连不上路由器 1 分钟」不论开机还是中途断网都成立。
- 扫描和"保存后等 IP"都在 `httpd` 任务里做（阻塞 2s / 最多 20s），不碰事件循环。
  `hc.stack_size = 6144`（扫描 + 连接等待都在这里跑）。
- 扫描把射频拉到各信道（`esp_wifi_scan_start(&sc, true)`，channel=0），**连着热点的手机可能瞬断
  1~2 秒**，属正常；扫完在 STA 未连接时 `esp_wifi_set_channel(PROV_AP_CHANNEL, ...)` 拉回热点信道。
- 配网期间 STA 保持重连，但间隔从 2s 放宽到 10s 并降为 `LOG_I`，日志干净、少抢信道。
  一旦 STA 连上（`IP_EVENT_STA_GOT_IP`）就自动 `wifi_prov_request_stop()` 收摊关热点。
- 关热点前会再确认一次「真的连上了」，刚保存完又掉线的情况会保留热点让用户重试。

**构建改动**：`main/CMakeLists.txt` 的 `SRCS` 加 `wifi_cred.c`/`wifi_prov.c`，
加 `EMBED_TXTFILES "wifi_prov.html"`（符号 `_binary_wifi_prov_html_start`，文本模式末尾自带
`'\0'`，配 `HTTPD_RESP_USE_STRLEN` 用），`PRIV_REQUIRES` 加 `esp_http_server`。

**怎么验**（无需路由器密码就能测）：编译烧录一台"没配过 WiFi"的板子（`RAN_WIFI_SSID` 留默认
`myssid`，或用 `idf.py erase-flash` 清掉 NVS）→ 串口应出现「==== 配网模式已开启 ====」→
手机连 `RAN-xxxxxx` → 打开 192.168.4.1 → 扫描 → 填（可以先故意填错密码，页面应报"密码不对"）
→ 改对保存 → 页面显示 IP，热点关闭，日志出现「配网成功：已连上 ... 热点即将关闭」，
随后 WS 连上服务端、hello 里 `fw` = `2.5.1`。

> **2.5.1 修了什么**：2.5.0 首次实机烧录在配网启动瞬间崩 —— `***ERROR*** A stack overflow in
> task prov_dns has been detected`，随后 `rst:0xc (RTC_SW_CPU_RST)` 反复重启。原因是 `dns_task`
> 只给了 3072 字节栈，函数里却声明了 `uint8_t buf[512]` + `resp[512]` 两个局部数组（合计 1KB），
> 加上 lwIP socket 调用与日志格式化开销直接穿栈。修法：两个缓冲改为静态数组 `s_dns_rx`/`s_dns_tx`
> （同一时刻只有一个 DNS 任务，`wifi_prov_start` 带 `s_state` 重入保护），`prov_dns` 栈
> 3072→4096、`prov_task` 4096→5120。协议、页面、行为零改动，只影响配网链路。

### 10.25 v2.5.2：连接状态一致性（修「开关 WiFi 后设备一直连不上」）

**现场**（2026-09-16 用户串口日志）：WiFi 断开（reason=1 → 201）后设备连上另一 AP 并拿到 IP，
但此后 50s 内**没有任何 WS 建连日志**；`diag` 连续多行 `ws=1 推流=0 监听 0`，
`发送失败 1 慢发送 0 最慢 78ms` 与 `重连 203 建连失败 203 硬复位 32` 全部冻结；服务端
`/api/devices` 里没有这台设备（线上服务端已用探针验证注册/清理正常）。

**根因**：① `ws_client_is_connected()` 只读本模块事件位 `BIT_CONNECTED`；② `wifi_mgr.c` 的
`WIFI_EVENT_STA_DISCONNECTED` 只清自己的事件位并重连 WiFi，**从不通知 ws_client**。WiFi 断了之后
底层 socket 已死、组件又没上报 `DISCONNECTED` → 事件位残留 → `ws_task` 以为"已连接"、永不重连；
③ `send_text()` 在组件未连接时静默 `return false` 且不计 `s_send_fail` → 发送统计冻结；
④ 旧逻辑把「WiFi 没拿到 IP」也算一次建连失败 → 断网能把计数器刷到几百次，掩盖现场。

**修法**（改 `ws_client.c/.h`、`wifi_mgr.c`、`Kconfig.projbuild`，协议零改动）：
- `wifi_mgr.c` 掉线回调 → `ws_client_notify_link_down()`（只置 volatile 标志，不阻塞事件循环）；
  `ws_task` 下一轮清 `BIT_CONNECTED`、`audio_stream_set_active(false)`、停客户端，等 WiFi 回来再
  重连，且**不计失败、不进硬复位**；
- `ws_client_is_connected()` 交叉核对 `esp_websocket_client_is_connected(s_client)`；
- 进重连分支前 / 中断分支显式清残留状态位（否则 `xEventGroupWaitBits` 因残留位"假成功"、`start()`
  被组件以 "The client has started" 拒绝）；
- 应用层 ping 连续 2 次发不出去 → 判链路已死；静默看门狗 180s 收不到服务端任何数据（服务端 20s
  一条下行心跳）→ 判链路已死重建；`s_last_rx_ms` 在 `CONNECTED`/`DATA` 事件更新；
- 「Wi-Fi 没拿到 IP」只等（≤15s）不计失败。`RAN_FW_VERSION` = `2.5.2`。

**怎么验**：烧录后开机应出现「连接 wss://remoteaudio.coze.site ...」→「WebSocket 已连接」；
开/关 WiFi（含切到别的 SSID）应看到「WiFi 链路断开 → 清本地连接状态并停客户端」并在 WiFi 回来后
自动重连；服务端 `/api/devices` 应出现该真机；`diag` 的 `ws` 随链路真实变化（不再恒为 1）。
老固件现场缓解手段：断电重启一次（重启即重建 WS 客户端）。

---

## 11. 测试与联调

### 准备
1. 启动服务端：`python3 -m uvicorn server.main:app --port 8000`（remoteAudioServer 项目）
2. 浏览器打开服务端 Web 页面（http://localhost:8000/）
3. 固件烧录并连上同一局域网

### 验证清单
- [ ] 固件连上 WiFi + SNTP 同步后，监听台顶部看到真实设备上线（与模拟器并列）
- [ ] 点"开始监听" → 能听到麦克风实时 PCM（提示音或环境声）
- [ ] 切换到录音 tab → 能看到 SD 卡上的录音列表（如有）
- [ ] 点击一段录音 → Ogg Opus 回放应有声音
- [ ] 在服务端点"录音开关" → 设备日志打印状态变化 + 事件上报（OLED 默认关，
      无屏可看属正常）
- [ ] 在服务端修改 VAD 阈值/保留天数 → 设备重启后仍保留
- [ ] 断开 WiFi / 服务端 → 设备自动重连（v2.2：指数退避 300ms-3s，日志带「第 N 次重连 + 退避 ms」）
- [ ] 推流中把服务端掐断（或拔网线再插回）→ 重连后**不需要重新点「开始监听」**，日志出现
      `重连成功：断线前的推流意图仍为开，自动恢复实时推流`，Web 页面接着有声音
- [ ] 挂机 ≥ 10 分钟（不开任何监听）→ 连接**不再每 5 分钟被平台掐断**（v2.2 协议级 ping 生效）
- [ ] 按 GPIO0 按键 → 录音开关切换 + 上报事件
- [ ] 录音触发 → 能看到前滚缓冲 + 录音文件 + recording_saved 事件
- [ ] 静音 1.5s 后 → 录音自动结束 + .tmp 改名 + 上报 size/duration
- [ ] **网页配网（v2.5）**：把 `RAN_WIFI_SSID` 留默认 `myssid`（或先 `idf.py erase-flash`）烧录 →
      日志出现「==== 配网模式已开启 ====」→ 手机连 `RAN-xxxxxx` →
      浏览器开 `192.168.4.1`（或随便输个网址被 302 过来）→ 扫描出列表 →
      先故意填错密码保存 → 页面报「认证失败：密码不对」且热点仍在 →
      改对后保存 → 页面显示 IP、热点关闭、日志「配网成功」→ 随后 hello `fw=2.5.1`
- [ ] **配网不删旧记录**：配好网后重启 → 直接连老 WiFi，不再进配网（NVS 记录仍在）；
      再故意把路由器断电/改密码 → 60s 后自动再进配网，页面里旧记录仍可用（不要求重填）

### e2e 测试脚本
服务端自带 `tools/e2e_test.py`，可自动化验证协议。固件端暂无自动化测试，
按上面清单手动验收。

---

## 12. 后续可扩展点

接手后可以考虑的方向（按优先级）：

1. **录音补传到云端**：设备离线时录音留 SD，在线时按指令上传到云端（服务端需扩展对应 API）
2. **实时流质量优化**：当前是固定 640B/帧，可考虑自适应压缩或丢包重传
3. **多麦克风阵列**：当前单麦克风，可扩展为双麦/多麦 + 波束成形
4. **播放本地录音到喇叭**：本固件目前不实现播放（原 I2S TX 占位代码因抢时钟已删除）。
   要加就按第 10.12 节改成 I2S0 全双工，并接上 ES8311 板载 DAC 或外接功放
5. **OTA 升级**：固件支持远程升级
6. **低功耗优化**：无监听者时关闭麦克风供电
7. **TLS 加密**：当前 WS 为 ws://，生产环境需 wss://
8. **录音加密**：SD 卡上的录音可以加密存储

---

## 附：相关文件路径

```
remote_audio_node/                       # 本固件工程
├── main/                                # 主要源码
│   ├── main.c / board.h / board_lckfb_s3.h
│   ├── device_config.c/h                # NVS 配置
│   ├── wifi_mgr.c/h                     # WiFi
│   ├── ws_client.c/h                    # WebSocket
│   ├── es7210_drv.c/h                   # ES7210 配置（包官方组件，勿改名为 es7210.*）
│   ├── audio_i2s.c/h                    # I2S RX 采集
│   ├── vad.c/h                          # VAD
│   ├── opus_encoder.c/h                 # Opus 封装（唯一 opus.h 依赖点）
│   ├── ogg_muxer.c/h                    # Ogg 容器
│   ├── recorder.c/h                     # 录音状态机
│   ├── sd_storage.c/h                   # SD 卡
│   ├── audio_stream.c/h                 # 实时流
│   ├── diag.c/h                         # 运行状态心跳（诊断黑匣子）
│   ├── btn.c/h                          # 按键
│   └── oled.c/h                         # OLED
├── CMakeLists.txt                       # 顶层工程
├── partitions.csv                       # 分区表
├── sdkconfig.defaults                   # 默认配置
└── PROJECT_HANDOFF.md                   # 本文档
```

对应服务端工程：`remoteAudioServer`（独立仓库/目录）。

---

## 维护者须知

- 修改协议字段时，**必须同步修改服务端**，否则对接失败
- 修改 Opus 相关代码时，只在 `opus_encoder.c` 一处改
- 修改 Ogg 容器格式时，只在 `ogg_muxer.c` 一处改
- 新增功能模块时，按 `main/CMakeLists.txt` 的格式加入 SRCS 和 PRIV_REQUIRES
- 任何改动后，用服务端 `tools/e2e_test.py` 跑一遍协议冒烟
