# remote_audio_node —— ESP32-S3 多设备音频采集节点固件

ESP-IDF v5.1+ 固件，与服务端 **remoteAudioServer**（FastAPI + WebSocket，端口 8000）
配套使用：板载 **ES7210** 音频 ADC 采集 16kHz/16bit 音频，VAD 触发录音并以
**Ogg Opus 32kbps** 存到 TF 卡；浏览器/监听端发起监听时，设备通过 WebSocket 实时
推送原始 PCM 帧，并支持录音文件列表查询与远程回放回传。

- 目标板：**立创实战派 ESP32-S3 (LCKFB SZPI)**，板载模组 ESP32-S3-WROOM-1
  **N16R8**（16MB Flash / 8MB 八线 PSRAM）
- 音频输入：板载 ES7210 四通道 ADC（本工程用 MIC1 + MIC2，取左声道 MIC1）
- 实时音频：16kHz / 16bit / 单声道，20ms 帧 = 320 samples = 640 字节 PCM
- 录音格式：Ogg Opus（RFC 7845），32kbps CBR，20ms 帧，静音 1.5s 自动切段，最长 10 分钟切片
- 通信：WebSocket 客户端 `ws://<HOST>:8000/ws/device/<device_id>`

> 早期版本目标是 ESP32-S3-DevKitC-1 + INMP441 外接麦克风，已整体切到实战派 S3 板载
> 麦克风。引脚定义集中在 `main/board_lckfb_s3.h`，换板只需改这一个文件。

---

## 1. 硬件引脚（板载，无需外接）

实战派 S3 的音频、TF 卡都是板载连接，正常情况下**一根线都不用接**。

| 外设 | 信号 | GPIO | 说明 |
|---|---|---|---|
| ES7210 ADC | I2C SDA | **GPIO1** | I2C0，100kHz |
| | I2C SCL | **GPIO2** | |
| | I2C 地址 | **0x41** | 板上 AD0 被拉高；驱动会在 0x40~0x43 自适应探测 |
| | MCLK | **GPIO38** | 256 × Fs = 4.096MHz @16kHz |
| | BCLK | **GPIO14** | I2S0 |
| | WS (LRCK) | **GPIO13** | I2S0 |
| | SDOUT | **GPIO12** | ES7210 → ESP32，16bit 立体声（L=MIC1, R=MIC2） |
| TF 卡 | CLK / CMD / D0 | **GPIO47 / 48 / 21** | SDMMC 1-bit 模式，挂载点 `/sdcard` |
| 录音开关按键 | BOOT 键 | **GPIO0** | 板载，按下接 GND，低有效，50ms 消抖 |
| 状态 LED | — | **GPIO39** | |
| OLED | — | 未接线 | 板上无 SSD1306，`RAN_ENABLE_OLED` 默认关闭 |
| 播放输出 | — | 未启用 | 本固件不实现播放（见第 10 节说明） |

> ES7210 与摄像头（GC0308）在 GPIO13/14/38 上有冲突，本固件不启用摄像头，无影响。

## 2. 编译与烧录

需要 ESP-IDF **v5.1 或更高**（推荐 v5.2 / v5.3 / v5.4），并已 `. ./export.sh`。

```bash
cd remote_audio_node
idf.py set-target esp32s3
idf.py menuconfig        # 见第 3 节：WiFi 可以留默认，开机用手机网页配（v2.5）
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # Windows 下为 COMx
```

首次 build 时组件管理器会自动下载三个托管组件到 `managed_components/`：
`78/esp-opus`、`espressif/es7210`、`espressif/esp_websocket_client`
（需要能访问 components.espressif.com）。

> 如果你之前已经 build 过（工作目录里已有 `sdkconfig`），`sdkconfig.defaults` 的新默认
> 值**不会**自动生效——删掉 `sdkconfig` 重新 `idf.py set-target esp32s3`，或直接
> `idf.py menuconfig` 手动把 Server 菜单改成 host=`remoteaudio.coze.site`、
> port=`443`、`Use TLS = y`。

### 3. menuconfig 必配项

进入 **Remote Audio Node 配置** 菜单：

