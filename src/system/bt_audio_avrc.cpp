#include "bt_audio_avrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_service.h"
#include "bt_phone_cover.h"
#include "bt_avrc_cover_descriptor.h"
#include "ui/artwork/cover_surface_cache.h"
#include "sdkconfig.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "BT_AVRCP";
constexpr size_t kMaxCommands = 8U;
constexpr TickType_t kStatusPoll = pdMS_TO_TICKS(4000);
constexpr TickType_t kPressHold = pdMS_TO_TICKS(90);

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active = false;
static bool s_ct_ready = false;
static bool s_tg_ready = false;
static bool s_ct_stopped = false;
static bool s_tg_stopped = false;
static bool s_connected = false;
static bool s_capabilities_known = false;
static uint16_t s_peer_events = 0U;
static bool s_metadata_needed = false;
static bool s_status_needed = false;
static uint8_t s_notifications_needed = 0U;
static bool s_volume_pending = false;
static uint8_t s_volume_pending_percent = 50U;
static bool s_local_volume_changed = false;
static bool s_volume_interim_pending = false;
static bool s_volume_registered = false;
static BtAudioAvrcSnapshot s_snapshot = {};
static uint8_t s_commands[kMaxCommands] = {};
static uint8_t s_cmd_head = 0U;
static uint8_t s_cmd_count = 0U;
static uint8_t s_next_label = 0U;
static uint8_t s_pressed_cmd = 0U;
static bool s_release_pending = false;
static TickType_t s_release_due = 0;
static TickType_t s_poll_due = 0;
static TickType_t s_caps_retry_due = 0;
static TickType_t s_metadata_poll_due = 0;
static TickType_t s_position_tick = 0;
static bool s_tg_capabilities_configured = false;
static uint32_t s_last_cover_submitted = 0U;
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
static uint8_t s_cover_handle[ESP_AVRC_CA_IMAGE_HANDLE_LEN] = {};
static bool s_cover_handle_ready = false;
static bool s_cover_link = false;
static bool s_cover_connect_pending = false;
static bool s_cover_fetch_pending = false;
static bool s_cover_tried_connect = false;
static bool s_cover_fetching = false;
static bool s_cover_peer_supported = false;
static bool s_cover_handle_requested = false; // Same image handle must not be fetched every 15s.
static uint32_t s_cover_epoch = 0U;

// Android/AVRCP BIP download stages: query properties, request native image,
// then fall back to the standard linked thumbnail if native is unavailable.
enum class CoverStep : uint8_t { Properties, NativeImage, Thumbnail };
static CoverStep s_cover_step = CoverStep::Properties;
static CoverStep s_cover_active_step = CoverStep::Properties;
static bool s_cover_discard_active = false; // Track changed during previous BIP request.
static bool s_cover_bad_data = false;
static size_t s_cover_rx_bytes = 0U;
// GetImageProperties is XML, not the JPEG. Keep it separately, bounded, and
// convert its advertised JPEG representation into the required Image Descriptor.
static char s_cover_properties[768] = {};
static size_t s_cover_properties_len = 0U;
static uint8_t s_cover_thumbnail_retries = 0U;
static uint8_t s_cover_connect_attempts = 0U;
static TickType_t s_cover_due = 0U;
static TickType_t s_cover_started = 0U;
static TickType_t s_cover_last_data = 0U;
static constexpr TickType_t kCoverNoProgressTimeout = pdMS_TO_TICKS(10000);
static constexpr TickType_t kCoverTotalTimeout = pdMS_TO_TICKS(45000);
static constexpr TickType_t kCoverRetryDelay = pdMS_TO_TICKS(1200);

static const char *cover_step_name(CoverStep step)
{
    switch (step) {
    case CoverStep::Properties: return "properties";
    case CoverStep::NativeImage: return "native";
    case CoverStep::Thumbnail: return "thumbnail";
    }
    return "unknown";
}

// Called ONLY under s_lock. Never call Bluetooth APIs or FreeRTOS mutexes here.
static void cover_complete_step_locked(CoverStep step, bool success, TickType_t now)
{
    s_cover_fetching = false;
    s_cover_bad_data = false;
    s_cover_rx_bytes = 0U;
    s_cover_discard_active = false;
    if (!s_cover_handle_ready) {
        s_cover_fetch_pending = false;
        return;
    }
    if (step == CoverStep::Properties) {
        // Properties are advisory. Some Targets reject the query but allow GetImage.
        s_cover_step = CoverStep::NativeImage;
        s_cover_fetch_pending = true;
    } else if (step == CoverStep::NativeImage && !success) {
        s_cover_step = CoverStep::Thumbnail;
        s_cover_fetch_pending = true;
    } else if (step == CoverStep::Thumbnail && !success &&
               s_cover_thumbnail_retries < 2U) {
        ++s_cover_thumbnail_retries;
        s_cover_step = CoverStep::Thumbnail;
        s_cover_due = now + kCoverRetryDelay;
        s_cover_fetch_pending = true;
    } else {
        // Complete or exhausted all bounded fallback/retry attempts.
        s_cover_handle_requested = true;
        s_cover_fetch_pending = false;
    }
}
#endif

static uint8_t next_label()
{
    portENTER_CRITICAL(&s_lock);
    const uint8_t tl = s_next_label;
    s_next_label = (s_next_label + 1U) & 0x0FU;
    portEXIT_CRITICAL(&s_lock);
    return tl;
}

