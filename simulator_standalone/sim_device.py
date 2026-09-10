#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
remoteAudioServer 独立设备模拟器（standalone simulator)
=====================================================
在【本地】以「真实 WebSocket 客户端」身份连接（云端或局域网）服务器，
把本机模拟成一台或多台 ESP32 采集设备，让整套系统无需任何真实硬件即可演示/测试。

它与项目内置的 `server/simulator.py` 同协议，但被抽成可独立运行的脚本：
  - 自带协议层（无包依赖，无需把本脚本放进项目里跑）；
  - 内置一段真实 Ogg Opus 录音样本（base64 内嵌），即使本地没有 .opus 资产也能回放；
  - 用纯 Python 合成蜂鸣 PCM（不依赖 numpy）。

依赖：Python 3.8+，只需 `websockets`。
    pip install websockets

参考用法
--------
# 1) 连云端 HTTPS 部署（自动使用 wss://），模拟 2 台设备
python3 sim_device.py --url https://<你的云端域名> --device-ids sim-101,sim-102

# 2) 连 http(局域网/本机)，指定端口，模拟 1 台
python3 sim_device.py --host 127.0.0.1 --port 8000 --device-ids sim-103

# 3) 指定 tone 频率、关闭自动生成录音事件
python3 sim_device.py --url https://<域名> --device-ids sim-101 --freq 520 --no-auto-record

