#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Classic A2DP Sink: phone -> FakePod (internal SBC decode) -> PSRAM PCM ring.
// Bluetooth callbacks NEVER access I2S, CS43131, LVGL or the SD card.
// The AudioTask is the sole consumer and sole owner of the wired DAC.
struct BtAudioSinkSnapshot {
    bool available = false;
    bool initialized = false;
    bool connected = false;
    bool streaming = false;
    uint32_t sample_rate_hz = 44100U;
    uint32_t queued_frames = 0U;
    uint32_t dropped_frames = 0U;
    uint32_t underrun_frames = 0U;
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
void bt_audio_sink_flush();
