# remoteAudioServer

多设备音频采集系统的「服务端 + Web 监听端」。采集端为 ESP32 设备（本项目以**内置设备模拟器**代替），通过 WebSocket 长连接接入；服务端是无状态 WebSocket 路由中枢（状态仅存内存，不存储任何录音文件）；监听端是服务端直接托管的深色科技风原生 HTML/JS 单页应用，多终端可同时打开、无需登录鉴权。

> 本仓库**不包含 ESP32 固件**。内置的「设备模拟器」以真实 WebSocket 客户端身份接入系统，**无需任何硬件即可完整演示采集 / 实时监听 / 录音回放 / 远程开关**全链路。

---

## 一、快速启动

```bash
# 1. 安装依赖（已有 fastapi/uvicorn 可跳过）
pip3 install -r requirements.txt

# 2. 启动服务（默认端口 8000）
python3 -m uvicorn server.main:app --host 0.0.0.0 --port 8000
```

> 若设置了环境变量 `DEPLOY_RUN_PORT`，会优先使用该端口（便于在云端/容器中部署，启动脚本已按 `.coze` 自动读取）。

打开浏览器访问 `http://localhost:8000`：

1. 点击顶部「启动 2 台模拟设备」；
2. 在设备卡片上点击「▶ 开始监听」——戴上耳机可听到 880Hz / 660Hz 的蜂鸣提示音；
3. 展开「录音文件」→ 点击某条记录，可在右下角播放器回放虚拟录音；
4. 切换设备卡片上的录音开关，可远程开启/关闭设备的录音功能。

---

## 二、项目结构

```
remoteAudioServer/
├── server/
│   ├── main.py          # FastAPI 应用：REST + WebSocket 路由 + 静态托管
│   ├── hub.py           # WebSocket 路由中枢（设备/监听者管理、广播、请求-响应路由）
│   ├── protocol.py      # 协议层：二进制帧编解码、常量
│   ├── simulator.py     # 设备模拟器（虚拟 ESP32，真实 WebSocket 客户端）
│   ├── static/
│   │   ├── index.html       # 监听端单页应用（深色科技风、中文界面）
│   │   └── pcm-worklet.js   # AudioWorkletProcessor（实时 PCM 播放）
│   └── assets/*.opus    # 预置的 Ogg Opus 录音资产（供虚拟设备回放）
├── requirements.txt
├── README.md
└── .coze                # 云环境构建/运行配置
```

---

## 三、协议说明

### 3.1 二进制帧格式（设备 ↔ 服务端 ↔ 监听端 统一）

| 字节 | 字段 | 说明 |
|------|------|------|
| Byte0 | type | `0x01` 实时音频帧（广播）；`0x02` 录音文件块（按 req_id 路由） |
| Byte1-4 | req_id | uint32 大端；实时帧固定为 0 |
| Byte5-6 | seq | uint16；文件块序号从 0 递增，`0xFFFF` 表示最后一块 |
| Byte7+ | payload | 实时帧=640 字节 PCM(16kHz/16bit/mono)；文件块=Ogg Opus 字节(2048/块) |

### 3.2 WebSocket 端点

- **设备**：`ws://host/ws/device/{device_id}`
  - 首条消息 `hello`：`{type:"hello", device_id, fw, recording_enabled, sd_ok, config}`
  - 每 15s 发送 `{type:"ping", ts}`；服务端 45s 无消息判离线清理
  - 上报事件：`{type:"event", event:"recording_saved", file, duration}`
- **监听者**：`ws://host/ws/listen/{device_id}`
  - 服务端为每个设备维护监听者集合：
    - 首个监听者加入（0→1）→ 下发 `{"cmd":"start_stream"}`
    - 最后监听者离开（1→0）→ 下发 `{"cmd":"stop_stream"}`
  - 实时帧（0x01）原样广播给该设备全部监听者
  - 事件（`recording_saved` / `device_offline` / `stream_state` / `request_failed`）广播给所有监听连接

### 3.3 请求-响应中继（监听端发起的命令）

