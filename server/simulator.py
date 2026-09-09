"""
内置设备模拟器 (Simulator)
--------------------------
以「真实 WebSocket 客户端」身份连到本机 /ws/device/{device_id}，完整走设备协议，
让整套系统无需任何真实硬件即可演示与测试。

每台虚拟设备运行在独立线程的独立 asyncio 事件循环中：
  - 发送 hello，定时 ping；
  - 响应 start_stream/stop_stream：推送合成的 0x01 PCM(16kHz/16bit/mono/640B) 蜂鸣音；
  - 内存中维护录音列表，响应 list_recordings；
  - 响应 play_file：从预置 Ogg Opus 资产分块按 0x02 协议回传；
  - 响应 set_recording / set_config；
  - 录音开关打开时，定时随机产生 recording_saved 事件。
"""

import asyncio
import json
import logging
import os
import random
import threading
import time

import numpy as np
import websockets

from . import protocol as P

log = logging.getLogger("sim")

ASSETS_DIR = os.path.join(os.path.dirname(__file__), "assets")
SIM_IDS = ["sim-101", "sim-102", "sim-103"]
SIM_TONES = {  # device_id -> 基础频率(Hz)
    "sim-101": 880.0,
    "sim-102": 660.0,
    "sim-103": 440.0,
}
SIM_CONF = {"vad_threshold": 1200, "retention_days": 30}


def _load_samples() -> list:
    """加载预置的 Ogg Opus 资产（供虚拟录音回放）。"""
    samples = []
    if os.path.isdir(ASSETS_DIR):
        for fn in sorted(os.listdir(ASSETS_DIR)):
            if fn.endswith(".opus"):
                with open(os.path.join(ASSETS_DIR, fn), "rb") as f:
                    samples.append((fn, f.read()))
    if not samples:
        log.warning("no opus assets found in %s", ASSETS_DIR)
    return samples


