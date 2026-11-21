#pragma once

#include <stdint.h>
#include "esp_err.h"

// AVRCP Phase 1: A2DP Sink-side Controller (phone playback/metadata)
// and Target (phone->FakePod absolute volume and local volume notifications).
// All Bluetooth callbacks only update tiny snapshots; actual DAC writes stay
// on AudioTask. No image or lyrics transfer in this phase.
struct BtAudioAvrcSnapshot {
    bool active = false;
    bool connected = false;
    bool metadata_valid = false;
    bool playback_known = false;
    bool playing = false;
    bool absolute_volume_supported = false;
    uint8_t volume_percent = 50U;
    uint32_t duration_ms = 0U;
    uint32_t position_ms = 0U;
    uint32_t metadata_revision = 0U;
    uint32_t playback_revision = 0U;
    char title[128] = {};
    char artist[96] = {};
    char album[96] = {};
};

enum class BtAvrcMediaCommand : uint8_t {
    TogglePause = 0,
    Next,
    Previous,
    Play,
    Pause,
};

// Called only by the shared Bluedroid owner's A2DP Sink lifecycle.
// Per esp_avrc_api.h, AVRCP init/deinit must precede A2DP init/deinit.
esp_err_t bt_audio_avrc_host_start();
void bt_audio_avrc_host_stop();
void bt_audio_avrc_update();
bool bt_audio_avrc_is_sink_mode();
bool bt_audio_avrc_queue_control(BtAvrcMediaCommand command);
bool bt_audio_avrc_get_snapshot(BtAudioAvrcSnapshot *out);
void bt_audio_avrc_local_volume_changed(uint8_t percent);