浏览器发送 `{type:"request", req_id:<int>, cmd:<string>, ...参数}`，服务端将其转发给对应设备；设备以 `{type:"response", req_id, ok:true, ...}` 或二进制文件块回传，服务端按 req_id 路由回**发起请求的那个浏览器连接**。

支持的 `cmd`：

| cmd | 参数 | 设备响应 |
|-----|------|----------|
| `list_recordings` | `date`（可选） | `{type:"response", ok, files:[{name,date,size,duration}]}` |
| `play_file` | `file:"日期/文件名.opus"` | 以 `0x02` 二进制帧按 req_id 回传文件内容 |
| `stop_file` | `req_ref` | 停止发送文件块 |
| `set_recording` | `enabled:bool` | 返回当前 `recording_enabled` |
| `set_config` | `vad_threshold` / `retention_days` | 返回更新后的 `config` |

> 服务端会给每次监听端请求分配**独立内部 req_id**（并做原 id↔内部 id 双向映射），避免多个监听者/多设备间 req_id 冲突。

### 3.4 REST 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 监听端单页应用 |
| GET | `/api/devices` | 设备列表 `[{device_id, online, recording_enabled, listener_count, fw, sd_ok}]` |
| GET | `/api/simulator/status` | 模拟器运行状态 |
| POST | `/api/simulator/start` | 启动模拟设备 `{device_ids?或count}` |
| POST | `/api/simulator/connect` | 重连指定模拟设备 `{device_ids}` |
| POST | `/api/simulator/disconnect` | 断开指定模拟设备 `{device_ids}`（演示离线） |

### 3.5 音频参数

- 实时流：16kHz / 16bit / 单声道 PCM，每帧 640 字节 = 20ms
- 录音文件：Ogg Opus（44.1k 内任意；本项目资产为 16kHz mono），分块 2048 字节，末块 seq=0xFFFF

---

## 四、内置设备模拟器

`server/simulator.py` 实现虚拟 ESP32：每个模拟设备跑在**独立线程 + 独立 asyncio 事件循环**中，以真实 WebSocket 客户端连到本机 `/ws/device/{id}`，完整走设备协议。

- `hello` / 定时 `ping`（保持在线）；
- 响应 `start_stream`：生成 1 秒循环的合成蜂鸣音（`sim-101` 880Hz、`sim-102` 660Hz、`sim-103` 440Hz，周期 0.35s 响 / 0.65s 静），按 20ms/帧实时推送 0x01 PCM 帧——浏览器端能真实听到；
- 内存维护虚拟录音列表（预置 + 运行时新增），响应 `list_recordings`；
- 响应 `play_file`：从 `server/assets/*.opus` 读取真实有效的 Ogg Opus 字节，按 0x02 分块路由回传（末块 seq=0xFFFF）；
- 响应 `set_recording` / `set_config`；
- 录音开关打开时每 15~30s 随机产生一个 `recording_saved` 事件（模拟 VAD 触发录音）。

**可断连 / 重连**：页面顶部模拟器区域每台设备有「连接 / 断开」按钮，用于演示设备离线、恢复在线。

---

## 五、健壮性设计

- WebSocket 异常断开统一清理：设备心跳超时（45s）由巡检任务强制下线并广播 `device_offline`；
- req_id 路由超时/设备离线清理：设备失效后其挂起的请求自动失败并通知监听端；
- 监听者与设备各自维护集合，断开即回收，0↔1 正确触发 `start_stream` / `stop_stream`；
- 音频播放全部走 **AudioWorklet**（`pcm-worklet.js`），不使用已废弃的 ScriptProcessorNode；
- 页面强制「同一时间仅一个音源」：开始监听会自动停止回放，开始回放则忽略实时帧。

---

## 六、常见问题

- **听不到声音**：确认浏览器允许自动播放（点击「开始监听」属于用户手势，通常已解锁）；音量电平条有跳动说明链路正常。
- **录音列表空白**：稍后重试（设备需先在线），或先「开始监听」建立连接再展开录音面板。
- **端口冲突**：用 `--port` 指定其它端口，浏览器端代码会自动使用当前域名/端口。