#pragma once

#include <stddef.h>
#include <stdint.h>

// A2DP phone cover transfer staging, independent of TF/NAS catalog identity.
// JPEG/PNG bytes are bounded in PSRAM and never decoded in Bluetooth callbacks.
enum class BtPhoneCoverSource : uint8_t { None = 0, Avrcp = 2 };
struct BtPhoneCoverSnapshot {
    bool ready = false;
    // Pending on track change/download; terminal only when the peer has no
    // artwork, all requests fail, or the decoded Surface cannot be built.
    // This prevents the UI treating a transient !ready as "show default".
    bool unavailable = false;
    uint32_t track_epoch = 0;
    uint32_t image_revision = 0;
    size_t image_size = 0;
    BtPhoneCoverSource source = BtPhoneCoverSource::None;
};

// Native AVRCP linked thumbnails use only this bounded PSRAM staging buffer.
// Android Remote BLE album-art upload and GetImage HD path are disabled.
static constexpr size_t BT_PHONE_COVER_MAX_BYTES = 192U * 1024U;

// Call from ordinary startup, not from a Bluetooth stack callback.
bool bt_phone_cover_init();
// Release compressed staging allocation on A2DP Sink -> Source role switch.
void bt_phone_cover_release_staging();
void bt_phone_cover_track_changed();
uint32_t bt_phone_cover_track_epoch();
// Called outside AVRCP spinlocks. No-op for an obsolete epoch or an image
// that is already ready / still receiving.
bool bt_phone_cover_mark_unavailable(uint32_t epoch);
// Surface conversion failed for the exact published image. This is a terminal
// failure even though the compressed JPEG/PNG had been downloaded successfully.
bool bt_phone_cover_render_failed(uint32_t image_revision);
bool bt_phone_cover_begin(BtPhoneCoverSource source, uint32_t epoch, size_t expected_bytes);
bool bt_phone_cover_append(BtPhoneCoverSource source, const uint8_t *data, size_t size, bool final);
bool bt_phone_cover_get_snapshot(BtPhoneCoverSnapshot *out);
// Caller owns and frees the PSRAM copy via heap_caps_free(); safe against track changes.
bool bt_phone_cover_copy_image(uint32_t image_revision, uint8_t **out_data,
    size_t *out_size, bool *out_png);
