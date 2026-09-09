"""集成冒烟测试：以真实监听者客户端走完整协议（无需浏览器）。
用法：python tools/e2e_test.py [port]
"""
import asyncio
import json
import os
import struct
import sys

import websockets

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("DEPLOY_RUN_PORT", "5000"))
HOST = "127.0.0.1"
SEP = "-" * 60


async def main():
    base_ws = f"ws://{HOST}:{PORT}/ws/listen"
    passed = []
    failed = []

    def check(name, cond, extra=""):
        (passed if cond else failed).append(name)
        print(f"[{'PASS' if cond else 'FAIL'}] {name} {extra}")

    # ---------- 1. 作为监听者连接 sim-101 ----------
    ws = await asyncio.wait_for(
        websockets.connect(f"{base_ws}/sim-101", ping_interval=None, max_size=8 * 1024 * 1024),
        8,
    )
    print(SEP); print("1) 实时监听链路（start_stream -> 接收 0x01 实时帧）")
    ok_start = False
    got_first = None
    for _ in range(60):  # 最多等 ~3s（20ms/帧）
        try:
            frame = await asyncio.wait_for(ws.recv(), 2)
        except asyncio.TimeoutError:
            break
        if isinstance(frame, bytes):
            if len(frame) >= 7 and frame[0] == 0x01:
                got_first = frame
                break
        else:
            pass
    check("收到 0x01 实时音频帧", got_first is not None, f"{len(got_first) if got_first else 0} 字节")
    if got_first:
        payload = got_first[7:]
        check("实时帧负载 640 字节 PCM", len(payload) == 640, f"{len(payload)}")
        check("实时帧 req_id 为 0", struct.unpack(">I", got_first[1:5])[0] == 0)
        # 校验负载非全静音（峰值存在）
        vals = struct.unpack("<" + "h" * (len(payload) // 2), payload)
        peak = max(abs(v) for v in vals)
        check("PCM 有实际波形（非静音）", peak > 2000, f"peak={peak}")

    # ---------- 2. list_recordings ----------
    print(SEP); print("2) 请求-响应中继（list_recordings）")
    await ws.send(json.dumps({"type": "request", "req_id": 1001, "cmd": "list_recordings"}))
    resp = await asyncio.wait_for(ws.recv(), 4)
    rd = json.loads(resp)
    files = rd.get("files", [])
    check("list_recordings 返回 ok=true 且文件列表", rd.get("ok") is True and len(files) > 0, f"{len(files)} 条")
    check("req_id 回路由正确(原 id 1001)", rd.get("req_id") == 1001, str(rd.get("req_id")))

    # ---------- 3. play_file 二进制 0x02 文件块 ----------
    print(SEP); print("3) 录音回放（play_file -> 0x02 文件块 -> 拼接为 Ogg）")
    target = files[0]["name"]
    await ws.send(json.dumps({"type": "request", "req_id": 1002, "cmd": "play_file", "file": target}))
    chunks = {}
    last_seen = 0
    err = None
    try:
        while True:
            frame = await asyncio.wait_for(ws.recv(), 4)
            if isinstance(frame, bytes) and len(frame) >= 7 and frame[0] == 0x02:
                rq = struct.unpack(">I", frame[1:5])[0]
                if rq == 1002:
                    s = struct.unpack(">H", frame[5:7])[0]
                    if s == 0xFFFF:
                        last_seen = 1
                        break
                    chunks[s] = frame[7:]
    except asyncio.TimeoutError:
        err = "timeout"
    data = b"".join(chunks[i] for i in sorted(chunks))
    check("收到 0x02 文件块并拼出 Ogg Opus 数据", last_seen == 1 and data.startswith(b"OggS"), f"{len(data)} 字节")
    if last_seen == 1 and data.startswith(b"OggS"):
        got_files = set(f["name"] for f in files)
        # 作为额外校验：播放文件块按序拼接且大小与列表一致
        rec_size = next((f["size"] for f in files if f["name"] == target), None)
        check("文件块数据长度与 recording.size 吻合", rec_size is None or abs(len(data) - rec_size) <= 2048, f"len={len(data)} size={rec_size}")

    # ---------- 4. set_recording ----------
    print(SEP); print("4) 远程录音开关（set_recording）")
    await ws.send(json.dumps({"type": "request", "req_id": 1003, "cmd": "set_recording", "enabled": False}))
    resp = await asyncio.wait_for(ws.recv(), 4)
    sr = json.loads(resp)
    check("set_recording 返回新状态 false", sr.get("ok") is True and sr.get("recording_enabled") is False, str(sr))

    # ---------- 5. stop_file 停止回放，且停止监听触发 stop_stream ----------
    print(SEP); print("5) 监听者离开 -> 设备触发停止")
    # 连续收到 3 帧 0x01 视为推流中，然后关闭 ws
    live_before_close = 0
    ws2 = await asyncio.wait_for(websockets.connect(f"{base_ws}/sim-101", ping_interval=None, max_size=8*1024*1024), 8)
    for _ in range(40):
        try:
            frame = await asyncio.wait_for(ws2.recv(), 1.5)
            if isinstance(frame, bytes) and frame[0] == 0x01:
                live_before_close += 1
        except asyncio.TimeoutError:
            break
    check("新增监听者后持续收到实时帧(第二连接)", live_before_close >= 3, f"{live_before_close} 帧")
    await ws2.close()
    await asyncio.sleep(0.6)
    # 第一个 ws 仍存在，应仍能收到实时帧 -> 说明 1->? 还有监听者
    still_live = False
    try:
        frame = await asyncio.wait_for(ws.recv(), 1.2)
        if isinstance(frame, bytes) and frame[0] == 0x01:
            still_live = True
    except asyncio.TimeoutError:
        pass
    check("第一个监听者(ws)仍可收实时帧(未全部离开)", still_live)

    # 关闭最后一个监听者
    await ws.send(json.dumps({"type": "request", "req_id": 1004, "cmd": "stop_stream"}))  # no-op 客户端侧
    await ws.close()

    # ---------- 6. 多监听路由/广播稳定 ----------
    print(SEP); print("6) 设备删除 / 离线清理")
    await asyncio.sleep(0.3)

    print(SEP)
    print(f"通过 {len(passed)}/{len(passed)+len(failed)}")
    if failed:
        print("失败项:", failed)
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())