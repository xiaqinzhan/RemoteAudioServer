# AGENTS.md

## 项目概览
多设备音频采集系统的服务端 + Web 监听端。技术栈：Python FastAPI + WebSocket，前端为服务端直接托管的原生 HTML/JS 单页（AudioWorklet 实时播放）。不含 ESP32 固件，内置设备模拟器可全链路演示。

## 构建 / 运行
```bash
pip3 install -r requirements.txt
python3 -m uvicorn server.main:app --host 0.0.0.0 --port 8000
# 端口读取顺序：环境变量 DEPLOY_RUN_PORT > 8000
```

## 组件职责
- `server/protocol.py`：二进制帧编解码（Byte0 type / Byte1-4 req_id 大端 / Byte5-6 seq / Byte7+ payload）与常量。实时帧 0x01(640B PCM)，文件块 0x02(2048B，末块 seq=0xFFFF)。
- `server/hub.py`：无状态路由中枢。设备 `devices`、监听者 `listeners`、挂起请求 `pending`（内部 id↔原 id 双向映射）均存内存。
- `server/simulator.py`：设备模拟器。每台虚拟设备在独立线程的独立 asyncio 事件循环中，以真实 `websockets` 客户端连 `/ws/device/{id}`。
- `server/main.py`：FastAPI 应用，装配 hub 与 simulator，托管 `server/static`。
- `server/static/index.html` + `pcm-worklet.js`：监听端单页与 AudioWorklet 播放器。

## 关键约定
- 实时音频：16kHz / 16bit / mono，每帧 640 字节(20ms)。
- 页面「同一时间仅一个音源」：监听(live) 与回放(file) 二选一，通过全局 `active.mode` 切换。
- 音频播放必须用 AudioWorklet，禁止 ScriptProcessorNode。
- 新增录音资产放 `server/assets/*.opus`（ffmpeg 生成，16kHz mono libopus）。

## 测试
- 自检：`python3 -m py_compile server/*.py`
- 冒烟：启动后依次调用 `POST /api/simulator/start`（count=2）→ `GET /api/devices`；再以 websocket 客户端验证监听/回放链路（自动测试见交付时）。

## 安全注意
- 服务端无鉴权、状态仅存内存，仅用于演示/内网；如需对外仅暴露 REST 状态与可信设备。
- 监听端到设备的二进制帧按 req_id 严格路由，避免串流。