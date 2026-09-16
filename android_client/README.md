# AudioMonitor（Android 监听客户端）

用 **Java + 原生 Android** 实现的多设备音频采集「监听端」App，功能与 Web 监听页（`/listen`）一致：
连到 `remoteAudioServer` 服务端后，**实时监听**设备推流（AudioTrack 播放 16kHz PCM）、**浏览并回放历史录音**
（Ogg Opus）、**远程切换录音开关**。

监听链路按用途拆成两条 WebSocket：常驻的 `role=control`（录音列表 / 回放 / 录音开关 / 事件）与
按需的 `role=live&sid=`（点「开始监听」才建立、才计入服务端监听人数）。
这样「只是打开 App 选中设备」不会触发设备推流；`sid` 让重连替换旧连接、不虚增人数。
需要服务端支持 `role`/`sid` 查询参数（`server/hub.py` 已实现；旧服务端会忽略这两个参数、行为退化为旧版）。

默认内置了云端服务器地址，打开 App 点「连接」即可使用；也可改成局域网地址。

---

## 1. 功能

- 服务器地址可配置（云端 `https://…` 走 wss，局域网 `http://192.168.x.x:8000` 走 ws，已允许明文流量）。
- 设备列表：在线状态灯、设备ID、固件版本、监听人数、录音开关、SD 卡状态；每 5 秒自动刷新。
- 控制会话（`role=control`，选中设备即建立、常驻）：`list_recordings` 拉列表、`play_file` / `stop_file` 回放、
  `set_recording` 切换录音开关，并接收 `stream_state` / `recording_saved` 事件。**不计监听人数、不推实时 PCM。**
- 实时监听（`role=live&sid=`，仅「开始监听」期间存在）：收到 `0x01` 实时帧（16kHz/16bit/单声道 PCM）
  用 **AudioTrack** 流式播放，并显示实时音量电平条；连接建立即让服务端 0→1 开流，停止时 1→0 延迟 10s 停流。
- 历史录音：`play_file` 后接收 `0x02` 文件块按 `req_id` 归集，拼成完整 Ogg Opus 文件后用 **MediaPlayer** 播放；
  再点一次停止（发 `stop_file`）。
- 录音开关：`set_recording` 远程切换设备录音使能。
- 单一音源：实时监听与录音回放互斥（与 Web 端约定一致；开始监听会停回放，回放开始会停监听）。

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
        │   ├── MainActivity.java     # 界面编排、连接、轮询、音源切换、控制/实时双会话
        │   ├── Proto.java            # 二进制帧协议（与 server/protocol.py 一致）
        │   ├── ApiClient.java        # REST：GET /api/devices；监听 URL 拼装（role/sid）与会话 id
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

> 仓库里只有 `gradle/wrapper/gradle-wrapper.properties`，没有 `gradlew` / `gradlew.bat` / `gradle-wrapper.jar`
> （沙箱内无法生成二进制）。用命令行构建时先在项目目录生成一次 wrapper：
>
> ```bash
> gradle wrapper --gradle-version 8.7   # 需要本机已装 Gradle 8.7+
> ./gradlew installDebug                # 之后即可用 wrapper 构建
> ```
>
> 用 Android Studio 打开则会自动同步，可忽略这一步。

### 常见报错：`Plugin [id: 'com.android.application', version: '8.5.2'] was not found …`

这不是本项目代码问题，而是 **Gradle 没能取到 AGP 的 plugin marker**。该 marker 只发布在 **Google Maven**
（`https://dl.google.com/dl/android/maven2/`，路径
`com/android/application/com.android.application.gradle.plugin/8.5.2/…pom`，可正常访问时返回 200），
Maven Central 与 Gradle Plugin Portal 里都没有。所以「搜过 Google / MavenRepo / Gradle Central Plugin Repository 但没找到」
通常意味着 Google Maven 实际没取到。按下面顺序排查：

1. **全局镜像脚本改写了仓库 URL**（最常见）：检查 `~/.gradle/init.gradle`、`~/.gradle/init.d/*.gradle`
   （Windows：`C:\Users\<你>\.gradle\init.gradle`）。若它把 `google()` 换成了缺 AGP 的第三方镜像，
   报错里的仓库名仍显示 `Google`，但拿到的其实是空结果——临时改名/删除该脚本再同步。
2. **网络直连不了 `dl.google.com`**：本项目 `settings.gradle` 已把**阿里云镜像放在官方源之前**
   （`gradle-plugin` / `google` / `public`），同步即可命中；`gradle/wrapper/gradle-wrapper.properties`
   的 `distributionUrl` 也已改为腾讯云镜像下载 Gradle 8.7。
3. **Gradle 版本过旧或开了离线模式**：`gradle --version` 需 **≥ 8.7**（AGP 8.5.2 的最低要求）；
   并确认 IDE 里没有勾选 *Offline work*（`--offline` 时未缓存的插件一律报「not found」）。

## 4. 连接云端服务器

App 启动后服务器地址栏已默认填好云端地址：

```
https://remoteaudio.coze.site
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
| 控制会话（常驻） | WebSocket `/ws/listen/{device_id}?role=control`：只收发录音列表 / 回放 / 录音开关 / 事件，**不计监听人数、不推实时 PCM** |
| 实时监听（按需） | WebSocket `/ws/listen/{device_id}?role=live&sid={客户端会话id}`：点「开始监听」才建立，计入人数并接收实时 PCM |
| 实时音频 | 二进制 `0x01`，payload=640B 16k/16bit/mono PCM |
| 录音文件块 | 二进制 `0x02`，Byte1-4 `req_id`，Byte5-6 `seq`（0xFFFF=末块） |
| 请求/响应/事件 | JSON：`request` / `response` / `event` |

`role` / `sid` 语义（与服务端 `server/hub.py` 一致）：

- `role=live`（缺省）：计入 `listener_count`，参与 0→1 开流 / 1→0 延迟停流判定，接收实时 PCM；
- `role=control`：只用于控制类消息，不计人数、不推 PCM，但其自身请求的响应与 `0x02` 文件块照常按 `req_id` 直投（故录音列表与回放不受影响）；
- `sid`：本文档代号存于 `SharedPreferences["audio_monitor"].client_sid`（首次用 `UUID` 生成后持久化），服务端按 `(device_id, sid)` 去重——同一会话重连会替换旧连接，避免残留条目虚增人数、或让真实监听者拿不到开流指令。

因此，**仅打开 App 选中设备不会触发设备推流**；只有点「开始监听」才会计入监听人数并让设备开始推流，停止监听即释放（服务端 1→0 延迟 10s 再停流）。

帧格式与 `server/protocol.py` 完全一致：`Byte0 type | Byte1-4 req_id(大端) | Byte5-6 seq | Byte7+ payload`。

## 6. 备注

- `minSdk 24`（Android 7.0+），`targetSdk/compileSdk 34`。
- 录音回放依赖系统 **MediaPlayer 的 Ogg/Opus 解码能力**（Android 7+ 大多支持）；
  若个别设备提示不支持，实时监听不受影响。
- App 无鉴权、面向演示/内网，与服务端定位一致。
