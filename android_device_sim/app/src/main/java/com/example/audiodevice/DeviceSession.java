package com.example.audiodevice;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

/**
 * 设备会话：以真实 WebSocket 客户端身份连 /ws/device/{id}，模拟一台 ESP32 采集设备。
 *  - 连接后发 hello，每 15s 发 ping；
 *  - 收 start_stream 开始推 0x01 PCM 帧（蜂鸣或麦克风），stop_stream 停止；
 *  - 响应 list_recordings / play_file(0x02 文件块) / stop_file / set_recording / set_config；
 *  - 周期性发 recording_saved 事件。
 */
public class DeviceSession {

    public interface Callback {
        void onLog(String msg);
        void onConnectionChanged(boolean connected);
        void onStreamingChanged(boolean streaming);
    }

    private final Context ctx;
    private final String httpBase;   // 如 https://xxx 或 http://ip:port
    private final String deviceId;
    private final Callback cb;

    private final OkHttpClient client = new OkHttpClient.Builder()
            .pingInterval(0, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.SECONDS)
            .build();
    private final ScheduledExecutorService scheduler = Executors.newScheduledThreadPool(2);
    private final Handler main = new Handler(Looper.getMainLooper());

    private WebSocket ws;
    private final AtomicBoolean connected = new AtomicBoolean(false);
    private final AtomicBoolean streaming = new AtomicBoolean(false);
    private volatile boolean useMic = false;

    private Thread streamThread;
    private AudioSource activeSource;

    private volatile boolean recordingEnabled = true;
    private int vadThreshold = 1200;
    private int retentionDays = 30;

    private byte[] opusBytes;          // 用于 play_file 的内置录音内容
    private final AtomicBoolean fileCancel = new AtomicBoolean(false);
    private long lastFileReq = -1;

    public DeviceSession(Context ctx, String httpBase, String deviceId, boolean useMic, Callback cb) {
        this.ctx = ctx.getApplicationContext();
        this.httpBase = httpBase;
        this.deviceId = deviceId;
        this.useMic = useMic;
        this.cb = cb;
    }

    public void setUseMic(boolean v) {
        this.useMic = v;
    }

    public boolean isConnected() {
        return connected.get();
    }

    public boolean isStreaming() {
        return streaming.get();
    }

    public void connect() {
        String wsBase = httpBase.replaceFirst("^http", "ws"); // https->wss, http->ws
        String url = wsBase + "/ws/device/" + deviceId;
        Request req = new Request.Builder().url(url).build();
        log("连接 " + url);
        client.newWebSocket(req, new WebSocketListener() {
            @Override
            public void onOpen(WebSocket webSocket, Response response) {
                ws = webSocket;
                connected.set(true);
                log("已连接，发送 hello");
                sendHello();
                cb.onConnectionChanged(true);
                scheduler.scheduleAtFixedRate(DeviceSession.this::sendPing, 15, 15, TimeUnit.SECONDS);
                scheduler.scheduleAtFixedRate(DeviceSession.this::maybeNewRecording, 20, 25, TimeUnit.SECONDS);
            }

            @Override
            public void onMessage(WebSocket webSocket, String text) {
                handleText(text);
            }

            @Override
            public void onClosing(WebSocket webSocket, int code, String reason) {
                webSocket.close(1000, null);
            }

            @Override
            public void onClosed(WebSocket webSocket, int code, String reason) {
                handleClose("连接关闭: " + reason);
            }

            @Override
            public void onFailure(WebSocket webSocket, Throwable t, Response response) {
                handleClose("连接失败: " + t.getMessage());
            }
        });
    }

    public void disconnect() {
        stopStream();
        connected.set(false);
        scheduler.shutdownNow();
        if (ws != null) {
            try { ws.close(1000, "bye"); } catch (Exception ignored) {}
        }
        cb.onConnectionChanged(false);
        cb.onStreamingChanged(false);
    }

    private void handleClose(String msg) {
        connected.set(false);
        stopStream();
        log(msg);
        cb.onConnectionChanged(false);
        cb.onStreamingChanged(false);
    }

    // ---------------- 协议消息 ----------------

