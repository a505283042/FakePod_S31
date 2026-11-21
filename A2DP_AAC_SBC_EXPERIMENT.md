# FakePod S31 AAC-LC + SBC A2DP 收发实验（ESP-IDF 6.1）

## 目的和范围

当前正常分支 `feat/phone-avrcp-cover-art` 使用 **ESP-IDF 内置 SBC**，PCM 以 44.1/48kHz S16 stereo 进入 FakePod 接收环，随后交由 AudioTask 转成 32-bit I2S 并写入 CS43131。AVRCP、封面、频谱、统一音量已与该音频路径解耦。

本分支 **`feat/a2dp-sink-aac-sbc`** 提供可选的编译环境 `fakepod_s31_aac`，使用乐鑫官方 ESP-IDF 6.1 外部解码器 API：
- 将 A2DP SEP0 注册为 AAC-LC（MPEG-2/4；44.1/48kHz；2声道），SEP1 注册为 SBC（必需的后备；44.1/48kHz）。
- Android 手机与 FakePod 自动协商；是否成功选择 AAC 由日志 `A2DP Sink codec=AAC` 确认，不能只看手机开发者设置。
- Bluedroid 音频回调只把 **编码后的** `esp_a2d_audio_buff_t` 交给 24 项有界队列，专门的 Core1 解码任务调用托管组件 `esp_audio_codec 2.6.0` 的 `esp_audio_dec_process()`，处理 AAC/SBC 并释放每个原始包。
- 解码结果直接通过原有的 S16 stereo PCM Ring → AudioTask（内含 110ms 启动后限延迟、微幅时钟校正和频谱）→ I2S → CS43131，既有 AVRCP 及封面无代码改动。
- 使用单独生成的 `sdkconfig.fakepod_s31_aac`，**不会覆盖** 原 `sdkconfig.fakepod_s31_bt_dma`。原有 SBC 环境 `fakepod_s31` 保留。

### 第一阶段已知限制

**AAC 实验环境现在包含 A2DP Source/Sink 双向实现（尚未编译/实机验证）**。ESP-IDF `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y` 禁用了旧 PCM Source 回调；本分支现新增 Core1 AAC/SBC 编码任务，从 AudioTask 的原有 PCM 缓冲取样，经外部 AAC/SBC 编码器和 `esp_a2d_source_audio_data_send()` 发射；手机接收路径仍通过 AAC/SBC 解码任务工作。接收与发射互斥，BLE 遥控 GATT 与音频方向独立；普通 `fakepod_s31` 仍保留旧内置 SBC 发射。

AAC 仅配置 44.1/48kHz、16-bit 解码输出。它不同于 24-bit / 96kHz 高解析蓝牙。音质提升与手机码率、播放源和无线环境有关，需实机试听和测量。

## 推荐构建命令

```powershell
git switch feat/a2dp-sink-aac-sbc
git pull --ff-only

# 先编译专用 AAC+SBC 收发实验固件
pio run -e fakepod_s31_aac

# 用旧版 SBC 固件回归对照（不更改分支）
pio run -e fakepod_s31
```

第一次构建需确保 PlatformIO 使用的实际 ESP-IDF 6.1 含有：
`esp_a2d_sink_register_audio_data_callback`、
`esp_a2d_sink_register_stream_endpoint`、
`CONFIG_BT_A2DP_CODEC_AAC_ENABLED`、
`CONFIG_BT_A2DP_SEP_NUM_MAX`。
检查生成配置 `sdkconfig.fakepod_s31_aac` 是否包含：

```text
CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y
CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y
CONFIG_BT_A2DP_SEP_NUM_MAX=2
```

若 AAC Build 环境使用旧生成配置，删除**仅** `sdkconfig.fakepod_s31_aac` 并重新构建，不要删除运行中其他环境的 sdkconfig。

## 串口检查点

预期：

```text
BT_AAC: A2DP_EXT worker started core=1 queue=24
BT_SINK: A2DP_EXT registering SEP0 AAC-LC=ESP_OK SEP1 SBC=ESP_OK
BT_SINK: A2DP_EXT SEP seid=0 status=0
BT_SINK: A2DP_EXT SEP seid=1 status=0
BT_SINK: A2DP_EXT discoverable AAC=1 SBC=1
BT_SINK: A2DP Sink codec=AAC PCM=44100Hz/S16/stereo
BT_AAC: A2DP_EXT decoder codec=AAC-LC 44100Hz/2ch open=1
BT_SINK: A2DP_EXT codec=AAC open=1 raw=... raw_drop=0 decode_err=0 decoded_frames=...
BT_SINK: A2DP_RX ... drop=... underrun=... start_trim=... smooth_extra=...
AVRCP_COVER request=thumbnail ...
```

