#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#include "esp_a2dp_api.h"

// Worker consumes the existing AudioTask -> A2DP PCM ring. Never call from a
// Bluedroid callback with a blocking operation; stop() is for the role worker.
using BtA2dpPcmReader = int32_t (*)(uint8_t *destination, int32_t bytes);
esp_err_t bt_a2dp_ext_encoder_start(
    esp_a2d_conn_hdl_t connection, uint16_t audio_mtu,
    const esp_a2d_mcc_t *codec, uint32_t sample_rate_hz, BtA2dpPcmReader reader);
void bt_a2dp_ext_encoder_request_stop();
bool bt_a2dp_ext_encoder_stop();
bool bt_a2dp_ext_encoder_is_running();
#endif
