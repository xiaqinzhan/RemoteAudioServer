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

# 新增消息类型（不得改变既有类型语义）：
#  - hb: 应用层心跳 {"type":"hb","ts":<ms>}，无 cmd 字段，老端会静默忽略
#  - device_reconnecting: 设备掉线进入重连宽限期，通知监听者
_TYPE_HB = "hb"
EV_DEVICE_RECONNECTING = "device_reconnecting"

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


class _Listener:
    """每个监听者一个：有界队列 + 独立 writer task，避免慢消费者堵住设备读取循环。"""
    __slots__ = ("ws", "queue", "task", "dropped")
    _QUEUE_MAX = 100  # 约 2 秒 @100Hz(20ms) 实时帧

    def __init__(self, ws):
        self.ws = ws
        self.queue = asyncio.Queue(maxsize=self._QUEUE_MAX)
        self.task = None
        self.dropped = 0


class Hub:
    # 每条 WebSocket 连接每 20s 下发一条应用层心跳 {"type":"hb","ts":..}
    # （必须用应用层数据帧；uvicorn 协议级 ping 不足以对平台网关保活）
    HEARTBEAT_INTERVAL = 20.0
    # 设备掉线后的重连宽限期：期内保留监听端、可下发 device_reconnecting；
    # 设备在期内重连并 hello 则按现有逻辑自动 start_stream；超期才真正 offline 清理
    DEVICE_RECONNECT_GRACE_MS = 45_000

    def __init__(self):
        self.devices: dict[str, _Device] = {}
        self.listeners: dict[str, set] = {}          # device_id -> {listener _Listener}
        self.pending: dict[int, tuple] = {}          # internal_id -> (device_id, listener_ws, orig_req_id)
        # 设备掉线进入重连宽限期的截止时间(ms)：device_id -> deadline
        self.reservations: dict[str, float] = {}
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

    async def _heartbeat_loop(self, ws, label: str):
        """每 HEARTBEAT_INTERVAL 秒下发一条应用层心跳，保证过长连接不被平台网关切断。

        只新增消息类型 {"type":"hb","ts":..}：无 cmd 字段，老固件/老前端会静默忽略。
        发送失败说明连接已死，直接收尾返回，由对端主读取循环走既有清理路径，不抛异常。
        """
        try:
            while True:
                await asyncio.sleep(self.HEARTBEAT_INTERVAL)
                try:
                    await ws.send_json({"type": _TYPE_HB, "ts": P.now_ms()})
                except asyncio.CancelledError:
                    raise
                except Exception:
                    log.debug("[hb] %s send failed, stop heartbeat", label)
                    return
        except asyncio.CancelledError:
            raise
        except Exception:
            # 任何非预期异常都不得打断主循环
            log.debug("[hb] %s heartbeat task ended", label)

    async def send_binary_to_listener(self, ws, frame: bytes):
        try:
            await ws.send_bytes(frame)
        except Exception:
            pass

    async def broadcast_event(self, device_id: str, obj: _JSON):
        for ln in list(self.listeners.get(device_id, ())):
            await self.send_json_to_listener(ln.ws, obj)

    def fanout_live(self, device_id: str, frame: bytes):
        """非阻塞地把一帧实时音频推给全部监听者。绝不 await 发送：
        每个监听者写入其有界队列；队满时丢最旧一帧并计数。"""
        bucket = self.listeners.get(device_id)
        if not bucket:
            return
        for ln in list(bucket):
            try:
                ln.queue.put_nowait(frame)
            except asyncio.QueueFull:
                # 丢最旧的一帧，换入最新帧
                try:
                    ln.queue.get_nowait()
                except Exception:
                    pass
                try:
                    ln.queue.put_nowait(frame)
                except Exception:
                    pass
                ln.dropped += 1
                if ln.dropped % 50 == 0:
                    log.warning(
                        "fanout slow consumer dropping: device=%s listener=%r total_dropped=%s",
                        device_id, ln.ws, ln.dropped,
                    )

    # 内部写缓冲高/低水位阈值，用于识别“对端停止消费”的真实网络背压（字节）
    _BACKPRESSURE_HIGH = 128 * 1024
    _BACKPRESSURE_LOW = 16 * 1024

    async def _listener_writer(self, device_id: str, ln: _Listener):
        """把监听者队列里的帧逐个发给对端。

        发送用 3s `asyncio.wait_for` 兜底；与此同时，`websockets.send_bytes` 会把数据送进
        asyncio transport 的写缓冲，对端停止读取时该缓冲会持续增长——这里在每次发送后对有界
        背压做一次限时等待：若内部写缓冲高于高水位、且持续 3s 降不下来（即对端确实不消费），
        就判定为 slow consumer 并主动剔除该监听连接。
        """
        try:
            while True:
                frame = await ln.queue.get()
                try:
                    await ln.ws.send_bytes(frame)
                    await self._await_listener_backpressure(ln)
                except Exception as exc:
                    log.warning(
                        "slow/failed consumer kicked: device=%s listener=%r err=%r dropped=%d",
                        device_id, ln.ws, type(exc).__name__, ln.dropped,
                    )
                    try:
                        await ln.ws.close()
                    except Exception:
                        pass
                    return
        except asyncio.CancelledError:
            raise

    async def _await_listener_backpressure(self, ln: _Listener):
        """等待监听者传输写缓冲降到低水位；3s 内降不下来抛超时（对端停止消费）。"""
        tr = getattr(ln.ws, "transport", None)
        get_size = getattr(tr, "get_write_buffer_size", None)
        if get_size is None:
            return
        if get_size() <= self._BACKPRESSURE_HIGH:
            return  # 缓冲不大，说明对端仍在消费，无需等待

        async def _drain():
            while get_size() > self._BACKPRESSURE_LOW:
                await asyncio.sleep(0.02)

        try:
            await asyncio.wait_for(_drain(), timeout=3.0)
        except asyncio.TimeoutError:
            raise asyncio.TimeoutError("slow consumer: transport write buffer not draining")

    # ------------------------------------------------------------------
    # 设备接入
    # ------------------------------------------------------------------
    async def handle_device(self, device_id: str, ws):
        # 1. 等待 hello
        try:
            msg = await ws.receive_json()
        except Exception:
            log.warning("device %s bad hello: read failed / no message", device_id)
            try:
                await ws.close()
            except Exception:
                pass
            return

        if not isinstance(msg, dict) or msg.get("type") != "hello":
            summary = (msg or {}).get("type") if isinstance(msg, dict) else type(msg).__name__
            log.warning("device %s bad hello (first_type=%r), closing 1008", device_id, summary)
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
        # 设备回来了：清除掉线宽限期标记（若有），避免宽限期结束后再误触发 offline
        self.reservations.pop(device_id, None)
        await self.broadcast_event(device_id, {
            "type": "event",
            "event": P.EV_DEVICE_ONLINE,
            "device_id": device_id,
            "fw": dev.fw,
            "ts": P.now_ms(),
        })
        # 若已有监听者在等，设备一上线就开启推流（可让宽限期内的监听自动续上）
        if self.listeners.get(device_id):
            await self.send_json_to_device(device_id, {"cmd": "start_stream"})
        log.info("device online: %s", device_id)

        # 应用层心跳：保活长连接，防止平台网关按"单向上行/空闲"计时切断
        hb_task = asyncio.get_running_loop().create_task(
            self._heartbeat_loop(ws, f"device:{device_id}"))

        gone_reason = "read_loop_end"
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
                    if message.get("code") == 1001:  # sweeper 超时踢除使用 1001
                        gone_reason = "timeout_kick"
                    break
        except Exception:
            pass
        finally:
            if "hb_task" in locals():
                hb_task.cancel()
            await self._device_gone(device_id, reason=gone_reason)

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
            # 实时音频帧 -> 非阻塞广播给全部监听者（慢消费者有独立队列，不堵设备）
            self.fanout_live(device_id, frame)
        elif ftype == P.FRAME_FILE_BLOCK:
            # 录音文件块 -> 按 req_id 路由回发起请求的监听者
            entry = self.pending.get(req_id)
            if entry:
                _dev_id, listener_ws, orig_id = entry
                await self.send_binary_to_listener(
                    listener_ws, P.rewrite_req_id(frame, orig_id)
                )

    async def _device_gone(self, device_id: str, reason: str = "read_loop_end"):
        dev = self.devices.pop(device_id, None)
        if dev is None:
            return
        log.info("device disconnect: device_id=%s reason=%s last_seen_age=%.1fs "
                 "entering %ds reconnect grace",
                 device_id, reason, (P.now_ms() - dev.last_seen) / 1000.0,
                 self.DEVICE_RECONNECT_GRACE_MS / 1000)
        # 进入掉线宽限期：保留监听连接，先不下发 device_offline，等设备重连
        self.reservations[device_id] = P.now_ms() + self.DEVICE_RECONNECT_GRACE_MS
        await self.broadcast_event(device_id, {
            "type": "event",
            "event": EV_DEVICE_RECONNECTING,
            "device_id": device_id,
            "ts": P.now_ms(),
        })

    async def _device_offline_final(self, device_id: str):
        """宽限期结束，设备仍未回来：真正下线并清理。"""
        log.info("device offline(final): device_id=%s reconnect grace expired", device_id)
        self.reservations.pop(device_id, None)
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

    # ------------------------------------------------------------------
    # 监听者接入
    # ------------------------------------------------------------------
    async def handle_listener(self, device_id: str, ws):
        bucket = self.listeners.setdefault(device_id, set())
        was_empty = not bucket
        ln = _Listener(ws)
        bucket.add(ln)
        ln.task = asyncio.get_running_loop().create_task(self._listener_writer(device_id, ln))
        hb_task = asyncio.get_running_loop().create_task(
            self._heartbeat_loop(ws, f"listen:{device_id}"))
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
            hb_task.cancel()
            # 取消 writer task 并回收队列，防止任务/内存泄漏
            if ln.task is not None:
                ln.task.cancel()
            bucket.discard(ln)
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
            age = P.now_ms() - dev.last_seen
            online = age <= P.DEVICE_TIMEOUT_MS
            items.append({
                "device_id": device_id,
                "online": online,
                "fw": dev.fw,
                "recording_enabled": dev.recording_enabled,
                "sd_ok": dev.sd_ok,
                "listener_count": len(self.listeners.get(device_id, ())),
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
                    silent_s = (now - dev.last_seen) / 1000.0
                    log.warning("device %s silent %.0fs (limit %ss), kicking",
                                device_id, silent_s, P.DEVICE_TIMEOUT_MS / 1000)
                    try:
                        await dev.ws.close(code=1001)
                    except Exception:
                        pass
            # 设备重连宽限期到期：仍没回来才真正 device_offline 并清理
            for device_id, deadline in list(self.reservations.items()):
                if now >= deadline:
                    self.reservations.pop(device_id, None)
                    await self._device_offline_final(device_id)

    async def start_stream(self, device_id: str):
        await self.send_json_to_device(device_id, {"cmd": "start_stream"})

    async def stop_stream(self, device_id: str):
        await self.send_json_to_device(device_id, {"cmd": "stop_stream"})