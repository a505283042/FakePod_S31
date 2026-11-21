#pragma once

#include <stddef.h>
#include <stdint.h>

// A2DP phone cover transfer staging, independent of TF/NAS catalog identity.
// JPEG/PNG bytes are bounded in PSRAM and never decoded in Bluetooth callbacks.
enum class BtPhoneCoverSource : uint8_t { None = 0, Android = 1, Avrcp = 2 };
struct BtPhoneCoverSnapshot {
    bool ready = false;
    uint32_t track_epoch = 0;
    uint32_t image_revision = 0;
    size_t image_size = 0;
    BtPhoneCoverSource source = BtPhoneCoverSource::None;
};

static constexpr size_t BT_PHONE_COVER_MAX_BYTES = 192U * 1024U;

// Call from ordinary startup, not from a Bluetooth stack callback.
bool bt_phone_cover_init();
void bt_phone_cover_track_changed();
uint32_t bt_phone_cover_track_epoch();
bool bt_phone_cover_begin(BtPhoneCoverSource source, uint32_t epoch, size_t expected_bytes);
bool bt_phone_cover_append(BtPhoneCoverSource source, const uint8_t *data, size_t size, bool final);
bool bt_phone_cover_get_snapshot(BtPhoneCoverSnapshot *out);
// Caller owns and frees the PSRAM copy via heap_caps_free(); safe against track changes.
bool bt_phone_cover_copy_image(uint32_t image_revision, uint8_t **out_data,
    size_t *out_size, bool *out_png);
