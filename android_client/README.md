# AudioMonitor（Android 监听客户端）

用 **Java + 原生 Android** 实现的多设备音频采集「监听端」App，功能与 Web 监听页（`/listen`）一致：
连到 `remoteAudioServer` 服务端后，**实时监听**设备推流（AudioTrack 播放 16kHz PCM）、**浏览并回放历史录音**
（Ogg Opus）、**远程切换录音开关**。服务端无需任何改动。

默认内置了云端服务器地址，打开 App 点「连接」即可使用；也可改成局域网地址。

---

## 1. 功能

- 服务器地址可配置（云端 `https://…` 走 wss，局域网 `http://192.168.x.x:8000` 走 ws，已允许明文流量）。
- 设备列表：在线状态灯、设备ID、固件版本、监听人数、录音开关、SD 卡状态；每 5 秒自动刷新。
- 实时监听：与设备建立 `/ws/listen/{id}` WebSocket，收到 `0x01` 实时帧（16kHz/16bit/单声道 PCM）
  用 **AudioTrack** 流式播放，并显示实时音量电平条。
- 历史录音：`list_recordings` 拉取列表，点击条目发 `play_file`，接收 `0x02` 文件块按 `req_id` 归集，
  拼成完整 Ogg Opus 文件后用 **MediaPlayer** 播放；再点一次停止（发 `stop_file`）。
- 录音开关：`set_recording` 远程切换设备录音使能。
- 单一音源：实时监听与录音回放互斥（与 Web 端约定一致）。

## 2. 目录结构

```
android_client/
├── settings.gradle / build.gradle / gradle.properties
├── gradle/wrapper/gradle-wrapper.properties
└── app/
    ├── build.gradle
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/com/example/audiomonitor/
        │   ├── MainActivity.java     # 界面编排、连接、轮询、音源切换
        │   ├── Proto.java            # 二进制帧协议（与 server/protocol.py 一致）
        │   ├── ApiClient.java        # REST：GET /api/devices；URL 拼装
        │   ├── ListenSession.java    # WebSocket 监听会话（JSON 请求 + 二进制帧归集）
        │   ├── PcmPlayer.java        # AudioTrack 播放实时 PCM + 电平计算
        │   ├── OpusPlayer.java       # MediaPlayer 回放 Ogg Opus 录音
        │   ├── DeviceInfo.java / Recording.java
        │   └── DeviceAdapter.java / RecordingAdapter.java
        └── res/                      # 深色科技风主题、布局、图标
```

## 3. 用 Android Studio 构建 & 安装

> 沙箱内没有 Android 构建链，无法在此产出 APK；以下在你本机完成。

1. 安装 **Android Studio**（自带 Android SDK，建议 SDK Platform 34）。
2. `File → Open` 选择本目录 `android_client/`，等待 Gradle 同步（首次会自动下载
   Gradle 8.7、AGP 8.5.2 与依赖 OkHttp / AppCompat / Material / RecyclerView）。
3. 真机打开「开发者选项 → USB 调试」并连接电脑（或创建模拟器，API 24+）。
4. 点 ▶ Run，或命令行：
   ```bash
   cd android_client
   ./gradlew installDebug      # Windows: gradlew.bat installDebug
   ```

## 4. 连接云端服务器

App 启动后服务器地址栏已默认填好云端地址：

```
https://ad143af7-ddd7-4630-b71b-ffe68ff5a6bf.dev.coze.site
```

点「连接」→ 设备列表出现设备 → 点选一台在线设备 → 「开始监听」即可听到声音；
下方「历史录音」点条目可回放。

连**本地/局域网服务端**时把地址改成：

```
http://192.168.x.x:8000
```

> - 云端内置的 `sim-101/sim-102` 可直接监听；想让本手机听到你电脑上的独立模拟器，
>   先在电脑用 `simulator_standalone/sim_device.py --url <云端地址> --device-ids sim-103 …` 连上，
>   再在 App 里选 `sim-103`。
> - 实时监听声音由「设备端」决定：独立模拟器默认是合成蜂鸣，加 `--live-file xxx.mp3`
>   可推本地真实音频。

## 5. 与服务端的协议对应

| App 行为 | 通道 / 报文 |
|----------|-------------|
| 拉设备列表 | `GET /api/devices` |
| 监听会话 | WebSocket `/ws/listen/{device_id}` |
| 实时音频 | 二进制 `0x01`，payload=640B 16k/16bit/mono PCM |
| 录音文件块 | 二进制 `0x02`，Byte1-4 `req_id`，Byte5-6 `seq`（0xFFFF=末块） |
| 请求/响应/事件 | JSON：`request` / `response` / `event` |

帧格式与 `server/protocol.py` 完全一致：`Byte0 type | Byte1-4 req_id(大端) | Byte5-6 seq | Byte7+ payload`。

## 6. 备注

- `minSdk 24`（Android 7.0+），`targetSdk/compileSdk 34`。
- 录音回放依赖系统 **MediaPlayer 的 Ogg/Opus 解码能力**（Android 7+ 大多支持）；
  若个别设备提示不支持，实时监听不受影响。
- App 无鉴权、面向演示/内网，与服务端定位一致。
