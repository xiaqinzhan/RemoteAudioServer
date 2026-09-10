package com.example.audiomonitor;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

public class MainActivity extends AppCompatActivity {

    private static final String DEFAULT_SERVER =
            "https://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site";

    private EditText etServer;
    private TextView tvStatus, tvCurDevice, tvStreamState, tvRecEmpty;
    private Button btnConnect, btnListen, btnRefreshRec;
    private Switch swRec;
    private ProgressBar pbLevel;
    private View controlPanel;
    private RecyclerView rvDevices, rvRecordings;

    private DeviceAdapter deviceAdapter;
    private RecordingAdapter recordingAdapter;
    private final List<DeviceInfo> devices = new ArrayList<>();
    private final List<Recording> recordings = new ArrayList<>();

    private ApiClient api;
    private ListenSession session;
    private PcmPlayer pcmPlayer;
    private OpusPlayer opusPlayer;

    private String base;
    private String curDeviceId;
    private boolean liveOn = false;
    private int playingPos = -1;
    private long playingReqId = -1;

    private final AtomicLong reqSeq = new AtomicLong(1);
    private final Handler ui = new Handler(Looper.getMainLooper());
    private Runnable pollTask;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        etServer = findViewById(R.id.etServer);
        tvStatus = findViewById(R.id.tvStatus);
        tvCurDevice = findViewById(R.id.tvCurDevice);
        tvStreamState = findViewById(R.id.tvStreamState);
        tvRecEmpty = findViewById(R.id.tvRecEmpty);
        btnConnect = findViewById(R.id.btnConnect);
        btnListen = findViewById(R.id.btnListen);
        btnRefreshRec = findViewById(R.id.btnRefreshRec);
        swRec = findViewById(R.id.swRec);
        pbLevel = findViewById(R.id.pbLevel);
        controlPanel = findViewById(R.id.controlPanel);
        rvDevices = findViewById(R.id.rvDevices);
        rvRecordings = findViewById(R.id.rvRecordings);

