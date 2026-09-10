package com.example.audiomonitor;

/**
 * 与服务端一致的二进制协议常量（见 server/protocol.py）。
 * 帧格式：Byte0 type | Byte1-4 req_id(uint32 大端) | Byte5-6 seq(uint16) | Byte7+ payload
 */
public final class Proto {
    private Proto() {}

    public static final int FRAME_LIVE_AUDIO = 0x01; // 实时音频帧(广播)：payload=16k/16bit/mono PCM
    public static final int FRAME_FILE_BLOCK = 0x02; // 录音文件块(按 req_id 路由)

    public static final int SAMPLE_RATE = 16000;
    public static final int HEADER_LEN = 7;
    public static final int PCM_FRAME_BYTES = 640;
    public static final int FILE_BLOCK_SIZE = 2048;
    public static final int SEQ_LAST = 0xFFFF;

    /** 解析一帧二进制数据。 */
    public static Frame parse(byte[] data, int len) {
        if (data == null || len < HEADER_LEN) return null;
        int type = data[0] & 0xFF;
        long reqId = ((long)(data[1] & 0xFF) << 24) | ((data[2] & 0xFF) << 16)
                | ((data[3] & 0xFF) << 8) | (data[4] & 0xFF);
        int seq = ((data[5] & 0xFF) << 8) | (data[6] & 0xFF);
        int payloadLen = len - HEADER_LEN;
        byte[] payload = new byte[payloadLen];
        System.arraycopy(data, HEADER_LEN, payload, 0, payloadLen);
        return new Frame(type, reqId, seq, payload);
    }

    public static final class Frame {
        public final int type;
        public final long reqId;
        public final int seq;
        public final byte[] payload;
        Frame(int type, long reqId, int seq, byte[] payload) {
            this.type = type; this.reqId = reqId; this.seq = seq; this.payload = payload;
        }
        public boolean isLastBlock() { return seq == SEQ_LAST; }
    }
}
