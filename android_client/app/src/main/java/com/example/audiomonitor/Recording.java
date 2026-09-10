package com.example.audiomonitor;

import org.json.JSONObject;

/** 一条历史录音（list_recordings 返回的 files 项）。 */
public class Recording {
    public String name;      // "2026-09-05/14-22-10.opus"
    public String date;      // "2026-09-05"
    public long size;        // 字节
    public long duration;    // 秒

    public static Recording fromJson(JSONObject o) {
        Recording r = new Recording();
        r.name = o.optString("name", "");
        r.date = o.optString("date", "");
        r.size = o.optLong("size", 0);
        r.duration = o.optLong("duration", 0);
        return r;
    }
}
