"""
remoteAudioServer 协议层
------------------------
统一二进制帧格式（设备 <-> 服务端 <-> 监听端）：

    Byte0    : 帧类型     0x01 = 实时音频帧（广播）；0x02 = 录音文件块（按 req_id 路由）
    Byte1-4  : req_id     uint32 大端；实时帧固定为 0
    Byte5-6  : seq        uint16；文件块序号从 0 递增，0xFFFF 表示最后一块；实时帧忽略
    Byte7+   : payload    实时帧 = 640 字节 PCM(16kHz/16bit/mono)；文件块 = Ogg Opus 字节(2048/块)

JSON 消息均为 UTF-8 文本帧。
"""

import struct

# ---- 帧类型常量 ----
FRAME_LIVE_AUDIO = 0x01  # 实时音频帧（服务端广播给该设备全部监听者）
FRAME_FILE_BLOCK = 0x02  # 录音文件块（服务端按 req_id 路由回发起请求的监听者）

# ---- 音频参数 ----
SAMPLE_RATE = 16000       # 16kHz
BYTES_PER_SAMPLE = 2      # 16bit
CHANNELS = 1              # 单声道
PCM_FRAME_BYTES = 640     # 20ms @16kHz/16bit/mono (16000*2*0.020)
FILE_BLOCK_SIZE = 2048    # 录音文件块负载大小
SEQ_LAST = 0xFFFF         # 文件最后一块标记

# ---- 状态 / 存活 ----
DEVICE_PING_INTERVAL = 15.0   # 设备每 15s 发送一次 ping
DEVICE_TIMEOUT_MS = 45_000    # 45s 无消息判定离线

_HEADER = struct.Struct(">B I H")       # type(1) + req_id(4) + seq(2) = 7 字节头
HEAD_LEN = _HEADER.size

# ---- 事件类型 ----
EV_RECORDING_SAVED = "recording_saved"
EV_DEVICE_OFFLINE = "device_offline"
EV_DEVICE_ONLINE = "device_online"
EV_STREAM_STATE = "stream_state"
EV_REQUEST_CMD = "request"       # 浏览器 -> 服务端 -> 设备 的命令请求
EV_RESPONSE = "response"         # 设备 -> 服务端 -> 浏览器 的响应


def encode_frame(frametype: int, req_id: int, seq: int, payload: bytes) -> bytes:
    """编码一帧二进制数据。"""
    return _HEADER.pack(frametype, req_id, seq) + payload


def decode_frame(data: bytes) -> tuple:
    """解码二进制帧，返回 (frametype, req_id, seq, payload)。"""
    if len(data) < HEAD_LEN:
        raise ValueError("frame too short")
    frametype, req_id, seq = _HEADER.unpack_from(data, 0)
    return frametype, req_id, seq, data[HEAD_LEN:]


def rewrite_req_id(frame: bytes, req_id: int) -> bytes:
    """重写一帧的 req_id 字段（用于服务端内部 id <-> 监听端原 id 的映射）。"""
    return frame[:1] + struct.pack(">I", req_id) + frame[5:]


def now_ms() -> int:
    import time
    return int(time.time() * 1000)