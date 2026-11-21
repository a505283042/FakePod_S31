#include "bt_audio_sink.h"
#include "bt_audio_avrc.h"
#include "bt_pcm_drift.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !defined(CONFIG_BT_CLASSIC_ENABLED) || !CONFIG_BT_CLASSIC_ENABLED || \
    !defined(CONFIG_BT_A2DP_ENABLE) || !CONFIG_BT_A2DP_ENABLE || \
    (defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC)
#error "FakePod A2DP Sink requires Classic BT, A2DP and internal SBC codec"
#endif

namespace {
static const char *TAG = "BT_SINK";
// The extra startup-only headroom absorbs DAC/I2S initialisation (typically
// >100ms). Playback is trimmed to 110ms after start, not left with 370ms latency.
static constexpr size_t kRingFrames = 16384U; // S16 stereo: 64 KiB PSRAM.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_ring = nullptr;
static size_t s_read = 0U;
static size_t s_queued = 0U;
static uint32_t s_rate_hz = 44100U;
static uint32_t s_dropped_frames = 0U;
static uint32_t s_underrun_frames = 0U;
static uint64_t s_received_frames = 0U;
static uint64_t s_consumed_frames = 0U;
static uint32_t s_startup_trim_frames = 0U;
static uint32_t s_smooth_adjust_frames = 0U;
static uint32_t s_peak_queued = 0U;
static bool s_requested = false;
static bool s_profile_ready = false;
static bool s_connected = false;
static bool s_streaming = false;
static bool s_deinit_confirmed = false;
static TickType_t s_stats_due = 0;
static esp_bd_addr_t s_peer = {};
static char s_peer_name[48] = {};

static void flush_locked()
{
    s_read = 0U;
    s_queued = 0U;
}

static void audio_data_cb(const uint8_t *data, uint32_t length)
{
    // IDF internal SBC decoder delivers interleaved S16 stereo. No blocking,
    // heap allocation, or I2S work in the Bluedroid callback thread.
    if (data == nullptr || length < 4U) return;
    const size_t count = static_cast<size_t>(length) / 4U;
    portENTER_CRITICAL(&s_mux);
    if (!s_requested || !s_connected || !s_streaming || s_ring == nullptr) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_received_frames += count;
    size_t start = 0U;
    size_t frames = count;
    if (frames > kRingFrames) {
        start = frames - kRingFrames;
        frames = kRingFrames;
        s_dropped_frames += static_cast<uint32_t>(start);
    }
    const size_t overflow = (s_queued + frames > kRingFrames)
        ? s_queued + frames - kRingFrames : 0U;
    if (overflow != 0U) {
        s_read = (s_read + overflow) % kRingFrames;
        s_queued -= overflow;
        s_dropped_frames += static_cast<uint32_t>(overflow);
    }
    size_t write = (s_read + s_queued) % kRingFrames;
    const size_t first = frames < kRingFrames - write ? frames : kRingFrames - write;
    memcpy(s_ring + write * 4U, data + start * 4U, first * 4U);
    if (frames > first) memcpy(s_ring, data + (start + first) * 4U,
                               (frames - first) * 4U);
    s_queued += frames;
    if (s_queued > s_peak_queued) s_peak_queued = static_cast<uint32_t>(s_queued);
    portEXIT_CRITICAL(&s_mux);
}

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (param == nullptr) return;
    switch (event) {
        case ESP_A2D_PROF_STATE_EVT: {
            const bool ready = param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS;
            portENTER_CRITICAL(&s_mux);
            s_profile_ready = ready && s_requested;
            if (!ready) s_deinit_confirmed = true;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP Sink profile %s", ready ? "ready" : "stopped");
            break;
        }
        case ESP_A2D_CONNECTION_STATE_EVT: {
            const esp_a2d_connection_state_t state = param->conn_stat.state;
            portENTER_CRITICAL(&s_mux);
            s_connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            s_streaming = false;
            flush_locked();
            if (s_connected) {
                memcpy(s_peer, param->conn_stat.remote_bda, sizeof(s_peer));
                snprintf(s_peer_name, sizeof(s_peer_name), "%02X:%02X:%02X:%02X:%02X:%02X",
                    s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                memset(s_peer, 0, sizeof(s_peer));
                s_peer_name[0] = '\0';
            }
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP Sink connection state=%d", static_cast<int>(state));
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            uint32_t rate = 44100U;
            if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
                const uint8_t sf = param->audio_cfg.mcc.cie.sbc_info.samp_freq;
                if (sf & ESP_A2D_SBC_CIE_SF_48K) rate = 48000U;
                else if (sf & ESP_A2D_SBC_CIE_SF_44K) rate = 44100U;
                else if (sf & ESP_A2D_SBC_CIE_SF_32K) rate = 32000U;
                else if (sf & ESP_A2D_SBC_CIE_SF_16K) rate = 16000U;
            }
            portENTER_CRITICAL(&s_mux);
            s_rate_hz = rate;
            flush_locked();
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP Sink PCM %luHz/S16/stereo", (unsigned long)rate);
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            const bool playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            portENTER_CRITICAL(&s_mux);
            s_streaming = playing && s_connected && s_requested;
            if (!s_streaming) flush_locked();
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP Sink audio %s", playing ? "STARTED" : "SUSPENDED");
            break;
        }
        default: break;
    }
}

