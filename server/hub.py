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
from . import presence

log = logging.getLogger("hub")

# 新增消息类型（不得改变既有类型语义）：
#  - hb: 应用层心跳 {"type":"hb","ts":<ms>}，无 cmd 字段，老端会静默忽略
#  - device_reconnecting: 设备掉线进入重连宽限期，通知监听者
_TYPE_HB = "hb"
EV_DEVICE_RECONNECTING = "device_reconnecting"

_JSON = dict


def _cmd_of(obj) -> str:
    """取一条下行消息里的命令名，仅用于日志（没有 cmd 时退化为 type）。"""
    if isinstance(obj, dict):
        cmd = obj.get("cmd")
        if cmd:
            return str(cmd)
        return str(obj.get("type", "?"))
    return "?"


async def _safe(aw_or_coro, default=None):
    try:
        return await aw_or_coro
    except Exception:
        return default


class _Device:
    """一条设备 WS 连接。replaced 用来标记"已被同 device_id 的新连接取代"，
    使其收尾时不再按 device_id 误删当前注册（P0-1）。"""

    __slots__ = ("ws", "fw", "recording_enabled", "sd_ok", "config", "last_seen",
                 "replaced")

    def __init__(self, ws, fw, recording_enabled, sd_ok, config):
        self.ws = ws
        self.fw = fw
        self.recording_enabled = recording_enabled
        self.sd_ok = sd_ok
        self.config = config or {}
        self.last_seen = P.now_ms()
        self.replaced = False


class _Listener:
    """每个监听者一个：有界队列 + 独立 writer task，避免慢消费者堵住设备读取循环。

    role=live（缺省，兼容旧前端）：计入监听人数、参与 0->1 / 1->0 判定、接收实时 PCM；
    role=control                ：只用于控制类消息（录音列表/回放等），不计人数、不推 PCM。
    sid：客户端会话 id。同一 device_id 下同 sid 出现新连接时，旧条目会被踢掉，
         避免"页面重连/刷新"残留旧连接把人数虚增（幽灵监听者）。
    """

    __slots__ = ("ws", "queue", "task", "dropped", "role", "sid", "last_active",
                 "fail_sends")
    _QUEUE_MAX = 100  # 约 2 秒 @100Hz(20ms) 实时帧

    def __init__(self, ws, role: str = "live", sid: str | None = None):
        self.ws = ws
        self.queue = asyncio.Queue(maxsize=self._QUEUE_MAX)
        self.task = None
        self.dropped = 0
        self.role = role
        self.sid = sid
        self.last_active = P.now_ms()
        self.fail_sends = 0

    @property
    def is_live(self) -> bool:
        """是否计入人数（只有 live 参与人数统计与 0->1/1->0 判定）。"""
        return self.role != "control"


