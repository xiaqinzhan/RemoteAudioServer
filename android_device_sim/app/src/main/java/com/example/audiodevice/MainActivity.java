package com.example.audiodevice;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.widget.Button;
import android.widget.EditText;
import android.widget.RadioGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public class MainActivity extends AppCompatActivity {

    private static final int REQ_MIC = 1001;
    private static final String DEFAULT_SERVER =
            "https://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site";

    private EditText inputServer;
    private EditText inputDevice;
    private EditText inputFreq;
    private RadioGroup groupSource;
    private Button btnConnect;
    private TextView textStatus;
    private TextView textStream;
    private TextView textLog;
    private android.view.View dotStatus;

    private DeviceSession session;
    private final StringBuilder logBuf = new StringBuilder();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        inputServer = findViewById(R.id.input_server);
        inputDevice = findViewById(R.id.input_device);
        inputFreq = findViewById(R.id.input_freq);
        groupSource = findViewById(R.id.group_source);
        btnConnect = findViewById(R.id.btn_connect);
        textStatus = findViewById(R.id.text_status);
        textStream = findViewById(R.id.text_stream);
        textLog = findViewById(R.id.text_log);
        dotStatus = findViewById(R.id.dot_status);

        inputServer.setText(DEFAULT_SERVER);
        inputDevice.setText("dev-android-001");

        btnConnect.setOnClickListener(v -> {
            if (session != null && session.isConnected()) {
                disconnect();
            } else {
                connect();
            }
        });
    }

    private boolean useMic() {
        return groupSource.getCheckedRadioButtonId() == R.id.src_mic;
    }

    private void connect() {
        String server = inputServer.getText().toString().trim().replaceAll("/+$", "");
        String dev = inputDevice.getText().toString().trim();
        if (server.isEmpty() || dev.isEmpty()) {
            appendLog("请填写服务器地址和设备 ID");
            return;
        }
        // 麦克风模式需先拿权限
        if (useMic() && !ensureMicPermission()) {
            appendLog("需要麦克风权限，授权后再点连接");
            return;
        }
        appendLog("准备连接 " + server + " 设备 " + dev);
        session = new DeviceSession(this, server, dev, useMic(), new DeviceSession.Callback() {
            @Override
            public void onLog(String msg) {
                appendLog(msg);
            }

            @Override
            public void onConnectionChanged(boolean connected) {
                runOnUiThread(() -> setConnectionUi(connected));
            }

            @Override
            public void onStreamingChanged(boolean streaming) {
                runOnUiThread(() -> textStream.setText(streaming ? "● 推流中" : "未推流"));
            }
        });
        session.connect();
    }

    private void disconnect() {
        if (session != null) {
            session.disconnect();
            session = null;
        }
        setConnectionUi(false);
    }

    private void setConnectionUi(boolean connected) {
        textStatus.setText(connected ? "已连接" : "未连接");
        dotStatus.setBackgroundResource(connected ? R.drawable.dot_online : R.drawable.dot_offline);
        textStream.setText(connected ? "已连接(待机)" : "未推流");
        btnConnect.setText(connected ? "断开" : "连接服务器");
        btnConnect.setBackgroundResource(connected ? R.drawable.btn_stop : R.drawable.btn_accent);
        btnConnect.setTextColor(connected ? 0xFFFFFFFF : 0xFF06222B);
    }

    private boolean ensureMicPermission() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            return true;
        }
        ActivityCompat.requestPermissions(this,
                new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
        return false;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_MIC) {
            boolean granted = grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            appendLog(granted ? "麦克风权限已授权" : "麦克风权限被拒绝");
        }
    }

    private void appendLog(String msg) {
        String ts = new SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(new Date());
        logBuf.append(ts).append("  ").append(msg).append('\n');
        if (logBuf.length() > 4000) {
            logBuf.delete(0, logBuf.length() - 4000);
        }
        runOnUiThread(() -> textLog.setText(logBuf.toString()));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        disconnect();
    }
}
