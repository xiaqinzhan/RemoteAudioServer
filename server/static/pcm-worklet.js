/*
 * PCM Sink AudioWorkletProcessor（自适应采样率重采样）
 * 主线程把 16kHz/16bit/mono 的 Float32 采样推入 port，本处理器按音频时钟
 * 消费并插值重采样为 AudioContext 的采样率（可能为 16000/44100/48000 等），
 * 实现低延迟实时播放（无 ScriptProcessorNode）。
 */
class PCMSinkProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.SRC = 16000;                 // 来源采样率（采集设备固定 16kHz）
    this.ratio = sampleRate / this.SRC; // 每 1 个源样本对应多少个输出样本
    this.src = [];                    // 积压的源样本（小数组，随手写指针旁路，无 GC 压力）
    this.pos = 0;                     // 虚拟采样位置（浮点），范围保持在 [0,1)

    this.port.onmessage = (e) => {
      if (e.data && e.data.length) this.__push(e.data);
    };
  }

  __push(arr) {
    for (let i = 0; i < arr.length; i++) this.src.push(arr[i]);
    // 内存保护：积压超过 10 秒时丢弃最旧数据，保证实时性
    const max = this.SRC * 10;
    if (this.src.length > max) this.src.splice(0, this.src.length - max);
  }

  // 在位置 p 处线性插值取源样本；越界返回 0（缓冲欠载时输出静音）
  __readAt(p) {
    const i = Math.floor(p);
    if (i < 0) return 0;
    if (i + 1 < this.src.length) {
      const f = p - i;
      return this.src[i] * (1 - f) + this.src[i + 1] * f;
    }
    if (i < this.src.length) return this.src[i];
    return 0;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out) return true;
    const ch0 = out[0];
    const block = ch0.length;
    const step = 1 / this.ratio;      // 每输出 1 个样本消耗 step 个源样本
    // 多声道做同样处理
    for (let ch = 0; ch < out.length; ch++) {
      const chan = out[ch];
      let p = this.pos;
      for (let i = 0; i < block; i++) {
        chan[i] = this.__readAt(p);
        p += step;
      }
    }
    // 按消耗量丢弃已播放的源样本，pos 只保留小数部分，保持有界
    const total = Math.floor(this.pos + block * step);
    if (total > 0) {
      this.src.splice(0, total);
      this.pos = this.pos + block * step - total;
    } else {
      this.pos += block * step;
    }
    return true;
  }
}

registerProcessor('pcm-sink', PCMSinkProcessor);