#pragma once
#include <stddef.h>
#include <stdint.h>

// Small clock-drift compensation for a Bluetooth SBC source feeding a separate
// CS43131/I2S hardware clock. Once buffered PCM exceeds the high watermark,
// consume 1-2 extra S16 frames per 256 output frames, with linear interpolation.
// The output remains S32 stereo for I2S and is always exactly output_frames.
// This is a tiny (~0.4%-0.8%) pitch adjustment, not a sample-rate switch.
namespace bt_pcm_drift {
static constexpr size_t kOutputBlockFrames = 256U;
static constexpr size_t kMaxExtraSourceFrames = 2U;

inline void convert_s16_stereo(const int16_t *source, size_t source_frames,
                               int32_t *output, size_t output_frames)
{
    if (source == nullptr || output == nullptr || output_frames == 0U ||
        source_frames < output_frames ||
        source_frames > output_frames + kMaxExtraSourceFrames) return;
    if (source_frames == output_frames || output_frames == 1U) {
        for (size_t i = 0; i < output_frames * 2U; ++i)
            output[i] = static_cast<int32_t>(source[i]) * 65536;
        return;
    }
    const size_t denominator = output_frames - 1U;
    const size_t source_intervals = source_frames - 1U;
    for (size_t i = 0U; i < output_frames; ++i) {
        const size_t numerator = i * source_intervals;
        const size_t j = numerator / denominator;
        const size_t fraction = numerator % denominator;
        for (size_t c = 0U; c < 2U; ++c) {
            const int32_t left = source[j * 2U + c];
            const int32_t right = source[(j + (fraction == 0 ? 0U : 1U)) * 2U + c];
            const int64_t weighted =
                static_cast<int64_t>(left) * static_cast<int64_t>(denominator - fraction) +
                static_cast<int64_t>(right) * static_cast<int64_t>(fraction);
            output[i * 2U + c] =
                static_cast<int32_t>(weighted / static_cast<int64_t>(denominator)) * 65536;
        }
    }
}
}  // namespace bt_pcm_drift
