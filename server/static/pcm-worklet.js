/*
 * PCM Sink AudioWorkletProcessor
 * 主线程把 16kHz/16bit/mono 的 Float32 采样推入 port，本处理器按音频时钟消费，
 * 实现零丢帧、低延迟的实时播放（无 ScriptProcessorNode）。
 */
class PCMSinkProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    // 环形缓冲：2 秒 @16kHz
    this.capacity = 16000 * 2;
    this.buf = new Float32Array(this.capacity);
    this.write = 0;
    this.read = 0;
    this.size = 0;

    this.port.onmessage = (e) => {
      if (e.data && e.data.buffer) {
        this.__push(e.data);
      }
    };
  }

  __push(data) {
    const len = data.length;
    if (len <= 0) return;
    // 缓冲溢出时丢弃最旧数据，保证实时性
    const drop = Math.max(0, this.size + len - this.capacity);
    if (drop > 0) {
      this.read = (this.read + drop) % this.capacity;
      this.size -= drop;
    }
    for (let i = 0; i < len; i++) {
      this.buf[this.write] = data[i];
      this.write = (this.write + 1) % this.capacity;
      this.size++;
    }
  }

  __next() {
    if (this.size <= 0) return 0;
    const v = this.buf[this.read];
    this.read = (this.read + 1) % this.capacity;
    this.size--;
    return v;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out) return true;
    for (let ch = 0; ch < out.length; ch++) {
      const chan = out[ch];
      for (let i = 0; i < chan.length; i++) {
        chan[i] = this.__next();
      }
    }
    return true;
  }
}

registerProcessor('pcm-sink', PCMSinkProcessor);