class SimulatorManager:
    def __init__(self, port: int, host: str = "127.0.0.1"):
        self.port = port
        self.host = host
        self.samples = _load_samples()
        self._threads: dict[str, threading.Thread] = {}
        self._stop_events: dict[str, threading.Event] = {}
        self._conns: dict[str, "websockets.ClientConnection"] = {}
        self._loops: dict[str, asyncio.AbstractEventLoop] = {}
        self._started_ts: dict[str, float] = {}

    # ------------------------------------------------------------------
    # 对外控制
    # ------------------------------------------------------------------
    def status(self) -> dict:
        running = [
            did for did in SIM_IDS
            if did in self._threads and self._threads[did].is_alive()
            and not self._stop_events[did].is_set()
        ]
        return {
            "running": running,
            "available": [i for i in SIM_IDS if i not in running],
        }

    def start_ids(self, device_ids) -> list:
        started = []
        for did in device_ids:
            if did not in SIM_IDS:
                continue
            if did in self._threads and self._threads[did].is_alive():
                continue
            ev = threading.Event()
            self._stop_events[did] = ev
            t = threading.Thread(
                target=self._run_thread, args=(did, ev), daemon=True, name=f"sim-{did}"
            )
            self._threads[did] = t
            t.start()
            started.append(did)
        return started

    def stop_ids(self, device_ids) -> list:
        stopped = []
        for did in device_ids:
            if did not in SIM_IDS:
                continue
            self._stop_events.get(did, threading.Event()).set()
            ws = self._conns.get(did)
            if ws is not None:
                try:
                    loop2 = getattr(self, "_loops", {}).get(did)
                    if loop2 is not None:
                        asyncio.run_coroutine_threadsafe(ws.close(), loop2)
                except Exception:
                    pass
            stopped.append(did)
        return stopped

    def _run_thread(self, did: str, stop_ev: threading.Event):
        asyncio.run(self._sim_loop(did, stop_ev))

    # ------------------------------------------------------------------
    # 单台虚拟设备主循环
    # ------------------------------------------------------------------
    async def _sim_loop(self, did: str, stop_ev: threading.Event):
        loop = asyncio.get_running_loop()
        self._loops[did] = loop
        url = f"ws://{self.host}:{self.port}/ws/device/{did}"
        freq = SIM_TONES.get(did, 660.0)
        tone_buf = self._build_tone(freq)
        recordings = self._seed_recordings()
        recording_enabled = True
        config = dict(SIM_CONF)
        seq_rand = random.Random()

        while not stop_ev.is_set():
            try:
                async with websockets.connect(url, ping_interval=None) as ws:
                    self._conns[did] = ws
                    await ws.send(json.dumps({
                        "type": "hello",
                        "device_id": did,
                        "fw": "2.0.0",
                        "recording_enabled": recording_enabled,
                        "sd_ok": True,
                        "config": config,
                    }))
                    log.info("[sim] %s connected", did)

                    stream_stop = asyncio.Event()
                    stream_task: "asyncio.Task | None" = None
                    offset = 0
                    active_plays: dict[int, asyncio.Task] = {}

                    async def ping_loop():
                        while True:
                            await ws.send(json.dumps({
                                "type": "ping",
                                "ts": int(time.time() * 1000),
                            }))
                            await asyncio.sleep(P.DEVICE_PING_INTERVAL)

                    async def stream_loop():
                        nonlocal offset
                        try:
                            while not stream_stop.is_set():
                                chunk = tone_buf[offset:offset + P.PCM_FRAME_BYTES]
                                if len(chunk) < P.PCM_FRAME_BYTES:
                                    chunk += tone_buf[:P.PCM_FRAME_BYTES - len(chunk)]
                                    offset = P.PCM_FRAME_BYTES - len(
                                        tone_buf[offset:offset + P.PCM_FRAME_BYTES]
                                    )
                                else:
                                    offset = (offset + P.PCM_FRAME_BYTES) % len(tone_buf)
                                await ws.send(P.encode_frame(P.FRAME_LIVE_AUDIO, 0, 0, chunk))
                                await asyncio.sleep(0.020)
                        except asyncio.CancelledError:
                            raise
                        except Exception:
                            pass

                    async def event_loop():
                        while True:
                            await asyncio.sleep(seq_rand.uniform(15, 30))
                            if recording_enabled and recordings:
                                new_rec = self._new_recording()
                                recordings.insert(0, new_rec)
                                await ws.send(json.dumps({
                                    "type": "event",
                                    "event": P.EV_RECORDING_SAVED,
                                    "file": new_rec["name"],
                                    "date": new_rec["date"],
                                    "duration": new_rec["duration"],
                                }))
                                log.info("[sim] %s new recording %s", did, new_rec["name"])

                    async def play_loop(req_id, content):
                        total = len(content)
                        seq = 0
                        off = 0
                        while off < total:
                            chunk = content[off:off + P.FILE_BLOCK_SIZE]
                            is_last = (off + len(chunk)) >= total
                            s = seq if not is_last else P.SEQ_LAST
                            await ws.send(P.encode_frame(P.FRAME_FILE_BLOCK, req_id, s, chunk))
                            await asyncio.sleep(0.02)
                            off += len(chunk)
                            seq += 1

                    async def handle_request(data):
                        nonlocal recording_enabled, config
                        req_id = data.get("req_id")
                        cmd = data.get("cmd")
                        if cmd == "list_recordings":
                            await ws.send(json.dumps({
                                "type": P.EV_RESPONSE, "req_id": req_id, "ok": True,
                                "files": [
                                    {
                                        "name": r["name"], "date": r["date"],
                                        "size": r["size"], "duration": r["duration"],
                                    }
                                    for r in recordings
                                ],
                            }))
                        elif cmd == "play_file":
                            fname = data.get("file", "")
                            content = self._find_recording(fname, recordings)
                            if content is None:
                                await ws.send(json.dumps({
                                    "type": P.EV_RESPONSE, "req_id": req_id,
                                    "ok": False, "msg": "file not found",
                                }))
                                return
                            # 取消该 req 此前可能存在的同 req 播放
                            old = active_plays.get(req_id)
                            if old and not old.done():
                                old.cancel()
                            task = asyncio.create_task(play_loop(req_id, content))
                            active_plays[req_id] = task
                            task.add_done_callback(
                                lambda t, r=req_id: active_plays.pop(r, None)
                            )
                        elif cmd == "stop_file":
                            ref = data.get("req_ref")
                            for rid, t in list(active_plays.items()):
                                if ref is None or rid == ref:
                                    if not t.done():
                                        t.cancel()
                                    active_plays.pop(rid, None)
                        elif cmd == "set_recording":
                            recording_enabled = bool(data.get("enabled", False))
                            await ws.send(json.dumps({
                                "type": P.EV_RESPONSE, "req_id": req_id, "ok": True,
                                "recording_enabled": recording_enabled,
                            }))
                        elif cmd == "set_config":
                            if "vad_threshold" in data:
                                config["vad_threshold"] = int(data["vad_threshold"])
                            if "retention_days" in data:
                                config["retention_days"] = int(data["retention_days"])
                            await ws.send(json.dumps({
                                "type": P.EV_RESPONSE, "req_id": req_id, "ok": True,
                                "config": config,
                            }))
                        else:
                            await ws.send(json.dumps({
                                "type": P.EV_RESPONSE, "req_id": req_id,
                                "ok": False, "msg": "unknown cmd",
                            }))

                    async def first_done(*tasks):
                        await asyncio.gather(*tasks)

                    ping_t = asyncio.create_task(ping_loop())
                    event_t = asyncio.create_task(event_loop())
                    try:
                        async for raw in ws:
                            if stop_ev.is_set():
                                break
                            if isinstance(raw, bytes):
                                continue
                            try:
                                data = json.loads(raw)
                            except Exception:
                                continue
                            cmd = data.get("cmd")
                            if cmd == "start_stream":
                                if stream_task is None or stream_task.done():
                                    stream_stop.clear()
                                    stream_task = asyncio.create_task(stream_loop())
                            elif cmd == "stop_stream":
                                if stream_task is not None:
                                    stream_stop.set()
                                    stream_task.cancel()
                                    stream_task = None
                            elif data.get("type") == P.EV_REQUEST_CMD:
                                await handle_request(data)
                    finally:
                        for t in (stream_task, ping_t, event_t):
                            if t is not None:
                                t.cancel()
                        for t in active_plays.values():
                            t.cancel()
                        await asyncio.gather(
                            *[t for t in (stream_task, ping_t, event_t) if t],
                            return_exceptions=True,
                        )
                        self._conns.pop(did, None)
            except Exception as exc:
                log.warning("[sim] %s connection error: %s", did, exc)
                if stop_ev.is_set():
                    break
                await asyncio.sleep(2.0)  # 退避重连

    # ------------------------------------------------------------------
    # 合成数据
    # ------------------------------------------------------------------
    @staticmethod
    def _build_tone(freq: float) -> bytes:
        """合成 1 秒 16k/16bit/mono PCM 蜂鸣（每 0.35s 响/0.65s 静，便于带耳机听清）。"""
        sr = P.SAMPLE_RATE
        n = sr
        t = np.arange(n) / sr
        gate = np.where((t % 1.0) < 0.35, 1.0, 0.04)
        wave = 0.55 * np.sin(2.0 * np.pi * freq * t) * gate
        pcm16 = (np.clip(wave, -1.0, 1.0) * 32767).astype("<i2")
        return pcm16.tobytes()

    def _seed_recordings(self) -> list:
        """预置几条假录音记录，内容取自 Opus 资产。"""
        out = []
        specs = [
            ("2026-09-05/07-12-40.opus", "2026-09-05", 6),
            ("2026-09-06/10-03-11.opus", "2026-09-06", 6),
            ("2026-09-07/14-27-52.opus", "2026-09-07", 6),
            ("2026-09-08/09-00-05.opus", "2026-09-08", 6),
            ("2026-09-09/08-30-15.opus", "2026-09-09", 6),
        ]
        for i, (name, date, dur) in enumerate(specs):
            content = self.samples[i % max(len(self.samples), 1)][1] if self.samples else b""
            out.append({
                "name": name, "date": date,
                "size": len(content), "duration": dur, "content": content,
            })
        out.sort(key=lambda r: r["name"], reverse=True)
        return out

    def _new_recording(self) -> dict:
        if self.samples:
            _n, content = self.samples[random.randrange(len(self.samples))]
        else:
            content = b""
        now = time.localtime()
        date = time.strftime("%Y-%m-%d", now)
        name = date + "/" + time.strftime("%H-%M-%S", now) + ".opus"
        return {
            "name": name, "date": date,
            "size": len(content), "duration": round(len(content) / 2000, 1),
            "content": content,
        }

    def _find_recording(self, fname: str, recordings: list):
        for r in recordings:
            if r["name"] == fname:
                return r["content"]
        return None