static void classic_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_CFM_REQ_EVT && param != nullptr) {
        // SSP Just Works. Do not pair if the user has not selected Sink mode.
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    }
}
}

esp_err_t bt_audio_sink_host_start()
{
    if (s_ring == nullptr) {
        s_ring = static_cast<uint8_t *>(heap_caps_malloc(
            kRingFrames * 4U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_ring == nullptr) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_mux);
    if (s_requested) { portEXIT_CRITICAL(&s_mux); return ESP_OK; }
    s_requested = true;
    s_profile_ready = false;
    s_connected = false;
    s_streaming = false;
    s_deinit_confirmed = false;
    s_stats_due = 0;
    s_dropped_frames = 0U;
    s_underrun_frames = 0U;
    s_received_frames = 0U;
    s_consumed_frames = 0U;
    s_startup_trim_frames = 0U;
    s_smooth_adjust_frames = 0U;
    s_peak_queued = 0U;
    flush_locked();
    portEXIT_CRITICAL(&s_mux);

    // ESP-IDF contract: AVRCP CT/TG must initialize before A2DP Sink.
    esp_err_t err = bt_audio_avrc_host_start();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_requested = false;
        portEXIT_CRITICAL(&s_mux);
        return err;
    }
    err = esp_bt_gap_register_callback(classic_gap_cb);
    if (err == ESP_OK) err = esp_bt_gap_set_device_name("FakePod Audio");
    if (err == ESP_OK) err = esp_a2d_register_callback(a2dp_cb);
    bool profile_start_sent = false;
    if (err == ESP_OK) {
        err = esp_a2d_sink_init();
        profile_start_sent = err == ESP_OK;
    }
    // Follow the official release/v6.1 sink example: register the decoded SBC
    // PCM callback after a successful sink init request, then advertise.
    if (err == ESP_OK) err = esp_a2d_sink_register_data_callback(audio_data_cb);
    if (err == ESP_OK) {
        err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP Sink启动失败：%s", esp_err_to_name(err));
        if (profile_start_sent) {
            // A successful init request must be paired with deinit even when
            // subsequent GAP setup fails, before Source may register callbacks.
            bt_audio_sink_host_stop();
        } else {
            bt_audio_avrc_host_stop();
            portENTER_CRITICAL(&s_mux);
            s_requested = false;
            portEXIT_CRITICAL(&s_mux);
        }
        return err;
    }
    ESP_LOGI(TAG, "A2DP Sink可发现：FakePod Audio，手机配对后经CS43131有线输出");
    return ESP_OK;
}

esp_err_t bt_audio_sink_disconnect_peer()
{
    esp_bd_addr_t peer = {};
    portENTER_CRITICAL(&s_mux);
    const bool connected = s_requested && s_connected;
    if (connected) memcpy(peer, s_peer, sizeof(peer));
    portEXIT_CRITICAL(&s_mux);
    return connected ? esp_a2d_sink_disconnect(peer) : ESP_ERR_INVALID_STATE;
}

