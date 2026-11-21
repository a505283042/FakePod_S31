#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_a2dp_api.h"

// Only built into the experimental CONFIG_BT_A2DP_USE_EXTERNAL_CODEC profile.
// Decoding runs on a dedicated worker; Bluedroid callbacks only enqueue the
// ownership-transferred esp_a2d_audio_buff_t and never touch the DAC.
struct BtA2dpExtStats {
    uint32_t raw_packets = 0;
    uint32_t raw_drop_packets = 0;
    uint32_t decode_errors = 0;
    uint64_t decoded_frames = 0;
    uint8_t codec_type = 0xff;
    bool decoder_open = false;
};
using BtA2dpExtPcmCallback = void (*)(const uint8_t *, uint32_t);

bool bt_a2dp_ext_decoder_start(BtA2dpExtPcmCallback on_pcm);
void bt_a2dp_ext_decoder_configure(const esp_a2d_mcc_t *codec);
void bt_a2dp_ext_decoder_stream_state(bool playing);
void bt_a2dp_ext_decoder_receive(esp_a2d_conn_hdl_t hdl, esp_a2d_audio_buff_t *packet);
void bt_a2dp_ext_decoder_stop();
BtA2dpExtStats bt_a2dp_ext_decoder_stats();