**这些是期望格式，不是已完成的真机测试结果。** 手机实际可能选择 SBC，日志将改为 `codec=SBC`，属于合法协商结果。

### 建议依次测试

1. 连接红米 K50 / QQ 音乐，切到手机蓝牙设备开发者设置，查看实际 AAC 选择；听 60 秒，查看 A2DP_EXT 的解码错误、raw_drop、A2DP_RX drop 是否连续增长。
2. 强制 SBC（手机允许时），确认同一外部解码路径仍能输出 SBC，再切换回 AAC。
3. 重复暂停/恢复和断开重连、快速切歌 20 次，检查任务无泄漏、封面不会串歌。
4. AVRCP 控制、单一音量和实时频谱仍能使用；Wi-Fi/TF/NAS功能应回归测试。
5. 观察 Internal RAM、PSRAM 余量、音频解码 Task、缓存队列、长时间播放的声音伪影。若出现静音或有杂音，将 `BT_AAC`、`BT_SINK`、`BT_AVRCP`、`I2S` 日志一起反馈。

### 返回原来稳定固件

无须修改源码，只需在本分支下执行 `pio run -e fakepod_s31` 并烧录该环境产物；或者切回 `feat/phone-avrcp-cover-art`。

## 实现和检验状态

已完成 GitHub 云端源码变更、ESP-IDF 6.1 官方接口/示例的源码交叉核对。**没有在用户 PC 上完成整套固件编译，没有对 ESP32-S31、QQ 音乐、CS43131 做真实 AAC 播放验证**；切勿直接覆盖唯一可工作的固件而不保留回退方式。

参考：ESP-IDF release/v6.1 官方 `examples/bluetooth/bluedroid/classic_bt/a2dp_sink_stream_aac`；`esp_a2dp_api.h`、`components/bt/host/bluedroid/Kconfig.in`。

## 蓝牙设置 / 控件页实际编码显示

本分支现在允许通过 **设置 → 连接 → 蓝牙 → 音频编码** 选择（收发共用）：
- `自动·AAC优先`：优先公布 AAC-LC（SEID 0）和必需的 SBC（SEID 1），手机自主协商 AAC 或 SBC；
- `仅SBC`：两个 SEP 均以 SBC 注册，手机不会与 FakePod 协商 AAC；
- 选择结果持久化到 `device_cfg/btaacpref` NVS。切换时如果 A2DP 正处于接收或发射状态，异步重启该音频 Profile 并重新注册 codec SEP；**音频链路需要重新连接**，BLE GATT 不重启。若仅启用手机遥控，下一次开启音频时生效。

蓝牙设置中的 **音频发射、音频接收两个入口保持不变**，增加「仅手机遥控」（关闭音频 Profile，保留 BLE GATT）。移除「音量同步」显示项，但保留后台 AVRCP 同步。AAC 实验固件现在具有 AAC/SBC 外部编码器的发射代码，需实机验收；原 `fakepod_s31` SBC 发射环境保留。

播放器控件页已移除写死的 `蓝牙接收 · SBC · 44100 Hz`。现在从 `ESP_A2D_AUDIO_CFG_EVT` 捕获双方实际协商的 `mcc.type` 与采样率，更新为：
- `蓝牙接收 · AAC · 44.1 kHz`
- `蓝牙接收 · SBC · 48.0 kHz`
- `蓝牙接收 · 编码协商中`（还没有合法 Audio Config）
- `蓝牙接收 · 等待手机连接`（未连接）

**重要：设置里的「自动·AAC优先」不是“已使用AAC”。以控件页显示及串口 `A2DP Sink codec=` 为准。** 断连时清除上次编码，防止保留过期的 SBC/AAC 标签。

### 代码核验 / 测试建议

1. AAC 环境编译后，在蓝牙设置找到第六行「音频编码」，点选「仅SBC」；手机重新连接，控件页应显示 SBC。
2. 切换到「自动·AAC优先」，重新连接，控件页根据手机协商显示 AAC 或 SBC，不能总显示 SBC。
3. 暂停/恢复不改变显示编码；断开蓝牙应显示等待连接；快速切歌不影响编码标签。
4. 原 `fakepod_s31` 环境第六行显示「音频编码: 固定SBC」，不再显示音量同步设置项，原发射路径保持不变。
5. 在 AAC 实验环境从「仅手机遥控」切换到音频发射，检查 `A2DP_TX SEPs`、`A2DP_TX negotiated codec`、`BT_TX_ENC Encoder running` 与 `A2DP_TX_EXT sent`。分别用 AAC 和 SBC-only 耳机验证编码协商。
6. 在「仅手机遥控」下验证蓝牙音频收发均停止、BLE App 仍可操作本地音乐播放、暂停、切歌和音量。
7. 尚未完成本机 ESP-IDF 6.1 全量编译和真机测试，以上为期望测试结果，非已验证事实。