    private void sendHello() {
        try {
            JSONObject o = new JSONObject();
            o.put("type", "hello");
            o.put("device_id", deviceId);
            o.put("fw", "2.0.0");
            o.put("recording_enabled", recordingEnabled);
            o.put("sd_ok", true);
            JSONObject cfg = new JSONObject();
            cfg.put("vad_threshold", vadThreshold);
            cfg.put("retention_days", retentionDays);
            o.put("config", cfg);
            sendText(o.toString());
        } catch (Exception e) {
            log("hello 失败: " + e.getMessage());
        }
    }

    private void sendPing() {
        if (!connected.get()) return;
        try {
            JSONObject o = new JSONObject();
            o.put("type", "ping");
            o.put("ts", System.currentTimeMillis());
            sendText(o.toString());
        } catch (Exception ignored) {
        }
    }

    private void maybeNewRecording() {
        if (!connected.get() || !recordingEnabled) return;
        try {
            String date = new java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US)
                    .format(new java.util.Date());
            String time = new java.text.SimpleDateFormat("HH-mm-ss", java.util.Locale.US)
                    .format(new java.util.Date());
            JSONObject o = new JSONObject();
            o.put("type", "event");
            o.put("event", "recording_saved");
            o.put("file", date + "/" + time + ".opus");
            o.put("duration", 12 + (int) (Math.random() * 40));
            sendText(o.toString());
            log("产生新录音 " + date + "/" + time + ".opus");
        } catch (Exception ignored) {
        }
    }

    private void handleText(String text) {
        try {
            JSONObject m = new JSONObject(text);
            String type = m.optString("type", "");
            // 兼容直接下发的 {"cmd":"start_stream"}
            String cmd = m.optString("cmd", type.equals("request") ? m.optString("cmd") : "");
            if (cmd.isEmpty()) return;

            if ("start_stream".equals(cmd)) {
                startStream();
                return;
            }
            if ("stop_stream".equals(cmd)) {
                stopStream();
                return;
            }

            // 请求-响应类
            long reqId = m.optLong("req_id", -1);
            switch (cmd) {
                case "list_recordings":
                    respondRecordings(reqId);
                    break;
                case "play_file": {
                    String file = m.optString("file", "");
                    startFileTransfer(reqId, file);
                    break;
                }
                case "stop_file":
                    fileCancel.set(true);
                    log("停止文件发送 req=" + reqId);
                    ack(reqId, true);
                    break;
                case "set_recording":
                    recordingEnabled = m.optBoolean("enabled", recordingEnabled);
                    log("录音开关 -> " + recordingEnabled);
                    JSONObject r = baseResp(reqId, true);
                    r.put("recording_enabled", recordingEnabled);
                    sendText(r.toString());
                    break;
                case "set_config":
                    if (m.has("vad_threshold")) vadThreshold = m.optInt("vad_threshold");
                    if (m.has("retention_days")) retentionDays = m.optInt("retention_days");
                    JSONObject rr = baseResp(reqId, true);
                    JSONObject cfg = new JSONObject();
                    cfg.put("vad_threshold", vadThreshold);
                    cfg.put("retention_days", retentionDays);
                    rr.put("config", cfg);
                    sendText(rr.toString());
                    break;
                default:
                    ack(reqId, false);
            }
        } catch (Exception e) {
            log("处理消息异常: " + e.getMessage());
        }
    }

    private void respondRecordings(long reqId) throws Exception {
        JSONArray files = new JSONArray();
        String[][] seed = {
                {"2026-09-05/21-12-03.opus", "2026-09-05", "49"},
                {"2026-09-05/21-30-40.opus", "2026-09-05", "31"},
                {"2026-09-09/08-30-15.opus", "2026-09-09", "47"},
                {"2026-09-09/12-05-52.opus", "2026-09-09", "22"},
        };
        for (String[] s : seed) {
            JSONObject f = new JSONObject();
            f.put("name", s[0]);
            f.put("date", s[1]);
            f.put("size", getOpus().length);
            f.put("duration", Integer.parseInt(s[2]));
            files.put(f);
        }
        JSONObject resp = baseResp(reqId, true);
        resp.put("files", files);
        sendText(resp.toString());
        log("返回录音列表 " + files.length() + " 条");
    }

    private void startFileTransfer(long reqId, String file) {
        fileCancel.set(false);
        lastFileReq = reqId;
        log("开始回放文件 " + file + " req=" + reqId);
        scheduler.execute(() -> {
            try {
                byte[] data = getOpus();
                int seq = 0;
                for (int off = 0; off < data.length; off += Proto.FILE_BLOCK_SIZE) {
                    if (fileCancel.get() || !connected.get()) break;
                    int len = Math.min(Proto.FILE_BLOCK_SIZE, data.length - off);
                    int thisSeq;
                    if (off + Proto.FILE_BLOCK_SIZE >= data.length) {
                        thisSeq = Proto.SEQ_LAST;
                    } else {
                        thisSeq = seq++;
                    }
                    byte[] frame = Proto.encodeFrame(Proto.TYPE_FILE_BLOCK, reqId, thisSeq, data, off, len);
                    if (ws != null) ws.send(ByteString.of(frame));
                    // 稍微让出，避免瞬间占满
                    Thread.sleep(8);
                }
                if (!fileCancel.get()) {
                    JSONObject done = baseResp(reqId, true);
                    done.put("done", true);
                    sendText(done.toString());
                    log("文件发送完成 req=" + reqId);
                }
            } catch (Exception e) {
                log("文件发送异常: " + e.getMessage());
            }
        });
    }

    // ---------------- 实时推流 ----------------

    private void startStream() {
        if (streaming.get()) return;
        try {
            AudioSource src;
            if (useMic) {
                MicSource mic = new MicSource();
                try {
                    mic.start();
                    src = mic;
                    log("开始推流：麦克风");
                } catch (Exception e) {
                    log("麦克风不可用，回退蜂鸣: " + e.getMessage());
                    ToneSource tone = new ToneSource(660);
                    tone.start();
                    src = tone;
                }
            } else {
                ToneSource tone = new ToneSource(660);
                tone.start();
                src = tone;
                log("开始推流：合成蜂鸣");
            }
            activeSource = src;
            streaming.set(true);
            cb.onStreamingChanged(true);

            streamThread = new Thread(() -> {
                byte[] buf = new byte[Proto.PCM_FRAME_BYTES];
                while (streaming.get() && connected.get()) {
                    long t0 = System.currentTimeMillis();
                    int n = src.readFrame(buf);
                    if (n > 0 && ws != null) {
                        byte[] frame = Proto.liveFrame(buf, 0, n);
                        ws.send(ByteString.of(frame));
                    }
                    long elapsed = System.currentTimeMillis() - t0;
                    long sleep = 20 - elapsed;
                    if (sleep > 0) {
                        try { Thread.sleep(sleep); } catch (InterruptedException e) { break; }
                    }
                }
            }, "pcm-pusher");
            streamThread.start();
        } catch (Exception e) {
            log("启动推流失败: " + e.getMessage());
            streaming.set(false);
            cb.onStreamingChanged(false);
        }
    }

    private void stopStream() {
        if (!streaming.getAndSet(false)) return;
        log("停止推流");
        if (streamThread != null) {
            streamThread.interrupt();
            streamThread = null;
        }
        if (activeSource != null) {
            try { activeSource.stop(); } catch (Exception ignored) {}
            activeSource = null;
        }
        cb.onStreamingChanged(false);
    }

    // ---------------- 工具 ----------------

    private byte[] getOpus() {
        if (opusBytes != null) return opusBytes;
        try (InputStream is = ctx.getAssets().open("sample.opus")) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] tmp = new byte[4096];
            int r;
            while ((r = is.read(tmp)) != -1) bos.write(tmp, 0, r);
            opusBytes = bos.toByteArray();
        } catch (Exception e) {
            // 没有资产时给一个极小占位（非真实音频，但保证流程）
            opusBytes = new byte[]{0};
            log("内置 opus 资产缺失: " + e.getMessage());
        }
        return opusBytes;
    }

    private JSONObject baseResp(long reqId, boolean ok) throws Exception {
        JSONObject o = new JSONObject();
        o.put("type", "response");
        o.put("req_id", reqId);
        o.put("ok", ok);
        return o;
    }

    private void ack(long reqId, boolean ok) {
        try {
            sendText(baseResp(reqId, ok).toString());
        } catch (Exception ignored) {
        }
    }

    private void sendText(String s) {
        if (ws != null) ws.send(s);
    }

    private void log(String msg) {
        main.post(() -> cb.onLog("[" + deviceId + "] " + msg));
    }
}
