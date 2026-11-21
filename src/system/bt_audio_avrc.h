#pragma once

#include <stdint.h>
#include "esp_err.h"

// AVRCP Phase 1: A2DP Sink-side Controller (phone playback/metadata)
// and Target (phone->FakePod absolute volume and local volume notifications).
// All Bluetooth callbacks only update tiny snapshots; actual DAC writes stay
// on AudioTask. No image or lyrics transfer in this phase.
//
// IDF 6.1 has a SendSetPlayerValue API, but no high-level response carrying
// the phone's actual repeat/shuffle setting. Never mark a sent command as
// "confirmed" merely because the API returned ESP_OK.
enum class BtAvrcLoopSupport : uint8_t {
    Checking = 0,          // Waiting for the peer's AVRCP feature response
    Unverified,            // May send settings; cannot prove phone applied them
    Unsupported,           // Peer explicitly lacks AVRCP player/recorder category
    Confirmed,             // Reserved for reliable readback (not produced by IDF 6.1)
};
struct BtAudioAvrcSnapshot {
    bool active = false;
    bool connected = false;
    bool metadata_valid = false;
    bool playback_known = false;
    bool playing = false;
    bool absolute_volume_supported = false;
    BtAvrcLoopSupport loop_support = BtAvrcLoopSupport::Checking;
    bool loop_request_pending = false;
    bool loop_request_sent = false; // Enqueued in host, NOT confirmed on phone
    uint8_t loop_requested_mode = 0U; // 0=seq 1=list 2=single 3=shuffle
    uint32_t loop_revision = 0U;
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
// Queues a phone-side Repeat+Shuffle request. No changes to local TF/NAS mode.
// Returns false while capability is being checked or definitely unsupported.
bool bt_audio_avrc_request_loop_mode(uint8_t mode);
bool bt_audio_avrc_get_snapshot(BtAudioAvrcSnapshot *out);
void bt_audio_avrc_local_volume_changed(uint8_t percent);
