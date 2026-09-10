package com.example.audiomonitor;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

import java.util.concurrent.LinkedBlockingQueue;

/**
 * 用 AudioTrack 流式播放 16kHz/16bit/单声道 PCM（对应浏览器端的 AudioWorklet）。
 * 内部用独立线程 + 阻塞队列写入，保证实时帧按时播放、不阻塞 WebSocket 线程。
 */
public class PcmPlayer {

    public interface LevelListener {
        /** rms 为 0~1 的线性能量，UI 可自行换算成 dB / 进度。 */
        void onLevel(double rms);
    }

    private final LinkedBlockingQueue<byte[]> queue = new LinkedBlockingQueue<>(600);
    private AudioTrack track;
    private Thread worker;
    private volatile boolean running = false;
    private LevelListener levelListener;

    public void setLevelListener(LevelListener l) { this.levelListener = l; }

    public void start() {
        if (running) return;
        int minBuf = AudioTrack.getMinBufferSize(
                Proto.SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bufSize = Math.max(minBuf, Proto.PCM_FRAME_BYTES * 8);

        track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(Proto.SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build())
                .setBufferSizeInBytes(bufSize)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();

        track.play();
        running = true;
        queue.clear();

        worker = new Thread(() -> {
            byte[] flush = new byte[bufSize];
            // 预填静音，减少起始 underrun
            track.write(flush, 0, flush.length);
            while (running) {
                try {
                    byte[] chunk = queue.take();
                    if (chunk == null) break;
                    reportLevel(chunk);
                    int off = 0;
                    while (off < chunk.length && running) {
                        int n = track.write(chunk, off, chunk.length - off);
                        if (n <= 0) break;
                        off += n;
                    }
                } catch (InterruptedException e) {
                    break;
                }
            }
        }, "pcm-player");
        worker.start();
    }

    /** 投喂一帧 PCM（640 字节）。 */
    public void enqueue(byte[] pcm) {
        if (!running) return;
        // 队列满说明播放跟不上，丢最旧的一帧，避免延迟累积
        if (!queue.offer(pcm)) {
            queue.poll();
            queue.offer(pcm);
        }
    }

    public void stop() {
        running = false;
        queue.offer(new byte[0]); // 唤醒 take
        if (worker != null) {
            worker.interrupt();
            try { worker.join(500); } catch (InterruptedException ignored) {}
            worker = null;
        }
        if (track != null) {
            try {
                track.pause();
                track.flush();
                track.stop();
            } catch (Exception ignored) {}
            track.release();
            track = null;
        }
        queue.clear();
    }

    private void reportLevel(byte[] pcm) {
        if (levelListener == null || pcm.length < 2) return;
        long sumSq = 0;
        int samples = pcm.length / 2;
        for (int i = 0; i + 1 < pcm.length; i += 2) {
            int lo = pcm[i] & 0xFF;
            int hi = pcm[i + 1];
            int s = (hi << 8) | lo; // little-endian int16
            sumSq += (long) s * s;
        }
        double rms = Math.sqrt((double) sumSq / Math.max(1, samples)) / 32768.0;
        levelListener.onLevel(rms);
    }
}
