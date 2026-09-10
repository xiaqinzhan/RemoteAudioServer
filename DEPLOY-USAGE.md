# remoteAudioServer 部署与使用指南

> 本文汇总本项目的**服务端部署**、**独立设备模拟器使用**、以及**浏览器监听/回放**的完整操作说明。

---

## 目录

1. [项目结构速览](#一项目结构速览)
2. [方式 A：本地部署服务器端](#二方式a本地部署服务器端)
3. [方式 B：云端部署（当前）](#三方式b云端部署当前)
4. [独立设备模拟器（本地运行、连接远程服务器）](#四独立设备模拟器本地运行连接远程服务器)
5. [浏览器使用（监听 / 回放）](#五浏览器使用监听--回放)
6. [常见问题与要点](#六常见问题与要点)

---

## 一、项目结构速览

```
remoteAudioServer/
├── server/
│   ├── main.py          # FastAPI 应用：REST + WebSocket 路由 + 静态托管
│   ├── hub.py           # WebSocket 路由中枢（设备/监听者管理、广播、请求-响应）
│   ├── protocol.py      # 协议层：二进制帧编解码、常量
│   ├── simulator.py     # 内置设备模拟器（随服务启动，自动起 sim-101/102）
│   ├── static/          # index.html 控制台页 + listen.html 监听页 + pcm-worklet.js
│   └── assets/*.opus    # 预置 Ogg Opus 录音资产
├── simulator_standalone/
│   ├── sim_device.py    # 独立设备模拟器（单文件、自带协议层、可连远程服务器）
│   └── README.md
├── requirements.txt
└── .coze                # 云环境构建/运行配置
```

---

## 二、方式 A：本地部署服务器端

> 一个 ws/wss「无状态路由中枢」：设备接入、监听者接入、实时音频广播、录音请求回放路由。**不存储任何录音文件。**

### 1. 拷贝到本机

把以下目录/文件拷到本地：

```
server/                 # main.py / hub.py / protocol.py / simulator.py / static/ / assets/
requirements.txt
```

### 2. 安装依赖

```bash
python3 -m venv .venv && source .venv/bin/activate   # 建议虚拟环境
pip install -r requirements.txt
```

### 3. 启动服务

```bash
uvicorn server.main:app --host 0.0.0.0 --port 8000 --reload   # 开发热重载
# 或
python3 -m uvicorn server.main:app --host 0.0.0.0 --port 8000
```

### 4. 访问

| 地址 | 内容 |
|------|------|
| `http://localhost:8000` | 控制台页面（设备卡片 / 实时监听 / 录音回放 / 录音开关） |
| `http://localhost:8000/listen` | 监听客户端页面（选设备实时听音 / 回放历史录音） |
| `http://localhost:8000/api/devices` | 设备状态 JSON |

服务端启动后**自动模拟起 `sim-101`、`sim-102`**，打开页面即可直接看到设备并监听，无需硬件。

### 5. 端口说明

- 端口读取顺序：环境变量 `DEPLOY_RUN_PORT`（存在则优先）→ 默认 `8000`。
- 换端口：`--port 5000`；浏览器端会自动使用当前访问的域名/端口。

### 6. 局域网 / 对外

- **局域网**：用电脑局域网 IP（如 `http://192.168.1.x:8000`），别用 `127.0.0.1`，放行防火墙端口。
- **公网 HTTPS**：可用内网穿透把 8000 映射成 `https://你的域名`，外部用 wss 连接。
- ⚠️ 服务端无鉴权、状态仅存内存，仅适合内网/演示。

---

## 三、方式 B：云端部署（当前）

当前云端访问域名（示例）：

```
https://<你的云端域名>.dev.coze.site
```

- 云端运行 `.coze` 配置自动读取端口 + HTTPS，外部统一走 `wss://`。
- 云端同样随服务自动模拟 `sim-101` / `sim-102`。
- 部署/更新代码后刷新页面即可，模型侧无需手动启停。

---

## 四、独立设备模拟器（本地运行、连接远程服务器）

> 用**本机 Python 脚本**模拟真实 ESP32 设备，连到「云端或局域网」服务器，全链路无需硬件。

### 前提

```bash
pip install websockets     # 唯一依赖
```

### A. 连云端（https 自动 wss）

```bash
python3 sim_device.py --url https://<你的云端域名> --device-ids sim-101
python3 sim_device.py --url https://<你的云端域名> --device-ids sim-101,sim-102,sim-103
```

### B. 连本地 http 服务

```bash
python3 sim_device.py --host 127.0.0.1 --port 8000 --device-ids sim-103
python3 sim_device.py --host 192.168.1.10 --port 8000 --device-ids sim-101,sim-102
```

### C. 常用参数

| 参数 | 说明 | 默认 |
|------|------|------|
| `--url` | 服务器根地址（https 自动 wss；与 `--host` 二选一必填） | - |
| `--host / --port` | 主机 + 端口（连 http 用） | 8000 |
| `--wss` | 配合 `--host` 强制 wss | 关 |
| `--device-ids` | 设备 ID，逗号分隔 | `sim-101` |
| `--freq` | 蜂鸣频率 Hz（单台时自定义音调） | 设备默认 |
| `--assets-dir` | 本机 `.opus` 目录（虚拟录音内容） | 内置样本 |
| `--no-auto-record` | 关闭定时自动生成录音事件 | 关 |

运行成功会打印：
```
开始模拟设备： sim-101 | 服务器 wss://... | 资产 1 个
[sim-101] connected to wss://.../ws/device/sim-101
[sim-101] new recording 2026-09-10/09-2x-xx.opus
```

> 连云端时建议用不重复的 ID（如 `sim-103` / `sim-201`），与云端内置的 `sim-101/102` 错开。

### 关于录音文件的位置

**模拟器的录音文件不会落盘到你的电脑**，是纯内存模拟：

- 虚拟录音列表保存在脚本进程内存里（预置几条 + 运行时随机新增），进程结束即消失。
- “回放”的音频内容来自脚本**内嵌的 base64 Opus 样本**，或 `--assets-dir` 指定的 `.opus` 文件，仅临时读入内存按协议回传，不写新文件。
- 需要“真正保存录音文件”是监听端/真实设备的职责，模拟器不负责写盘。

---

## 五、浏览器使用（监听 / 回放）

### 1. 控制台页 `/`

- 顶部「模拟设备」区域：启动/断开模拟设备。
- 设备卡片：在线状态灯（绿=在线/灰=离线）、固件、录音开关、监听人数、SD 卡状态。
- 每张在线卡片有「开始监听 / 停止监听」：建立 `/ws/listen/{device}`，用 AudioWorklet 实时播放 PCM，带音量电平条。
- 展开「录音文件」：发 `list_recordings` 按日期分组展示；`recording_saved` 事件自动插入顶部。
- 点击某条录音：回放（Ogg Opus），可暂停/停止。
- 录音开关：远程 `set_recording` 切换，状态实时更新。

### 2. 监听客户端页 `/listen`

- 设备下拉选择要监听的设备，**选设备即自动刷新该设备的录音**。
- 「开始监听」→ 实时听音；「录音回放」→ 选择历史录音播放。

### 3. 使用要点

- 同一页面**同一时间仅一个音源**：开始监听会自动停止回放，反之亦然。
- 实时音频：16kHz / 16bit / mono PCM，每帧 640B（20ms）。
- 聆听时请允许浏览器自动播放。

---

## 六、常见问题与要点

- **听不到声音**：确认浏览器允许自动播放（点击「开始监听」属用户手势，一般已解锁）；音量电平条有跳动说明链路正常。
- **录音列表空白**：先确认设备在线，或先「开始监听」建立连接再展开。
- **连云端无声**：云端是 HTTPS，必须用 wss —— `--url https://...` 会自动处理。
- **端口冲突**：用 `--port` 换成其它端口，页面自动跟随。
- **安全**：服务端无鉴权、状态仅存内存，仅用于演示/内网；对外部署需自行加防护。

---

## 附：协议速记

- 二进制帧：`Byte0 type(0x01 实时 / 0x02 文件块)` · `Byte1-4 req_id` · `Byte5-6 seq(0xFFFF=末块)` · `Byte7+ payload`。
- 设备端点：`ws://host/ws/device/{device_id}`；监听端点：`ws://host/ws/listen/{device_id}`。
- 首个监听者加入→下发 `start_stream`，全部离开→下发 `stop_stream`。
- REST：`GET /api/devices`、`GET/POST /api/simulator/*`。