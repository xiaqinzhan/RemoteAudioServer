# 安卓设备模拟器（android_device_sim）

一个 **Android 采集端模拟器**，扮演真实 ESP32 设备：通过 WebSocket 连服务器 `/ws/device/{id}`，
完整走设备协议（hello / ping / start_stream / stop_stream / 录音列表 / 文件回放 / 录音开关）。
连上后，在网页 `/`、`/listen` 或微信小程序里就能看到这台“设备”，可实时监听、回放录音。

> 与 `android_client`（监听端）的区别：本工程是**设备端（采集端）**，主动推音频给服务器；
> `android_client` 是**监听端**，接收并播放。服务端 `remoteAudioServer` 无需任何改动。

## 功能

- 连服务器：`/ws/device/{id}`，进入即发 `hello`，每 15s 发 `ping`。
- 实时推流（`0x01` PCM 帧，16kHz/16bit/单声道，640B/20ms）：
  - **合成蜂鸣**：正弦提示音（内置节拍包络）；
  - **手机麦克风**：用 `AudioRecord` 采集真实声音推送（更接近真实采集设备）。
  - 收到服务端 `{"cmd":"start_stream"}` 开始、`{"cmd":"stop_stream"}` 停止。
- 录音相关：
  - `list_recordings`：返回预置虚拟录音列表；
  - `play_file`：把内置的 Ogg/Opus 资产按 `0x02` 文件块（2048B/块，末块 `seq=0xFFFF`）回传；
  - `stop_file`：取消发送；`set_recording`：切换录音开关；`set_config`：更新参数。
- 每约 20~45s 发一个 `recording_saved` 事件，监听端列表会自动出现新录音。
- UI：服务器地址、设备 ID、音源切换（蜂鸣/麦克风）、连接状态灯、推流状态、运行日志。

## 用什么构建

- Android Studio（任意较新版本），Gradle 8.7 / AGP 8.5.2，minSdk 24，Java 17。
- 依赖：OkHttp（WebSocket）、AppCompat、Material。

## 运行步骤

1. Android Studio → **Open** 选择 `android_device_sim/` 目录，等待 Gradle 同步。
2. 真机开 **USB 调试**（或建 API 24+ 模拟器），点 ▸ **Run**。
3. App 内：
   - 服务器地址默认填好云端 `https://ad143af7-…dev.coze.site`（也可改局域网 `http://192.168.x.x:8000`）；
   - 设备 ID 填一个不和云端内置 `sim-101/102` 冲突的值，例如 `dev-android-001`；
   - 音源选「合成蜂鸣」或「手机麦克风」（选麦克风会请求录音权限）；
   - 点「连接服务器」，状态灯变绿、日志出现“已连接”。
4. 打开监听端（网页 `/` 或 `/listen`、微信小程序、或 `android_client`），找到这台设备：
   - 点「开始监听」：蜂鸣模式听到提示音；麦克风模式听到手机周围声音；
   - 展开录音列表可回放内置录音。

## 目录结构

```
android_device_sim/
├── settings.gradle / build.gradle / gradle.properties
└── app/
    ├── build.gradle
    └── src/main/
        ├── AndroidManifest.xml          # 网络 + 麦克风权限，允许明文流量
        ├── assets/sample.opus            # play_file 回传用的内置录音内容
        ├── res/ ...                      # 深色科技风 UI 资源
        └── java/com/example/audiodevice/
            ├── MainActivity.java         # UI 编排、权限、日志
            ├── DeviceSession.java        # WebSocket 客户端：hello/ping/命令/文件块/推流
            ├── Proto.java                # 二进制帧协议常量与编码（与 server/protocol.py 对应）
            ├── AudioSource.java          # 音源接口
            ├── ToneSource.java           # 合成蜂鸣 PCM
            └── MicSource.java            # AudioRecord 麦克风采集
```

帧格式：`Byte0=type | Byte1-4=req_id(BE) | Byte5-6=seq(BE,0xFFFF=末块) | Byte7+ payload`。

## 备注

- 选「手机麦克风」需授予录音权限；未授权会自动回退为蜂鸣。
- App 无鉴权、面向演示/内网；正式对外请在服务端加鉴权与 HTTPS。
- 设备 ID 建议与云端内置模拟器错开，避免列表里出现同名设备混淆。