        etServer.setText(DEFAULT_SERVER);
        etServer.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);

        api = new ApiClient();
        pcmPlayer = new PcmPlayer();
        opusPlayer = new OpusPlayer();

        deviceAdapter = new DeviceAdapter(devices, this::pickDevice);
        rvDevices.setLayoutManager(new LinearLayoutManager(this));
        rvDevices.setAdapter(deviceAdapter);

        recordingAdapter = new RecordingAdapter(recordings, this::onRecordingClicked);
        rvRecordings.setLayoutManager(new LinearLayoutManager(this));
        rvRecordings.setAdapter(recordingAdapter);

        // 实时电平回调（后台线程）→ 更新进度条
        pcmPlayer.setLevelListener(rms -> {
            int pct = (int) Math.min(100, Math.round(rms * 220)); // 线性映射，留余量
            ui.post(() -> pbLevel.setProgress(pct));
        });

        opusPlayer.setListener(new OpusPlayer.StateListener() {
            @Override public void onPlaybackStarted(long durationMs) {
                ui.post(() -> tvStreamState.setText("正在回放录音…"));
            }
            @Override public void onCompletion() {
                ui.post(() -> {
                    playingPos = -1;
                    playingReqId = -1;
                    recordingAdapter.setPlaying(-1);
                    tvStreamState.setText(liveOn ? "实时监听中" : "空闲");
                });
            }
            @Override public void onError(String msg) {
                ui.post(() -> {
                    toast(msg);
                    playingPos = -1;
                    recordingAdapter.setPlaying(-1);
                });
            }
        });

        btnConnect.setOnClickListener(v -> connect());
        btnListen.setOnClickListener(v -> toggleLive());
        btnRefreshRec.setOnClickListener(v -> requestRecordings());
        swRec.setOnClickListener(v -> {
            if (session != null && curDeviceId != null) {
                session.setRecording(nextReqId(), swRec.isChecked());
                toast("已发送录音开关: " + (swRec.isChecked() ? "开" : "关"));
            }
        });
    }

    private long nextReqId() { return reqSeq.incrementAndGet(); }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    // ---- 连接服务器 + 轮询设备 ----

    private void connect() {
        base = ApiClient.normalizeBase(etServer.getText().toString());
        etServer.setText(base);
        tvStatus.setText("连接中…");
        btnConnect.setEnabled(false);

        new Thread(() -> {
            try {
                List<DeviceInfo> list = api.fetchDevices(base);
                ui.post(() -> {
                    btnConnect.setEnabled(true);
                    tvStatus.setText("已连接 · " + list.size() + " 台设备");
                    devices.clear();
                    devices.addAll(list);
                    deviceAdapter.setItems(devices);
                    startPolling();
                    // 默认选中第一台在线设备
                    for (DeviceInfo d : list) {
                        if (d.online) { pickDevice(d); break; }
                    }
                });
            } catch (Exception e) {
                ui.post(() -> {
                    btnConnect.setEnabled(true);
                    tvStatus.setText("连接失败: " + e.getMessage());
                });
            }
        }).start();
    }

    private void startPolling() {
        if (pollTask != null) ui.removeCallbacks(pollTask);
        pollTask = new Runnable() {
            @Override public void run() {
                if (base == null) return;
                new Thread(() -> {
                    try {
                        List<DeviceInfo> list = api.fetchDevices(base);
                        ui.post(() -> {
                            devices.clear();
                            devices.addAll(list);
                            deviceAdapter.setItems(devices);
                            // 同步当前设备录音开关状态
                            for (DeviceInfo d : list) {
                                if (d.deviceId.equals(curDeviceId)) {
                                    swRec.setChecked(d.recordingEnabled);
                                }
                            }
                        });
                    } catch (Exception ignored) {}
                }).start();
                ui.postDelayed(this, 5000);
            }
        };
        ui.postDelayed(pollTask, 5000);
    }

    // ---- 选择设备：开监听会话 ----

    private void pickDevice(DeviceInfo d) {
        if (!d.online) {
            toast("设备离线，无法监听");
            return;
        }
        if (d.deviceId.equals(curDeviceId) && session != null) {
            deviceAdapter.setSelected(curDeviceId);
            return;
        }
        teardownSession();
        curDeviceId = d.deviceId;
        deviceAdapter.setSelected(curDeviceId);
        controlPanel.setVisibility(View.VISIBLE);
        tvCurDevice.setText(d.deviceId);
        swRec.setChecked(d.recordingEnabled);
        tvStreamState.setText("连接设备…");
        recordings.clear();
        recordingAdapter.setItems(recordings);
        tvRecEmpty.setVisibility(View.VISIBLE);
        tvRecEmpty.setText("加载录音中…");

        session = new ListenSession();
        session.connect(ApiClient.listenWsUrl(base, d.deviceId), new ListenSession.Callback() {
            @Override public void onOpen() {
                ui.post(() -> tvStreamState.setText("已连接，可开始监听"));
                requestRecordings();
            }
            @Override public void onClosed(String reason) {
                ui.post(() -> tvStreamState.setText("设备连接已断开"));
            }
            @Override public void onFailure(String msg) {
                ui.post(() -> {
                    tvStreamState.setText("连接错误: " + msg);
                    stopLive();
                });
            }
            @Override public void onRecordings(List<Recording> files) {
                ui.post(() -> {
                    recordings.clear();
                    recordings.addAll(files);
                    recordingAdapter.setItems(recordings);
                    tvRecEmpty.setVisibility(files.isEmpty() ? View.VISIBLE : View.GONE);
                    if (files.isEmpty()) tvRecEmpty.setText("该设备暂无录音文件");
                });
            }
            @Override public void onStreamState(boolean streaming) {
                ui.post(() -> {
                    if (streaming && liveOn) tvStreamState.setText("实时监听中 · 设备推流");
                });
            }
            @Override public void onRecordingEvent(String file) {
                ui.post(() -> {
                    toast("新录音: " + file);
                    requestRecordings();
                });
            }
            @Override public void onFileComplete(long reqId, byte[] data, String hintName) {
                if (reqId != playingReqId) return;
                ui.post(() -> {
                    stopLiveOnly();
                    opusPlayer.play(data, getCacheDir());
                });
            }
            @Override public void onPcmFrame(byte[] pcm) {
                if (liveOn && pcmPlayer != null) pcmPlayer.enqueue(pcm);
            }
        });
    }

    private void requestRecordings() {
        if (session != null) session.listRecordings(nextReqId());
    }

    // ---- 实时监听开关 ----

    private void toggleLive() {
        if (liveOn) {
            stopLive();
        } else {
            startLive();
        }
    }

    private void startLive() {
        if (session == null) { toast("请先选择设备"); return; }
        opusPlayer.stop();           // 单一音源：停回放
        playingPos = -1;
        recordingAdapter.setPlaying(-1);
        liveOn = true;
        pcmPlayer.start();
        btnListen.setText(R.string.stop_listen);
        btnListen.setBackgroundResource(R.drawable.btn_stop);
        btnListen.setTextColor(0xFFFFFFFF);
        tvStreamState.setText("实时监听中…");
    }

    private void stopLive() {
        stopLiveOnly();
        ui.post(() -> tvStreamState.setText("已停止监听"));
    }

    private void stopLiveOnly() {
        liveOn = false;
        pcmPlayer.stop();
        pbLevel.setProgress(0);
        btnListen.setText(R.string.start_listen);
        btnListen.setBackgroundResource(R.drawable.btn_accent);
        btnListen.setTextColor(0xFF06202A);
    }

    // ---- 录音回放 ----

    private void onRecordingClicked(Recording r, int position) {
        if (session == null) { toast("请先选择设备"); return; }
        if (position == playingPos) {
            // 再次点击 = 停止回放
            opusPlayer.stop();
            if (playingReqId >= 0) session.stopFile(playingReqId);
            playingPos = -1;
            playingReqId = -1;
            recordingAdapter.setPlaying(-1);
            tvStreamState.setText("已停止回放");
            return;
        }
        opusPlayer.stop();
        playingPos = position;
        playingReqId = nextReqId();
        recordingAdapter.setPlaying(position);
        tvStreamState.setText("获取录音…");
        session.playFile(playingReqId, r.name);
    }

    private void teardownSession() {
        stopLiveOnly();
        opusPlayer.stop();
        if (session != null) {
            session.close();
            session = null;
        }
        liveOn = false;
        playingPos = -1;
        playingReqId = -1;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (pollTask != null) ui.removeCallbacks(pollTask);
        teardownSession();
        pcmPlayer.stop();
        opusPlayer.stop();
    }
}