static void copy_string(char *out, size_t cap, const uint8_t *src, int len)
{
    if (out == nullptr || cap == 0U) return;
    if (src == nullptr || len <= 0) { out[0] = '\0'; return; }
    size_t n = static_cast<size_t>(len) < cap - 1U
        ? static_cast<size_t>(len) : cap - 1U;
    // Do not leave a partial UTF-8 codepoint in the UI string.
    if (n < static_cast<size_t>(len)) {
        while (n > 0U && (src[n] & 0xC0U) == 0x80U) --n;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

static void ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (param == nullptr) return;
    switch (event) {
    case ESP_AVRC_CT_PROF_STATE_EVT: {
        portENTER_CRITICAL(&s_lock);
        s_ct_ready = param->avrc_ct_init_stat.state == ESP_AVRC_INIT_SUCCESS && s_active;
        if (s_ct_ready && s_connected) {
            s_metadata_needed = true;
            s_status_needed = true;
        }
        if (param->avrc_ct_init_stat.state == ESP_AVRC_DEINIT_SUCCESS)
            s_ct_stopped = true;
        portEXIT_CRITICAL(&s_lock);
        break;
    }
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        const bool connected = param->conn_stat.connected;
        bt_phone_cover_track_changed();
        // Never take a FreeRTOS mutex while holding the AVRCP spinlock.
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
        const uint32_t cover_epoch = bt_phone_cover_track_epoch();
#endif
        portENTER_CRITICAL(&s_lock);
        s_connected = connected && s_active;
        s_snapshot.connected = s_connected;
        s_snapshot.playback_known = false;
        // CT and TG connect independently. Do not erase an already received
        // TG volume subscription just because CT finished connecting later.
        if (!connected) {
            s_snapshot.absolute_volume_supported = false;
            s_volume_registered = false;
            s_volume_interim_pending = false;
        }
        s_snapshot.metadata_valid = false;
        s_snapshot.title[0] = '\0';
        s_snapshot.artist[0] = '\0';
        s_snapshot.album[0] = '\0';
        s_snapshot.duration_ms = 0U;
        s_snapshot.position_ms = 0U;
        ++s_snapshot.metadata_revision;
        ++s_snapshot.playback_revision;
        s_capabilities_known = false;
        s_caps_retry_due = 0;
        s_metadata_poll_due = 0;
        s_position_tick = 0;
        s_peer_events = 0U;
        s_metadata_needed = connected;
        s_status_needed = connected;
        s_notifications_needed = 0U;
        s_release_pending = false;
        s_cmd_count = 0U;
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
        s_cover_handle_ready = false;
        s_cover_link = false;
        s_cover_connect_pending = false;
        s_cover_fetch_pending = false;
        s_cover_tried_connect = false;
        s_cover_fetching = false;
        s_cover_peer_supported = false;
        s_cover_handle_requested = false;
        s_cover_epoch = cover_epoch;
        s_cover_step = s_cover_active_step = CoverStep::Properties;
        s_cover_discard_active = false;
        s_cover_bad_data = false;
        s_cover_rx_bytes = 0U;
        s_cover_properties_len = 0U;
        s_cover_properties[0] = '\0';
        s_cover_thumbnail_retries = 0U;
        s_cover_connect_attempts = 0U;
        s_cover_due = s_cover_started = s_cover_last_data = 0U;
#endif
        s_last_cover_submitted = 0U;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_CT %s", connected ? "connected" : "disconnected");
        break;
    }
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
        const uint16_t tg_flags = param->rmt_feats.tg_feat_flag;
        ESP_LOGI(TAG,
            "AVRCP_PEER features=0x%08lx tg_flag=0x%04x (RN flags alone do not imply Cover Art)",
            static_cast<unsigned long>(param->rmt_feats.feat_mask),
            static_cast<unsigned>(tg_flags));
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
        const bool supports_cover = (tg_flags & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) != 0U;
        portENTER_CRITICAL(&s_lock);
        if (s_active && s_connected) {
            s_cover_peer_supported = supports_cover;
            // Follow ESP-IDF release/v6.1 example: SDP feature -> OBEX connect.
            // Never wait for a 0x80 image handle before opening OBEX.
            if (supports_cover && !s_cover_link && !s_cover_tried_connect)
                s_cover_connect_pending = true;
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_COVER peer_support=%u (SDP TG 0x0100)",
            supports_cover ? 1U : 0U);
        if (!supports_cover)
            ESP_LOGI(TAG, "AVRCP_COVER native unavailable; Android BLE artwork fallback required");
#endif
        break;
    }
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        portENTER_CRITICAL(&s_lock);
        s_peer_events = param->get_rn_caps_rsp.evt_set.bits;
        s_capabilities_known = true;
        // 1=playing, 2=track, 3=position. Register only supported events.
        s_notifications_needed = 0x07U;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_CT remote notifications mask=0x%04x",
            static_cast<unsigned>(param->get_rn_caps_rsp.evt_set.bits));
        break;
    }
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        const auto &m = param->meta_rsp;
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
        const uint32_t cover_epoch = m.attr_id == ESP_AVRC_MD_ATTR_COVER_ART
            ? bt_phone_cover_track_epoch() : 0U;