void bt_audio_sink_host_stop()
{
    bool active, connected;
    esp_bd_addr_t peer = {};
    portENTER_CRITICAL(&s_mux);
    active = s_requested;
    connected = s_connected;
    memcpy(peer, s_peer, sizeof(peer));
    s_requested = false;
    s_streaming = false;
    s_connected = false;
    flush_locked();
    portEXIT_CRITICAL(&s_mux);
    if (!active) return;
    (void)esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    // AVRCP deinit BEFORE A2DP Sink deinit, preserving BLE GATT lifetime.
    bt_audio_avrc_host_stop();
    if (connected) (void)esp_a2d_sink_disconnect(peer);
    const esp_err_t ret = esp_a2d_sink_deinit();
    if (ret == ESP_OK) {
        const TickType_t begin = xTaskGetTickCount();
        bool complete = false;
        do {
            portENTER_CRITICAL(&s_mux);
            complete = s_deinit_confirmed;
            portEXIT_CRITICAL(&s_mux);
            if (complete) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while (xTaskGetTickCount() - begin < pdMS_TO_TICKS(1200));
        if (!complete) ESP_LOGW(TAG, "A2DP Sink反初始化确认超时");
    } else {
        ESP_LOGW(TAG, "A2DP Sink停止失败：%s", esp_err_to_name(ret));
    }
}

bool bt_audio_sink_get_snapshot(BtAudioSinkSnapshot *out)
{
    if (out == nullptr) return false;
    BtAudioSinkSnapshot s = {};
    s.available = true;
    portENTER_CRITICAL(&s_mux);
    s.initialized = s_requested && s_profile_ready;
    s.connected = s_connected;
    s.streaming = s_streaming;
    s.sample_rate_hz = s_rate_hz;
    s.queued_frames = static_cast<uint32_t>(s_queued);
    s.dropped_frames = s_dropped_frames;
    s.underrun_frames = s_underrun_frames;
    s.received_frames = s_received_frames;
    s.consumed_frames = s_consumed_frames;
    s.startup_trim_frames = s_startup_trim_frames;
    s.smooth_adjust_frames = s_smooth_adjust_frames;
    s.queued_peak_frames = s_peak_queued;
    memcpy(s.peer_name, s_peer_name, sizeof(s.peer_name));
    portEXIT_CRITICAL(&s_mux);
    *out = s;
    return true;
}

size_t bt_audio_sink_read_pcm32(int32_t *pcm, size_t max_frames)
{
    if (pcm == nullptr || max_frames == 0U) return 0U;
    portENTER_CRITICAL(&s_mux);
    if (!s_requested || !s_streaming || s_ring == nullptr || s_queued == 0U) {
        portEXIT_CRITICAL(&s_mux);
        return 0U;
    }
    const size_t frames = max_frames < s_queued ? max_frames : s_queued;
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t *p = s_ring + ((s_read + i) % kRingFrames) * 4U;
        int16_t left, right;
        memcpy(&left, p, sizeof(left));
        memcpy(&right, p + 2U, sizeof(right));
        pcm[i * 2U] = static_cast<int32_t>(static_cast<int64_t>(left) * 65536LL);
        pcm[i * 2U + 1U] = static_cast<int32_t>(static_cast<int64_t>(right) * 65536LL);
    }
    s_read = (s_read + frames) % kRingFrames;
    s_queued -= frames;
    s_consumed_frames += frames;
    portEXIT_CRITICAL(&s_mux);
    return frames;
}

size_t bt_audio_sink_trim_latency(size_t max_frames)
{
    if (max_frames == 0U) return 0U;
    portENTER_CRITICAL(&s_mux);
    // Drop the oldest delayed startup samples once, after I2S setup. This is
    // intentional low-latency resynchronisation, not ring overflow.
    const size_t stale = s_queued > max_frames ? s_queued - max_frames : 0U;
    if (stale != 0U) {
        s_read = (s_read + stale) % kRingFrames;
        s_queued -= stale;
        s_startup_trim_frames += static_cast<uint32_t>(stale);
    }
    portEXIT_CRITICAL(&s_mux);
    return stale;
}

