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
- `server/hub.py`：路由中枢。设备 `devices`、监听者 `listeners`、挂起请求 `pending`（内部 id↔原 id 双向映射）、宽限期 `reservations` 存内存；每连接 20s 应用层心跳。`device_list()` 会把「本地内存设备」与「共享在线状态」合并（见下）。
- `server/presence.py`：**设备在线状态跨实例共享**读写（Supabase/PostgREST，表 `public.device_presence`）。hello 时 upsert、心跳周期 upsert `last_seen`、断开时按 `instance_id` 删除、sweeper 周期清理过期行。DB 不可用时自动降级为「仅本地内存」（单实例行为），不影响主流程。SDK 为同步调用，统一经 `asyncio.to_thread` 执行避免阻塞事件循环。
- `storage/database/supabase_client.py`：官方 Supabase 客户端（COZE_SUPABASE_URL/ANON_KEY 环境变量，缺失时回退 coze_workload_identity 动态获取；本地无 token → service_role_key）。`storage/database/model.py`：SQLAlchemy 表结构（`device_presence`）单一事实来源。
- `server/simulator.py`：设备模拟器。每台虚拟设备在独立线程的独立 asyncio 事件循环中，以真实 `websockets` 客户端连 `/ws/device/{id}`。
- `server/main.py`：FastAPI 应用，装配 hub 与 simulator，托管 `server/static`。
- `server/static/index.html` + `pcm-worklet.js`：监听端单页与 AudioWorklet 播放器。
- `simulator_standalone/sim_device.py`：独立设备模拟器（单文件、自带协议层、内嵌 Opus 样本、纯 Python 合成音，仅依赖 `websockets`）。可本地运行连云端/局域网服务器，等价于 `server/simulator.py` 的外部形态。参数见 `--help` 及该目录 README。

## 关键约定
- 实时音频：16kHz / 16bit / mono，每帧 640 字节(20ms)。
- 页面「同一时间仅一个音源」：监听(live) 与回放(file) 二选一，通过全局 `active.mode` 切换。
- 音频播放必须用 AudioWorklet，禁止 ScriptProcessorNode。
- 新增录音资产放 `server/assets/*.opus`（ffmpeg 生成，16kHz mono libopus）。
- **设备在线状态为跨实例共享**：线上可能多实例承载同一域名，WS 长连接粘在单实例、HTTP 可能落到另一实例。故 `/api/devices` 必须读共享表 `public.device_presence`（按 `last_seen` 新鲜度判定 online），不能只依赖本地 `self.devices`；音频转发仍本地进行。

## 测试
- 自检：`python3 -m py_compile server/*.py`
- 冒烟：启动后依次调用 `POST /api/simulator/start`（count=2）→ `GET /api/devices`；再以 websocket 客户端验证监听/回放链路（自动测试见交付时）。

## 安全注意
- 服务端无鉴权、内存态 + 共享表；共享表 `device_presence` 启用 RLS 且无策略（仅后端 service_role 读写，前端不可直连）；仅用于演示/内网，如需对外仅暴露 REST 状态与可信设备。
- 监听端到设备的二进制帧按 req_id 严格路由，避免串流。