#endif
        portENTER_CRITICAL(&s_lock);
        if (s_active && s_connected) {
            // ESP-IDF 6.1 meta_rsp.attr_id is esp_avrc_md_attr_mask_t
            // (0x01 title, 0x02 artist, 0x04 album, 0x40 time, 0x80 cover).
            if (m.attr_id == ESP_AVRC_MD_ATTR_TITLE) copy_string(s_snapshot.title,
                sizeof(s_snapshot.title), m.attr_text, m.attr_length);
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
            else if (m.attr_id == ESP_AVRC_MD_ATTR_COVER_ART &&
                     m.attr_text != nullptr &&
                     m.attr_length == ESP_AVRC_CA_IMAGE_HANDLE_LEN) {
                const bool changed_handle = !s_cover_handle_ready ||
                    memcmp(s_cover_handle, m.attr_text, ESP_AVRC_CA_IMAGE_HANDLE_LEN) != 0 ||
                    s_cover_epoch != cover_epoch;
                if (changed_handle) {
                    memcpy(s_cover_handle, m.attr_text, ESP_AVRC_CA_IMAGE_HANDLE_LEN);
                    s_cover_handle_ready = true;
                    s_cover_handle_requested = false;
                    s_cover_epoch = cover_epoch;
                    s_cover_step = CoverStep::Properties;
                    s_cover_properties_len = 0U;
                    s_cover_properties[0] = '\0';
                    s_cover_thumbnail_retries = 0U;
                    s_cover_due = 0U;
                    // Do not overlap a new BIP operation with a previous song.
                    // Drain its remaining callbacks, then request the new handle.
                    if (s_cover_fetching) s_cover_discard_active = true;
                    s_cover_fetch_pending = s_cover_peer_supported && s_cover_link;
                }
            }
#endif
            else if (m.attr_id == ESP_AVRC_MD_ATTR_ARTIST) copy_string(s_snapshot.artist,
                sizeof(s_snapshot.artist), m.attr_text, m.attr_length);
            else if (m.attr_id == ESP_AVRC_MD_ATTR_ALBUM) copy_string(s_snapshot.album,
                sizeof(s_snapshot.album), m.attr_text, m.attr_length);
            else if (m.attr_id == ESP_AVRC_MD_ATTR_PLAYING_TIME &&
                     m.attr_text != nullptr && m.attr_length > 0) {
                char duration[16] = {};
                copy_string(duration, sizeof(duration), m.attr_text, m.attr_length);
                s_snapshot.duration_ms = static_cast<uint32_t>(strtoul(duration, nullptr, 10));
            }
            s_snapshot.metadata_valid = s_snapshot.title[0] != '\0' ||
                s_snapshot.artist[0] != '\0';
            ++s_snapshot.metadata_revision;
        }
        portEXIT_CRITICAL(&s_lock);
        if (m.attr_id == ESP_AVRC_MD_ATTR_TITLE) ESP_LOGI(TAG,
            "AVRCP_META title received bytes=%d", m.attr_length);
        // A received cover handle is NOT the image itself; it is required for
        // establishing the OBEX/BIP cover-art data connection.
        if (m.attr_id == ESP_AVRC_MD_ATTR_COVER_ART) ESP_LOGI(TAG,
            "AVRCP_COVER image handle metadata bytes=%d expected=%u accepted=%u",
            m.attr_length,
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
            static_cast<unsigned>(ESP_AVRC_CA_IMAGE_HANDLE_LEN),
            (m.attr_text != nullptr && m.attr_length == ESP_AVRC_CA_IMAGE_HANDLE_LEN) ? 1U : 0U);
#else
            0U, 0U);