class Hub:
    # 每条 WebSocket 连接每 20s 下发一条应用层心跳 {"type":"hb","ts":..}
    # （必须用应用层数据帧；uvicorn 协议级 ping 不足以对平台网关保活）
    HEARTBEAT_INTERVAL = 20.0
    # 设备掉线后的重连宽限期：期内保留监听端、可下发 device_reconnecting
    # （超期才真正 offline 清理）。期内设备重连并 hello 时，是否推流**不再无条件
    # start_stream**，而是由 hello 复盘逻辑按"当前真实监听者数量"决定 start/stop。
    DEVICE_RECONNECT_GRACE_MS = 45_000
    # 监听者 1->0 后延迟多久再给设备下发 stop_stream（秒）。
    # 期内若又有监听者接入则取消停流，避免刷新页面就掐断设备推流。
    LISTENER_STOP_DELAY_S = 10.0
    # 监听者空闲回收阈值（毫秒）：超过该时长既无任何上行交互、又有发送失败记录的
    # 监听连接，判定为"半开/死链"由 sweeper 主动关掉并从集合移除（P0-4）。
    LISTENER_IDLE_MS = 90_000

    def __init__(self):
        self.devices: dict[str, _Device] = {}
        # device_id -> {真·监听者 _Listener}：只有 role=live 的连接在此，是"监听人数"
        # 与 0->1/1->0 判定的唯一数据源，也是 fanout_live 的推送目标。
        self.listeners: dict[str, set] = {}
        # device_id -> {role=control 的连接}：只收发控制类消息（录音列表/回放）与设备事件，
        # 不计人数、不推实时 PCM。单独一张表是为了不破坏"人数"语义。
        self.control_listeners: dict[str, set] = {}
        self.pending: dict[int, tuple] = {}          # internal_id -> (device_id, listener_ws, orig_req_id)
        # 设备掉线进入重连宽限期的截止时间(ms)：device_id -> deadline
        self.reservations: dict[str, float] = {}
        # 监听者 1->0 后的"延迟停流"定时任务：device_id -> asyncio.Task
        self._stop_timers: dict[str, asyncio.Task] = {}
        self._id_counter = 0

    # ------------------------------------------------------------------
    # 工具
    # ------------------------------------------------------------------
    def _next_internal_id(self) -> int:
        self._id_counter += 1
        return self._id_counter

    async def send_json_to_device(self, device_id: str, obj: _JSON) -> bool:
        """向该设备当前的 WS 连接下发一条 JSON（命令/请求）。

        成功与失败都会打日志（P0-2）：这条路径以前是"静默失败"的黑洞——
        一旦 self.devices 里没有该设备（例如注册被旧连接误删），start_stream /
        stop_stream / 配置 / 回放全部静默失效，而心跳照发、/api/devices 仍显示在线，
        线上无从排查。
        """
        dev = self.devices.get(device_id)
        if dev is None:
            log.warning("cmd send FAILED: device=%s cmd=%s reason=device_not_registered",
                        device_id, _cmd_of(obj))
            return False
        try:
            await dev.ws.send_json(obj)
            log.info("cmd sent: device=%s cmd=%s", device_id, _cmd_of(obj))
            return True
        except Exception as exc:
            log.warning("cmd send FAILED: device=%s cmd=%s reason=%s: %s",
                        device_id, _cmd_of(obj), type(exc).__name__, exc)
            return False

    async def send_json_to_listener(self, ws, obj: _JSON):
        try:
            await ws.send_json(obj)
        except Exception:
            pass

    async def _heartbeat_loop(self, ws, label: str, device_id: str | None = None,
                              listeners_device_id: str | None = None):
        """每 HEARTBEAT_INTERVAL 秒下发一条应用层心跳，保证过长连接不被平台网关切断。

        只新增消息类型 {"type":"hb","ts":..,"listeners":N}：无 cmd 字段，老固件/老前端
        会静默忽略未知字段（仅新增，不改现有语义）。
        listeners_device_id 用于在心跳里带上"该设备当前监听者数量"（只读，不给监听连接
        触发 presence.touch，避免监听连接把已死设备的共享在线状态误刷新）。
        发送失败说明连接已死，直接收尾返回，由对端主读取循环走既有清理路径，不抛异常。
        对设备连接（device_id 非空）顺带刷新共享状态里的 last_seen，保证多实例下
        /api/devices 不会因为设备静默（无上行帧）而被误判离线。
        """
        try:
            while True:
                await asyncio.sleep(self.HEARTBEAT_INTERVAL)
                hb: dict = {"type": _TYPE_HB, "ts": P.now_ms()}
                target = listeners_device_id or device_id
                if target is not None:
                    hb["listeners"] = len(self.listeners.get(target, ()))
                try:
                    await ws.send_json(hb)
                except asyncio.CancelledError:
                    raise
                except Exception:
                    log.debug("[hb] %s send failed, stop heartbeat", label)
                    return
                if device_id is not None:
                    # 连接仍然活着：刷新共享状态的时间戳（失败不影响心跳）
                    await presence.touch(device_id, P.now_ms())
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
        """设备事件广播给 live 与 control 连接。

        live 是监听页主连接；control 只是"不实时听音"，它仍需要 recording_saved
        之类的事件来刷新录音列表，所以事件照发（事件与实时 PCM 是两条路径）。
        """
        for ln in list(self.listeners.get(device_id, ())):
            await self.send_json_to_listener(ln.ws, obj)
        for ln in list(self.control_listeners.get(device_id, ())):
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
        # 同一 device_id 的新连接"取代"旧连接：先把旧连接标记为 replaced 并尽力关闭，
        # 再写入新注册。旧连接稍后的收尾会在 _device_gone 里因身份不匹配被忽略，
        # 不会误删这条新注册（否则所有下行命令都会静默失效）。
        old = self.devices.get(device_id)
        if old is not None and old.ws is not ws:
            old.replaced = True
            log.info("device re-registered: device=%s supersedes stale socket", device_id)
            try:
                await old.ws.close()
            except Exception:
                pass  # 关不掉不致命：它的收尾已被身份校验拦住
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
        # 设备上线即"复盘"：按该 device_id 当前真实监听者数量决定是否推流。
        # 首次连接与重连一律复盘。这修复了"设备断网期间监听者离开(1→0) → stop_stream
        # 因设备不在线而丢失 → 设备重连后按本地记忆继续推流(对着空气白推)"的问题。
        n_listeners = len(self.listeners.get(device_id, ()))
        if n_listeners > 0:
            await self.send_json_to_device(device_id, {"type": "cmd", "cmd": "start_stream"})
            reconciled = "start_stream"
        else:
            await self.send_json_to_device(device_id, {"type": "cmd", "cmd": "stop_stream"})
            reconciled = "stop_stream"
        log.info("device online: %s (listeners=%d, reconciled=%s)",
                 device_id, n_listeners, reconciled)

        # 写入跨实例共享的在线状态（失败自动降级为本地内存，不影响主流程）
        await presence.upsert_device(
            device_id, dev.fw, dev.recording_enabled, dev.sd_ok,
            dev.config, dev.last_seen)

        # 应用层心跳：保活长连接，防止平台网关按"单向上行/空闲"计时切断
        hb_task = asyncio.get_running_loop().create_task(
            self._heartbeat_loop(ws, f"device:{device_id}", device_id=device_id))

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
            await self._device_gone(device_id, ws, reason=gone_reason)

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

    async def _device_gone(self, device_id: str, ws, reason: str = "read_loop_end"):
        """设备连接断开时的收尾（带连接身份校验）。

        只有"当前注册对应的就是这条断掉的 ws"时才执行清理；否则说明这是被新连接
        取代的旧 socket（或设备已不在册）的迟到收尾，直接忽略——绝不允许它按
        device_id 无条件 pop 掉别人的注册。
        """
        dev = self.devices.get(device_id)
        if dev is None or dev.ws is not ws:
            log.info("stale device socket gone, ignored: device_id=%s reason=%s registered=%s",
                     device_id, reason,
                     "none" if dev is None else "another_socket")
            return
        self.devices.pop(device_id, None)
        log.info("device disconnect: device_id=%s reason=%s last_seen_age=%.1fs "
                 "entering %ds reconnect grace",
                 device_id, reason, (P.now_ms() - dev.last_seen) / 1000.0,
                 self.DEVICE_RECONNECT_GRACE_MS / 1000)
        # 进入掉线宽限期：保留监听连接，先不下发 device_offline，等设备重连
        self.reservations[device_id] = P.now_ms() + self.DEVICE_RECONNECT_GRACE_MS
        # 从跨实例共享状态移除，使任意实例的 /api/devices 都能及时反映该设备已断开
        await presence.remove(device_id)
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
    async def handle_listener(self, device_id: str, ws, role: str = "live",
                              sid: str | None = None):
        """监听端连接。

        role=live（缺省，兼容旧前端）：计入 self.listeners[device_id] 人数，参与
          0→1 开流 / 1→0 延迟停流，并接收 fanout_live 推送的实时 PCM。
        role=control：不计人数、不参与 0→1/1→0、不进 fanout 集合（因此不推 PCM）。
          它自己发起的请求（录音列表/回放等）响应不经集合，而是按 pending 里的
          listener_ws 直接写回它这条 WS（见 _on_device_text / _on_device_binary /
          send_binary_to_listener），所以控制连接照常能收到自己的响应。
        sid：同 device_id 下同 sid 的旧 live 连接会被踢掉并移出集合，避免
          "页面刷新/重连"残留旧连接把人数虚增（幽灵监听者）。
        """
        ln = _Listener(ws, role=role, sid=sid)
        bucket = self.listeners.setdefault(device_id, set()) if ln.is_live else None
        cbucket = None if ln.is_live else self.control_listeners.setdefault(device_id, set())
        # 0->1 判定看"这次替换之前是否已有 live 听者"：同 sid 重连属于同一听众的延续，
        # 既不该多下发一次 start_stream，也不该触发 1->0 停流抖动。
        had_live = bool(bucket)

        if bucket is not None and sid:
            for old in [o for o in bucket if o.sid == sid]:
                bucket.discard(old)
                if old.task is not None:
                    old.task.cancel()
                log.info("listener replaced (same sid): device=%s sid=%s", device_id, sid)
                try:
                    await old.ws.close()
                except Exception:
                    pass  # 关不掉不致命：它已不在集合里，不再计入人数

        was_empty = bucket is not None and not had_live
        if bucket is not None:
            bucket.add(ln)
            ln.task = asyncio.get_running_loop().create_task(
                self._listener_writer(device_id, ln))
        else:
            cbucket.add(ln)
            log.info("control listener attached (not counted): device=%s", device_id)
        hb_task = asyncio.get_running_loop().create_task(
            self._heartbeat_loop(ws, f"listen:{device_id}"))
        if was_empty:
            # 0 -> 1：取消可能正在计时的"延迟停流"，并通知设备开流
            self._cancel_stop_timer(device_id)
            log.info("listeners 0->1: device=%s count=%d cmd=start_stream",
                     device_id, len(bucket))
            ok = await self.send_json_to_device(device_id, {"type": "cmd", "cmd": "start_stream"})
            log.info("listeners 0->1: device=%s count=%d cmd=start_stream delivered=%s",
                     device_id, len(bucket), ok)
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
            # control 连接不在人数集合里，不参与 1->0 判定（也不吞掉 CancelledError）；
            # 只需从 control 注册表里摘掉，避免残留引用。
            if ln.is_live:
                bucket.discard(ln)
                if not bucket:
                    self.listeners.pop(device_id, None)
                    # 1 -> 0：不立即停流，延迟 LISTENER_STOP_DELAY_S 秒再停；
                    # 期内若有监听者回来则取消（见 _delayed_stop）
                    log.info("listeners 1->0: device=%s count=0 schedule stop_stream in %.0fs",
                             device_id, self.LISTENER_STOP_DELAY_S)
                    self._schedule_stop(device_id)
            else:
                cbucket.discard(ln)
                if not cbucket:
                    self.control_listeners.pop(device_id, None)

    def _cancel_stop_timer(self, device_id: str):
        t = self._stop_timers.pop(device_id, None)
        if t is not None and not t.done():
            t.cancel()

    def _schedule_stop(self, device_id: str):
        self._cancel_stop_timer(device_id)
        self._stop_timers[device_id] = asyncio.get_running_loop().create_task(
            self._delayed_stop(device_id))

    async def _delayed_stop(self, device_id: str):
        """延迟停流：到期时若仍无监听者，才给设备下发 stop_stream 并广播 idle。"""
        try:
            await asyncio.sleep(self.LISTENER_STOP_DELAY_S)
            if self.listeners.get(device_id):
                return  # 有监听者回来了，不停流
            await self.send_json_to_device(device_id, {"type": "cmd", "cmd": "stop_stream"})
            await self.broadcast_event(device_id, {
                "type": "event", "event": P.EV_STREAM_STATE,
                "device_id": device_id, "state": "idle",
                "listener_count": 0, "ts": P.now_ms(),
            })
        except asyncio.CancelledError:
            raise
        finally:
            if self._stop_timers.get(device_id) is asyncio.current_task():
                self._stop_timers.pop(device_id, None)

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
    async def device_list(self) -> list:
        """合并本进程内存 + 跨实例共享状态，返回全部在线设备。

        - 本进程持有的设备：使用内存中的实时信息（含 listener_count、最新 last_seen）。
        - 其它实例持有的设备：从共享表读取（last_seen 由持有实例周期刷新）。
        - 共享状态不可用时自动降级为仅本地内存（单实例行为），不影响主流程。
        """
        now = P.now_ms()
        items: dict[str, dict] = {}

        # 1) 本地内存中的设备
        for device_id, dev in self.devices.items():
            age = now - dev.last_seen
            items[device_id] = {
                "device_id": device_id,
                "online": age <= P.DEVICE_TIMEOUT_MS,
                "fw": dev.fw,
                "recording_enabled": dev.recording_enabled,
                "sd_ok": dev.sd_ok,
                "listener_count": len(self.listeners.get(device_id, ())),
                "config": dev.config,
                "last_seen": dev.last_seen,
            }

        # 2) 跨实例共享状态（其它实例上的设备）
        rows = await presence.list_devices()
        if rows:
            for row in rows:
                device_id = row.get("device_id")
                if not device_id or device_id in items:
                    continue
                last_seen = int(row.get("last_seen") or 0)
                items[device_id] = {
                    "device_id": device_id,
                    "online": (now - last_seen) <= P.DEVICE_TIMEOUT_MS,
                    "fw": row.get("fw") or "unknown",
                    "recording_enabled": bool(row.get("recording_enabled", True)),
                    "sd_ok": bool(row.get("sd_ok", True)),
                    # 监听者连接本身是实例本地的；非本实例设备此处为 0
                    "listener_count": len(self.listeners.get(device_id, ())),
                    "config": row.get("config") or {},
                    "last_seen": last_seen,
                }

        return list(items.values())

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
            # 清理共享库中的僵尸行（实例崩溃未 delete 的行，保留 5 分钟）
            await presence.purge_stale(now - 300_000)

    async def start_stream(self, device_id: str):
        await self.send_json_to_device(device_id, {"cmd": "start_stream"})

    async def stop_stream(self, device_id: str):
        await self.send_json_to_device(device_id, {"cmd": "stop_stream"})