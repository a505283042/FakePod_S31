#include "bt_phone_cover.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
constexpr const char *TAG = "BT_COVER";
SemaphoreHandle_t s_mutex = nullptr;
uint8_t *s_buffer = nullptr;
BtPhoneCoverSnapshot s_ready = {};
uint32_t s_epoch = 1U;
uint32_t s_next_revision = 1U;
bool s_receiving = false;
size_t s_received = 0U;
size_t s_expected = 0U;
BtPhoneCoverSource s_incoming = BtPhoneCoverSource::None;

bool lock() { return s_mutex != nullptr && xSemaphoreTake(s_mutex, 0) == pdTRUE; }
void unlock() { xSemaphoreGive(s_mutex); }
bool valid_jpeg(const uint8_t *p, size_t n) {
    return n >= 4U && p[0] == 0xFFU && p[1] == 0xD8U &&
        p[n - 2U] == 0xFFU && p[n - 1U] == 0xD9U;
}
bool valid_png(const uint8_t *p, size_t n) {
    static const uint8_t sig[] = {137U,80U,78U,71U,13U,10U,26U,10U};
    return n >= sizeof(sig) + 12U && memcmp(p,sig,sizeof(sig)) == 0 &&
        // IEND chunk footer (CRC may differ).
        memcmp(p + n - 8U, "IEND", 4U) == 0;
}
}

bool bt_phone_cover_init()
{
    if (s_mutex == nullptr) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) return false;
    if (s_buffer == nullptr) {
        s_buffer = static_cast<uint8_t *>(heap_caps_malloc(
            BT_PHONE_COVER_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_buffer == nullptr) {
        ESP_LOGW(TAG, "PSRAM insufficient for bounded remote cover staging");
        return false;
    }
    return true;
}

void bt_phone_cover_track_changed()
{
    if (!lock()) return;
    ++s_epoch;
    if (s_epoch == 0U) s_epoch = 1U;
    s_ready = {};
    s_receiving = false;
    s_received = s_expected = 0U;
    s_incoming = BtPhoneCoverSource::None;
    unlock();
}

uint32_t bt_phone_cover_track_epoch()
{
    if (!lock()) return 0U;
    const uint32_t epoch = s_epoch;
    unlock();
    return epoch;
}

bool bt_phone_cover_begin(BtPhoneCoverSource source, uint32_t epoch, size_t expected_bytes)
{
    if (source == BtPhoneCoverSource::None || epoch == 0U || s_buffer == nullptr ||
        expected_bytes > BT_PHONE_COVER_MAX_BYTES || !lock()) return false;
    const bool allow = epoch == s_epoch &&
        (source == BtPhoneCoverSource::Avrcp ||
         (!s_ready.ready || s_ready.source != BtPhoneCoverSource::Avrcp));
    if (allow) {
        s_receiving = true;
        s_received = 0U;
        s_expected = expected_bytes;
        s_incoming = source;
        // Keep the existing rendered Surface until replacement is decoded.
        // The old compressed image is no longer valid once staging overwrites it.
        s_ready = {};
    }
    unlock();
    return allow;
}

bool bt_phone_cover_append(BtPhoneCoverSource source, const uint8_t *data, size_t size, bool final)
{
    if (s_buffer == nullptr || (size != 0U && data == nullptr) || !lock()) return false;
    const bool valid = s_receiving && source == s_incoming &&
        size <= BT_PHONE_COVER_MAX_BYTES - s_received &&
        (s_expected == 0U || s_received + size <= s_expected);
    if (!valid) {
        // An old AVRCP callback must not cancel a newer Android transaction.
        if (s_receiving && source == s_incoming) s_receiving = false;
        unlock();
        return false;
    }
    if (size != 0U) memcpy(s_buffer + s_received, data, size);
    s_received += size;
    bool ok = true;
    if (final) {
        ok = (s_expected == 0U || s_received == s_expected) &&
            (valid_jpeg(s_buffer, s_received) || valid_png(s_buffer, s_received));
        if (ok) {
            s_ready.ready = true;
            s_ready.track_epoch = s_epoch;
            s_ready.image_revision = s_next_revision++;
            if (s_next_revision == 0U) s_next_revision = 1U;
            s_ready.image_size = s_received;
            s_ready.source = source;
        } else {
            s_ready = {};
            ESP_LOGW(TAG, "Rejected phone image bytes=%u expected=%u",
                static_cast<unsigned>(s_received), static_cast<unsigned>(s_expected));
        }
        s_receiving = false;
    }
    unlock();
    return ok;
}

bool bt_phone_cover_get_snapshot(BtPhoneCoverSnapshot *out)
{
    if (out == nullptr || !lock()) return false;
    *out = s_ready;
    out->track_epoch = s_epoch;
    unlock();
    return true;
}

bool bt_phone_cover_copy_image(uint32_t revision, uint8_t **out_data,
    size_t *out_size, bool *out_png)
{
    if (out_data == nullptr || out_size == nullptr || out_png == nullptr) return false;
    *out_data = nullptr;
    *out_size = 0U;
    *out_png = false;
    if (!lock()) return false;
    if (!s_ready.ready || s_ready.image_revision != revision || s_buffer == nullptr) {
        unlock();
        return false;
    }
    const size_t n = s_ready.image_size;
    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) {
        memcpy(copy, s_buffer, n);
        *out_data = copy;
        *out_size = n;
        *out_png = valid_png(copy, n);
    }
    unlock();
    return copy != nullptr;
}
