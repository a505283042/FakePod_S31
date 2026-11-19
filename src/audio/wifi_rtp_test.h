#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Optional test-only RTP L24 sender. The sender is disabled in normal builds.
// PCM is stereo, signed 32-bit with valid source bits left-aligned in each word.
esp_err_t wifi_rtp_test_start();

// Non-blocking SPSC enqueue. It never waits for Wi-Fi or a socket operation.
void wifi_rtp_test_publish_pcm32(
    const int32_t *interleaved_stereo,
    size_t frames,
    uint32_t sample_rate_hz);
