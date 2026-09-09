"""
WebSocket 路由中枢 (Hub)
------------------------
无状态路由中枢：一切设备/监听者状态仅存内存。

路由：
  /ws/device/{device_id}   : 采集设备
  /ws/listen/{device_id}   : 浏览器监听者

职责：
  - 设备接入（hello / ping / event / response / binary 帧处理）
  - 监听者集合维护，0<->1 触发 start_stream / stop_stream
  - 实时音频帧 (0x01) 广播给该设备全部监听者
  - 请求-响应 + 文件块 (0x02) 按 req_id 路由（内部 id 与监听端原 id 双向映射，避免冲突）
  - 事件广播与设备离线通知
"""

import asyncio
import logging
import json

from . import protocol as P

log = logging.getLogger("hub")

_JSON = dict


async def _safe(aw_or_coro, default=None):
    try:
        return await aw_or_coro
    except Exception:
        return default


class _Device:
    __slots__ = ("ws", "fw", "recording_enabled", "sd_ok", "config", "last_seen")

    def __init__(self, ws, fw, recording_enabled, sd_ok, config):
        self.ws = ws
        self.fw = fw
        self.recording_enabled = recording_enabled
        self.sd_ok = sd_ok
        self.config = config or {}
        self.last_seen = P.now_ms()


