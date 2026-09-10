package com.example.audiomonitor;

/** 设备信息（对应 /api/devices 返回项）。 */
public class DeviceInfo {
    public String deviceId;
    public boolean online;
    public String fw;
    public boolean recordingEnabled;
    public boolean sdOk;
    public int listenerCount;

    public DeviceInfo() {}
}