#endif
        break;
    }
    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT: {
        portENTER_CRITICAL(&s_lock);
        if (s_active && s_connected) {
            s_snapshot.playback_known = true;
            s_snapshot.playing =
                param->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_PLAYING;
            const bool known_position = param->play_status_rsp.song_position != UINT32_MAX;
            if (known_position)
                s_snapshot.position_ms = param->play_status_rsp.song_position;
            s_position_tick = known_position ? xTaskGetTickCount() : 0U;
            if (param->play_status_rsp.song_length != UINT32_MAX)
                s_snapshot.duration_ms = param->play_status_rsp.song_length;
            ++s_snapshot.playback_revision;
        }
        portEXIT_CRITICAL(&s_lock);
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        const bool track_changed = param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE;
        if (track_changed) bt_phone_cover_track_changed();
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
        const uint32_t cover_epoch = track_changed ? bt_phone_cover_track_epoch() : 0U;
#endif
        portENTER_CRITICAL(&s_lock);
        if (s_active && s_connected) {
            const uint8_t evt = param->change_ntf.event_id;
            if (evt == ESP_AVRC_RN_TRACK_CHANGE) {
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
                s_cover_handle_ready = false;
                s_cover_fetch_pending = false;
                // The old OBEX transfer may still be in flight: drain, do not
                // launch a new request until its final callback or timeout.
                if (s_cover_fetching) s_cover_discard_active = true;
                s_cover_handle_requested = false;
                s_cover_step = CoverStep::Properties;
                s_cover_properties_len = 0U;
                s_cover_properties[0] = '\0';
                s_cover_thumbnail_retries = 0U;
                s_cover_epoch = cover_epoch;
#endif
                s_last_cover_submitted = 0U;
                s_snapshot.title[0] = '\0';
                s_snapshot.artist[0] = '\0';
                s_snapshot.album[0] = '\0';
                s_snapshot.metadata_valid = false;
                s_snapshot.duration_ms = 0U;
                s_snapshot.position_ms = 0U;
                s_position_tick = 0;
                s_metadata_needed = true;
                s_status_needed = true;
                s_notifications_needed |= 0x02U;
                ++s_snapshot.metadata_revision;
            } else if (evt == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                s_snapshot.playing =
                    param->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING;
                s_snapshot.playback_known = true;
                s_status_needed = true;
                s_notifications_needed |= 0x01U;
                ++s_snapshot.playback_revision;
            } else if (evt == ESP_AVRC_RN_PLAY_POS_CHANGED) {
                s_snapshot.position_ms = param->change_ntf.event_parameter.play_pos;
                s_position_tick = xTaskGetTickCount();
                s_notifications_needed |= 0x04U;
                ++s_snapshot.playback_revision;
            }
        }
        portEXIT_CRITICAL(&s_lock);
        break;
    }
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    case ESP_AVRC_CT_COVER_ART_STATE_EVT: {
        const bool linked = param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
        const TickType_t now = xTaskGetTickCount();
        portENTER_CRITICAL(&s_lock);
        s_cover_link = linked && s_active && s_connected;
        if (s_cover_link) {
            s_cover_connect_attempts = 0U;
            s_cover_fetching = false;
            s_cover_discard_active = false;
            s_cover_fetch_pending = s_cover_handle_ready &&
                !s_cover_handle_requested;
            s_metadata_needed = true; // Refresh image handle after OBEX reconnect.
            s_metadata_poll_due = 0;
        } else {
            if (s_cover_fetching && !s_cover_discard_active)
                cover_complete_step_locked(s_cover_active_step, false, now);
            s_cover_fetching = false;
            s_cover_discard_active = false;
            // Bounded reconnect, with a delay to avoid repeated SDP/OBEX spam.
            s_cover_tried_connect = false;
            s_cover_connect_pending = s_active && s_connected &&
                s_cover_peer_supported && s_cover_connect_attempts < 3U;
            s_cover_due = now + kCoverRetryDelay;
            s_cover_fetch_pending = s_cover_handle_ready &&
                !s_cover_handle_requested;
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_COVER OBEX=%s reason=%u",
            linked ? "CONNECTED" : "DISCONNECTED",
            static_cast<unsigned>(param->cover_art_state.reason));
        break;
    }
    case ESP_AVRC_CT_COVER_ART_DATA_EVT: {
        const auto &data = param->cover_art_data;
        const TickType_t now = xTaskGetTickCount();
        portENTER_CRITICAL(&s_lock);
        const bool expected = s_active && s_connected && s_cover_link && s_cover_fetching;
        const bool discard = s_cover_discard_active;
        const CoverStep step = s_cover_active_step;
        const bool prior_error = s_cover_bad_data;
        if (expected) {
            s_cover_last_data = now;
            s_cover_rx_bytes += data.data_len;
            if (!discard && step == CoverStep::Properties && !s_cover_bad_data &&
                data.status == ESP_BT_STATUS_SUCCESS) {
                if (data.data_len != 0U && data.p_data != nullptr &&
                    s_cover_properties_len + data.data_len < sizeof(s_cover_properties)) {
                    memcpy(s_cover_properties + s_cover_properties_len,
                        data.p_data, data.data_len);
                    s_cover_properties_len += data.data_len;
                    s_cover_properties[s_cover_properties_len] = '\0';
                } else if (data.data_len != 0U) {
                    s_cover_bad_data = true;
                }
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (!expected) break;
        const bool received = data.status == ESP_BT_STATUS_SUCCESS;
        const bool finished = data.final || !received;
        bool good = received && !prior_error;
        if (!discard && step != CoverStep::Properties && good) {
            good = bt_phone_cover_append(BtPhoneCoverSource::Avrcp,
                data.p_data, data.data_len, data.final);
        }
        portENTER_CRITICAL(&s_lock);
        if (s_cover_fetching && !s_cover_discard_active && !good)
            s_cover_bad_data = true;
        const bool final_good = !s_cover_bad_data && good;
        const size_t total_bytes = s_cover_rx_bytes;
        if (finished && s_cover_fetching) {
            if (s_cover_discard_active) {
                s_cover_fetching = false;
                s_cover_discard_active = false;
                s_cover_fetch_pending = s_cover_handle_ready &&
                    !s_cover_handle_requested;
            } else {
                cover_complete_step_locked(step,
                    step == CoverStep::Properties ? received : final_good, now);
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (finished) {
            ESP_LOGI(TAG, "AVRCP_COVER %s completed ok=%u bytes=%u stale=%u status=%u",
                cover_step_name(step), final_good ? 1U : 0U,
                static_cast<unsigned>(total_bytes), discard ? 1U : 0U,
                static_cast<unsigned>(data.status));
            if (step == CoverStep::Properties && !discard && final_good) {
                // Visible in diagnostic logs; enables validating what the
                // HyperOS BIP server actually advertises per track.
                char xml[sizeof(s_cover_properties)] = {};
                portENTER_CRITICAL(&s_lock);
                const size_t n = s_cover_properties_len;
                if (n != 0U && n < sizeof(xml)) memcpy(xml, s_cover_properties, n);
                portEXIT_CRITICAL(&s_lock);
                if (n != 0U) ESP_LOGI(TAG, "AVRCP_COVER properties XML: %s", xml);
            }
        } else if (!good && !discard && !prior_error) {
            ESP_LOGW(TAG, "AVRCP_COVER %s rejected chunk bytes=%u total=%u (limit=%u)",
                cover_step_name(step), static_cast<unsigned>(data.data_len),
                static_cast<unsigned>(total_bytes),
                static_cast<unsigned>(BT_PHONE_COVER_MAX_BYTES));
        }
        break;
    }
#endif
    default: break;
    }
}

static void tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (param == nullptr) return;
    switch (event) {
    case ESP_AVRC_TG_PROF_STATE_EVT:
        portENTER_CRITICAL(&s_lock);
        s_tg_ready = param->avrc_tg_init_stat.state == ESP_AVRC_INIT_SUCCESS && s_active;
        if (param->avrc_tg_init_stat.state == ESP_AVRC_DEINIT_SUCCESS)
            s_tg_stopped = true;
        portEXIT_CRITICAL(&s_lock);
        break;
    case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
        portENTER_CRITICAL(&s_lock);
        if (!param->conn_stat.connected) {
            s_volume_registered = false;
            s_volume_interim_pending = false;
            s_snapshot.absolute_volume_supported = false;
        }
        portEXIT_CRITICAL(&s_lock);
        break;
    }
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        const uint8_t raw = param->set_abs_vol.volume > 127U
            ? 127U : param->set_abs_vol.volume;
        const uint8_t percent = static_cast<uint8_t>((static_cast<uint16_t>(raw) * 100U + 63U) / 127U);
        portENTER_CRITICAL(&s_lock);
        if (s_active) {
            s_volume_pending = true;
            s_volume_pending_percent = percent;
            s_snapshot.volume_percent = percent;
            s_snapshot.absolute_volume_supported = true;
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_VOLUME phone->DAC percent=%u",
            static_cast<unsigned>(percent));
        break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            portENTER_CRITICAL(&s_lock);
            s_volume_registered = true;
            s_volume_interim_pending = true;
            s_snapshot.absolute_volume_supported = true;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "AVRCP_VOLUME phone registered volume notifications");
        }
        break;
    default: break;
    }
}

