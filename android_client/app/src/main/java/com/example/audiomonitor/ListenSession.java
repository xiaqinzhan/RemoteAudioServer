package com.example.audiomonitor;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

/**
 * 与服务端 /ws/listen/{deviceId} 的会话：发送 JSON 请求、接收 JSON 响应/事件
 * 与二进制帧（0x01 实时 PCM 直接喂给 PcmPlayer；0x02 文件块按 req_id 归集，末块回调）。
 */
public class ListenSession {

    public interface Callback {
        void onOpen();
        void onClosed(String reason);
        void onFailure(String msg);
        void onRecordings(List<Recording> files);
        void onStreamState(boolean streaming);
        void onRecordingEvent(String file);          // recording_saved 事件
        void onFileComplete(long reqId, byte[] data, String hintName);
        void onPcmFrame(byte[] pcm);
    }

    private static final String TAG = "ListenSession";
    private final OkHttpClient client;
    private WebSocket ws;
    private Callback callback;
    private volatile boolean closed = false;

    // 文件块归集
    private final Map<Long, ByteArrayOutputStream> fileBuffers = new HashMap<>();
    private final Map<Long, String> fileNames = new HashMap<>();

    public ListenSession() {
        this.client = new OkHttpClient.Builder()
                .connectTimeout(8, TimeUnit.SECONDS)
                .pingInterval(15, TimeUnit.SECONDS)
                .readTimeout(0, TimeUnit.SECONDS)
                .build();
    }

    public void connect(String wsUrl, Callback cb) {
        this.callback = cb;
        this.closed = false;
        Request request = new Request.Builder().url(wsUrl).build();
        ws = client.newWebSocket(request, new WebSocketListener() {
            @Override
            public void onOpen(WebSocket webSocket, Response response) {
                Log.i(TAG, "ws open");
                if (callback != null) callback.onOpen();
            }

            @Override
            public void onMessage(WebSocket webSocket, String text) {
                handleText(text);
            }

            @Override
            public void onMessage(WebSocket webSocket, ByteString bytes) {
                handleBinary(bytes.toByteArray());
            }

            @Override
            public void onClosing(WebSocket webSocket, int code, String reason) {
                webSocket.close(1000, null);
            }

            @Override
            public void onClosed(WebSocket webSocket, int code, String reason) {
                Log.i(TAG, "ws closed: " + reason);
                if (callback != null) callback.onClosed(reason);
            }

            @Override
            public void onFailure(WebSocket webSocket, Throwable t, Response response) {
                Log.e(TAG, "ws failure", t);
                if (!closed && callback != null) callback.onFailure(t.getMessage());
            }
        });
    }

    // ---- 发送请求 ----

    public void sendRequest(long reqId, String cmd, JSONObject extra) {
        if (ws == null) return;
        try {
            JSONObject o = new JSONObject();
            o.put("type", "request");
            o.put("req_id", reqId);
            o.put("cmd", cmd);
            if (extra != null) {
                for (java.util.Iterator<String> it = extra.keys(); it.hasNext(); ) {
                    String k = it.next();
                    o.put(k, extra.get(k));
                }
            }
            ws.send(o.toString());
        } catch (Exception e) {
            Log.e(TAG, "sendRequest failed", e);
        }
    }

    public void listRecordings(long reqId) {
        sendRequest(reqId, "list_recordings", null);
    }

    public void playFile(long reqId, String file) {
        try {
            JSONObject extra = new JSONObject().put("file", file);
            fileNames.put(reqId, file);
            fileBuffers.put(reqId, new ByteArrayOutputStream());
            sendRequest(reqId, "play_file", extra);
        } catch (Exception ignored) {}
    }

    public void stopFile(long reqId) {
        sendRequest(reqId, "stop_file", null);
    }

    public void setRecording(long reqId, boolean enabled) {
        try {
            sendRequest(reqId, "set_recording", new JSONObject().put("enabled", enabled));
        } catch (Exception ignored) {}
    }

    public void close() {
        closed = true;
        if (ws != null) {
            try { ws.close(1000, "bye"); } catch (Exception ignored) {}
            ws = null;
        }
        fileBuffers.clear();
        fileNames.clear();
    }

    // ---- 接收处理 ----

    private void handleText(String text) {
        if (callback == null) return;
        try {
            JSONObject d = new JSONObject(text);
            String type = d.optString("type", "");
            if ("response".equals(type)) {
                String cmd = d.optString("cmd", "");
                if ("list_recordings".equals(cmd) && d.optBoolean("ok", false)) {
                    List<Recording> files = new ArrayList<>();
                    JSONArray arr = d.optJSONArray("files");
                    if (arr != null) {
                        for (int i = 0; i < arr.length(); i++) {
                            files.add(Recording.fromJson(arr.getJSONObject(i)));
                        }
                    }
                    callback.onRecordings(files);
                }
            } else if ("event".equals(type)) {
                String ev = d.optString("event", "");
                if ("stream_state".equals(ev)) {
                    callback.onStreamState(d.optBoolean("streaming", false));
                } else if ("recording_saved".equals(ev)) {
                    callback.onRecordingEvent(d.optString("file", ""));
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "handleText error", e);
        }
    }

    private void handleBinary(byte[] data) {
        if (callback == null) return;
        Proto.Frame f = Proto.parse(data, data.length);
        if (f == null) return;
        if (f.type == Proto.FRAME_LIVE_AUDIO) {
            callback.onPcmFrame(f.payload);
        } else if (f.type == Proto.FRAME_FILE_BLOCK) {
            ByteArrayOutputStream bos = fileBuffers.get(f.reqId);
            if (bos == null) {
                bos = new ByteArrayOutputStream();
                fileBuffers.put(f.reqId, bos);
            }
            bos.write(f.payload, 0, f.payload.length);
            if (f.isLastBlock()) {
                byte[] all = bos.toByteArray();
                String name = fileNames.get(f.reqId);
                fileBuffers.remove(f.reqId);
                fileNames.remove(f.reqId);
                callback.onFileComplete(f.reqId, all, name);
            }
        }
    }
}
