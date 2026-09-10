// utils/pcm-player.js
// 基于 wx.createWebAudioContext() 的实时 PCM 流式播放器
// 设备推流为 16kHz/16bit/单声道；播放队列通过连续调度 AudioBuffer 实现低延迟播放。

const SAMPLE_RATE = 16000;

function createPcmPlayer() {
  let ctx = null;
  let nextTime = 0;     // 下一个 buffer 计划开始播放的时间（AudioContext 时间轴）
  let started = false;
  let queued = 0;       // 已排队但未播完的采样数
  let level = 0;        // 最近电平 0~1
  const MAX_QUEUED_MS = 600; // 超过则丢弃旧数据，避免延迟累积

  function ensureCtx() {
    if (!ctx) {
      ctx = wx.createWebAudioContext();
    }
    if (ctx.state === 'suspended') {
      ctx.resume();
    }
    return ctx;
  }

  // f32: Float32Array, [-1,1]，采样率 SAMPLE_RATE
  function enqueue(f32) {
    const c = ensureCtx();
    // 更新电平（简单平滑）
    let sum = 0;
    for (let i = 0; i < f32.length; i++) { const v = f32[i]; sum += v * v; }
    const r = Math.sqrt(sum / Math.max(1, f32.length));
    level = level * 0.6 + r * 0.4;

    const samples = f32.length;
    // 防延迟累积：排队太多就丢
    if (queued + samples > (SAMPLE_RATE * MAX_QUEUED_MS) / 1000) {
      return;
    }

    const buffer = c.createBuffer(1, samples, SAMPLE_RATE);
    const ch = buffer.getChannelData(0);
    ch.set(f32);

    const src = c.createBufferSource();
    src.buffer = buffer;
    src.connect(c.destination);

    const now = c.currentTime;
    if (!started || nextTime < now) {
      nextTime = now;
      started = true;
    }
    src.start(nextTime);
    nextTime += samples / SAMPLE_RATE;
    queued += samples;

    // 播完后扣减排队计数
    src.onended = function () {
      queued = Math.max(0, queued - samples);
    };
  }

  function getLevel() {
    // 自然衰减
    level *= 0.85;
    return Math.min(1, level);
  }

  function stop() {
    started = false;
    nextTime = 0;
    queued = 0;
    level = 0;
    if (ctx) {
      try { ctx.suspend(); } catch (e) {}
    }
  }

  function resume() {
    if (ctx && ctx.state === 'suspended') { ctx.resume(); }
    started = true;
  }

  return { enqueue: enqueue, getLevel: getLevel, stop: stop, resume: resume };
}

module.exports = { createPcmPlayer: createPcmPlayer, SAMPLE_RATE: SAMPLE_RATE };