# 4) 使用本地 .opus 资产作为虚拟录音内容（覆盖内置样本）
python3 sim_device.py --host 192.168.1.10 --port 8000 --device-ids sim-101,sim-102 --assets-dir ./opus_assets
"""

import argparse
import asyncio
import base64
import json
import math
import random
import struct
import sys
import time

try:
    import websockets
except ImportError:
    print("缺少依赖 websockets，请先执行：pip install websockets", file=sys.stderr)
    sys.exit(2)


# =====================================================================
# 协议层（与 server/protocol.py 保持一致，独立内置，避免包依赖）
# =====================================================================
FRAME_LIVE_AUDIO = 0x01   # 实时音频帧（服务端广播给该设备全部监听者）
FRAME_FILE_BLOCK = 0x02   # 录音文件块（服务端按 req_id 路由回发起请求的监听者）
SAMPLE_RATE = 16000       # 16kHz
BYTES_PER_SAMPLE = 2      # 16bit
CHANNELS = 1              # 单声道
PCM_FRAME_BYTES = 640     # 20ms @16kHz/16bit/mono (16000*2*0.020)
FILE_BLOCK_SIZE = 2048    # 录音文件块负载大小
SEQ_LAST = 0xFFFF         # 文件最后一块标记
DEVICE_PING_INTERVAL = 15.0

_EV_RECORDING_SAVED = "recording_saved"
_EV_REQUEST_CMD = "request"
_EV_RESPONSE = "response"

_HEADER = struct.Struct(">B I H")  # type(1) + req_id(4) + seq(2) = 7 字节头
_DEFAULT_IDS = ["sim-101", "sim-102", "sim-103"]
_DEFAULT_TONES = {"sim-101": 880.0, "sim-102": 660.0, "sim-103": 440.0}
_SIM_CONF = {"vad_threshold": 1200, "retention_days": 30}


def _encode_frame(ftype: int, req_id: int, seq: int, payload: bytes) -> bytes:
    return _HEADER.pack(ftype, req_id, seq) + payload


# 内置 Ogg Opus 录音样本（无外部资产时用于回放演示）
_EMBEDDED_OPUS_B64 = "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"


# =====================================================================
# 工具：合成 / 录音
# =====================================================================
def build_tone(freq: float) -> bytes:
    """合成 1 秒 16k/16bit/mono 蜂鸣（每 0.35s 响 / 0.65s 弱，便于听清）。"""
    n = SAMPLE_RATE
    buf = bytearray()
    for i in range(n):
        t = i / SAMPLE_RATE
        gate = 1.0 if (t % 1.0) < 0.35 else 0.04
        v = 0.55 * math.sin(2.0 * math.pi * freq * t) * gate
        s = int(max(-1.0, min(1.0, v)) * 32767)
        buf += struct.pack("<h", s)
    return bytes(buf)


def load_samples(assets_dir: str) -> list:
    """优先读本地 .opus 资产，否则回退到内置样本。返回 [(name, bytes), ...]"""
    samples = []
    if assets_dir and os.path.isdir(assets_dir):
        for fn in sorted(os.listdir(assets_dir)):
            if fn.endswith(".opus"):
                with open(os.path.join(assets_dir, fn), "rb") as f:
                    samples.append((fn, f.read()))
    if not samples and _EMBEDDED_OPUS_B64 and _EMBEDDED_OPUS_B64 != "__OPUS_B64__":
        try:
            samples.append(("embedded.opus", base64.b64decode(_EMBEDDED_OPUS_B64)))
        except Exception:
            pass
    return samples


# =====================================================================
# 虚拟设备
# =====================================================================
class SimDevice:
    def __init__(self, device_id: str, url: str, samples: list,
                 auto_record: bool = True, freq: float = 0.0):
        self.did = device_id
        self.url = url
        self.samples = samples
        self.auto_record = auto_record
        self.freq = freq or _DEFAULT_TONES.get(device_id, 700.0)
        self.tone_buf = build_tone(self.freq)
        self.recording_enabled = True
        self.config = dict(_SIM_CONF)
        self._rng = random.Random()

    # ---- 录音清单 ----
    def _seed_recordings(self):
        specs = [
            ("2026-09-05/07-12-40.opus", "2026-09-05", 6),
            ("2026-09-06/10-03-11.opus", "2026-09-06", 6),
            ("2026-09-07/14-27-52.opus", "2026-09-07", 6),
            ("2026-09-08/09-00-05.opus", "2026-09-08", 6),
            ("2026-09-09/08-30-15.opus", "2026-09-09", 6),
        ]
        out = []
        for i, (name, date, dur) in enumerate(specs):
            content = self.samples[i % max(len(self.samples), 1)][1] if self.samples else b""
            out.append({"name": name, "date": date, "size": len(content),
                        "duration": dur, "content": content})
        out.sort(key=lambda r: r["name"], reverse=True)
        return out

    def _new_recording(self):
        content = self.samples[self._rng.randrange(len(self.samples))][1] if self.samples else b""
        now = time.localtime()
        date = time.strftime("%Y-%m-%d", now)
        return {
            "name": date + "/" + time.strftime("%H-%M-%S", now) + ".opus",
            "date": date, "size": len(content),
            "duration": round(len(content) / 2000, 1), "content": content,
        }

    def _find_recording(self, fname, recordings):
        for r in recordings:
            if r["name"] == fname:
                return r["content"]
        return None

    # ---- 主循环 ----
    async def _run_once(self, ws):
        await ws.send(json.dumps({
            "type": "hello", "device_id": self.did, "fw": "2.0.0",
            "recording_enabled": self.recording_enabled, "sd_ok": True,
            "config": self.config,
        }))
        print(f"[{self.did}] connected to {self.url}")

        recordings = self._seed_recordings()
        stream_stop = asyncio.Event()
        stream_task = None
        offset = 0
        active_plays = {}

        async def ping_loop():
            while True:
                await ws.send(json.dumps({"type": "ping", "ts": int(time.time() * 1000)}))
                await asyncio.sleep(DEVICE_PING_INTERVAL)

        async def stream_loop():
            nonlocal offset
            try:
                while not stream_stop.is_set():
                    chunk = self.tone_buf[offset:offset + PCM_FRAME_BYTES]
                    if len(chunk) < PCM_FRAME_BYTES:
                        chunk += self.tone_buf[:PCM_FRAME_BYTES - len(chunk)]
                        offset = PCM_FRAME_BYTES - len(self.tone_buf[offset:offset + PCM_FRAME_BYTES])
                    else:
                        offset = (offset + PCM_FRAME_BYTES) % len(self.tone_buf)
                    await ws.send(_encode_frame(FRAME_LIVE_AUDIO, 0, 0, chunk))
                    await asyncio.sleep(0.020)
            except asyncio.CancelledError:
                raise
            except Exception:
                pass

        async def event_loop():
            while True:
                await asyncio.sleep(self._rng.uniform(15, 30))
                if self.auto_record and self.recording_enabled and recordings:
                    nr = self._new_recording()
                    recordings.insert(0, nr)
                    await ws.send(json.dumps({
                        "type": "event", "event": _EV_RECORDING_SAVED,
                        "file": nr["name"], "date": nr["date"], "duration": nr["duration"],
                    }))
                    print(f"[{self.did}] new recording {nr['name']}")

        async def play_loop(req_id, content):
            total = len(content)
            seq = 0
            off = 0
            while off < total:
                chunk = content[off:off + FILE_BLOCK_SIZE]
                is_last = (off + len(chunk)) >= total
                await ws.send(_encode_frame(FRAME_FILE_BLOCK, req_id,
                                            seq if not is_last else SEQ_LAST, chunk))
                await asyncio.sleep(0.02)
                off += len(chunk)
                seq += 1

        async def handle_request(data):
            nonlocal stream_stop, stream_task
            req_id = data.get("req_id")
            cmd = data.get("cmd")
            if cmd == "list_recordings":
                await ws.send(json.dumps({
                    "type": _EV_RESPONSE, "req_id": req_id, "ok": True,
                    "files": [{"name": r["name"], "date": r["date"],
                               "size": r["size"], "duration": r["duration"]}
                              for r in recordings],
                }))
            elif cmd == "play_file":
                fname = data.get("file", "")
                content = self._find_recording(fname, recordings)
                if content is None:
                    await ws.send(json.dumps({
                        "type": _EV_RESPONSE, "req_id": req_id, "ok": False,
                        "msg": "file not found"}))
                    return
                old = active_plays.get(req_id)
                if old and not old.done():
                    old.cancel()
                task = asyncio.create_task(play_loop(req_id, content))
                active_plays[req_id] = task
                task.add_done_callback(lambda t, r=req_id: active_plays.pop(r, None))
            elif cmd == "stop_file":
                ref = data.get("req_ref")
                for rid, t in list(active_plays.items()):
                    if ref is None or rid == ref:
                        if not t.done():
                            t.cancel()
                        active_plays.pop(rid, None)
            elif cmd == "set_recording":
                self.recording_enabled = bool(data.get("enabled", False))
                await ws.send(json.dumps({
                    "type": _EV_RESPONSE, "req_id": req_id, "ok": True,
                    "recording_enabled": self.recording_enabled}))
            elif cmd == "set_config":
                if "vad_threshold" in data:
                    self.config["vad_threshold"] = int(data["vad_threshold"])
                if "retention_days" in data:
                    self.config["retention_days"] = int(data["retention_days"])
                await ws.send(json.dumps({
                    "type": _EV_RESPONSE, "req_id": req_id, "ok": True,
                    "config": self.config}))
            else:
                await ws.send(json.dumps({
                    "type": _EV_RESPONSE, "req_id": req_id, "ok": False,
                    "msg": "unknown cmd"}))

        ping_t = asyncio.create_task(ping_loop())
        event_t = asyncio.create_task(event_loop())
        try:
            async for raw in ws:
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
                elif data.get("type") == _EV_REQUEST_CMD:
                    await handle_request(data)
        finally:
            for t in (stream_task, ping_t, event_t):
                if t is not None:
                    t.cancel()
            for t in active_plays.values():
                t.cancel()
            await asyncio.gather(
                *[t for t in (stream_task, ping_t, event_t) if t],
                return_exceptions=True)

    async def run(self):
        while True:
            print(f"[{self.did}] (re)connecting...")
            try:
                async with websockets.connect(self.url, ping_interval=None) as ws:
                    await self._run_once(ws)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                print(f"[{self.did}] connection error: {exc}")
            await asyncio.sleep(2.0)


# =====================================================================
# CLI
# =====================================================================
def _arg_parser():
    p = argparse.ArgumentParser(
        description="remoteAudioServer 独立设备模拟器（本地连接云端/局域网服务器）")
    grp = p.add_mutually_exclusive_group(required=True)
    grp.add_argument("--url", help="服务器根地址，如 https://xxx.dev.coze.site "
                                   "或 http://192.168.1.10:8000（自动判断 ws/wss）")
    grp.add_argument("--host", help="服务器主机/域名（配合 --port）")
    p.add_argument("--port", type=int, default=8000, help="服务器端口（配合 --host；默认 8000）")
    p.add_argument("--wss", action="store_true", help="使用 wss://（HTTPS 需开启）")
    p.add_argument("--device-ids", default="sim-101",
                   help="设备ID，逗号分隔，如 sim-101,sim-102,sim-103")
    p.add_argument("--freq", type=float, default=0.0,
                   help="蜂鸣频率 Hz（0=按设备ID默认；仅单设备时有意义）")
    p.add_argument("--assets-dir", default="", help="本地 .opus 资产目录（虚拟录音内容）")
    p.add_argument("--no-auto-record", action="store_true",
                   help="关闭定时自动生成 recording_saved 事件")
    return p


def main(argv=None):
    args = _arg_parser().parse_args(argv)
    device_ids = [d.strip() for d in args.device_ids.split(",") if d.strip()]
    if not device_ids:
        print("--device-ids 至少给一个", file=sys.stderr)
        sys.exit(2)

    if args.url:
        base = args.url.rstrip("/")
        scheme = "wss" if base.startswith("https://") else "ws"
        hostport = base.split("://", 1)[1]
    else:
        scheme = "wss" if args.wss else "ws"
        hostport = args.host
        if args.port:
            hostport = f"{hostport}:{args.port}"

    samples = load_samples(args.assets_dir)

    async def _run_all():
        devs = []
        for i, did in enumerate(device_ids):
            url = f"{scheme}://{hostport}/ws/device/{did}"
            freq = args.freq if args.freq > 0 else _DEFAULT_TONES.get(did, 700.0)
            if len(device_ids) == 1 and args.freq > 0:
                freq = args.freq
            devs.append(SimDevice(did, url, samples,
                                  auto_record=not args.no_auto_record, freq=freq))
        print("开始模拟设备：", ", ".join(device_ids),
              f"| 服务器 {scheme}://{hostport} | 资产 {len(samples)} 个")
        await asyncio.gather(*(d.run() for d in devs))

    try:
        asyncio.run(_run_all())
    except KeyboardInterrupt:
        print("\n已退出")


if __name__ == "__main__":
    main()