"""
设备在线状态的「跨实例共享」读写（Supabase / PostgREST）
=====================================================
背景：服务可能由多个内存态互相独立的实例承载。设备 WebSocket 长连接只粘在
某一个实例上，其它实例的内存里没有这台设备，导致 GET /api/devices 看不到它。

本模块把「在线设备」这份状态从进程内存搬到共享库（表 device_presence）：
  - 设备 hello 后：upsert 一行（携带 fw / 录音开关 / sd / config / last_seen）；
  - 连接存活期间：心跳循环里周期 touch last_seen（保证新鲜，>0 且 < 设备超时窗口）；
  - 设备断开：delete 该行。
这样任意实例的 /api/devices 都能读到「全部实例」的在线设备。

设计要点：
  - Supabase SDK 是同步的，所有调用统一用 asyncio.to_thread 包住，绝不阻塞事件循环；
  - 任何异常都被吞掉（只记日志），数据库不可用时自动降级为「仅本地内存」（即单实例行为），
    不影响设备接入 / 音频转发等主链路；
  - 表结构见 storage/database/model.py。
"""

import asyncio
import logging
import socket

log = logging.getLogger("presence")

TABLE = "device_presence"
_INSTANCE_ID = socket.gethostname()

_client = None
_disabled = False
_warned = False


def _get_client():
    """惰性初始化共享库客户端；不可用时置 _disabled 并返回 None。"""
    global _client, _disabled
    if _client is not None:
        return _client
    if _disabled:
        return None
    try:
        from storage.database.supabase_client import get_supabase_client

        _client = get_supabase_client()
        log.info("presence: shared state enabled (instance=%s)", _INSTANCE_ID)
    except Exception as e:  # 初始化失败：降级为本地内存，不影响主流程
        _disabled = True
        log.warning("presence: shared state unavailable, fallback to local-only: %s", e)
        return None
    return _client


def _warn_once(msg: str):
    global _warned
    if not _warned:
        _warned = True
        log.warning("%s", msg)


# --------------------------- 同步底层操作 ---------------------------
def _upsert(device_id, fw, recording_enabled, sd_ok, config, last_seen):
    c = _get_client()
    if c is None:
        return
    c.table(TABLE).upsert({
        "device_id": device_id,
        "fw": fw,
        "recording_enabled": recording_enabled,
        "sd_ok": sd_ok,
        "config": config or {},
        "instance_id": _INSTANCE_ID,
        "last_seen": last_seen,
    }).execute()


def _touch(device_id, last_seen):
    # 用 upsert 而非 update：若该行被其它实例误删，本实例下一次心跳会重新写入，
    # 避免"同名设备（如各实例都会自启的 sim-101/102）因实例退出删行而整体消失"。
    c = _get_client()
    if c is None:
        return
    c.table(TABLE).upsert({
        "device_id": device_id,
        "instance_id": _INSTANCE_ID,
        "last_seen": last_seen,
    }).execute()


def _remove(device_id):
    # 只删除「本实例持有」的那一行：同名设备可能同时存在于多个实例，
    # 不能因为本实例断开就把别的实例还在线的同名设备一起删掉。
    c = _get_client()
    if c is None:
        return
    c.table(TABLE).delete().eq("device_id", device_id).eq(
        "instance_id", _INSTANCE_ID).execute()


def _purge(cutoff_ms):
    # 清理长期未刷新（实例崩溃/异常退出导致未 delete）的僵尸行，防止表膨胀。
    c = _get_client()
    if c is None:
        return
    c.table(TABLE).delete().lt("last_seen", cutoff_ms).execute()


def _list():
    c = _get_client()
    if c is None:
        return None
    r = c.table(TABLE).select(
        "device_id,fw,recording_enabled,sd_ok,config,last_seen"
    ).execute()
    return r.data or []


# --------------------------- async 包装（永不抛异常） ---------------------------
async def upsert_device(device_id, fw, recording_enabled, sd_ok, config, last_seen):
    try:
        await asyncio.to_thread(_upsert, device_id, fw, recording_enabled, sd_ok, config, last_seen)
    except Exception as e:
        _warn_once(f"presence upsert failed (will keep trying): {e}")


async def touch(device_id, last_seen):
    try:
        await asyncio.to_thread(_touch, device_id, last_seen)
    except Exception as e:
        _warn_once(f"presence touch failed (will keep trying): {e}")


async def remove(device_id):
    try:
        await asyncio.to_thread(_remove, device_id)
    except Exception as e:
        _warn_once(f"presence remove failed (will keep trying): {e}")


async def list_devices():
    """返回全部实例的在线设备行列表；共享库不可用时返回 None（调用方回退本地内存）。"""
    try:
        return await asyncio.to_thread(_list)
    except Exception as e:
        _warn_once(f"presence list failed (fallback to local): {e}")
        return None


async def purge_stale(cutoff_ms):
    """删除 last_seen 早于 cutoff_ms 的僵尸行（实例崩溃未清理的行）。"""
    try:
        await asyncio.to_thread(_purge, cutoff_ms)
    except Exception as e:
        _warn_once(f"presence purge failed (will keep trying): {e}")
