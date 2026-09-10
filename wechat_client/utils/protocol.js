// utils/protocol.js
// 二进制帧协议（与服务端 server/protocol.py 完全一致）
// Byte0 type | Byte1-4 req_id(uint32 BE) | Byte5-6 seq(uint16) | Byte7+ payload
const FRAME_LIVE_AUDIO = 0x01; // 实时音频帧（640B 16k/16bit/mono PCM）
const FRAME_FILE_BLOCK = 0x02; // 录音文件块（seq 0xFFFF = 末块）
const HEADER_LEN = 7;
const PCM_FRAME_BYTES = 640;
const FILE_BLOCK_SIZE = 2048;
const SEQ_LAST = 0xffff;

// 解析一个 ArrayBuffer 帧
function parseFrame(buf) {
  const dv = new DataView(buf);
  const type = dv.getUint8(0);
  const reqId = dv.getUint32(1, false); // 大端
  const seq = dv.getUint16(5, false); // 大端
  const payload = buf.slice(HEADER_LEN);
  return { type: type, reqId: reqId, seq: seq, payload: payload };
}

// 将 16-bit PCM 的 ArrayBuffer 解码为 Float32 [-1,1]
function decodePcmToFloat32(pcmBuf) {
  const bytes = new Uint8Array(pcmBuf);
  // 保证偶数长度
  const sampleCount = bytes.length >> 1;
  const out = new Float32Array(sampleCount);
  const dv = new DataView(pcmBuf);
  for (let i = 0; i < sampleCount; i++) {
    const s = dv.getInt16(i * 2, true); // 小端 PCM
    out[i] = s / 32768;
  }
  return out;
}

// 计算一段 Float32 的 RMS（0~1），用于电平条
function rms(f32) {
  let sum = 0;
  for (let i = 0; i < f32.length; i++) {
    const v = f32[i];
    sum += v * v;
  }
  return Math.sqrt(sum / Math.max(1, f32.length));
}

module.exports = {
  FRAME_LIVE_AUDIO: FRAME_LIVE_AUDIO,
  FRAME_FILE_BLOCK: FRAME_FILE_BLOCK,
  HEADER_LEN: HEADER_LEN,
  PCM_FRAME_BYTES: PCM_FRAME_BYTES,
  FILE_BLOCK_SIZE: FILE_BLOCK_SIZE,
  SEQ_LAST: SEQ_LAST,
  parseFrame: parseFrame,
  decodePcmToFloat32: decodePcmToFloat32,
  rms: rms
};
