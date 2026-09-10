package com.example.audiodevice;

/**
 * 实时音源：每次产出一个 20ms（640 字节 = 320 samples）的 16bit/16kHz/mono PCM 块。
 */
public interface AudioSource {
    /** 读取一个 640 字节 PCM 帧；返回实际字节数，<=0 表示结束。 */
    int readFrame(byte[] out640);

    void start() throws Exception;

    void stop();
}
