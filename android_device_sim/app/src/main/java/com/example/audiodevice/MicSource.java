package com.example.audiodevice;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;

/**
 * 手机麦克风音源：AudioRecord 采集 16kHz/16bit/单声道 PCM，输出 20ms 帧。
 * 让这台 Android 设备像真实 ESP32 采集端一样推麦克风声音。
 */
public class MicSource implements AudioSource {
    private AudioRecord recorder;
    private boolean started = false;

    @Override
    public void start() throws Exception {
        int minBuf = AudioRecord.getMinBufferSize(
                Proto.SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bufSize = Math.max(minBuf, Proto.PCM_FRAME_BYTES * 4);
        recorder = new AudioRecord(
                MediaRecorder.AudioSource.MIC,
                Proto.SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufSize);
        if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
            recorder.release();
            recorder = null;
            throw new IllegalStateException("AudioRecord 初始化失败（检查麦克风权限）");
        }
        recorder.startRecording();
        started = true;
    }

    @Override
    public int readFrame(byte[] out) {
        if (!started || recorder == null) return -1;
        int want = Proto.PCM_FRAME_BYTES;
        int total = 0;
        while (total < want) {
            int n = recorder.read(out, total, want - total);
            if (n <= 0) break;
            total += n;
        }
        // 不足一帧则补 0（静音），保持帧节奏
        for (int i = total; i < want; i++) out[i] = 0;
        return want;
    }

    @Override
    public void stop() {
        started = false;
        if (recorder != null) {
            try {
                recorder.stop();
            } catch (IllegalStateException ignored) {
            }
            recorder.release();
            recorder = null;
        }
    }
}
