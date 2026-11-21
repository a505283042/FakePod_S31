#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "device_settings.h"

// Classic A2DP Sink: phone -> FakePod (internal SBC decode) -> PSRAM PCM ring.
// Bluetooth callbacks NEVER access I2S, CS43131, LVGL or the SD card.
// The AudioTask is the sole consumer and sole owner of the wired DAC.
struct BtAudioSinkSnapshot {
    bool available = false;
    bool initialized = false;
    bool connected = false;
    bool streaming = false;
    uint32_t sample_rate_hz = 44100U;
    // 0xFF = not yet negotiated, 0=SBC, 2=MPEG-2/4 AAC.
    uint8_t negotiated_codec = 0xFFU;
    DeviceBluetoothSinkCodec preferred_codec = DeviceBluetoothSinkCodec::AutoAac;
    uint32_t queued_frames = 0U;
    uint32_t dropped_frames = 0U;       // Actual ring overflow (unwanted)
    uint32_t underrun_frames = 0U;      // Missing PCM while output was active
    uint64_t received_frames = 0U;
    uint64_t consumed_frames = 0U;
    uint32_t startup_trim_frames = 0U;  // Intentional startup latency reduction
    uint32_t smooth_adjust_frames = 0U; // Extra source frames consumed by interpolation
    uint32_t queued_peak_frames = 0U;
    char peer_name[48] = {};
};

// Bluedroid host/controller lifecycle remains owned by ble_remote_service.
// These APIs require an already enabled Bluedroid host.
esp_err_t bt_audio_sink_host_start();
esp_err_t bt_audio_sink_disconnect_peer();
void bt_audio_sink_host_stop();
void bt_audio_sink_update();
bool bt_audio_sink_get_snapshot(BtAudioSinkSnapshot *out);
size_t bt_audio_sink_read_pcm32(int32_t *pcm, size_t max_frames);
// AudioTask-only: 256-frame smooth drift correction (~0.4%/step) when queue is
// too deep. This never blocks the Bluetooth callback or changes I2S sample rate.
size_t bt_audio_sink_read_pcm32_adjusted(int32_t *pcm, size_t max_frames);
// AudioTask-only: after DAC initialization, retain at most max_frames of the
// newest PCM, avoiding permanent 200ms+ startup latency.
size_t bt_audio_sink_trim_latency(size_t max_frames);
const char *bt_audio_sink_codec_name(uint8_t negotiated_codec);
void bt_audio_sink_flush();
