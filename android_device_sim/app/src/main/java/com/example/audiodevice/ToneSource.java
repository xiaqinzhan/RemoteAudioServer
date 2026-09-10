package com.example.audiodevice;

/**
 * 合成提示音音源：指定频率正弦波，带缓慢“节拍”包络（更有设备提示音的听感）。
 * 循环生成 20ms PCM 帧。
 */
public class ToneSource implements AudioSource {
    private final double freq;
    private int phaseSample = 0;

    public ToneSource(double freqHz) {
        this.freq = freqHz <= 0 ? 660.0 : freqHz;
    }

    @Override
    public void start() {
        phaseSample = 0;
    }

    @Override
    public void stop() {
    }

    @Override
    public int readFrame(byte[] out) {
        final int samples = Proto.PCM_FRAME_BYTES / 2; // 320
        for (int i = 0; i < samples; i++) {
            long total = phaseSample + i;
            // 缓慢节拍包络：约每 1.6s 一个起伏，幅度 0.35~1.0
            double beat = 0.65 + 0.35 * Math.sin(2 * Math.PI * (total / (double) Proto.SAMPLE_RATE) / 1.6);
            double v = Math.sin(2 * Math.PI * freq * (total / (double) Proto.SAMPLE_RATE)) * 0.55 * beat;
            short s = (short) Math.max(-32768, Math.min(32767, (int) Math.round(v * 32767)));
            int idx = i * 2;
            out[idx] = (byte) (s & 0xFF);
            out[idx + 1] = (byte) ((s >> 8) & 0xFF); // 小端，Android/PCM 惯用
        }
        phaseSample += samples;
        return Proto.PCM_FRAME_BYTES;
    }
}