| 配置项 | 说明 |
|---|---|
| WiFi SSID / WiFi 密码 | **可留默认**（v2.5 起）。这是编译期兜底值：保持默认 `myssid` 就表示"没配过"，设备开机会直接进**网页配网**（见 3.1）；填了真实路由器信息则按老流程自动连。**现场配网一次后 NVS 记录优先，再改这里不影响已配好的设备** |
| remoteAudioServer host | 默认 `remoteaudio.coze.site`（扣子云上已部署的服务端） |
| Server port | 默认 `443`（云端）；连局域网自建服务端改 8000 |
| Use TLS (wss://) | 默认**开**。云端走 `wss://`；连明文 `ws://` 的局域网服务端时关掉 |
| 设备 ID 覆盖 | 留空则自动生成 `esp32-<MAC后3字节>`，如 `esp32-aabbcc` |
| VAD 能量阈值 | 默认 1200（16bit 满量程 32767），可被服务端 set_config 动态修改 |
| 录音保留天数 | 默认 30，0=永久（空间不足仍会清理） |
| OLED 使能 | **默认关闭**（实战派 S3 无 OLED） |
| 网页配网（v2.5，默认开） | `Enable web provisioning`、热点名前缀 `RAN`、热点密码留空=开放、超时 60s 自动转配网、保存后等 20s 出结果 |

**默认行为**：设备上电 → 连 WiFi → SNTP 对时 → 自动连
`wss://remoteaudio.coze.site:443/ws/device/<device_id>`，连上即发 hello，
服务端「多设备音频监听系统」页面里就会出现这台真机设备。断线按 300ms→3s 指数退避重连
（v2.2 提速，原为 1s→30s），断线重连后若断线前正在监听会自动续流（v2.4 起先等服务端裁决）。

**没有 WiFi 凭据时**（v2.5 新增）：设备自动开一个开放热点 `RAN-xxxxxx`（前缀 + MAC 后 3 字节），
手机连上后浏览器打开 `http://192.168.4.1`（内置 DNS 劫持，多数手机会自动弹窗）填 SSID/密码 →
设备立刻试连，成功即关热点回到正常采集，失败把原因（密码错/找不到网络/信号差）显示在页面上让用户改。
有凭据但**连续 60 秒**仍未连上路由器（开机起算，或运行中掉线起算），也会自动转配网。
**进配网不会删除已存的旧记录**，只有用户在页面保存新网络时才覆盖。

### 3.1 网页配网（v2.5 新增）：没配过 WiFi 也能上手

出厂/新板子不需要事先知道 WiFi 密码，也不需要重新编译：

1. 上电后设备发现"没有可用 WiFi 凭据"（NVS 里没配网记录，且 `RAN_WIFI_SSID` 仍是默认 `myssid`），
   自动开热点。日志会打印：
   ```
   W (xxx) wifi_prov: ==== 配网模式已开启 ====
   W (xxx) wifi_prov:  1) 手机连接 WiFi：RAN-7A44A0 （无密码）
   W (xxx) wifi_prov:  2) 浏览器打开：http://192.168.4.1 （多数手机会自动弹出配网页）
   W (xxx) wifi_prov:  3) 选择网络 + 填密码 → 保存后设备立即连接
   ```
2. 手机连上这个开放热点（名字 = `RAN-` + 本机 MAC 后 3 字节，多台设备不重名）。
   多数手机会自动弹出"登录网络"页面；没弹就手动打开浏览器访问 `http://192.168.4.1`。
   （设备内置 DNS 服务把任意域名都解析到 192.168.4.1，所以随便输个网址也能进页面。）
3. 页面上点「扫描附近 WiFi」选网络（也可手动输入 SSID）→ 填密码 → 「保存并连接」。
4. 设备立刻用这组凭据连接并等待最多 20 秒（`RAN_PROV_CONNECT_TIMEOUT_S`）：
   - 成功：页面显示 IP，热点 1.5 秒后自动关闭，设备切回 STA 继续正常采集/推流。
   - 失败：页面显示原因（`没找到这个名字的 WiFi` / `认证失败：密码不对` / `密码校验超时` /
     `路由器拒绝关联` 等，带 reason code），热点保持开启，用户可以改完直接重试。

其它触发方式：有 WiFi 记录但**连续 60 秒**（`RAN_PROV_FALLBACK_S`）没有 IP → 自动进配网
（开机起算，运行中掉线也从掉线那一刻起算，路由器恢复拿到 IP 就取消）；
配网期间设备仍在后台重连路由器，路由器恢复了（比如只是路由器自己重启）会自动连上并关掉热点。

配网期间的实现细节见第 10 节第 11 条；`wifi_cred.c` 负责把凭据存进 NVS（namespace `wifi`）。

> 想改成连自己局域网的服务端：host 填电脑 IP、port 填 8000、`Use TLS` 关掉即可，
> 其余代码不用动。
>
> 已删除的选项：早期版本有一个 `RAN_ENABLE_I2S_TX`（I2S 播放占位通道，默认开）。
> 它用 I2S_NUM_1 却复用了 I2S0 的 BCLK/WS 引脚，会让录音失去时钟，已连同占位代码
> 一起删除。

服务端联调：先启动服务端 `uvicorn`（见 remoteAudioServer 工程 README），
设备上电后在服务端 Web 页面设备列表中应看到设备上线，点「开始监听」即可听到
实时声音（设备日志会打印 `实时推流 开始`）。

## 4. 与服务端 remoteAudioServer 的协议对应

设备连接 URL：`ws://<host>:8000/ws/device/<device_id>`，全部 JSON 为文本帧。

| 协议内容 | 固件实现位置 |
|---|---|
| 上线 `{"type":"hello","device_id","fw":"2.5.2","recording_enabled","sd_ok","config":{"vad_threshold","retention_days"}}` | `ws_client.c::send_hello()`，每次连接/重连后发送 |
| 心跳 `{"type":"ping","ts":..}`，每 15s | `ws_client.c::ws_task()`（服务端 45s 判离线） |
| 下行心跳 `{"type":"hb","ts":..,"listeners":N}`（服务端每 20s） | `ws_client.c::dispatch_text()` 读 `listeners`（v2.4：连续 3 条为 0 → 自动停流） |
| **WS 协议级心跳**（RFC6455 PING 帧，每 10s；pong 超时 30s 判死链） | `ws_client.c::ws_task()` 的 `ping_interval_sec` / `pingpong_timeout_sec`（v2.2 新增） |
| 事件 `recording_saved`（file/size/duration_ms，另带 duration 秒） | `recorder.c` 编码任务落盘后 → `ws_client_send_recording_saved()` |
| 事件 `recording_state`（enabled） | `recorder_set_enabled()` → `ws_client_send_recording_state()` |
| 命令 `start_stream` / `stop_stream` | `ws_client.c::dispatch_text()` → `audio_stream_set_active()` |
| 命令 `set_recording`（持久化 NVS + 回 response + 上报事件） | `cmd_set_recording()` → `recorder_set_enabled()` → `device_config.c` NVS |
| 命令 `set_config`（vad_threshold / retention_days，持久化 NVS，回当前 config） | `cmd_set_config()` |
| 请求 `list_recordings`（可带 date 过滤） | `cmd_list_recordings()` → `sd_storage_list_recordings()`，files 元素同时含 `file/name/date/size/mtime/duration` |
| 请求 `play_file` | `cmd_play_file()` → `xfer_task2()` 分块任务 |
| 请求 `stop_file` | `cmd_stop_file()`（置传输取消标志，旧任务自行退出） |
| 断线指数退避重连（v2.2：300ms 起，上限 3s），重连重发 hello；若断线前在推流则自动续流 | `ws_task()`（自行管理）+ `ws_event_handler()` 的 `s_want_stream` |

**binary 帧（设备发送）字节布局**（`board.h` / `audio_stream.c` / `ws_client.c`）：

| 字节 | 含义 |
|---|---|
| Byte0 | 帧类型：`0x01` 实时 PCM 广播 / `0x02` 录音文件块（按 req_id 路由） |
| Byte1-4 | req_id，uint32 大端；实时帧固定 0 |
| Byte5-6 | seq，uint16 大端；文件块从 0 递增，**0xFFFF 表示最后一块**；实时帧忽略 |
| Byte7+ | payload：实时帧=640 字节原始 PCM；文件块=≤2048 字节 Ogg Opus 原始内容 |

文件回传结束后发 `{"type":"response","req_id":N,"ok":true,"done":true}`；
找不到文件回 `ok:false,"error":"file_not_found"`。路径做了 `..`/绝对路径过滤。

## 5. 录音与存储

- 目录：`/sdcard/recordings/YYYY-MM-DD/HH-MM-SS.opus`（时间未同步时写入
  `unsynced/` 目录，避免错误日期）；时区固定 UTC+8（SNTP 同步后）。
- 先写 `*.opus.tmp`，录完 flush Ogg EOS 页后 `rename` 为 `.opus`（同目录原子落盘）。
- 保留策略：剩余空间 < 200MB 或文件年龄 > retention_days 时，按日期目录
  从旧到新删除；启动时与每次录音结束后检查（`sd_storage_apply_retention()`）。
- 无 SD 卡或录音开关关闭时：**不做 VAD、不写文件，但实时推流照常工作**。
- VAD：滑动窗（默认 8 帧=160ms）能量法，RMS 超阈判有声；触发时携带
  200ms 前滚缓冲（默认 10 帧）一起写入文件开头。

## 6. 组件依赖说明（适配点）

依赖声明在 `main/idf_component.yml`：

### 6.1 78/esp-opus（Opus 编解码）

- xiaozhi-esp32 作者维护的 libopus 官方移植，提供标准 libopus API：
  `opus.h` / `opus_encoder_create` / `opus_encoder_ctl` / `opus_encode`，
  组件管理器自动下载到 `managed_components/78__esp-opus/`。
- 注意：组件仓库里不存在 `espressif/esp-opus` 这个包（会报 version solving
  failed），包名必须写 `78/esp-opus`。
- 所有 Opus 调用隔离在 **`main/opus_encoder.c`** 薄封装内（`opus_encoder_open /
  opus_encoder_encode / opus_encoder_close`），参数固定 16kHz/单声道/VOIP/32kbps
  CBR/复杂度3。`main/CMakeLists.txt` 会自动探测 `managed_components/` 下实际
  下载到的组件目录名（`78__esp-opus` / `espressif__esp-opus` /
  `esphome__micro-opus` 等）并加入链接，无需手改 CMake。
- **若组件管理器拉取失败或想用其它实现**，只需替换 `main/idf_component.yml`
  中的依赖为下列任意一个 API 兼容组件，代码无需改动：

  ```yaml
  dependencies:
    esphome/micro-opus: "^0.4.1" # 备选：ESPHome 的 libopus 移植
    # 或把官方 libopus/xiph opus 源码放进 components/opus（CMakeLists 会自动探测）
  ```

  备选组件都提供标准 `opus.h`。若使用的组件只暴露私有封装头（如 esp_audio_codec
  的 `esp_audio_enc.h`），则只需重写 `opus_encoder.c` 三个函数的内部实现，
  头文件接口保持不变。

### 6.2 espressif/es7210（音频 ADC 配置）

- 只负责通过 I2C 写 ES7210 寄存器，**I2S 数据通路仍由本工程自己管**
  （`main/audio_i2s.c`），不引入 esp_codec_dev。
- 本工程**不再自维护 ES7210 寄存器表**：上一版手写序列里有未经真机验证的分频值
  （作者自己标注"简化设置"），已整体换成官方组件。
- 本工程自己的封装文件叫 **`main/es7210_drv.c` / `es7210_drv.h`**，故意不叫
  `es7210.c/h` —— 官方组件头文件就叫 `es7210.h`，同名会把官方头文件遮住。
- 调用顺序：`es7210_init()` → 装 I2C 总线 → 在 0x40~0x43 探测芯片地址 →
  `es7210_new_codec()` → `es7210_config_codec()`（16kHz/16bit/标准 I2S/非 TDM/
  MIC1+MIC2/24dB）→ `es7210_config_volume(0)`。

## 7. Ogg Opus muxer

自行实现的轻量 muxer（`ogg_muxer.c`，参照 RFC 3533/7845）：

- 文件头两页：`OpusHead`（ver=1, ch=1, pre-skip=312, input rate=16000, gain=0,
  mapping=0）与 `OpusTags`（vendor + 0 注释）；BOS 页 header_type=0x02。
- 每个 20ms Opus 包封装为一个 Ogg 页；**granule_position 按 48kHz 域累计**
  （每包 +960，RFC 7845 规定 Ogg Opus granule 恒为 48kHz）。
- 结束页置 EOS（0x04）：close 时回读最后一页改写 header_type 并重算 CRC。
- CRC32 为标准 Ogg 多项式 `0x04c11db7`（表驱动，初值 0、不反射）。
- 文件时长（list_recordings 的 duration 字段）由末页 granule/48000 解析，
  不依赖文件名。

## 8. 任务划分

| 任务 | 核 | 优先级 | 栈 | 职责 |
|---|---|---|---|---|
| audio_pipe | 0 | 12 | 8KB | I2S 读帧 → 推流发送 → VAD → 录音状态机 → 编码队列 |
| opus_enc | 1 | 11 | 32KB | 取队列帧 → Opus 编码 → Ogg 写页 → STOP 时 rename/清理/上报 |
| diag | 0 | 2 | 3KB | 每 10s 打一行运行状态心跳（诊断黑匣子，见第 10 节第 7 条） |
| ws_mgr | 1 | 10 | 6KB | WS 建连/心跳/退避重连/JSON 命令分发（事件上下文） |
| file_xfer | 1 | 8 | 6KB | play_file 分块读 SD 发 0x02 帧（按需创建，结束自删） |
| oled | 不固定 | 4 | 4KB | 1Hz 刷新状态屏（默认关闭） |
| btn | 不固定 | 6 | 2.5KB | GPIO0 轮询消抖 |

大缓冲（回传帧 2KB+头、列表数组等）优先走 PSRAM；I2S DMA 缓冲为内部 RAM。
SD 目录操作/rename 用递归互斥锁保护；录音写新文件与回传读旧文件可并行。

## 9. 常见问题

- **看不到设备上线**：确认服务端 IP/端口（menuconfig）、防火墙放行 8000、
  WiFi 为 2.4GHz（ESP32-S3 不支持 5GHz）。
- **TF 卡挂载失败**：日志打印 TF 卡相关错误，检查卡是否插好、格式是否为
  FAT32（exFAT 也已启用长文件名支持）；实时监听不受影响。
- **ES7210 初始化失败**：日志会打 `ES7210 在 0x40~0x43 均无应答` 或
  `es7210_new_codec 失败`。先确认用的是本固件（地址会自动探测），再查排线与
  供电；这块板子上芯片地址是 0x41。
- **录音不触发**：说话音量不足或 VAD 阈值过高，可在 Web 端 set_config 调低
  阈值（如 500），或改 menuconfig 默认值。
- **实时推流有声音但录音文件很小**：多为 SD 卡写卡顿，检查卡是否为低速卡；
  I2S DMA 已配到 120ms 缓冲。
- **Opus 组件下载失败**：见第 6.1 节，改 `idf_component.yml` 换源或离线放入
  components 目录后 `idf.py reconfigure`。
- **OLED 不亮**：实战派 S3 上没有 OLED，`RAN_ENABLE_OLED` 默认关闭，属正常。
- **设备"无声掉线"（串口日志突然停在某一行，服务端显示离线，之后什么都没有）**：
  先看 `diag` 心跳（默认每 10s 一行 `心跳 Ns | ws=.. 推流=.. | 内部堆 ..`）：
  心跳还在继续 → CPU 活着，看 `ws=` 是否为 0（只是断链，ws_mgr 会自动重连）以及
  `慢发送/最慢/丢帧` 是否在涨（网络写卡住）；心跳也停了、后面直接是启动日志 →
  芯片复位；心跳停了又没有启动日志 → 有任务挂死。见第 10 节第 7 条。

## 10. 已知坑（改过的地方，别再踩）

1. **ES7210 的 I2C 地址是 0x41，不是 0x40**。板上 AD0 被拉高。旧代码写死 0x40，
   每次写寄存器都 NACK，但 `es7210_init()` 照样返回 `ESP_OK`，现象是"初始化
   成功、录出来全是静音"。现在驱动会在 0x40~0x43 探测，探不到直接返回错误。
2. **不要给 I2S0 之外再建一个 TX 控制器去用同一组 BCLK/WS 引脚**。同一组引脚
   只能由一个控制器驱动，否则先把 I2S0 的时钟挤掉，录音直接失去时钟
   （现象：`i2s_channel_read` 每帧都等满超时、录音慢几十倍）。
   以后要加播放，改成 I2S0 全双工（TX/RX 两个句柄共用同一份时钟配置），并且
   记得**先 enable TX 再 enable RX**（全双工下时钟发生器在 TX 侧）。
3. **`es7210.h` 这个文件名被官方组件占用了**，本工程自己的封装必须叫
   `es7210_drv.*`，否则 include 会命中自己的头文件、官方声明全丢。
4. **`i2s_channel_read()` 的 timeout 单位是毫秒**，不要传 `pdMS_TO_TICKS()`；
   本项目 `CONFIG_FREERTOS_HZ=1000` 时数值正好相等，换配置就会算错。
5. **WebSocket 握手响应头缓冲区默认不够**（ESP-IDF v5.5 实测）。Coze 网关回的 101
   响应头约 980 字节（`X-Tt-Trace-Host` 一条就 352 字节，另有 `X-Tt-Trace-Id`、
   `x-tt-logid`、`EagleId`、`Via`、`X-Faas-Gateway-Instance-Name` 等），而 IDF 里 ws
   传输层用**同一块** `CONFIG_WS_BUFFER_SIZE` 缓冲先拼 Upgrade 请求、再累积读响应头，
   默认 1024 不够，超了直接报 `transport_ws: Header size exceeded buffer size`，
   表现为 TLS 握手已成功（`Certificate validated`）但紧接着
   `esp_transport_connect() failed with -1`、握手永远走不完。
   本工程已在 `sdkconfig.defaults` 设 `CONFIG_WS_BUFFER_SIZE=4096`。
6. **`opus_enc` 任务栈必须 32KB**。libopus 编码器（浮点构建）很吃栈，16KB 会在
   真机第一次 `opus_encode()` 时直接栈溢出：日志是
   `***ERROR*** A stack overflow in task opus_enc has been detected`，紧接着 panic
   重启，现象固定在打印"开始录音 -> …"之后不久出现。本工程 `ENC_TASK_STACK` 已改为
   32768（与 `s3_opus_rec_play` 工程一致），不要再往下调。
7. **不要用无限等待去抢 WS 发送锁**（v2.1 加固）。`ws_client.c` 里所有发送的
   `xSemaphoreTake(s_send_mtx, ...)` 必须是**有界等待**（当前 500ms），发送耗时超
   300ms 会打 `慢发送` 警告、取锁超时/发送失败会打日志并计数。原因：真机上出现过
   一次"服务端反复开关监听几次后设备掉线"，日志停在最后一条 `stream: 实时推流 停止`
   之后**再无任何输出**（没有 `WebSocket 断开`、也没有 panic），服务端设备列表里该
   设备消失。服务端侧已实测确认：`/ws/listen` 开关只驱动下发
   `start_stream`/`stop_stream`，设备 WS 不会被踢；服务端"在线"= 设备 WS 会话还在，
   所以"掉线"一定是设备的 TCP 连接真的断了。为把这种"无声现场"变成可判读的日志，
   v2.1 同时加了独立 `diag` 心跳任务（每 10s 一行）与发送统计，并把 PCM 帧缓冲改成
   静态预分配（不再每帧 `malloc/free`）。固件版本号随之升到 **2.1.0**。

8. **外网长连接约 300s 被平台接入层掐断 → 必须发 WS 协议级 PING（v2.2）**。实测从外网连
   `wss://remoteaudio.coze.site/ws/device/<id>`：不开协议级 ping 时连接在 288 / 300.4 /
   302.5 / 302.8 秒被平台接入层掐断，客户端连 close 帧都收不到；**应用层 JSON ping（15s）
   和 50Hz 音频数据都不算"活着"**。开启协议级 ping（10~20s）后同一连接活过 425s，越过
   5 分钟不再断。因此 `ws_task()` 的 `esp_websocket_client_config_t` 显式设
   `ping_interval_sec = 10`、`pingpong_timeout_sec = 30`（3×ping 间隔，容忍连丢 2 个 pong
   不误杀；框架不显式设置时默认 120s，现场要多等 2 分钟才判死）。
   同时重连退避 1s/30s → **300ms/3s**，并新增 `s_want_stream` 意图：断开时照旧停推流
   （断开期间不发），重连成功重发 hello 后若意图仍为开就自动恢复推流，用户不必重新点
   "开始监听"；重连日志会打出第几次重连 + 本次退避毫秒数。固件版本号随之升到 **2.2.0**。
   注意：协议级 PING 由 `espressif/esp_websocket_client` 组件发出，**1.6.0 之前**的版本在
   收到数据后会重置 ping 计时（历史实现相当于"只有无流量才发 ping"，1.6.0 修复为
   "active traffic 下也周期性发 PING"）。若本地解析到 1.2.x~1.5.x，协议 ping 可能被服务端
   流量抑制，务必确认本地组件版本 ≥1.6.0。
   注：`main/idf_component.yml` 已把约束提到 `espressif/esp_websocket_client: "^1.6.0"`。

9. **固件"连不上"的三个真凶（v2.3 修复）**。现象：v2.1 能连、v2.2 连不上（串口停在
   `getaddrinfo() returns 202` / `EAI_FAIL`，随后 `Reconnect after 10000 ms` 循环）。
   逐条查证后的结论：
   - **`reconnect_timeout_ms = 0` 不等于禁用自动重连**。上游组件源码（1.2.3 与最新版是同一段）：
     `if (!disable_auto_reconnect && reconnect_timeout_ms <= 0) wait_timeout_ms = 10000;`
     ——`0` 只是回落成默认 10s，`auto_reconnect` 仍为 true，**各 1.x 版本语义一致**。
     所以 v2.1/v2.2 一直是「组件自己每 10s 重连」+「`ws_task` 停掉再 start」两套逻辑抢同一个
     客户端（日志里 `Reconnect after 10000 ms` 是组件打的，不是固件打的）。v2.3 显式设
     `.disable_auto_reconnect = true`，重连只剩 `ws_task()` 一条路径，节奏完全可控。
   - **建连前只等 SNTP、没等 IP**。Wi-Fi 掉线窗口里 `wss` 直接硬连 → TCP 能试、DNS 解析不了
     → `getaddrinfo() returns 202`。v2.3 在建连前加 `wifi_mgr_is_connected()` 门（最多等 15s）。
   - **`esp_websocket_client_start()` 返回值从没被检查**。组件在 `state >= INIT` 时直接返回
     `ESP_FAIL` 并打 `The client has started`；旧代码把这次失败当"正常"，既没清客户端状态也
     没退避，会一直连不上。v2.3 检查返回值 + 失败退避重试。
   另两处加固：`wifi_mgr.c` 断线回调不再 `vTaskDelay(2000)` 阻塞事件循环（改一次性
   `esp_timer`），并新增 RSSI/信道日志；`ws_task` 连续失败 6 次（约 1 分钟）就
   `destroy + init` 重建客户端（`ws_client.c::ws_hard_reset()`），把残留状态清干净。
   诊断心跳行新增 `重连 N 建连失败 N 硬复位 N` 三个计数。固件版本号随之升到 **2.3.0**。

10. **别对着空气推流（v2.4 修复）**。场景：设备推流中断网 → 监听端在这期间关掉了网页（服务端
   `listeners` 1→0）→ 服务端那时下发的 `stop_stream` 因为设备不在线而丢失 → 设备网络恢复、重连
   后按本地记忆继续推流，但已经没人听了。空推的量不小：`647B/20ms` ≈ **0.26 Mbps，一小时约 116MB**，
   还让 Wi-Fi 一直醒着。
   根因是**状态归属错了**：v2.2 起 `s_want_stream` 是"设备自己记住的意图"，重连后无条件续流。
   服务端才是"现在有没有人在听"的唯一权威，所以 v2.4 把两边都改了：
   - **固件**：重连后不再凭记忆续流。发完 hello 先等最多 3s 的**服务端裁决**
     （`start_stream` = 有人听 → 续流；`stop_stream` = 没人听 → 不推；**一直没消息也不推**，
     宁可少推一次也不空推；服务端随时可用 `start_stream` 唤醒）。
   - **固件兜底看门狗**：读下行心跳里的 `listeners`，连续 3 条（≈60s）为 0 且正在推流 → 自动停流。
     即使服务端一条裁决都没发（老版本/换实现），也不会无限空推。
   - **服务端**：设备 hello 后按真实 `listeners` 复盘一次（0 → 下发 `stop_stream`，>0 → `start_stream`），
     并把"45s 宽限内设备回来就自动 start_stream"改成先看监听者数量；心跳带上 `listeners` 字段。
   诊断心跳行新增 `监听 N`（`-1` = 服务端没报）。固件版本号升到 **2.4.0**。
   注意：**停实时流不影响本地录音与回放**——VAD 录音照常落 SD 卡，回放也不依赖推流。

11. **配网别踩的三个坑（v2.5 网页配网）**
   - **凭据不能只认编译期**。老实现把 `CONFIG_RAN_WIFI_SSID/PASSWORD` 写死在 `wc.sta`，
     换路由器就得重新编译烧录。v2.5 改成三级优先级：`NVS("wifi")` → Kconfig（**仍是占位值
     `myssid` 视为"没配"**）→ 进配网。这样老板子行为不变，新板子开箱能配。
   - **进配网不删旧凭据**。用户明确要求"以前的记录不要删除，除非用新的 WiFi 设置去覆盖"，
     所以 `wifi_prov.c` 全程只读不写 NVS，只有 `wifi_mgr_connect_with()`（用户点保存）才覆盖。
   - **别在定时器/事件回调里做 WiFi 重活**。60s 超时转配网走 `esp_timer` → 只置标志 +
     `xTaskNotifyGive`，真正的 `esp_wifi_set_mode(APSTA)`、HTTP 服务、DNS 任务都由
     `prov_task`（绑 core 1，不挤 core 0 的音频任务）去做；扫描和"保存后等 IP"则放在
     `httpd` 任务里（配网页请求本来就在那跑）。反例是把 `esp_wifi_scan_start(block=true)`
     塞进事件回调，会把默认事件循环整条堵住（v2.3 那个 `vTaskDelay(2000)` 是同一类病）。
   - 附带一条现场现象：**扫描会把射频拉到各信道走一圈**，连着热点的手机可能瞬断 1~2 秒，
     页面刷新一下就好；扫完固件会把信道拉回热点信道。固件版本号升到 **2.5.1**。
   - **别把大数组放在任务栈上**。2.5.0 首次实机烧录开机就崩：`***ERROR*** A stack overflow in
     task prov_dns has been detected` → `rst:0xc` 重启循环。原因：`prov_dns` 栈只给 3KB，
     而 `dns_task` 里有 `uint8_t buf[512]` + `resp[512]` 两个局部数组（合计 1KB），再加上
     lwIP socket 调用与日志开销，直接穿栈。2.5.1 把这两个缓冲改成静态数组，任务栈提到
     4KB（`prov_task` 同时 4KB→5KB 留余量）。

12. **连接状态必须与底层一致（v2.5.2：修「开关 WiFi 后设备一直连不上」）**
   - **现象**：开/关 WiFi 之后设备再也连不上服务端，但 `diag` 一路显示 `ws=1`、服务端设备列表
     里根本没有它；`重连/建连失败/硬复位` 计数完全冻结，`发送失败/最慢` 也不再变化；只有断电
     重启才能恢复。
   - **根因**：`ws_client_is_connected()` 只读本模块事件位 `BIT_CONNECTED`，而 `wifi_mgr.c` 的
     `STA_DISCONNECTED` 只重连 WiFi、**从不通知 WS 模块**。WiFi 掉了之后底层 socket 已经死了，
     组件又没上报 `DISCONNECTED`，于是事件位残留 → `ws_task` 一直以为"已连接"、永远不进重连分支；
     `send_text()` 又在组件状态为未连接时静默 `return false`（不计 `s_send_fail`），现场看不出异常。
   - **修法**：① `wifi_mgr.c` 掉线回调调用 `ws_client_notify_link_down()`（只置 volatile 标志，
     不阻塞事件循环），`ws_task` 下一轮清 `BIT_CONNECTED`、停客户端、等 WiFi 回来再重连，且
     **不计失败**；② `ws_client_is_connected()` 交叉核对 `esp_websocket_client_is_connected()`；
     ③ 进重连分支前 / 中断分支显式清残留状态位（否则 `xEventGroupWaitBits` 会"假成功"、`start()`
     被组件以 "The client has started" 拒绝）；④ 应用层 ping 连续 2 次发不出去 → 判死链；
     ⑤ 静默看门狗：180s 收不到服务端任何数据（服务端 20s 一条下行心跳）→ 判死链重建；
     ⑥ "Wi-Fi 没拿到 IP" 只等不计失败，避免断网把 重连/建连失败/硬复位 刷到几百次。
   - 固件版本号升到 **2.5.2**，协议零改动。
