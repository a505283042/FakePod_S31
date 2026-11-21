#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// A2DP Source 与手机 BLE GATT 共享 Bluedroid Host/Controller。
// 此服务绝不独立初始化/销毁 Bluetooth Host；生命周期由 ble_remote_service 统一管理。
// Classic Bluetooth (BR/EDR) receivers found during inquiry.
static constexpr size_t BT_AUDIO_SOURCE_MAX_DEVICES = 12U;
struct BtAudioSourceDevice {
    uint8_t address[6] = {};
    char name[48] = {};
    int8_t rssi = -127;
};
struct BtAudioSourceDeviceList {
    uint32_t revision = 0U;
    uint8_t count = 0U;
    bool scanning = false;
    BtAudioSourceDevice devices[BT_AUDIO_SOURCE_MAX_DEVICES] = {};
};

struct BtAudioSourceSnapshot {
    bool available = false;
    bool initialized = false;
    bool scanning = false;
    bool candidate_found = false;
    bool connecting = false;
    bool connected = false;
    bool streaming = false;
    uint32_t sample_rate_hz = 44100U;
    uint8_t negotiated_codec = 0xFFU; // 0=SBC, FF=not negotiated.
    uint32_t underrun_frames = 0U;
    uint32_t dropped_frames = 0U;
    // Output PCM frames currently buffered in the A2DP PSRAM ring (target-rate).
    // AudioTask uses this to pace BT-only decoding without I2S DMA backpressure.
    uint32_t queued_frames = 0U;
    char peer_name[48] = {};
};

// 调用条件：Bluedroid 已 enable；停止须早于 Bluedroid disable。
esp_err_t bt_audio_source_host_start();
void bt_audio_source_host_stop();

// 只从非蓝牙回调任务触发扫描和连接；不影响 GATT 手机遥控会话。
// Scan 5.12 seconds, deduplicate/sort up to 12 audio receivers and select by BD_ADDR.
// The list remains available until the next scan or A2DP Source host shutdown.
esp_err_t bt_audio_source_scan();
bool bt_audio_source_get_device_list(BtAudioSourceDeviceList *out);
esp_err_t bt_audio_source_connect_device(const uint8_t address[6]);
// Legacy strongest-candidate API retained for older callers.
esp_err_t bt_audio_source_connect_candidate();
esp_err_t bt_audio_source_disconnect();
void bt_audio_source_update();
bool bt_audio_source_get_snapshot(BtAudioSourceSnapshot *out);

// AudioTask 非阻塞 PCM：32-bit 左对齐、交错立体声，内部转 S16 及重采样。
// 有线模式旁路 I2S PCM；蓝牙专用模式不初始化 I2S/DAC，由 AudioTask 根据
// queued_frames 限速直接送 A2DP。不得在 BT 回调里阻塞或读 SD。
void bt_audio_source_feed_pcm32(const int32_t *pcm, size_t frames,
                                uint32_t source_rate_hz, uint8_t volume_percent);
void bt_audio_source_flush();