size_t bt_audio_sink_read_pcm32_adjusted(int32_t *pcm, size_t max_frames)
{
    if (pcm == nullptr || max_frames == 0U) return 0U;
    if (max_frames != bt_pcm_drift::kOutputBlockFrames)
        return bt_audio_sink_read_pcm32(pcm, max_frames);

    int16_t source[(bt_pcm_drift::kOutputBlockFrames +
                    bt_pcm_drift::kMaxExtraSourceFrames) * 2U] = {};
    size_t output_frames = 0U, extra = 0U, taken = 0U;
    portENTER_CRITICAL(&s_mux);
    if (s_requested && s_connected && s_streaming && s_ring != nullptr) {
        output_frames = s_queued < max_frames ? s_queued : max_frames;
        if (output_frames == max_frames) {
            const size_t q_high = static_cast<size_t>(s_rate_hz) * 110U / 1000U;
            const size_t q_critical = static_cast<size_t>(s_rate_hz) * 160U / 1000U;
            if (s_queued > q_critical && s_queued >= max_frames + 2U)
                extra = 2U; // 0.78% temporary catch-up
            else if (s_queued > q_high && s_queued >= max_frames + 1U)
                extra = 1U; // 0.39% drift correction
        }
        taken = output_frames + extra;
        // Copy input under lock, but do all interpolation outside the BT lock.
        const size_t first = taken < kRingFrames - s_read
            ? taken : kRingFrames - s_read;
        if (first != 0U)
            memcpy(source, s_ring + s_read * 4U, first * 4U);
        if (taken > first)
            memcpy(source + first * 2U, s_ring,
                   (taken - first) * 4U);
        s_read = (s_read + taken) % kRingFrames;
        s_queued -= taken;
        s_consumed_frames += taken;
        s_smooth_adjust_frames += static_cast<uint32_t>(extra);
        if (output_frames == 0U)
            s_underrun_frames += static_cast<uint32_t>(max_frames);
    }
    portEXIT_CRITICAL(&s_mux);
    if (output_frames != 0U)
        bt_pcm_drift::convert_s16_stereo(
            source, taken, pcm, output_frames);
    return output_frames;
}

void bt_audio_sink_flush()
{
    portENTER_CRITICAL(&s_mux);
    flush_locked();
    portEXIT_CRITICAL(&s_mux);
}

void bt_audio_sink_update()
{
    // Non-Bluetooth SystemLoop processes remote metadata/control and enqueues
    // incoming phone absolute-volume changes to the sole AudioTask.
    bt_audio_avrc_update();
    const TickType_t now = xTaskGetTickCount();
    uint32_t q=0U, rate=0U, drop=0U, under=0U, trim=0U, smooth=0U, peak=0U;
    uint64_t received=0U, consumed=0U;
    bool should_log = false;
    portENTER_CRITICAL(&s_mux);
    if (s_requested && s_streaming &&
        (s_stats_due == 0U || static_cast<int32_t>(now - s_stats_due) >= 0)) {
        s_stats_due = now + pdMS_TO_TICKS(10000);
        q = static_cast<uint32_t>(s_queued);
        rate = s_rate_hz;
        drop = s_dropped_frames;
        under = s_underrun_frames;
        received = s_received_frames;
        consumed = s_consumed_frames;
        trim = s_startup_trim_frames;
        smooth = s_smooth_adjust_frames;
        peak = s_peak_queued;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (should_log) {
        ESP_LOGI(TAG,
            "A2DP_RX rate=%luHz queued=%lu/%u peak=%lu rx=%llu played_src=%llu "
            "drop=%lu underrun=%lu start_trim=%lu smooth_extra=%lu "
            "DMA_free=%u largest=%u",
            (unsigned long)rate, (unsigned long)q, (unsigned)kRingFrames,
            (unsigned long)peak,
            (unsigned long long)received, (unsigned long long)consumed,
            (unsigned long)drop, (unsigned long)under,
            (unsigned long)trim, (unsigned long)smooth,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
}