class Hub:
    def __init__(self):
        self.devices: dict[str, _Device] = {}
        self.listeners: dict[str, set] = {}          # device_id -> {listener ws}
        self.pending: dict[int, tuple] = {}          # internal_id -> (device_id, listener_ws, orig_req_id)
        self._id_counter = 0

    # ------------------------------------------------------------------
    # 工具
    # ------------------------------------------------------------------
    def _next_internal_id(self) -> int:
        self._id_counter += 1
        return self._id_counter

    async def send_json_to_device(self, device_id: str, obj: _JSON) -> bool:
        dev = self.devices.get(device_id)
        if dev is None:
            return False
        try:
            await dev.ws.send_json(obj)
            return True
        except Exception:
            return False

    async def send_json_to_listener(self, ws, obj: _JSON):
        try:
            await ws.send_json(obj)
        except Exception:
            pass

    async def send_binary_to_listener(self, ws, frame: bytes):
        try:
            await ws.send_bytes(frame)
        except Exception:
            pass

    async def broadcast_event(self, device_id: str, obj: _JSON):
        for ws in list(self.listeners.get(device_id, ())):
            await self.send_json_to_listener(ws, obj)

    # ------------------------------------------------------------------
    # 设备接入
    # ------------------------------------------------------------------
    async def handle_device(self, device_id: str, ws):
        # 1. 等待 hello
        try:
            msg = await ws.receive_json()
        except Exception:
            try:
                await ws.close()
            except Exception:
                pass
            return

        if not isinstance(msg, dict) or msg.get("type") != "hello":
            try:
                await ws.close(code=1008)
            except Exception:
                pass
            return

        dev = _Device(
            ws=ws,
            fw=str(msg.get("fw", "unknown")),
            recording_enabled=bool(msg.get("recording_enabled", True)),
            sd_ok=bool(msg.get("sd_ok", True)),
            config=msg.get("config") or {},
        )
        self.devices[device_id] = dev
        await self.broadcast_event(device_id, {
            "type": "event",
            "event": P.EV_DEVICE_ONLINE,
            "device_id": device_id,
            "fw": dev.fw,
            "ts": P.now_ms(),
        })
        # 若已有监听者在等，设备一上线就开启推流
        if self.listeners.get(device_id):
            await self.send_json_to_device(device_id, {"cmd": "start_stream"})
        log.info("device online: %s", device_id)

        try:
            while True:
                message = await ws.receive()
                if "text" in message:
                    await self._on_device_text(device_id, message["text"])
                    dev.last_seen = P.now_ms()
                elif "bytes" in message:
                    await self._on_device_binary(device_id, message["bytes"])
                    dev.last_seen = P.now_ms()
                elif message.get("type") == "websocket.disconnect":
                    break
        except Exception:
            pass
        finally:
            await self._device_gone(device_id)

    async def _on_device_text(self, device_id: str, text: str):
        try:
            data = json.loads(text)
        except Exception:
            return
        kind = data.get("type")
        if kind == "ping":
            return  # last_seen 已由外层更新
        if kind == "event":
            await self.broadcast_event(device_id, data)
        elif kind == P.EV_RESPONSE:
            internal_id = data.get("req_id")
            entry = self.pending.get(internal_id)
            if entry:
                _dev_id, listener_ws, orig_id = entry
                if _dev_id == device_id:
                    data = dict(data)
                    data["req_id"] = orig_id
                    await self.send_json_to_listener(listener_ws, data)
        # 其它类型忽略

    async def _on_device_binary(self, device_id: str, frame: bytes):
        try:
            ftype, req_id, seq, payload = P.decode_frame(frame)
        except Exception:
            return
        if ftype == P.FRAME_LIVE_AUDIO:
            # 实时音频帧 -> 广播给全部监听者
            for ws in list(self.listeners.get(device_id, ())):
                await self.send_binary_to_listener(ws, frame)
        elif ftype == P.FRAME_FILE_BLOCK:
            # 录音文件块 -> 按 req_id 路由回发起请求的监听者
            entry = self.pending.get(req_id)
            if entry:
                _dev_id, listener_ws, orig_id = entry
                await self.send_binary_to_listener(
                    listener_ws, P.rewrite_req_id(frame, orig_id)
                )

    async def _device_gone(self, device_id: str):
        dev = self.devices.pop(device_id, None)
        if dev is None:
            return
        await self.broadcast_event(device_id, {
            "type": "event",
            "event": P.EV_DEVICE_OFFLINE,
            "device_id": device_id,
            "ts": P.now_ms(),
        })
        # 失效该设备关联的挂起请求
        dead = [i for i, (d, _w, _o) in self.pending.items() if d == device_id]
        for i in dead:
            _d, ws, _o = self.pending.pop(i)
            await self.send_json_to_listener(ws, {
                "type": "event",
                "event": "request_failed",
                "device_id": device_id,
                "msg": "设备离线，请求未完成",
            })
        log.info("device offline: %s", device_id)

    # ------------------------------------------------------------------
    # 监听者接入
    # ------------------------------------------------------------------
    async def handle_listener(self, device_id: str, ws):
        bucket = self.listeners.setdefault(device_id, set())
        was_empty = not bucket
        bucket.add(ws)
        if was_empty:
            # 0 -> 1：首次监听者加入，通知设备开流
            await self.send_json_to_device(device_id, {"cmd": "start_stream"})
            await self.broadcast_event(device_id, {
                "type": "event", "event": P.EV_STREAM_STATE,
                "device_id": device_id, "state": "streaming",
                "listener_count": len(bucket), "ts": P.now_ms(),
            })

        try:
            while True:
                message = await ws.receive()
                if "text" in message:
                    await self._on_listener_text(device_id, ws, message["text"])
                elif message.get("type") == "websocket.disconnect":
                    break
        except Exception:
            pass
        finally:
            bucket.discard(ws)
            if not bucket:
                self.listeners.pop(device_id, None)
                await self.send_json_to_device(device_id, {"cmd": "stop_stream"})
                await self.broadcast_event(device_id, {
                    "type": "event", "event": P.EV_STREAM_STATE,
                    "device_id": device_id, "state": "idle",
                    "listener_count": 0, "ts": P.now_ms(),
                })

    async def _on_listener_text(self, device_id: str, ws, text: str):
        try:
            req = json.loads(text)
        except Exception:
            return
        if not isinstance(req, dict) or req.get("type") != P.EV_REQUEST_CMD:
            return

        dev = self.devices.get(device_id)
        if dev is None:
            await self.send_json_to_listener(ws, {
                "type": "event", "event": "request_failed",
                "device_id": device_id, "msg": "设备不在线",
            })
            return

        cmd = req.get("cmd")
        internal_id = self._next_internal_id()
        orig_id = req.get("req_id", internal_id)
        self.pending[internal_id] = (device_id, ws, orig_id)

        forward = {"type": P.EV_REQUEST_CMD, "req_id": internal_id, "cmd": cmd}
        for key, val in req.items():
            if key not in ("type", "req_id", "cmd"):
                forward[key] = val

        ok = await self.send_json_to_device(device_id, forward)
        if not ok:
            self.pending.pop(internal_id, None)
            await self.send_json_to_listener(ws, {
                "type": "event", "event": "request_failed",
                "device_id": device_id, "msg": "设备不在线",
            })

    # ------------------------------------------------------------------
    # REST 支持 / 生命周期
    # ------------------------------------------------------------------
    def device_list(self) -> list:
        items = []
        for device_id, dev in self.devices.items():
            items.append({
                "device_id": device_id,
                "online": True,
                "fw": dev.fw,
                "recording_enabled": dev.recording_enabled,
                "sd_ok": dev.sd_ok,
                "listener_count": len(self.listeners.get(device_id, set())),
                "config": dev.config,
                "last_seen": dev.last_seen,
            })
        return items

    async def sweeper(self):
        """定期巡检：超过 45s 无消息的设备强制下线。"""
        while True:
            await asyncio.sleep(10)
            now = P.now_ms()
            for device_id, dev in list(self.devices.items()):
                if now - dev.last_seen > P.DEVICE_TIMEOUT_MS:
                    log.warning("device %s silent over timeout, kicking", device_id)
                    try:
                        await dev.ws.close(code=1001)
                    except Exception:
                        pass

    async def start_stream(self, device_id: str):
        await self.send_json_to_device(device_id, {"cmd": "start_stream"})

    async def stop_stream(self, device_id: str):
        await self.send_json_to_device(device_id, {"cmd": "stop_stream"})