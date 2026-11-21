# FakePod ESP32-S31：纯 AVRCP 封面 + A2DP 音频低延迟（2026-11-21）

## 本次目标

红米 K50（HyperOS 2 / QQ 音乐）已通过 AVRCP 1.6 成功下载专辑封面，但手机的 GetImageProperties 连续显示仅支持 `<native encoding="JPEG" pixel="200*200"/>`，没有高清变体。停止为这台手机逐曲查询图片属性并尝试 GetImage，直接获取标准 Linked Thumbnail。

Android Remote 的 BLE 封面指令 0x40、0x41、0x42 在固件侧被拒绝。**Remote 的普通 20-byte GATT 控制、Wi-Fi 配网、BLE 手柄等不受影响。** 用户如果仍安装了历史版 Android Remote 的后台封面服务，需要在 App 设置中关掉封面上传，或停止/卸载其后台服务，否则旧 App 仍可能尝试上传，固件拒绝并不意味着手机端停止发送。

## A2DP 音频修改

原始日志（44.1kHz）：
- 19:13:40 启动 PCM，queue 0/8192、drop 0；
- 19:13:50 queue 7680/8192、drop 1536；
- 19:14:00 queue 6400/8192、drop 1920。
`drop` 是 **PCM 环形缓冲区满时丢弃的帧数**，不是 HCI 丢包数。

本版:
1. 启动期 PSRAM 接收环从 8192 帧（32KiB、约186ms）增至 16384 帧（64KiB、约372ms），覆盖 DAC/I2S 初始化时的短时积压。
2. DAC/I2S 初始化完成后，主动保留最新约 110ms 的音频，丢弃此前过期缓存，避免长时间额外延迟；这项主动操作计入 `start_trim`，**不计入溢出 `drop`**。
3. 播放时正常每 256 帧输出 256 帧。队列超过 110ms 时用线性插值输出 256 帧、消耗 257 帧，约 0.39% 微幅校正；超过 160ms 则消耗 258 帧（约 0.78%），追上略快的手机 SBC 时钟。只改变 BT Sink PCM 读取，DAC/I2S 采样率和其他音乐播放链路不变。
4. 所有插值计算在 AudioTask、BT ring 自旋锁之外完成。回调只 memcpy PCM，严禁在回调内进行 I2S 或解码/图像处理。
5. 将 `rx` / `played_src` / `drop` / `underrun` / `start_trim` / `smooth_extra` / `peak` 分别记录于每 10 秒的 A2DP_RX 行。

注：启动缩短延迟会跳过尚未送至耳机的过期音频，属于可见的取舍。由于蓝牙 SBC 与 I2S 的实际时钟精度尚未测量，线性插值是低成本微调方案，不等于专业无变调 time-stretch；如果用户听见音高或接缝失真，应先关闭此校正，再研究精确时钟反馈。

## 构建与测试

```powershell
git switch feat/phone-avrcp-cover-art
git pull --ff-only
# 按原有平台配置运行 idf.py / PlatformIO 编译烧录
```

独立于 ESP-IDF 的平滑校正数学测试（需 g++）：
```bash
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/bt_pcm_drift_smoke.cpp -o bt_pcm_drift_smoke
./bt_pcm_drift_smoke
```

**实机测试顺序：**
1. 手机 AVRCP 开发者设置保持 1.6；停用 Android Remote 封面后台服务。
2. 连接 FakePod，正常打开 QQ 音乐播放，不连续切歌 60 秒。
3. 确认串口包含 `AVRCP_COVER request=thumbnail` → `thumbnail completed ok=1` → `surface requested`；不再有 `request=properties` 或 `request=native`。
4. 看 `BT_SINK_OUTPUT startup trim=...` 和持续 `A2DP_RX ... drop= ...`。期待启动后 `drop` 不再增加；`smooth_extra` 在队列高于 110ms 时可能逐渐增加。
5. 连续切歌 10–20 次，确认旧封面 stale 数据不覆盖新歌；对某首经常缺封面的歌曲，观察图片最多 3 次请求/超时恢复。
6. 暂停恢复、断开重连、切换音频输出模式，确认没有卡顿、崩溃、缓冲无限增长。
7. 如果 `drop` 仍不断增长，按时间一起提供 `A2DP_RX`、`PCM_FEED_PERF`、`BT_SINK_OUTPUT`。特别检查 I2S write timeout 或 AudioTask 被其他任务长时间阻塞。

## 预期日志示例（不是实机已验证数据）

```text
BT_SINK_OUTPUT startup trim=... frames (...ms) target=110ms
AVRCP_COVER request=thumbnail err=ESP_OK max_bytes=196608 retry=0
AVRCP_COVER thumbnail completed ok=1 bytes=... stale=0 status=0
A2DP_RX rate=44100Hz queued=.../16384 peak=... rx=... played_src=... drop=0 underrun=... start_trim=... smooth_extra=...
```

**状态：** 云端提交后需在用户本地 ESP-IDF 环境编译并用红米 K50 实机验证。不要将预期日志当作结果。原先高清测试文件已不适用于本版纯缩略图方案。
