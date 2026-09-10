package com.example.audiomonitor;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;

/** REST 访问：拉取设备列表。 */
public class ApiClient {
    private final OkHttpClient http;

    public ApiClient() {
        this.http = new OkHttpClient.Builder()
                .connectTimeout(8, TimeUnit.SECONDS)
                .readTimeout(10, TimeUnit.SECONDS)
                .build();
    }

    /** 规范化服务器基地址：去掉末尾斜杠，补全 scheme。 */
    public static String normalizeBase(String input) {
        String s = input.trim();
        while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
        if (!s.startsWith("http://") && !s.startsWith("https://")) {
            s = "http://" + s;
        }
        return s;
    }

    /** http(s) -> ws(s)，拼出设备监听 WebSocket 地址。 */
    public static String listenWsUrl(String base, String deviceId) {
        String ws = base.replace("https://", "wss://").replace("http://", "ws://");
        return ws + "/ws/listen/" + deviceId;
    }

    /** 拉取设备列表。失败抛 IOException。 */
    public List<DeviceInfo> fetchDevices(String base) throws IOException {
        Request req = new Request.Builder().url(base + "/api/devices").get().build();
        try (Response resp = http.newCall(req).execute()) {
            if (!resp.isSuccessful() || resp.body() == null) {
                throw new IOException("HTTP " + resp.code());
            }
            String body = resp.body().string();
            List<DeviceInfo> list = new ArrayList<>();
            JSONObject root = new JSONObject(body);
            JSONArray arr = root.optJSONArray("devices");
            if (arr != null) {
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject o = arr.getJSONObject(i);
                    DeviceInfo d = new DeviceInfo();
                    d.deviceId = o.optString("device_id", "unknown");
                    d.online = o.optBoolean("online", false);
                    d.fw = o.optString("fw", "");
                    d.recordingEnabled = o.optBoolean("recording_enabled", false);
                    d.sdOk = o.optBoolean("sd_ok", false);
                    d.listenerCount = o.optInt("listener_count", 0);
                    list.add(d);
                }
            }
            return list;
        } catch (Exception e) {
            if (e instanceof IOException) throw (IOException) e;
            throw new IOException("解析设备列表失败: " + e.getMessage(), e);
        }
    }
}
