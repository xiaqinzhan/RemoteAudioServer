# 微信小程序监听端（wechat_client）

把「多设备音频采集系统」的监听端做成**微信小程序**，功能与 Web 监听页（`/listen`）一致：
**实时监听**设备推流（WebAudio 流式播放 16kHz PCM）、**浏览并回放历史录音**、**远程切换录音开关**。
服务端无需任何改动。

> 为什么不用微信内 H5：微信内置浏览器对 AudioWorklet / Ogg-Opus 支持很差，实时音频基本不可用；
> 小程序提供 `wx.connectSocket`（二进制帧）+ `wx.createWebAudioContext()`（PCM 流式播放），是微信内最稳的方案。

---

## 1. 功能

- 服务器地址可配置：默认云端 `https://…dev.coze.site`（自动走 wss）；局域网填 `http://192.168.x.x:8000`。
- 设备列表：`GET /api/devices`，在线灯/设备ID/固件/监听人数/录音开关/SD 状态，5 秒自动刷新。
- 实时监听：连 `/ws/listen/{id}`，`0x01` 实时帧（16k/16bit/mono PCM）经 **WebAudio AudioBuffer 队列**
  连续调度播放，带实时音量电平条（dB）。
- 历史录音：`list_recordings` 列表 → 点条目发 `play_file` → `0x02` 文件块按 `req_id` 归集、末块拼成
  Ogg Opus → 写入本地临时文件 → **InnerAudioContext** 播放；再点一次停止（发 `stop_file`）。
- 录音开关：`set_recording` 远程切换。
- 单一音源：实时监听与录音回放互斥。

## 2. 目录结构

```
wechat_client/
├── app.js / app.json / app.wxss      # 全局逻辑、页面注册、深色科技风主题
├── project.config.json               # 工程配置（AppID 占位、已关 urlCheck 便于调试）
├── sitemap.json
└── pages/monitor/
    ├── monitor.wxml / .wxss / .js / .json
└── utils/
    ├── protocol.js                   # 二进制帧解析 + PCM 解码 + RMS（与 server/protocol.py 一致）
    └── pcm-player.js                 # WebAudio 实时 PCM 流式播放器（队列调度 + 防延迟累积）
```

帧格式：`Byte0 type | Byte1-4 req_id(大端) | Byte5-6 seq(0xFFFF=末块) | Byte7+ payload`。

## 3. 导入与运行（微信开发者工具）

1. 下载并打开 **微信开发者工具**（稳定版即可）。
2. 「导入项目」→ 目录选择本 `wechat_client/`。
3. AppID：
   - 没有正式 AppID 时，选「**测试号**」（或用工具默认的测试 AppID）即可预览/真机调试。
   - 有正式小程序 AppID，在 `project.config.json` 把 `appid` 改成你的 AppID。
4. 开发期：右上角「详情 → 本地设置」勾选 **「不校验合法域名、web-view…」**（`project.config.json`
   里已设 `urlCheck:false`），即可直连云端/局域网。
5. 点「编译」预览；真机调试点「预览」扫码，在手机微信里打开。

> 服务器地址在页面顶部输入框，默认已填云端地址，点「连接」即可。

## 4. 正式发布前的域名白名单（必做）

微信小程序正式版**只允许**连接在公众平台配置过的合法域名（必须是 **HTTPS / WSS**、已备案）：

登录 [微信公众平台](https://mp.weixin.qq.com/) → 开发管理 → 开发设置 → 服务器域名：

- **request 合法域名**：`https://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site`
- **socket 合法域名**：`wss://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site`

（换成你自己的正式部署域名时，两处都改。局域网 `ws://` / `http://` 仅开发期勾选「不校验域名」可用，
正式版无法使用明文地址。）

## 5. 已知限制 / 说明

- **实时监听**用 WebAudio 播放 PCM，兼容性好，是本小程序的核心能力。
- **录音回放**依赖微信 `InnerAudioContext` 对 Ogg/Opus 的解码；少数 iOS 机型可能不支持，
  届时会弹「格式可能不支持」提示——**不影响实时监听**。如需在所有机型稳放录音，可让服务端/设备端
  额外输出 AAC/MP3 格式（属于后续增强）。
- `wx.createWebAudioContext()` 需要较新基础库（工程 `libVersion` 设为 2.25.4）；极旧版本微信建议升级。
- 小程序无鉴权，面向演示/内网，与服务端定位一致。

## 6. 连云端后听到什么

- 云端内置 `sim-101/sim-102`：实时监听是合成蜂鸣；点历史录音可回放。
- 想听到真实音频：在电脑运行独立模拟器并推本地音频，再在小程序选对应设备：
  ```bash
  python3 sim_device.py --url https://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site \
      --device-ids sim-103 --live-file ./讲话.mp3
  ```
