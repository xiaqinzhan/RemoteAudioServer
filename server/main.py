"""
remoteAudioServer - 多设备音频采集系统的服务端 + Web 监听端
============================================================
启动：
    python -m uvicorn server.main:app --host 0.0.0.0 --port 8000
默认端口 8000；若设置了环境变量 DEPLOY_RUN_PORT 则优先使用该端口。

REST：
    GET  /                  -> 监听端单页应用
    GET  /api/devices       -> 设备列表
    GET  /api/simulator/status
    POST /api/simulator/start       {device_ids?:[...], count?:int}
    POST /api/simulator/connect     {device_ids:[...]}  重连
    POST /api/simulator/disconnect  {device_ids:[...]}  断开（演示离线）

WebSocket：
    ws://host/ws/device/{device_id}   采集设备
    ws://host/ws/listen/{device_id}   浏览器监听者
"""

import asyncio
import logging
import os

from fastapi import FastAPI, WebSocket
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .hub import Hub
from . import simulator as sim_mod
from . import protocol as P

# 日志
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
)
log = logging.getLogger("main")

PORT = int(os.environ.get("DEPLOY_RUN_PORT", "8000"))
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")
ASSET_DIR = os.path.join(BASE_DIR, "assets")

app = FastAPI(title="remoteAudioServer", version="2.0.0", docs_url=None, redoc_url=None)
hub = Hub()
sim = sim_mod.SimulatorManager(port=PORT)


@app.on_event("startup")
async def _startup():
    asyncio.get_running_loop().create_task(hub.sweeper())
    log.info("remoteAudioServer starting on port %s", PORT)
    # 自动拉起 2 台虚拟设备，保证打开页面立即可见设备卡片（无需手动点击）
    started = sim.start_ids(["sim-101", "sim-102"])
    log.info("auto-start simulator devices: %s", started)


# ------------------------------------------------------------------ REST
@app.get("/")
async def index():
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))


@app.get("/listen")
async def listen_page():
    """监听客户端：选择设备实时听音 + 历史录音回放（独立于控制台的页面）"""
    return FileResponse(os.path.join(STATIC_DIR, "listen.html"))


@app.get("/api/devices")
async def api_devices():
    return {"devices": await hub.device_list(), "ts": P.now_ms()}


@app.get("/api/simulator/status")
async def simulator_status():
    return sim.status()


@app.post("/api/simulator/start")
async def simulator_start(body: dict):
    ids = body.get("device_ids") or []
    count = int(body.get("count", 2))
    if not ids:
        ids = [i for i in sim_mod.SIM_IDS if i not in sim._threads]
        ids = ids[:max(count, 1)]
    started = sim.start_ids(ids)
    return {"started": started, **sim.status()}


@app.post("/api/simulator/connect")
async def simulator_connect(body: dict):
    ids = body.get("device_ids") or []
    started = sim.start_ids(ids)
    return {"started": started, **sim.status()}


@app.post("/api/simulator/disconnect")
async def simulator_disconnect(body: dict):
    ids = body.get("device_ids") or []
    if not ids:
        ids = [i for i in sim_mod.SIM_IDS]
    stopped = sim.stop_ids(ids)
    return {"stopped": stopped, **sim.status()}


# ------------------------------------------------------------------ WebSocket
@app.websocket("/ws/device/{device_id}")
async def ws_device(ws: WebSocket, device_id: str):
    await ws.accept()
    await hub.handle_device(device_id, ws)


@app.websocket("/ws/listen/{device_id}")
async def ws_listen(ws: WebSocket, device_id: str):
    """监听端 WS。

    查询参数：
      ?role=live|control  默认 live（兼容旧前端）。只有 live 计入"监听人数"、
                          参与 0→1 开流 / 1→0 延迟停流、接收实时 PCM；
                          control 只用于控制类消息（录音列表/回放），不计人数、不推 PCM。
      ?sid=<客户端会话 id> 同一 device_id 下同 sid 的新连接会替换旧连接，避免重连残留虚增人数。
    """
    await ws.accept()
    role = (ws.query_params.get("role") or "live").strip().lower()
    if role not in ("live", "control"):
        role = "live"  # 未知取值按 live 处理，兼容旧前端
    sid = (ws.query_params.get("sid") or "").strip() or None
    await hub.handle_listener(device_id, ws, role=role, sid=sid)


# 静态资源（在 API / WS 路由之后挂载，避免覆盖）
if os.path.isdir(STATIC_DIR):
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")