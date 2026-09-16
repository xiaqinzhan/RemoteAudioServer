package com.example.audiomonitor;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
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

    /**
     * http(s) -> ws(s)，拼出监听 WebSocket 地址。
     *
     * role=live    真监听：计入服务端监听人数、触发设备推流、接收实时 PCM；
     * role=control 控制类会话：只收发录音列表/回放/录音开关/事件，不计人数、不推实时 PCM
     *              （但它自己请求的响应与文件块照常返回，服务端按 req_id 直投）。
     * sid 传 {@link #clientSid}(context)：同一客户端会话重连时服务端会用新连接替换旧条目，
     * 避免旧连接残留导致"监听人数"虚高、真实监听者拿不到开流指令。
     */
    public static String listenWsUrl(String base, String deviceId, String role, String sid) {
        String ws = base.replace("https://", "wss://").replace("http://", "ws://");
        StringBuilder sb = new StringBuilder(ws)
                .append("/ws/listen/").append(urlEncode(deviceId))
                .append("?role=").append(urlEncode(role));
        if (sid != null && !sid.isEmpty()) {
            sb.append("&sid=").append(urlEncode(sid));
        }
        return sb.toString();
    }

    private static String urlEncode(String s) {
        try {
            return java.net.URLEncoder.encode(s, "UTF-8");
        } catch (Exception e) {
            return s;
        }
    }

    /** 客户端会话 id：首次生成随机 UUID 后持久化，重连/重启保持稳定。 */
    public static String clientSid(Context ctx) {
        SharedPreferences sp = ctx.getSharedPreferences("audio_monitor", Context.MODE_PRIVATE);
        String sid = sp.getString("client_sid", null);
        if (sid == null || sid.isEmpty()) {
            sid = UUID.randomUUID().toString();
            sp.edit().putString("client_sid", sid).apply();
        }
        return sid;
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