static void reset_state()
{
    s_ct_ready = false;
    s_tg_ready = false;
    s_ct_stopped = false;
    s_tg_stopped = false;
    s_connected = false;
    s_capabilities_known = false;
    s_peer_events = 0U;
    s_metadata_needed = false;
    s_status_needed = false;
    s_notifications_needed = 0U;
    s_volume_pending = false;
    s_local_volume_changed = false;
    s_volume_interim_pending = false;
    s_volume_registered = false;
    s_cmd_count = s_cmd_head = 0U;
    s_release_pending = false;
    s_poll_due = 0;
    s_caps_retry_due = 0;
    s_metadata_poll_due = 0;
    s_position_tick = 0;
    s_tg_capabilities_configured = false;
    s_last_cover_submitted = 0U;
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    s_cover_handle_ready = false;
    s_cover_link = false;
    s_cover_connect_pending = false;
    s_cover_fetch_pending = false;
    s_cover_tried_connect = false;
    s_cover_fetching = false;
    s_cover_peer_supported = false;
    s_cover_handle_requested = false;
    s_cover_epoch = 0U;
    s_cover_step = s_cover_active_step = CoverStep::Properties;
    s_cover_discard_active = false;
    s_cover_bad_data = false;
    s_cover_rx_bytes = 0U;
    s_cover_properties_len = 0U;
    s_cover_properties[0] = '\0';
    s_cover_thumbnail_retries = 0U;
    s_cover_connect_attempts = 0U;
    s_cover_due = s_cover_started = s_cover_last_data = 0U;
#endif
    s_snapshot = {};
    s_snapshot.active = s_active;
}
}

esp_err_t bt_audio_avrc_host_start()
{
    (void)bt_phone_cover_init();
    bt_phone_cover_track_changed();
    portENTER_CRITICAL(&s_lock);
    if (s_active) { portEXIT_CRITICAL(&s_lock); return ESP_OK; }
    s_active = true;
    reset_state();
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = esp_avrc_ct_register_callback(ct_callback);
    if (err == ESP_OK) err = esp_avrc_tg_register_callback(tg_callback);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_lock);
        return err;
    }
    // Official IDF 6.1 contract: AVRCP must initialize before A2DP.
    err = esp_avrc_ct_init();
    if (err == ESP_OK) err = esp_avrc_tg_init();
    if (err != ESP_OK) {
        (void)esp_avrc_tg_deinit();
        (void)esp_avrc_ct_deinit();
        portENTER_CRITICAL(&s_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_lock);
    }
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    ESP_LOGI(TAG, "AVRCP_COVER compile enabled=1, OBEX/BIP image handle requested");
#else
    ESP_LOGW(TAG, "AVRCP_COVER compile enabled=0; inspect generated sdkconfig.fakepod_s31_bt_dma");
#endif
    ESP_LOGI(TAG, "AVRCP CT+TG init: %s", esp_err_to_name(err));
    return err;
}

