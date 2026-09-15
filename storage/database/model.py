"""
数据模型（共享状态）
==================
唯一对外暴露的库表定义都在本文件（model.py），供数据库迁移工具统一管理。

当前表：device_presence —— 「在线设备」的跨实例共享视图。
服务实例在设备 hello 后 upsert，心跳期间 touch last_seen，断开时 delete；
GET /api/devices 读取本表以聚合所有实例的在线设备（详见 server/presence.py）。
"""

from sqlalchemy import BigInteger, Boolean, Column, DateTime, Text, text
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import declarative_base

Base = declarative_base()


class DevicePresence(Base):
    __tablename__ = "device_presence"

    device_id = Column(Text, primary_key=True)
    fw = Column(Text, nullable=False, server_default="unknown")
    recording_enabled = Column(Boolean, nullable=False, server_default=text("true"))
    sd_ok = Column(Boolean, nullable=False, server_default=text("true"))
    config = Column(JSONB, nullable=False, server_default=text("'{}'::jsonb"))
    instance_id = Column(Text)                    # 哪个实例当前持有该设备（排查用）
    last_seen = Column(BigInteger, nullable=False)  # 毫秒时间戳
    updated_at = Column(DateTime(timezone=True), server_default=text("now()"))
