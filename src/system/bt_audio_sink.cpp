#include "bt_audio_sink.h"
#include "bt_audio_avrc.h"
#include "bt_pcm_drift.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_a2dp_api.h"
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#include "bt_a2dp_ext_decoder.h"
#else
#include "esp_a2dp_legacy_api.h"
#endif
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !defined(CONFIG_BT_CLASSIC_ENABLED) || !CONFIG_BT_CLASSIC_ENABLED || \
    !defined(CONFIG_BT_A2DP_ENABLE) || !CONFIG_BT_A2DP_ENABLE
#error "FakePod A2DP Sink requires Classic BT and A2DP"
#endif
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#if !defined(CONFIG_BT_A2DP_CODEC_AAC_ENABLED) || !CONFIG_BT_A2DP_CODEC_AAC_ENABLED
#error "AAC experiment must enable CONFIG_BT_A2DP_CODEC_AAC_ENABLED"
#endif
#if !defined(CONFIG_BT_A2DP_SEP_NUM_MAX) || CONFIG_BT_A2DP_SEP_NUM_MAX < 2
#error "AAC + SBC Sink requires at least two A2DP SEPs"
#endif
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
static uint8_t s_negotiated_codec = 0xFFU;
static DeviceBluetoothSinkCodec s_codec_preference = DeviceBluetoothSinkCodec::AutoAac;
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
static bool s_sep_ok[2] = {};
static bool s_sep_reported[2] = {};
static bool s_sep_advertised = false;
static bool s_sep_registration_requested = false;

static void register_aac_sbc_endpoints()
{
    // ESP-IDF release/v6.1 pre-registers both slots as SBC. Replace SEID 0
    // with preferred MPEG-2/4 AAC-LC; keep mandatory SBC in SEID 1.
    esp_a2d_mcc_t aac = {};
    aac.type = ESP_A2D_MCT_M24;
    aac.cie.m24_info.drc = ESP_A2D_M24_CIE_DRC_NS;
    aac.cie.m24_info.obj_type =
        ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC |
        ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    aac.cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_44K;
    aac.cie.m24_info.samp_freq2 = ESP_A2D_M24_CIE_SF2_48K;
    aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
    aac.cie.m24_info.vbr = ESP_A2D_M24_CIE_VBR_SUPPORT;
    aac.cie.m24_info.br1 = ESP_A2D_M24_CIE_BR1_MSK;
    aac.cie.m24_info.br2 = ESP_A2D_M24_CIE_BR2_MSK;
    aac.cie.m24_info.br3 = ESP_A2D_M24_CIE_BR3_MSK;

    esp_a2d_mcc_t sbc = {};
    sbc.type = ESP_A2D_MCT_SBC;
    sbc.cie.sbc_info.samp_freq =
        ESP_A2D_SBC_CIE_SF_44K | ESP_A2D_SBC_CIE_SF_48K;
    sbc.cie.sbc_info.ch_mode =
        ESP_A2D_SBC_CIE_CH_MODE_MONO |
        ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
        ESP_A2D_SBC_CIE_CH_MODE_STEREO |
        ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    sbc.cie.sbc_info.block_len =
        ESP_A2D_SBC_CIE_BLOCK_LEN_4 | ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
        ESP_A2D_SBC_CIE_BLOCK_LEN_12 | ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    sbc.cie.sbc_info.num_subbands =
        ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 | ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    sbc.cie.sbc_info.alloc_mthd =
        ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR | ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    sbc.cie.sbc_info.min_bitpool = 2;
    sbc.cie.sbc_info.max_bitpool = 250;

    // A2DP requires at least one SBC endpoint. Two SBC endpoints offer
    // compatibility-only mode; otherwise AAC SEP0 is preferred, SBC SEP1 fallback.
    const bool prefer_aac = s_codec_preference == DeviceBluetoothSinkCodec::AutoAac;
    const esp_err_t first_err =
        esp_a2d_sink_register_stream_endpoint(0, prefer_aac ? &aac : &sbc);
    const esp_err_t sbc_err = esp_a2d_sink_register_stream_endpoint(1, &sbc);
    ESP_LOGI(TAG, "A2DP_EXT registering SEP0 %s=%s SEP1 SBC=%s",
        prefer_aac ? "AAC-LC" : "SBC",
        esp_err_to_name(first_err), esp_err_to_name(sbc_err));
    // Wait for asynchronous SEP_REG_STATE callbacks before advertising.
}
#endif

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

