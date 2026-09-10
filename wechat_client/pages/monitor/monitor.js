// pages/monitor/monitor.js
const app = getApp();
const proto = require('../../utils/protocol.js');
const { createPcmPlayer } = require('../../utils/pcm-player.js');

function wsUrlFrom(httpBase, deviceId) {
  const b = (httpBase || '').replace(/\/+$/, '');
  const ws = b.replace(/^http:/, 'ws:').replace(/^https:/, 'wss:');
  return ws + '/ws/listen/' + encodeURIComponent(deviceId);
}

Page({
  data: {
    serverBase: app.globalData.serverBase,
    serverInput: app.globalData.serverBase,
    connected: false,
    devices: [],
    selectedDevice: null,
    recordings: [],
    streaming: false,   // 是否正在实时监听
    meterPercent: 0,
    levelDb: 0,
    hint: '',
    playingName: '',
    filePlaying: false
  },

  onLoad() {
    this.reqId = 0;
    this.socket = null;
    this.currentDeviceId = null;
    this.socketOpen = false;
    this.mode = '';            // 'live' | 'file' | ''
    this.pending = {};         // reqId -> resolve/metadata
    this.fileChunks = null;    // 正在归集的录音块
    this.pcm = createPcmPlayer();
    this.innerAudio = null;
    this._pollTimer = null;
    this._meterTimer = null;

    // 自动连接
    this.onConnect();
  },

  onUnload() {
    this.stopAll();
    if (this._pollTimer) clearInterval(this._pollTimer);
    if (this._meterTimer) clearInterval(this._meterTimer);
    this.closeSocket();
  },

  onServerInput(e) {
    this.setData({ serverInput: e.detail.value });
  },

  // ---------- 连接 / 设备列表 ----------
  onConnect() {
    const base = (this.data.serverInput || '').trim().replace(/\/+$/, '');
    if (!base) { wx.showToast({ title: '请输入服务器地址', icon: 'none' }); return; }
    app.globalData.serverBase = base;
    this.setData({ serverBase: base, connected: false, hint: '连接中…' });
    this.fetchDevices();
    if (this._pollTimer) clearInterval(this._pollTimer);
    this._pollTimer = setInterval(() => this.fetchDevices(true), 5000);
  },

  fetchDevices(silent) {
    const base = this.data.serverBase;
    wx.request({
      url: base + '/api/devices',
      method: 'GET',
      success: (res) => {
        const list = (res.data && res.data.devices) || [];
        this.setData({ devices: list, connected: true, hint: '' });
        // 保持选中设备信息同步
        if (this.currentDeviceId) {
          const sel = list.find(d => d.device_id === this.currentDeviceId);
          if (sel) this.setData({ selectedDevice: sel });
        }
      },
      fail: () => {
        this.setData({ connected: false });
        if (!silent) this.setData({ hint: '连接服务器失败，检查地址/网络/域名白名单' });
      }
    });
  },

  // ---------- 选择设备：建立监听 WebSocket ----------
  onSelectDevice(e) {
    const id = e.currentTarget.dataset.id;
    const dev = this.data.devices.find(d => d.device_id === id);
    if (!dev) return;
    if (!dev.online) { wx.showToast({ title: '设备离线', icon: 'none' }); return;
    }
    if (this.currentDeviceId === id) return;
    this.stopAll();
    this.closeSocket();
    this.currentDeviceId = id;
    this.setData({ selectedDevice: dev, recordings: [], hint: '正在建立监听连接…' });
    this.openSocket(id);
  },

  openSocket(deviceId) {
    const url = wsUrlFrom(this.data.serverBase, deviceId);
    const task = wx.connectSocket({
      url: url,
      // 小程序二进制帧
      success: () => {}
    });
    this.socket = task;

    task.onOpen(() => {
      this.socketOpen = true;
      this.setData({ hint: '已连接 ' + deviceId });
      this.loadRecordings();
    });

    task.onMessage((res) => this.handleMessage(res.data));

    task.onError(() => {
      this.socketOpen = false;
      this.setData({ hint: 'WebSocket 错误（开发期请勾选"不校验合法域名"）' });
    });

    task.onClose(() => {
      this.socketOpen = false;
      this.mode = '';
      this.setData({ streaming: false, meterPercent: 0 });
    });
  },

  closeSocket() {
    if (this.socket) {
      try { this.socket.close({}); } catch (e) {}
      this.socket = null;
    }
    this.socketOpen = false;
    this.currentDeviceId = null;
  },

  sendJson(obj) {
    if (this.socket && this.socketOpen) {
      this.socket.send({ data: JSON.stringify(obj) });
      return true;
    }
    return false;
  },

  nextReqId() { this.reqId += 1; return this.reqId; },

  // ---------- 消息处理 ----------
  handleMessage(data) {
    // 文本 JSON
    if (typeof data === 'string') {
      let msg;
      try { msg = JSON.parse(data); } catch (e) { return; }
      this.handleJson(msg);
      return;
    }
    // 二进制帧
    const ab = data instanceof ArrayBuffer ? data : (data.buffer || data);
    const f = proto.parseFrame(ab);
    if (f.type === proto.FRAME_LIVE_AUDIO) {
      if (this.mode === 'live') {
        const f32 = proto.decodePcmToFloat32(f.payload);
        this.pcm.enqueue(f32);
      }
    } else if (f.type === proto.FRAME_FILE_BLOCK) {
      this.handleFileBlock(f);
    }
  },

  setRecordings(files) {
    const list = (files || []).map(f => Object.assign({}, f, {
      sizeKb: (f.size / 1024).toFixed(1)
    }));
    this.setData({ recordings: list });
  },

  handleJson(msg) {
    if (msg.type === 'response') {
      const cb = this.pending[msg.req_id];
      if (cb) {
        delete this.pending[msg.req_id];
        cb.resolve && cb.resolve(msg);
      }
      if (cb && cb.kind === 'list') {
        this.setRecordings(msg.files);
      }
    } else if (msg.type === 'event') {
      if (msg.event === 'recording_saved') {
        this.loadRecordings(true);
      } else if (msg.event === 'stream_state') {
        // 服务端广播的推流状态
        this.setData({ streaming: !!msg.streaming });
      }
    }
  },

  request(cmd, params, kind) {
    return new Promise((resolve) => {
      const reqId = this.nextReqId();
      this.pending[reqId] = { resolve: resolve, kind: kind || '' };
      this.sendJson(Object.assign({ type: 'request', req_id: reqId, cmd: cmd }, params || {}));
      // 超时兜底
      setTimeout(() => {
        if (this.pending[reqId]) { delete this.pending[reqId]; resolve(null); }
      }, 8000);
    });
  },

  // ---------- 实时监听 ----------
  onToggleLive() {
    if (!this.currentDeviceId) { wx.showToast({ title: '请先选择设备', icon: 'none' }); return; }
    if (this.data.streaming) {
      this.stopLive();
    } else {
      this.startLive();
    }
  },

  startLive() {
    this.stopFilePlayback();   // 互斥
    this.mode = 'live';
    this.pcm.resume();
    this.sendJson({ type: 'request', req_id: this.nextReqId(), cmd: 'start_stream' });
    this.setData({ streaming: true, hint: '正在监听实时音频…' });
    this.startMeter();
  },

  stopLive() {
    this.mode = '';
    this.sendJson({ type: 'request', req_id: this.nextReqId(), cmd: 'stop_stream' });
    this.pcm.stop();
    this.setData({ streaming: false, meterPercent: 0, levelDb: 0 });
    this.stopMeter();
  },

  startMeter() {
    this.stopMeter();
    this._meterTimer = setInterval(() => {
      const lvl = this.pcm.getLevel();
      const pct = Math.round(Math.min(1, lvl * 3) * 100);
      const db = lvl > 0.0005 ? Math.round(20 * Math.log10(lvl)) : -60;
      this.setData({ meterPercent: pct, levelDb: db });
    }, 100);
  },
  stopMeter() {
    if (this._meterTimer) { clearInterval(this._meterTimer); this._meterTimer = null; }
  },

  // ---------- 录音列表 / 回放 ----------
  onLoadRecordings() {
    if (this.currentDeviceId) this.loadRecordings();
  },

  loadRecordings(silent) {
    if (!this.socketOpen) return;
    const reqId = this.nextReqId();
    this.pending[reqId] = {
      kind: 'list',
      resolve: (msg) => {
        this.setRecordings(msg && msg.files);
      }
    };
    this.sendJson({ type: 'request', req_id: reqId, cmd: 'list_recordings' });
    if (!silent) this.setData({ hint: '加载录音列表…' });
  },

  onPlayRecording(e) {
    const name = e.currentTarget.dataset.name;
    if (this.data.playingName === name) {
      this.stopFilePlayback();
      return;
    }
    this.stopLive();          // 互斥
    this.requestFile(name);
  },

  requestFile(name) {
    this.fileChunks = { name: name, parts: [] };
    this.setData({ playingName: name, filePlaying: false, hint: '下载录音…' });
    const reqId = this.nextReqId();
    this.fileReqId = reqId;
    this.sendJson({ type: 'request', req_id: reqId, cmd: 'play_file', file: name });
  },

  handleFileBlock(f) {
    if (!this.fileChunks || f.reqId !== this.fileReqId) return;
    // 归集 payload
    const u8 = new Uint8Array(f.payload);
    this.fileChunks.parts.push(u8);
    if (f.seq === proto.SEQ_LAST) {
      this.finishFile();
    }
  },

  finishFile() {
    const meta = this.fileChunks;
    this.fileChunks = null;
    let total = 0;
    meta.parts.forEach(p => { total += p.length; });
    const merged = new Uint8Array(total);
    let off = 0;
    meta.parts.forEach(p => { merged.set(p, off); off += p.length; });

    const fs = wx.getFileSystemManager();
    const filePath = `${wx.env.USER_DATA_PATH}/rec_${Date.now()}.opus`;
    fs.writeFile({
      filePath: filePath,
      data: merged.buffer,
      success: () => this.playLocalFile(filePath, meta.name),
      fail: () => this.setData({ hint: '录音写入失败', playingName: '' })
    });
  },

  playLocalFile(filePath, name) {
    this.stopInnerAudio();
    const audio = wx.createInnerAudioContext();
    audio.src = filePath;
    audio.autoplay = true;
    this.innerAudio = audio;
    this.setData({ filePlaying: true, hint: '正在回放录音…' });

    audio.onEnded(() => { this.setData({ playingName: '', filePlaying: false, hint: '' }); });
    audio.onError((err) => {
      this.setData({ playingName: '', filePlaying: false,
        hint: '该设备 InnerAudioContext 可能不支持 Ogg/Opus 播放（实时监听不受影响）' });
      wx.showToast({ title: '格式可能不支持', icon: 'none' });
    });
  },

  stopFilePlayback() {
    if (this.fileReqId) {
      this.sendJson({ type: 'request', req_id: this.nextReqId(), cmd: 'stop_file', req_ref: this.fileReqId });
    }
    this.fileChunks = null;
    this.stopInnerAudio();
    this.setData({ playingName: '', filePlaying: false });
  },

  stopInnerAudio() {
    if (this.innerAudio) {
      try { this.innerAudio.stop(); this.innerAudio.destroy(); } catch (e) {}
      this.innerAudio = null;
    }
  },

  // ---------- 录音开关 ----------
  onToggleRec() {
    const dev = this.data.selectedDevice;
    if (!dev) return;
    const target = !dev.recording_enabled;
    this.request('set_recording', { enabled: target }).then((msg) => {
      if (msg && msg.ok) {
        wx.showToast({ title: target ? '录音已开启' : '录音已关闭', icon: 'none' });
        setTimeout(() => this.fetchDevices(true), 500);
      }
    });
  },

  stopAll() {
    this.stopLive();
    this.stopFilePlayback();
  }
});