void bt_audio_avrc_host_stop()
{
    portENTER_CRITICAL(&s_lock);
    const bool was_active = s_active;
    s_active = false;
    s_connected = false;
    s_release_pending = false;
    s_cmd_count = 0U;
    portEXIT_CRITICAL(&s_lock);
    bt_phone_cover_track_changed();
    if (!was_active) return;
    // AVRCP deinit must precede A2DP Sink deinit. Both are asynchronous:
    // wait for profile callbacks before allowing a new Source registration.
    const esp_err_t tg = esp_avrc_tg_deinit();
    const esp_err_t ct = esp_avrc_ct_deinit();
    if (tg == ESP_OK || ct == ESP_OK) {
        const TickType_t started = xTaskGetTickCount();
        bool done = false;
        do {
            portENTER_CRITICAL(&s_lock);
            done = (tg != ESP_OK || s_tg_stopped) && (ct != ESP_OK || s_ct_stopped);
            portEXIT_CRITICAL(&s_lock);
            if (done) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while (xTaskGetTickCount() - started < pdMS_TO_TICKS(1200));
        if (!done) ESP_LOGW(TAG, "AVRCP deinit callback timeout");
    }
    portENTER_CRITICAL(&s_lock);
    reset_state();
    portEXIT_CRITICAL(&s_lock);
    bt_phone_cover_release_staging();
}

bool bt_audio_avrc_is_sink_mode()
{
    portENTER_CRITICAL(&s_lock);
    const bool active = s_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool bt_audio_avrc_queue_control(BtAvrcMediaCommand command)
{
    portENTER_CRITICAL(&s_lock);
    const bool ok = s_active && s_connected && s_ct_ready && s_cmd_count < kMaxCommands;
    if (ok) {
        s_commands[(s_cmd_head + s_cmd_count) % kMaxCommands] =
            static_cast<uint8_t>(command);
        ++s_cmd_count;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

bool bt_audio_avrc_get_snapshot(BtAudioAvrcSnapshot *out)
{
    if (out == nullptr) return false;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    out->active = s_active;
    out->connected = s_connected;
    if (s_position_tick != 0U && out->playback_known && out->playing &&
        out->position_ms != UINT32_MAX) {
        const uint32_t elapsed = static_cast<uint32_t>(
            (static_cast<uint64_t>(xTaskGetTickCount() - s_position_tick) * 1000ULL)
            / configTICK_RATE_HZ);
        const uint64_t advanced = static_cast<uint64_t>(out->position_ms) + elapsed;
        out->position_ms = static_cast<uint32_t>(
            advanced > UINT32_MAX ? UINT32_MAX : advanced);
        if (out->duration_ms > 0U && out->position_ms > out->duration_ms)
            out->position_ms = out->duration_ms;
    }
    portEXIT_CRITICAL(&s_lock);
    return true;
}

void bt_audio_avrc_local_volume_changed(uint8_t percent)
{
    if (percent > 100U) percent = 100U;
    portENTER_CRITICAL(&s_lock);
    if (s_active) {
        s_snapshot.volume_percent = percent;
        s_local_volume_changed = s_volume_registered;
    }
    portEXIT_CRITICAL(&s_lock);
}

void bt_audio_avrc_update()
{
    bool active, connected, ct_ready, tg_ready;
    bool volume_pending, volume_interim, volume_changed;
    bool need_meta, need_status, need_caps, config_tg_cap = false;
    uint8_t volume;
    uint8_t notif;
    uint16_t supported_events;
    bool cmd_send=false, cmd_release=false;
    uint8_t cmd=0U;
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    bool connect_cover = false, fetch_cover = false, request_cover_metadata = false;
    bool disconnect_cover = false;
    CoverStep cover_step = CoverStep::Properties;
    uint8_t cover_handle[ESP_AVRC_CA_IMAGE_HANDLE_LEN] = {};
    uint32_t cover_epoch = 0U;
#endif
    const TickType_t now = xTaskGetTickCount();

    portENTER_CRITICAL(&s_lock);
    active = s_active;
    connected = s_connected;
    ct_ready = s_ct_ready;
    tg_ready = s_tg_ready;
    volume_pending = s_volume_pending;
    volume = s_volume_pending_percent;
    s_volume_pending = false;
    volume_interim = s_volume_interim_pending;
    s_volume_interim_pending = false;
    // AVRCP requires INTERIM first; a volume change in the same update must
    // remain registered until a later CHANGED response is delivered.
    volume_changed = !volume_interim &&
        s_local_volume_changed && s_volume_registered;
    if (volume_changed) {
        s_local_volume_changed = false;
        s_volume_registered = false;
    }
    need_meta = connected && ct_ready && s_metadata_needed;
    need_status = connected && ct_ready && s_status_needed;
    if (need_meta) s_metadata_needed = false;
    if (need_status) s_status_needed = false;
    if (connected && ct_ready &&
        (s_metadata_poll_due == 0U ||
         static_cast<int32_t>(now - s_metadata_poll_due) >= 0)) {
        // Some phones don't advertise track-change notifications. Periodic
        // metadata fallback is low-rate and does not touch the PCM path.
        need_meta = true;
        s_metadata_poll_due = now + pdMS_TO_TICKS(15000);
    }
    need_caps = connected && !s_capabilities_known &&
        (s_caps_retry_due == 0U ||
         static_cast<int32_t>(now - s_caps_retry_due) >= 0);
    if (need_caps) s_caps_retry_due = now + pdMS_TO_TICKS(3000);
    if (tg_ready && !s_tg_capabilities_configured) {
        s_tg_capabilities_configured = true;
        config_tg_cap = true;
    }
    supported_events = s_peer_events;
    notif = s_notifications_needed;
    s_notifications_needed = 0U;
    if (connected && ct_ready && s_release_pending &&
        static_cast<int32_t>(now - s_release_due) >= 0) {
        cmd = s_pressed_cmd;
        s_release_pending = false;
        cmd_release = true;
    } else if (connected && ct_ready && !s_release_pending && s_cmd_count != 0U) {
        cmd = s_commands[s_cmd_head];
        s_cmd_head = (s_cmd_head + 1U) % kMaxCommands;
        --s_cmd_count;
        cmd_send = true;
        // Map UI play/pause toggle to the last phone playback status.
        if (cmd == static_cast<uint8_t>(BtAvrcMediaCommand::TogglePause)) {
            cmd = static_cast<uint8_t>(s_snapshot.playing
                ? BtAvrcMediaCommand::Pause : BtAvrcMediaCommand::Play);
        }
        s_pressed_cmd = cmd;
        s_release_pending = true;
        s_release_due = now + kPressHold;
    }
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    if (connected && ct_ready && s_cover_fetching &&
        ((now - s_cover_last_data > kCoverNoProgressTimeout) ||
         (now - s_cover_started > kCoverTotalTimeout))) {
        const CoverStep timed_out = s_cover_active_step;
        if (!s_cover_discard_active)
            cover_complete_step_locked(timed_out, false, now);
        else {
            s_cover_fetching = false;
            s_cover_discard_active = false;
        }
        s_cover_link = false;
        disconnect_cover = true;
    }
    if (connected && ct_ready && s_cover_connect_pending && !s_cover_tried_connect &&
        static_cast<int32_t>(now - s_cover_due) >= 0) {
        s_cover_connect_pending = false;
        s_cover_tried_connect = true;
        ++s_cover_connect_attempts;
        connect_cover = true;
    }
    if (connected && ct_ready && s_cover_peer_supported && s_cover_link &&
        s_cover_fetch_pending && s_cover_handle_ready && !s_cover_fetching &&
        !s_cover_handle_requested &&
        static_cast<int32_t>(now - s_cover_due) >= 0) {
        s_cover_fetch_pending = false;
        s_cover_fetching = true;
        s_cover_active_step = s_cover_step;
        s_cover_bad_data = false;
        s_cover_rx_bytes = 0U;
        s_cover_started = s_cover_last_data = now;
        cover_step = s_cover_active_step;
        fetch_cover = true;
        memcpy(cover_handle, s_cover_handle, sizeof(cover_handle));
        cover_epoch = s_cover_epoch;
    }
    request_cover_metadata = s_cover_peer_supported && s_cover_link;
#endif
    if (connected && ct_ready && (s_poll_due == 0U ||
        static_cast<int32_t>(now - s_poll_due) >= 0)) {
        s_poll_due = now + kStatusPoll;
        need_status = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!active) return;

#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    if (disconnect_cover) {
        ESP_LOGW(TAG, "AVRCP_COVER BIP timed out; resetting OBEX for fallback/recovery");
        (void)esp_avrc_ct_cover_art_disconnect();
    }
    if (connect_cover) {
        const esp_err_t err = esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX);
        ESP_LOGI(TAG, "AVRCP_COVER OBEX connect request: %s", esp_err_to_name(err));
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_cover_tried_connect = false;
            s_cover_connect_pending = s_cover_connect_attempts < 3U;
            s_cover_due = xTaskGetTickCount() + kCoverRetryDelay;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    if (fetch_cover) {
        const bool ready = cover_step == CoverStep::Properties ||
            bt_phone_cover_begin(BtPhoneCoverSource::Avrcp, cover_epoch, 0U);
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (ready) {
            switch (cover_step) {
            case CoverStep::Properties:
                err = esp_avrc_ct_cover_art_get_image_properties(cover_handle);
                break;
            case CoverStep::NativeImage: {
                // IDF 6.1 rejects a null/empty Image Descriptor with
                // ESP_ERR_INVALID_ARG. Select an advertised JPEG representation
                // from the preceding GetImageProperties response.
                char properties[sizeof(s_cover_properties)] = {};
                portENTER_CRITICAL(&s_lock);
                const size_t n = s_cover_properties_len;
                if (n != 0U && n < sizeof(properties))
                    memcpy(properties, s_cover_properties, n);
                portEXIT_CRITICAL(&s_lock);
                bt_avrc_cover_descriptor::Selection selected = {};
                char descriptor[160] = {};
                const bool found = bt_avrc_cover_descriptor::select(
                    properties, n, BT_PHONE_COVER_MAX_BYTES, &selected);
                const size_t descriptor_len = found
                    ? bt_avrc_cover_descriptor::make_xml(
                        selected, descriptor, sizeof(descriptor))
                    : 0U;
                if (descriptor_len != 0U) {
                    ESP_LOGI(TAG,
                        "AVRCP_COVER image descriptor jpeg=%ux%u variant=%u len=%u",
                        static_cast<unsigned>(selected.width),
                        static_cast<unsigned>(selected.height),
                        selected.variant ? 1U : 0U,
                        static_cast<unsigned>(descriptor_len));
                    err = esp_avrc_ct_cover_art_get_image(cover_handle,
                        reinterpret_cast<uint8_t *>(descriptor),
                        static_cast<uint16_t>(descriptor_len));
                } else {
                    ESP_LOGW(TAG,
                        "AVRCP_COVER no suitable JPEG representation, using thumbnail");
                    err = ESP_ERR_NOT_SUPPORTED;
                }
                break;
            }
            case CoverStep::Thumbnail:
                err = esp_avrc_ct_cover_art_get_linked_thumbnail(cover_handle);
                break;
            }
        }
        ESP_LOGI(TAG, "AVRCP_COVER request=%s err=%s max_bytes=%u",
            cover_step_name(cover_step), esp_err_to_name(err),
            static_cast<unsigned>(BT_PHONE_COVER_MAX_BYTES));
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            if (s_cover_fetching && s_cover_active_step == cover_step &&
                s_cover_epoch == cover_epoch && !s_cover_discard_active)
                cover_complete_step_locked(cover_step, false, xTaskGetTickCount());
            portEXIT_CRITICAL(&s_lock);
        }
    }
#endif
    // CoverSurface service normally starts from the TF catalog path. Phone playback
    // must also work when no SD card is inserted.
    if (connected && !cover_surface_cache_is_ready())
        (void)cover_surface_cache_start();

    // Schedule phone JPEG/PNG conversion only from the normal service pump, never
    // inside Bluedroid callbacks. Old Surface remains pinned during decoding.
    BtPhoneCoverSnapshot cover = {};
    if (connected && cover_surface_cache_is_ready() &&
        bt_phone_cover_get_snapshot(&cover) && cover.ready &&
        cover.image_revision != s_last_cover_submitted &&
        cover_surface_cache_request_phone(cover.image_revision)) {
        s_last_cover_submitted = cover.image_revision;
        ESP_LOGI(TAG, "AVRCP_COVER surface requested revision=%lu source=%u",
            static_cast<unsigned long>(cover.image_revision),
            static_cast<unsigned>(cover.source));
    }

    if (volume_pending) {
        // AudioTask is the only writer of CS43131: NO PCM attenuation stage.
        if (!audio_service_set_volume(volume, false))
            ESP_LOGW(TAG, "绝对音量写入AudioTask队列失败 volume=%u", (unsigned)volume);
    }

    if (config_tg_cap) {
        esp_avrc_rn_evt_cap_mask_t mask = {};
        esp_avrc_rn_evt_bit_mask_operation(
            ESP_AVRC_BIT_MASK_OP_SET, &mask, ESP_AVRC_RN_VOLUME_CHANGE);
        const esp_err_t cap_ret = esp_avrc_tg_set_rn_evt_cap(&mask);
        if (cap_ret != ESP_OK) {
            ESP_LOGW(TAG, "AVRCP Target volume notification registration: %s",
                esp_err_to_name(cap_ret));
            // One warning is sufficient; keep the local DAC-volume fallback
            // when a phone or stack does not support AVRCP absolute volume.
        }
    }
    if (tg_ready && (volume_interim || volume_changed)) {
        esp_avrc_rn_param_t p = {};
        uint8_t effective_volume = volume;
        if (!volume_pending) {
            AudioStateSnapshot audio = {};
            if (audio_service_get_snapshot(&audio))
                effective_volume = audio.volume_percent;
            else {
                BtAudioAvrcSnapshot snap = {};
                (void)bt_audio_avrc_get_snapshot(&snap);
                effective_volume = snap.volume_percent;
            }
        }
        p.volume = static_cast<uint8_t>((static_cast<uint16_t>(effective_volume) * 127U + 50U) / 100U);
        (void)esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
            volume_interim ? ESP_AVRC_RN_RSP_INTERIM : ESP_AVRC_RN_RSP_CHANGED, &p);
    }

    if (connected && ct_ready) {
        if (need_caps) (void)esp_avrc_ct_send_get_rn_capabilities_cmd(next_label());
        if (need_meta) {
            uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                                ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME;
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
            if (request_cover_metadata) attr_mask |= ESP_AVRC_MD_ATTR_COVER_ART;
#endif
            const esp_err_t ret = esp_avrc_ct_send_metadata_cmd(next_label(), attr_mask);
            if (ret != ESP_OK) ESP_LOGW(TAG,
                "AVRCP_META request failed mask=0x%02x %s",
                attr_mask, esp_err_to_name(ret));
        }
        if (need_status) (void)esp_avrc_ct_send_get_play_status_cmd(next_label());
        const uint8_t ids[3] = {ESP_AVRC_RN_PLAY_STATUS_CHANGE,
                                 ESP_AVRC_RN_TRACK_CHANGE, ESP_AVRC_RN_PLAY_POS_CHANGED};
        for (uint8_t i = 0; i < 3U; ++i) {
            if ((notif & (1U << i)) != 0U &&
                (supported_events & (1U << ids[i])) != 0U) {
                (void)esp_avrc_ct_send_register_notification_cmd(
                    next_label(), ids[i], i == 2U ? 1U : 0U);
            }
        }
        if (cmd_send || cmd_release) {
            const uint8_t key =
                cmd == static_cast<uint8_t>(BtAvrcMediaCommand::Next)
                    ? ESP_AVRC_PT_CMD_FORWARD :
                cmd == static_cast<uint8_t>(BtAvrcMediaCommand::Previous)
                    ? ESP_AVRC_PT_CMD_BACKWARD :
                cmd == static_cast<uint8_t>(BtAvrcMediaCommand::Pause)
                    ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
            const esp_err_t err = esp_avrc_ct_send_passthrough_cmd(next_label(), key,
                cmd_release ? ESP_AVRC_PT_CMD_STATE_RELEASED : ESP_AVRC_PT_CMD_STATE_PRESSED);
            if (err != ESP_OK) ESP_LOGW(TAG, "AVRCP key=%u %s: %s",
                (unsigned)key, cmd_release ? "release" : "press", esp_err_to_name(err));
            else if (cmd_send) ESP_LOGI(TAG, "AVRCP_KEY phone key=%u sent",
                static_cast<unsigned>(key));
        }
    }
}
