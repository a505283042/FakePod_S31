#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// A2DP Source 与手机 BLE GATT 共享 Bluedroid Host/Controller。
// 此服务绝不独立初始化/销毁 Bluetooth Host；生命周期由 ble_remote_service 统一管理。
struct BtAudioSourceSnapshot {
    bool available = false;
    bool initialized = false;
    bool scanning = false;
    bool candidate_found = false;
    bool connecting = false;
    bool connected = false;
    bool streaming = false;
    uint32_t sample_rate_hz = 44100U;
    uint32_t underrun_frames = 0U;
    uint32_t dropped_frames = 0U;
    char peer_name[48] = {};
};

// 调用条件：Bluedroid 已 enable；停止须早于 Bluedroid disable。
esp_err_t bt_audio_source_host_start();
void bt_audio_source_host_stop();

// 只从非蓝牙回调任务触发扫描和连接；不影响 GATT 手机遥控会话。
// 扫描 5.12 秒后保存信号最强的已发现音频接收设备，须再次点击连接。
esp_err_t bt_audio_source_scan();
esp_err_t bt_audio_source_connect_candidate();
esp_err_t bt_audio_source_disconnect();
void bt_audio_source_update();
bool bt_audio_source_get_snapshot(BtAudioSourceSnapshot *out);

// AudioTask 非阻塞 PCM 旁路：32-bit 左对齐、交错立体声，内部转 S16 及重采样。
// 播放主路径始终以 I2S/CS43131 为准，蓝牙慢时只丢弃蓝牙环形缓存旧帧。
void bt_audio_source_feed_pcm32(const int32_t *pcm, size_t frames,
                                uint32_t source_rate_hz, uint8_t volume_percent);
void bt_audio_source_flush();