#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
static void encoded_audio_cb(esp_a2d_conn_hdl_t handle, esp_a2d_audio_buff_t *packet)
{
    // Ownership of packet transfers to worker or is released immediately.
    bt_a2dp_ext_decoder_receive(handle, packet);
}
#endif

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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#endif
            break;
        }
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
        case ESP_A2D_SEP_REG_STATE_EVT: {
            const uint8_t seid = param->a2d_sep_reg_stat.seid;
            const auto status = param->a2d_sep_reg_stat.reg_state;
            bool advertise = false, fallback_sbc = false;
            portENTER_CRITICAL(&s_mux);
            if (seid < 2U) {
                s_sep_reported[seid] = true;
                s_sep_ok[seid] = status == ESP_A2D_SEP_REG_SUCCESS;
            }
            if (!s_sep_advertised && s_sep_reported[0] && s_sep_reported[1] &&
                s_sep_ok[1] && s_requested) {
                s_sep_advertised = true;
                advertise = true;
                fallback_sbc = !s_sep_ok[0];
            }
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP_EXT SEP seid=%u status=%d", seid, static_cast<int>(status));
            if (advertise) {
                ESP_LOGI(TAG, "A2DP_EXT discoverable AAC=%u SBC=1",
                    fallback_sbc ? 0U : 1U);
                const esp_err_t err = esp_bt_gap_set_scan_mode(
                    ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                if (err != ESP_OK)
                    ESP_LOGE(TAG, "A2DP_EXT discoverable failed: %s", esp_err_to_name(err));
            }
            break;
        }
#endif
        case ESP_A2D_CONNECTION_STATE_EVT: {
            const esp_a2d_connection_state_t state = param->conn_stat.state;
            portENTER_CRITICAL(&s_mux);
            s_connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
            s_streaming = false;
            s_negotiated_codec = 0xFFU;
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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
            if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
                bt_a2dp_ext_decoder_stream_state(false);
#endif
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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
            else if (param->audio_cfg.mcc.type == ESP_A2D_MCT_M24) {
                const auto &aac = param->audio_cfg.mcc.cie.m24_info;
                if (aac.samp_freq2 & ESP_A2D_M24_CIE_SF2_48K) rate = 48000U;
                else if (aac.samp_freq1 & ESP_A2D_M24_CIE_SF1_44K) rate = 44100U;
            }
            bt_a2dp_ext_decoder_configure(&param->audio_cfg.mcc);
#endif
            portENTER_CRITICAL(&s_mux);
            s_rate_hz = rate;
            s_negotiated_codec = param->audio_cfg.mcc.type;
            flush_locked();
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP Sink codec=%s PCM=%luHz/S16/stereo",
                param->audio_cfg.mcc.type == ESP_A2D_MCT_M24 ? "AAC" : "SBC",
                (unsigned long)rate);
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            const bool playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            portENTER_CRITICAL(&s_mux);
            s_streaming = playing && s_connected && s_requested;
            if (!s_streaming) flush_locked();
            portEXIT_CRITICAL(&s_mux);
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
            bt_a2dp_ext_decoder_stream_state(playing);
#endif
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
    // Read persisted intent once per profile start. Settings never directly
    // touches Bluedroid; a preference change restarts this Sink profile.
    DeviceSettingsSnapshot settings_snapshot = {};
    const bool have_settings = device_settings_get_snapshot(&settings_snapshot);
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
    s_negotiated_codec = 0xFFU;
    s_codec_preference = have_settings
        ? settings_snapshot.bluetooth_sink_codec : DeviceBluetoothSinkCodec::AutoAac;
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
    memset(s_sep_ok, 0, sizeof(s_sep_ok));
    memset(s_sep_reported, 0, sizeof(s_sep_reported));
    s_sep_advertised = false;
    s_sep_registration_requested = false;
#endif
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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
    if (!bt_a2dp_ext_decoder_start(audio_data_cb)) {
        bt_audio_avrc_host_stop();
        portENTER_CRITICAL(&s_mux);
        s_requested = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
#endif
    err = esp_bt_gap_register_callback(classic_gap_cb);
    if (err == ESP_OK) err = esp_bt_gap_set_device_name("FakePod Audio");
    if (err == ESP_OK) err = esp_a2d_register_callback(a2dp_cb);
    bool profile_start_sent = false;
    if (err == ESP_OK) {
        err = esp_a2d_sink_init();
        profile_start_sent = err == ESP_OK;
    }
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
    // Raw SBC/AAC callback; the codec worker releases each esp_a2d_audio_buff_t.
    // Advertising follows asynchronous SEP registration in PROFILE_STATE.
    if (err == ESP_OK)
        err = esp_a2d_sink_register_audio_data_callback(encoded_audio_cb);
#else
    // Original proven Bluedroid SBC decoder path, unchanged.
    if (err == ESP_OK)
        err = esp_a2d_sink_register_data_callback(audio_data_cb);
    if (err == ESP_OK)
        err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP Sink启动失败：%s", esp_err_to_name(err));
        if (profile_start_sent) {
            // A successful init request must be paired with deinit even when
            // subsequent GAP setup fails, before Source may register callbacks.
            bt_audio_sink_host_stop();
        } else {
            bt_audio_avrc_host_stop();
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
            bt_a2dp_ext_decoder_stop();
#endif
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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
    bt_a2dp_ext_decoder_stop();
#endif
}

const char *bt_audio_sink_codec_name(uint8_t negotiated_codec)
{
    if (negotiated_codec == ESP_A2D_MCT_M24) return "AAC";
    if (negotiated_codec == ESP_A2D_MCT_SBC) return "SBC";
    return "未知";
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
    s.negotiated_codec = s_negotiated_codec;
    s.preferred_codec = s_codec_preference;
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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
    bool register_seps = false;
    portENTER_CRITICAL(&s_mux);
    if (s_requested && s_profile_ready && !s_sep_registration_requested) {
        s_sep_registration_requested = true;
        register_seps = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (register_seps) register_aac_sbc_endpoints();
#endif
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
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
        const BtA2dpExtStats d = bt_a2dp_ext_decoder_stats();
        ESP_LOGI(TAG,
            "A2DP_EXT codec=%s open=%u raw=%lu raw_drop=%lu decode_err=%lu decoded_frames=%llu",
            d.codec_type == ESP_A2D_MCT_M24 ? "AAC" :
                d.codec_type == ESP_A2D_MCT_SBC ? "SBC" : "none",
            d.decoder_open ? 1U : 0U,
            (unsigned long)d.raw_packets, (unsigned long)d.raw_drop_packets,
            (unsigned long)d.decode_errors,
            (unsigned long long)d.decoded_frames);
#endif
    }
}
