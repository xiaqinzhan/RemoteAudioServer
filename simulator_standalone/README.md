# 独立设备模拟器（sim_device.py）

在【本地】以真实 WebSocket 客户端身份连接（云端或局域网）服务器，把本机模拟成一台或多台 **ESP32 采集设备**，让整套系统无需任何真实硬件即可演示 / 测试。

它与项目内置的 `server/simulator.py` 走**同一套设备协议**，但被抽成可独立运行的单文件脚本：

- 自带协议层（无包依赖，无需放入项目里运行）；
- 内嵌一段真实 Ogg Opus 录音样本（base64），即使本机没有 `.opus` 资产也能回放录音；
- 用纯 Python 合成蜂鸣 PCM，**不依赖 numpy**；
- **实时监听可推送本地音频**：用 `--live-file` 让设备把本地 opus/ogg/wav/mp3 等文件（经 ffmpeg 解码为 16k PCM）当作实时流推出去，替代合成蜂鸣。

## 依赖

Python 3.8+，只需 `websockets`：

```bash
pip install websockets
```

> 使用 `--live-file`（实时推本地音频）时另需系统安装 **ffmpeg**：
> macOS `brew install ffmpeg`；Ubuntu/Debian `sudo apt install ffmpeg`；Windows 见官网下载加入 PATH。
> 未安装或解码失败会自动回退为合成蜂鸣，不影响其余功能。

## 用法

```bash
# 连云端 HTTPS 部署（自动用 wss://），模拟 2 台设备
python3 sim_device.py --url https://<你的云端域名> --device-ids sim-101,sim-102

# 连局域网 / http（默认 ws://）
python3 sim_device.py --host 192.168.1.10 --port 8000 --device-ids sim-101

# 连 http 但服务器在 HTTPS 反向代理后需要 wss
python3 sim_device.py --host xxx.io --port 443 --wss --device-ids sim-103

# 单台、自定义音调频率；关闭自动生成录音事件
python3 sim_device.py --url https://<域名> --device-ids sim-101 --freq 520 --no-auto-record

# 使用本地 .opus 资产作为虚拟录音内容（覆盖内置样本）
python3 sim_device.py --host 192.168.1.10 --port 8000 --device-ids sim-101 --assets-dir ./opus_assets

# 【实时监听推送本地音频】设备开始监听时播放本地 opus（ffmpeg 解码后循环推流）
python3 sim_device.py --url https://<域名> --device-ids sim-103 --live-file ./voice.opus

# --live-file 也可传目录：目录内多个音频按文件名顺序拼接、整体循环
python3 sim_device.py --url https://<域名> --device-ids sim-103 --live-file ./live_audios
```

## 参数

| 参数 | 说明 | 默认 |
|------|------|------|
| `--url` | 服务器根地址，`https://...` 自动用 wss，`http://...` 用 ws（与 `--host` 二选一必填） | - |
| `--host` | 服务器主机/域名（配合 `--port`） | - |
| `--port` | 服务器端口 | `8000` |
| `--wss` | 强制使用 wss://（服务器走 HTTPS 时配合 `--host` 用） | 关 |
| `--device-ids` | 设备ID，逗号分隔 | `sim-101` |
| `--freq` | 蜂鸣频率 Hz（0=按设备默认；仅单设备有意义） | 0 |
| `--assets-dir` | 本地 `.opus` 资产目录（虚拟录音内容） | 内置样本 |
| `--live-file` | 实时监听推送的本地音频：单个文件或目录（opus/ogg/wav/mp3/m4a/flac），ffmpeg 解码为 16k PCM 循环推流；失败回退蜂鸣 | 无（用合成蜂鸣） |
| `--no-auto-record` | 关闭定时自动生成 `recording_saved` 事件 | 关 |

## 设备协议（与 `server/protocol.py` 一致）

- 连接 `ws(s)://host/ws/device/{device_id}`
- 首帧 JSON `hello` → 每 15s 发 `ping`；
- 响应服务端 `{"cmd":"start_stream"}`：持续推送 `0x01` PCM 帧（16kHz/16bit/mono，640B=20ms）。默认是合成蜂鸣音；指定 `--live-file` 时为本地音频（ffmpeg 解码后的 PCM，无缝循环），`{"cmd":"stop_stream"}` 停止；
- 响应 `list_recordings` / `play_file` / `stop_file` / `set_recording` / `set_config`（JSON 请求-响应，文件用 `0x02` 分块回传）；
- 录音开关打开时每 15~30s 随机产生一个 `recording_saved` 事件。