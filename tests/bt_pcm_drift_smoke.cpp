// Portable unit test for the A2DP -> I2S drift smoother.
// g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/bt_pcm_drift_smoke.cpp -o bt_pcm_drift_smoke
// ./bt_pcm_drift_smoke
#include "system/bt_pcm_drift.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
    using namespace bt_pcm_drift;
    static_assert(kOutputBlockFrames == 256U, "AudioTask block changed");
    static_assert(kMaxExtraSourceFrames == 2U, "Scratch buffer assumes 2 extra");
    int16_t samples[(kOutputBlockFrames + kMaxExtraSourceFrames) * 2U] = {};
    int32_t output[kOutputBlockFrames * 2U] = {};

    // No adjustment: bit-exact S16->S32 conversion.
    for (size_t i = 0; i < kOutputBlockFrames; ++i) {
        samples[i * 2U] = static_cast<int16_t>(i - 128);
        samples[i * 2U + 1U] = static_cast<int16_t>(128 - i);
    }
    convert_s16_stereo(samples, kOutputBlockFrames, output, kOutputBlockFrames);
    for (size_t i = 0; i < kOutputBlockFrames; ++i) {
        assert(output[i * 2U] == static_cast<int32_t>(samples[i * 2U]) * 65536);
        assert(output[i * 2U + 1U] == static_cast<int32_t>(samples[i * 2U + 1U]) * 65536);
    }

    // Positive 1/256 correction: preserve endpoints and interpolate inside.
    for (size_t i = 0; i < kOutputBlockFrames + 1U; ++i) {
        samples[i * 2U] = static_cast<int16_t>(i * 100U);
        samples[i * 2U + 1U] = -static_cast<int16_t>(i * 100U);
    }
    convert_s16_stereo(samples, kOutputBlockFrames + 1U, output, kOutputBlockFrames);
    assert(output[0] == 0 && output[1] == 0);
    assert(output[(kOutputBlockFrames - 1U) * 2U] == 25600 * 65536);
    assert(output[(kOutputBlockFrames - 1U) * 2U + 1U] == -25600 * 65536);
    for (size_t i = 1; i < kOutputBlockFrames; ++i) {
        assert(output[i * 2U] >= output[(i - 1U) * 2U]);
        assert(output[i * 2U + 1U] <= output[(i - 1U) * 2U + 1U]);
    }

    // Positive 2/256 adjustment: constant waveform must remain exactly constant.
    for (size_t i = 0; i < kOutputBlockFrames + 2U; ++i) {
        samples[i * 2U] = INT16_MIN;
        samples[i * 2U + 1U] = INT16_MAX;
    }
    convert_s16_stereo(samples, kOutputBlockFrames + 2U, output, kOutputBlockFrames);
    for (size_t i = 0; i < kOutputBlockFrames; ++i) {
        assert(output[i * 2U] == static_cast<int32_t>(INT16_MIN) * 65536);
        assert(output[i * 2U + 1U] == static_cast<int32_t>(INT16_MAX) * 65536);
    }
    std::puts("BT PCM drift smoother: 3 cases PASS");
}
