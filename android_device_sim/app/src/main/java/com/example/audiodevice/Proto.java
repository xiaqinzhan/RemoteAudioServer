package com.example.audiodevice;

/**
 * 设备端二进制帧协议常量与编码工具。
 * 帧格式：Byte0=type | Byte1-4=req_id(uint32 BE) | Byte5-6=seq(uint16 BE) | Byte7+ payload
 *   0x01 实时音频帧：payload 为 16kHz/16bit/单声道 PCM，每帧 640 字节(20ms)；req_id=0
 *   0x02 录音文件块：payload 为 Ogg/Opus 文件字节，2048/块；seq 从 0 递增，0xFFFF=最后一块
 */
public final class Proto {
    public static final int TYPE_LIVE_AUDIO = 0x01;
    public static final int TYPE_FILE_BLOCK = 0x02;

    public static final int HEADER_LEN = 7;
    public static final int PCM_FRAME_BYTES = 640;   // 20ms @16kHz/16bit/mono
    public static final int SAMPLE_RATE = 16000;
    public static final int FILE_BLOCK_SIZE = 2048;
    public static final int SEQ_LAST = 0xFFFF;

    private Proto() {}

    /** 组装一帧二进制数据。 */
    public static byte[] encodeFrame(int type, long reqId, int seq, byte[] payload, int off, int len) {
        byte[] out = new byte[HEADER_LEN + len];
        out[0] = (byte) (type & 0xFF);
        out[1] = (byte) ((reqId >> 24) & 0xFF);
        out[2] = (byte) ((reqId >> 16) & 0xFF);
        out[3] = (byte) ((reqId >> 8) & 0xFF);
        out[4] = (byte) (reqId & 0xFF);
        out[5] = (byte) ((seq >> 8) & 0xFF);
        out[6] = (byte) (seq & 0xFF);
        if (len > 0) {
            System.arraycopy(payload, off, out, HEADER_LEN, len);
        }
        return out;
    }

    /** 实时音频帧（req_id 固定 0，seq 忽略=0）。 */
    public static byte[] liveFrame(byte[] pcm, int off, int len) {
        return encodeFrame(TYPE_LIVE_AUDIO, 0L, 0, pcm, off, len);
    }
}
