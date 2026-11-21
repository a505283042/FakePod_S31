#include "bt_audio_avrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_service.h"
#include "bt_phone_cover.h"
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
static uint32_t s_cover_epoch = 0U;
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
        s_cover_epoch = bt_phone_cover_track_epoch();
#endif
        s_last_cover_submitted = 0U;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_CT %s", connected ? "connected" : "disconnected");
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
        portENTER_CRITICAL(&s_lock);
        if (s_active && s_connected) {
            // AVRCP metadata callback attr_id uses ordinal 1..7, not mask.
            if (m.attr_id == 1U) copy_string(s_snapshot.title,
                sizeof(s_snapshot.title), m.attr_text, m.attr_length);
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
            else if (m.attr_id == 8U && m.attr_text != nullptr &&
                     m.attr_length == ESP_AVRC_CA_IMAGE_HANDLE_LEN) {
                memcpy(s_cover_handle, m.attr_text, ESP_AVRC_CA_IMAGE_HANDLE_LEN);
                s_cover_handle_ready = true;
                s_cover_epoch = bt_phone_cover_track_epoch();
                if (s_cover_link) s_cover_fetch_pending = true;
                else if (!s_cover_tried_connect) s_cover_connect_pending = true;
            }
#endif
            else if (m.attr_id == 2U) copy_string(s_snapshot.artist,
                sizeof(s_snapshot.artist), m.attr_text, m.attr_length);
            else if (m.attr_id == 3U) copy_string(s_snapshot.album,
                sizeof(s_snapshot.album), m.attr_text, m.attr_length);
            else if (m.attr_id == 7U && m.attr_text != nullptr && m.attr_length > 0) {
                char duration[16] = {};
                copy_string(duration, sizeof(duration), m.attr_text, m.attr_length);
                s_snapshot.duration_ms = static_cast<uint32_t>(strtoul(duration, nullptr, 10));
            }
            s_snapshot.metadata_valid = s_snapshot.title[0] != '\0' ||
                s_snapshot.artist[0] != '\0';
            ++s_snapshot.metadata_revision;
        }
        portEXIT_CRITICAL(&s_lock);
        if (m.attr_id == 1U) ESP_LOGI(TAG,
            "AVRCP_META title received bytes=%d", m.attr_length);
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
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE)
            bt_phone_cover_track_changed();
        portENTER_CRITICAL(&s_lock);
        if (s_active && s_connected) {
            const uint8_t evt = param->change_ntf.event_id;
            if (evt == ESP_AVRC_RN_TRACK_CHANGE) {
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
                s_cover_handle_ready = false;
                s_cover_fetch_pending = false;
                s_cover_fetching = false;
                s_cover_epoch = bt_phone_cover_track_epoch();
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
        portENTER_CRITICAL(&s_lock);
        s_cover_link = linked;
        s_cover_fetch_pending = linked && s_cover_handle_ready;
        s_cover_fetching = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "AVRCP_COVER OBEX=%s reason=%u",
            linked ? "CONNECTED" : "DISCONNECTED",
            static_cast<unsigned>(param->cover_art_state.reason));
        break;
    }
    case ESP_AVRC_CT_COVER_ART_DATA_EVT: {
        const auto &data = param->cover_art_data;
        const bool success = data.status == ESP_BT_STATUS_SUCCESS;
        const bool ok = success &&
            bt_phone_cover_append(BtPhoneCoverSource::Avrcp,
                data.p_data, data.data_len, data.final);
        if (!ok || data.final) {
            portENTER_CRITICAL(&s_lock);
            s_cover_fetching = false;
            portEXIT_CRITICAL(&s_lock);
        }
        if (!ok) ESP_LOGW(TAG, "AVRCP_COVER chunk error status=%u final=%u bytes=%u",
            static_cast<unsigned>(data.status), data.final,
            static_cast<unsigned>(data.data_len));
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
    s_cover_epoch = 0U;
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
    bool connect_cover = false, fetch_cover = false;
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
    if (connected && ct_ready && s_cover_connect_pending && !s_cover_tried_connect) {
        s_cover_connect_pending = false;
        s_cover_tried_connect = true;
        connect_cover = true;
    }
    if (connected && ct_ready && s_cover_link && s_cover_fetch_pending &&
        s_cover_handle_ready && !s_cover_fetching) {
        s_cover_fetch_pending = false;
        s_cover_fetching = true;
        fetch_cover = true;
        memcpy(cover_handle, s_cover_handle, sizeof(cover_handle));
        cover_epoch = s_cover_epoch;
    }
#endif
    if (connected && ct_ready && (s_poll_due == 0U ||
        static_cast<int32_t>(now - s_poll_due) >= 0)) {
        s_poll_due = now + kStatusPoll;
        need_status = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!active) return;

#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    if (connect_cover) {
        const esp_err_t err = esp_avrc_ct_cover_art_connect(512U);
        ESP_LOGI(TAG, "AVRCP_COVER OBEX connect request: %s", esp_err_to_name(err));
    }
    if (fetch_cover) {
        const bool ready = bt_phone_cover_begin(BtPhoneCoverSource::Avrcp, cover_epoch, 0U);
        const esp_err_t err = ready
            ? esp_avrc_ct_cover_art_get_linked_thumbnail(cover_handle)
            : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_cover_fetching = false;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "AVRCP_COVER thumbnail request: %s", esp_err_to_name(err));
        }
    }
#endif
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
        if (need_meta) (void)esp_avrc_ct_send_metadata_cmd(next_label(),
            ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
            ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME
#if defined(CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED) && CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
            | ESP_AVRC_MD_ATTR_COVER_ART
#endif
            );
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
