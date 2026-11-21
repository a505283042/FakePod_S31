#include "bt_audio_source.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED && \
    defined(CONFIG_BT_CLASSIC_ENABLED) && CONFIG_BT_CLASSIC_ENABLED && \
    defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
#define FAKEPOD_A2DP_SUPPORTED 1
#else
#define FAKEPOD_A2DP_SUPPORTED 0
#endif
#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#define FAKEPOD_A2DP_EXTERNAL_CODEC 1
#else
#define FAKEPOD_A2DP_EXTERNAL_CODEC 0
#endif
#if !FAKEPOD_A2DP_SUPPORTED
#error "FakePod S31 A2DP Source requires Classic BT/A2DP. Check generated sdkconfig."
#endif

// ESP-IDF S31 has two independent options: Host Classic Bluetooth and controller
// BR/EDR + BLE dual mode. A2DP Host on a BLE-only controller can hang at
// esp_bluedroid_enable() while waiting indefinitely for the controller HCI reply.
#if !defined(CONFIG_BTDM_CTRL_MODE_BTDM) || !CONFIG_BTDM_CTRL_MODE_BTDM
#error "FakePod S31 requires CONFIG_BTDM_CTRL_MODE_BTDM=y (NOT BLE_ONLY). Regenerate sdkconfig.fakepod_s31_bt_dualmode."
#endif
#if !defined(CONFIG_BT_CTRL_BREDR_ENABLE) || !CONFIG_BT_CTRL_BREDR_ENABLE
#error "FakePod S31 controller BR/EDR disabled: check CONFIG_BTDM_CTRL_MODE_BTDM=y."
#endif
#if !defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) || CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL > 2048
#error "FakePod S31 dual-mode requires CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL<=2048 to protect SDMMC/I2S DMA. Regenerate sdkconfig.fakepod_s31_bt_dma."
#endif

#if FAKEPOD_A2DP_SUPPORTED
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"
#if FAKEPOD_A2DP_EXTERNAL_CODEC
#include "bt_a2dp_ext_encoder.h"
#include "device_settings.h"
#else
#include "esp_a2dp_legacy_api.h"
#endif
#endif

#if FAKEPOD_A2DP_SUPPORTED
static const char *TAG = "BT_AUDIO";
namespace {

// Keep the proven built-in SBC buffer unchanged. External codec workers
// need extra headroom against occasional LVGL full-screen PSRAM stalls.
#if FAKEPOD_A2DP_EXTERNAL_CODEC
static constexpr size_t kRingFrames = 16384U; // 64 KiB PSRAM, ~341ms @48kHz
static constexpr uint32_t kMaxQueueMs = 260U;
#else
static constexpr size_t kRingFrames = 8192U;  // 32 KiB, legacy SBC
static constexpr uint32_t kMaxQueueMs = 120U;
#endif
static constexpr TickType_t kMediaRetryTicks = pdMS_TO_TICKS(600);
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t *s_pcm_ring = nullptr;
static size_t s_read = 0U;
static size_t s_queued = 0U;
static uint32_t s_phase = 0U;
static uint32_t s_source_rate = 0U;
static uint32_t s_target_rate = 44100U;
static uint32_t s_underrun = 0U;
static uint32_t s_dropped = 0U;

static bool s_init_requested = false;
static bool s_profile_ready = false;
static bool s_scanning = false;
static bool s_candidate_found = false;
static BtAudioSourceDevice s_devices[BT_AUDIO_SOURCE_MAX_DEVICES] = {};
static uint8_t s_device_count = 0U;
static uint32_t s_discovery_revision = 0U;
static bool s_connecting = false;
static bool s_connected = false;
static bool s_streaming = false;
static bool s_media_ctrl_pending = false;
static bool s_deinit_confirmed = false;
static TickType_t s_retry_due = 0U;
static TickType_t s_stats_due = 0U;
static esp_bd_addr_t s_candidate_bda = {};
static esp_bd_addr_t s_peer_bda = {};
static char s_candidate_name[48] = {};
static char s_peer_name[48] = {};
static int8_t s_best_rssi = -127;
static uint8_t s_negotiated_codec = 0xFFU;
#if FAKEPOD_A2DP_EXTERNAL_CODEC
static esp_a2d_conn_hdl_t s_conn_hdl = 0U;
static uint16_t s_audio_mtu = 0U;
static esp_a2d_mcc_t s_negotiated_mcc = {};
static bool s_mcc_ready = false;
static bool s_sep_registration_requested = false;
static bool s_sep_reported[2] = {};
static bool s_sep_ok[2] = {};
static bool s_sep_ready = false;
static DeviceBluetoothSinkCodec s_codec_preference = DeviceBluetoothSinkCodec::AutoAac;
static TickType_t s_encoder_retry_due = 0;

static void register_source_endpoints()
{
    esp_a2d_mcc_t sbc = {};
    sbc.type = ESP_A2D_MCT_SBC;
    sbc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K | ESP_A2D_SBC_CIE_SF_48K;
    sbc.cie.sbc_info.ch_mode =
        ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL | ESP_A2D_SBC_CIE_CH_MODE_STEREO |
        ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    sbc.cie.sbc_info.block_len =
        ESP_A2D_SBC_CIE_BLOCK_LEN_4 | ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
        ESP_A2D_SBC_CIE_BLOCK_LEN_12 | ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    sbc.cie.sbc_info.num_subbands =
        ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 | ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    sbc.cie.sbc_info.alloc_mthd =
        ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR | ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    sbc.cie.sbc_info.min_bitpool = 2U;
    sbc.cie.sbc_info.max_bitpool = 53U;

    esp_a2d_mcc_t aac = {};
    aac.type = ESP_A2D_MCT_M24;
    aac.cie.m24_info.drc = ESP_A2D_M24_CIE_DRC_NS;
    aac.cie.m24_info.obj_type =
        ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC | ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
    aac.cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_44K;
    aac.cie.m24_info.samp_freq2 = ESP_A2D_M24_CIE_SF2_48K;
    aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
    aac.cie.m24_info.vbr = ESP_A2D_M24_CIE_VBR_NS;
    aac.cie.m24_info.br1 = 0x02U & ESP_A2D_M24_CIE_BR1_MSK;
    aac.cie.m24_info.br2 = 0x71U & ESP_A2D_M24_CIE_BR2_MSK;
    aac.cie.m24_info.br3 = 0x00U & ESP_A2D_M24_CIE_BR3_MSK;

    const bool prefer_aac = s_codec_preference == DeviceBluetoothSinkCodec::AutoAac;
    const esp_err_t first = esp_a2d_source_register_stream_endpoint(
        0U, prefer_aac ? &aac : &sbc);
    const esp_err_t fallback = esp_a2d_source_register_stream_endpoint(1U, &sbc);
    ESP_LOGI(TAG, "A2DP_TX SEPs 0=%s (%s) 1=SBC (%s)",
        prefer_aac ? "AAC" : "SBC", esp_err_to_name(first), esp_err_to_name(fallback));
}
#endif

static void flush_locked()
{
    s_read = 0U;
    s_queued = 0U;
    s_phase = 0U;
    s_source_rate = 0U;
}

static void set_peer_name_locked(const char *name)
{
    snprintf(s_peer_name, sizeof(s_peer_name), "%s",
             name != nullptr && name[0] != '\0' ? name : "蓝牙音频设备");
}

static void discovery_revision_bump_locked()
{
    if (++s_discovery_revision == 0U) ++s_discovery_revision;
}

static void remember_audio_device_locked(const uint8_t address[6], const char *name, int8_t rssi)
{
    if (!s_scanning || address == nullptr) return;
    int slot = -1;
    for (uint8_t i = 0; i < s_device_count; ++i) {
        if (memcmp(s_devices[i].address, address, 6U) == 0) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0 && s_device_count < BT_AUDIO_SOURCE_MAX_DEVICES)
        slot = static_cast<int>(s_device_count++);
    if (slot < 0) {
        // Keep stronger receivers if more than 12 are discovered.
        int weakest = 0;
        for (uint8_t i = 1; i < s_device_count; ++i)
            if (s_devices[i].rssi < s_devices[weakest].rssi) weakest = i;
        if (rssi <= s_devices[weakest].rssi) return;
        slot = weakest;
    }
    BtAudioSourceDevice &device = s_devices[slot];
    const bool same = memcmp(device.address, address, 6U) == 0;
    bool changed = !same;
    if (!same) {
        device = {};
        memcpy(device.address, address, 6U);
        device.rssi = rssi;
    } else if (rssi > device.rssi + 3) {
        // Small RSSI fluctuations should not rebuild the entire LVGL list.
        device.rssi = rssi;
        changed = true;
    }
    if (name != nullptr && name[0] != '\0' &&
        strcmp(device.name, name) != 0) {
        snprintf(device.name, sizeof(device.name), "%s", name);
        changed = true;
    }
    if (device.name[0] == '\0') {
        snprintf(device.name, sizeof(device.name), "未命名音频设备");
        changed = true;
    }
    if (changed) discovery_revision_bump_locked();
}

static void a2dp_event(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (param == nullptr) return;
    switch (event) {
    case ESP_A2D_PROF_STATE_EVT: {
        const bool ready = param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS;
        portENTER_CRITICAL(&s_lock);
        s_profile_ready = ready && s_init_requested;
        if (!ready) s_deinit_confirmed = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "A2DP profile %s", ready ? "ready" : "stopped");
        break;
    }
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    case ESP_A2D_SEP_REG_STATE_EVT: {
        const uint8_t seid = param->a2d_sep_reg_stat.seid;
        const bool ok = param->a2d_sep_reg_stat.reg_state == ESP_A2D_SEP_REG_SUCCESS;
        portENTER_CRITICAL(&s_lock);
        if (seid < 2U) {
            s_sep_reported[seid] = true;
            s_sep_ok[seid] = ok;
            // Successful mandatory SBC registration is sufficient to start.
            // An optional AAC endpoint failure must not block audio forever.
            s_sep_ready = s_sep_reported[1] && s_sep_ok[1];
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "A2DP_TX SEP seid=%u registered=%u", unsigned(seid), unsigned(ok));
        break;
    }
#endif
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const esp_a2d_connection_state_t state = param->conn_stat.state;
        portENTER_CRITICAL(&s_lock);
        s_connecting = state == ESP_A2D_CONNECTION_STATE_CONNECTING;
        s_connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        s_streaming = false;
        s_media_ctrl_pending = false;
        s_retry_due = xTaskGetTickCount() + kMediaRetryTicks;
        // ESP-IDF may deliver AUDIO_CFG before CONNECTION_STATE_CONNECTED.
        // Never erase the negotiated codec at CONNECTED: external encoder
        // startup must retain the earlier AUDIO_CFG_EVT's s_mcc_ready=true.
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTING ||
            state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_negotiated_codec = 0xFFU;
#if FAKEPOD_A2DP_EXTERNAL_CODEC
            s_negotiated_mcc = {};
            s_mcc_ready = false;
#endif
        }
        discovery_revision_bump_locked();
#if FAKEPOD_A2DP_EXTERNAL_CODEC
        s_conn_hdl = s_connected ? param->conn_stat.conn_hdl : 0U;
        s_audio_mtu = s_connected ? param->conn_stat.audio_mtu : 0U;
        const bool codec_ready_on_event = s_mcc_ready;
#endif
        if (s_connected) {
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(s_peer_bda));
            if (s_candidate_found &&
                memcmp(s_candidate_bda, s_peer_bda, sizeof(s_peer_bda)) == 0) {
                set_peer_name_locked(s_candidate_name);
            }
        } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            memset(s_peer_bda, 0, sizeof(s_peer_bda));
            s_candidate_found = false; // Retain scan list for manual reselection.
            discovery_revision_bump_locked();
            flush_locked();
        }
        portEXIT_CRITICAL(&s_lock);
#if FAKEPOD_A2DP_EXTERNAL_CODEC
        if (state != ESP_A2D_CONNECTION_STATE_CONNECTED)
            bt_a2dp_ext_encoder_request_stop();
#endif
#if FAKEPOD_A2DP_EXTERNAL_CODEC
        ESP_LOGI(TAG, "A2DP connection state=%d conn=%u mtu=%u codec_ready=%u",
            static_cast<int>(state),
            static_cast<unsigned>(param->conn_stat.conn_hdl),
            static_cast<unsigned>(param->conn_stat.audio_mtu),
            static_cast<unsigned>(codec_ready_on_event));
#else
        ESP_LOGI(TAG, "A2DP connection state=%d", static_cast<int>(state));
#endif
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
        } else if (param->audio_cfg.mcc.type == ESP_A2D_MCT_M24) {
            const esp_a2d_cie_m24_t &aac = param->audio_cfg.mcc.cie.m24_info;
            if (aac.samp_freq2 & ESP_A2D_M24_CIE_SF2_48K) rate = 48000U;
            else if (aac.samp_freq1 & ESP_A2D_M24_CIE_SF1_44K) rate = 44100U;
        }
        portENTER_CRITICAL(&s_lock);
        s_target_rate = rate;
        s_negotiated_codec = param->audio_cfg.mcc.type;
#if FAKEPOD_A2DP_EXTERNAL_CODEC
        s_negotiated_mcc = param->audio_cfg.mcc;
        s_mcc_ready = true;
#endif
        flush_locked();
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "A2DP_TX negotiated codec=%s PCM=%luHz/S16/stereo",
            param->audio_cfg.mcc.type == ESP_A2D_MCT_M24 ? "AAC" : "SBC",
            static_cast<unsigned long>(rate));
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        const bool playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        portENTER_CRITICAL(&s_lock);
        s_streaming = playing && s_connected;
        if (!s_streaming) flush_locked();
        portEXIT_CRITICAL(&s_lock);
#if FAKEPOD_A2DP_EXTERNAL_CODEC
        if (!playing) bt_a2dp_ext_encoder_request_stop();
#endif
        ESP_LOGI(TAG, "A2DP audio %s", playing ? "STARTED" : "SUSPENDED");
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        const esp_a2d_media_ctrl_t command = param->media_ctrl_stat.cmd;
        const bool ok = param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
        bool start = false;
        portENTER_CRITICAL(&s_lock);
        s_media_ctrl_pending = false;
        if (s_connected && command == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && ok) start = true;
        s_retry_due = xTaskGetTickCount() + kMediaRetryTicks;
        portEXIT_CRITICAL(&s_lock);
        if (start) {
            const esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            if (err == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s_media_ctrl_pending = true;
                portEXIT_CRITICAL(&s_lock);
            }
        }
        ESP_LOGI(TAG, "A2DP_TX media ctrl ACK cmd=%d result=%s",
            static_cast<int>(command), ok ? "OK" : "FAIL");
        break;
    }
    default: break;
    }
}

// 在 BT source task 内执行，不得等待 AudioTask 或访问 SD/GUI。
static int32_t a2dp_pcm_callback(uint8_t *data, int32_t len)
{
    if (len == -1) {
        portENTER_CRITICAL(&s_lock);
        flush_locked();
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }
    if (data == nullptr || len <= 0) return 0;
    memset(data, 0, static_cast<size_t>(len));
    const size_t requested = static_cast<size_t>(len) / 4U;
    size_t consumed_frames = 0U;
    portENTER_CRITICAL(&s_lock);
    if (s_init_requested && s_streaming && s_pcm_ring != nullptr) {
        const size_t read_frames = requested < s_queued ? requested : s_queued;
        consumed_frames = read_frames;
        const size_t first = read_frames < (kRingFrames - s_read)
            ? read_frames : kRingFrames - s_read;
        memcpy(data, s_pcm_ring + s_read * 4U, first * 4U);
        if (read_frames > first) {
            memcpy(data + first * 4U, s_pcm_ring, (read_frames - first) * 4U);
        }
        s_read = (s_read + read_frames) % kRingFrames;
        s_queued -= read_frames;
        s_underrun += static_cast<uint32_t>(requested - read_frames);
    }
    portEXIT_CRITICAL(&s_lock);
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    // Worker knows whether its PCM was real or silence padding. The output
    // remains zero-filled so the encoder always receives a complete frame.
    return static_cast<int32_t>(consumed_frames * 4U);
#else
    // ESP-IDF's legacy built-in SBC callback requires the full byte count.
    return len;
#endif
}

static void classic_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (param == nullptr) return;
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        uint32_t cod = 0U;
        int8_t rssi = -127;
        char name[48] = {};
        uint8_t *eir = nullptr;
        for (int i = 0; i < param->disc_res.num_prop; ++i) {
            const esp_bt_gap_dev_prop_t &prop = param->disc_res.prop[i];
            if (prop.val == nullptr) continue;
            switch (prop.type) {
                case ESP_BT_GAP_DEV_PROP_COD: memcpy(&cod, prop.val, sizeof(cod)); break;
                case ESP_BT_GAP_DEV_PROP_RSSI: memcpy(&rssi, prop.val, sizeof(rssi)); break;
                case ESP_BT_GAP_DEV_PROP_EIR: eir = static_cast<uint8_t *>(prop.val); break;
                case ESP_BT_GAP_DEV_PROP_BDNAME: {
                    if (prop.len <= 0) break;
                    const size_t n = static_cast<size_t>(prop.len) < sizeof(name) - 1U
                        ? static_cast<size_t>(prop.len) : sizeof(name) - 1U;
                    memcpy(name, prop.val, n);
                    name[n] = '\0';
                    break;
                }
                default: break;
            }
        }
        // Some headphones advertise A/V class without the optional rendering
        // service bit. Keep both, while excluding phones/keyboard peripherals.
        const bool audio_class = esp_bt_gap_is_valid_cod(cod) &&
            ((esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING) ||
             esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV);
        if (!audio_class) {
            // ESP-IDF may report a device name in a separate inquiry result
            // without CoD. Update names only for addresses already classified
            // as audio renderers, never create a non-audio entry.
            if (name[0] == '\0') return;
            portENTER_CRITICAL(&s_lock);
            for (uint8_t i = 0U; i < s_device_count; ++i) {
                if (memcmp(s_devices[i].address, param->disc_res.bda, 6U) == 0) {
                    remember_audio_device_locked(param->disc_res.bda, name, rssi);
                    break;
                }
            }
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        if (name[0] == '\0' && eir != nullptr) {
            uint8_t n = 0;
            uint8_t *p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &n);
            if (p == nullptr) p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &n);
            if (p != nullptr) {
                const size_t size = n < sizeof(name) - 1U ? n : sizeof(name) - 1U;
                memcpy(name, p, size);
                name[size] = '\0';
            }
        }
        portENTER_CRITICAL(&s_lock);
        remember_audio_device_locked(param->disc_res.bda, name, rssi);
        if (s_scanning && (!s_candidate_found || rssi > s_best_rssi)) {
            s_candidate_found = true;
            s_best_rssi = rssi;
            memcpy(s_candidate_bda, param->disc_res.bda, sizeof(s_candidate_bda));
            snprintf(s_candidate_name, sizeof(s_candidate_name), "%s", name[0] ? name : "未命名音频设备");
        }
        portEXIT_CRITICAL(&s_lock);
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            bool found;
            char name[48];
            portENTER_CRITICAL(&s_lock);
            s_scanning = false;
            discovery_revision_bump_locked();
            found = s_candidate_found;
            memcpy(name, s_candidate_name, sizeof(name));
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "A2DP scan finished: found=%d peer=%s", found, found ? name : "-");
        }
    } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        // 耳机/音箱常见的无输入设备 Just Works 配对。
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    }
}

} // namespace
#endif // FAKEPOD_A2DP_SUPPORTED

esp_err_t bt_audio_source_host_start()
{
#if FAKEPOD_A2DP_SUPPORTED
    portENTER_CRITICAL(&s_lock);
    if (s_init_requested) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    if (s_pcm_ring == nullptr) {
        s_pcm_ring = static_cast<uint8_t *>(heap_caps_malloc(
            kRingFrames * 4U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_pcm_ring == nullptr) {
        ESP_LOGE(TAG, "A2DP启动失败：无法分配%uB PSRAM PCM队列",
                static_cast<unsigned>(kRingFrames * 4U));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "A2DP启动：internal_free=%u psram_free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    esp_err_t err = esp_bt_gap_register_callback(classic_gap_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP GAP回调注册失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_gap_set_device_name("FakePod Audio");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP设备名设置失败：%s", esp_err_to_name(err));
        return err;
    }
    // 仍保留 BLE 广播：Classic 本机不作为可发现的音频接收端。
    err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP Classic扫描模式设置失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_a2d_register_callback(a2dp_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP Profile回调注册失败：%s", esp_err_to_name(err));
        return err;
    }
#if !FAKEPOD_A2DP_EXTERNAL_CODEC
    err = esp_a2d_source_register_data_callback(a2dp_pcm_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP PCM回调注册失败：%s", esp_err_to_name(err));
        return err;
    }
#endif
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    DeviceBluetoothSinkCodec preference = DeviceBluetoothSinkCodec::AutoAac;
    DeviceSettingsSnapshot settings = {};
    if (device_settings_get_snapshot(&settings)) preference = settings.bluetooth_sink_codec;
#endif
    portENTER_CRITICAL(&s_lock);
    s_init_requested = true;
    s_profile_ready = false;
    s_deinit_confirmed = false;
    s_scanning = false;
    s_candidate_found = false;
    s_device_count = 0U;
    for (BtAudioSourceDevice &device : s_devices) device = BtAudioSourceDevice{};
    discovery_revision_bump_locked();
    s_connecting = false;
    s_connected = false;
    s_streaming = false;
    s_target_rate = 44100U;
    s_best_rssi = -127;
    s_stats_due = 0;
    s_underrun = 0;
    s_dropped = 0;
    s_negotiated_codec = 0xFFU;
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    s_conn_hdl = 0U;
    s_audio_mtu = 0U;
    s_mcc_ready = false;
    s_sep_registration_requested = false;
    memset(s_sep_reported, 0, sizeof(s_sep_reported));
    memset(s_sep_ok, 0, sizeof(s_sep_ok));
    s_sep_ready = false;
    s_codec_preference = preference;
    s_encoder_retry_due = 0;
#endif
    flush_locked();
    portEXIT_CRITICAL(&s_lock);

    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_init_requested = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "A2DP Source初始化失败：%s", esp_err_to_name(err));
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void bt_audio_source_host_stop()
{
#if FAKEPOD_A2DP_SUPPORTED
    bool old_init;
    bool scanning;
    bool connected;
    portENTER_CRITICAL(&s_lock);
    old_init = s_init_requested;
    scanning = s_scanning;
    connected = s_connected || s_connecting;
    s_init_requested = false;
    s_streaming = false;
    s_connected = false;
    s_connecting = false;
    s_scanning = false;
    s_candidate_found = false;
    s_device_count = 0U;
    discovery_revision_bump_locked();
    flush_locked();
    portEXIT_CRITICAL(&s_lock);
    if (!old_init) return;
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    if (!bt_a2dp_ext_encoder_stop())
        ESP_LOGE(TAG, "A2DP Source encoder stop timeout");
#endif
    if (scanning) (void)esp_bt_gap_cancel_discovery();
    if (connected) (void)esp_a2d_source_disconnect(s_peer_bda);
    const esp_err_t err = esp_a2d_source_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "A2DP Source deinit: %s", esp_err_to_name(err));
    } else {
        // A2DP deinit 是异步的；等 Profile 销毁回调再关闭共享 Bluedroid Host。
        const TickType_t begin = xTaskGetTickCount();
        bool confirmed = false;
        do {
            portENTER_CRITICAL(&s_lock);
            confirmed = s_deinit_confirmed;
            portEXIT_CRITICAL(&s_lock);
            if (confirmed) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while ((xTaskGetTickCount() - begin) < pdMS_TO_TICKS(1200));
        if (!confirmed) ESP_LOGW(TAG, "A2DP Profile停止确认超时");
    }
    // PSRAM buffer is kept across Host restarts to prevent callback/free races.
#endif
}

esp_err_t bt_audio_source_scan()
{
#if FAKEPOD_A2DP_SUPPORTED
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_init_requested && s_profile_ready &&
                         !s_connected && !s_connecting && !s_scanning;
    if (allowed) {
        s_candidate_found = false;
        s_scanning = true;
        s_best_rssi = -127;
        s_device_count = 0U;
        for (BtAudioSourceDevice &device : s_devices) device = BtAudioSourceDevice{};
        memset(s_candidate_name, 0, sizeof(s_candidate_name));
        discovery_revision_bump_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 4, 0);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_scanning = false;
        discovery_revision_bump_locked();
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool bt_audio_source_get_device_list(BtAudioSourceDeviceList *out)
{
    if (out == nullptr) return false;
    BtAudioSourceDeviceList snapshot = {};
#if FAKEPOD_A2DP_SUPPORTED
    portENTER_CRITICAL(&s_lock);
    snapshot.revision = s_discovery_revision;
    snapshot.scanning = s_scanning;
    snapshot.count = s_device_count;
    memcpy(snapshot.devices, s_devices, sizeof(s_devices));
    portEXIT_CRITICAL(&s_lock);
    // Sort only the snapshot: callback-owned entries remain stable in memory.
    for (uint8_t i = 0U; i < snapshot.count; ++i) {
        for (uint8_t j = i + 1U; j < snapshot.count; ++j) {
            if (snapshot.devices[j].rssi > snapshot.devices[i].rssi) {
                BtAudioSourceDevice tmp = snapshot.devices[i];
                snapshot.devices[i] = snapshot.devices[j];
                snapshot.devices[j] = tmp;
            }
        }
    }
#endif
    *out = snapshot;
    return true;
}

esp_err_t bt_audio_source_connect_device(const uint8_t address[6])
{
#if FAKEPOD_A2DP_SUPPORTED
    if (address == nullptr) return ESP_ERR_INVALID_ARG;
    esp_bd_addr_t target = {};
    char name[48] = {};
    bool allowed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_init_requested && s_profile_ready &&
        !s_scanning && !s_connected && !s_connecting) {
        for (uint8_t i = 0U; i < s_device_count; ++i) {
            if (memcmp(s_devices[i].address, address, sizeof(target)) == 0) {
                memcpy(target, s_devices[i].address, sizeof(target));
                snprintf(name, sizeof(name), "%s", s_devices[i].name);
                allowed = true;
                break;
            }
        }
        if (allowed) {
            memcpy(s_peer_bda, target, sizeof(target));
            memcpy(s_candidate_bda, target, sizeof(target));
            snprintf(s_candidate_name, sizeof(s_candidate_name), "%s", name);
            s_candidate_found = true;
            set_peer_name_locked(name);
            s_connecting = true;
            discovery_revision_bump_locked();
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_a2d_source_connect(target);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_connecting = false;
        discovery_revision_bump_locked();
        portEXIT_CRITICAL(&s_lock);
    }
    ESP_LOGI(TAG, "A2DP selected %02X:%02X:%02X:%02X:%02X:%02X (%s): %s",
             static_cast<unsigned>(target[0]), static_cast<unsigned>(target[1]),
             static_cast<unsigned>(target[2]), static_cast<unsigned>(target[3]),
             static_cast<unsigned>(target[4]), static_cast<unsigned>(target[5]),
             name, esp_err_to_name(err));
    return err;
#else
    (void)address;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bt_audio_source_connect_candidate()
{
#if FAKEPOD_A2DP_SUPPORTED
    esp_bd_addr_t target = {};
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_init_requested && s_profile_ready &&
                         s_candidate_found && !s_scanning && !s_connected && !s_connecting;
    if (allowed) {
        memcpy(target, s_candidate_bda, sizeof(target));
        memcpy(s_peer_bda, target, sizeof(target));
        set_peer_name_locked(s_candidate_name);
        s_connecting = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_a2d_source_connect(target);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_connecting = false;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bt_audio_source_disconnect()
{
#if FAKEPOD_A2DP_SUPPORTED
    esp_bd_addr_t peer = {};
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_init_requested && (s_connected || s_connecting);
    if (allowed) memcpy(peer, s_peer_bda, sizeof(peer));
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    return esp_a2d_source_disconnect(peer);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void bt_audio_source_update()
{
#if FAKEPOD_A2DP_SUPPORTED
    const TickType_t now = xTaskGetTickCount();
    bool log_stats = false;
    uint32_t rate = 0U;
    uint32_t underrun = 0U;
    uint32_t dropped = 0U;
    size_t buffered = 0U;
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    bool register_endpoints = false;
    portENTER_CRITICAL(&s_lock);
    if (s_init_requested && s_profile_ready && !s_sep_registration_requested) {
        s_sep_registration_requested = true;
        register_endpoints = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (register_endpoints) register_source_endpoints();
#endif
    portENTER_CRITICAL(&s_lock);
    // 兼容个别耳机丢失 CHECK_SRC_RDY/START ACK 的情况：超时后允许重试。
    if (s_media_ctrl_pending && s_connected &&
        static_cast<int32_t>(now - (s_retry_due + pdMS_TO_TICKS(1500))) >= 0) {
        s_media_ctrl_pending = false;
    }
    if (s_init_requested && s_streaming &&
        (s_stats_due == 0U || static_cast<int32_t>(now - s_stats_due) >= 0)) {
        s_stats_due = now + pdMS_TO_TICKS(10000);
        rate = s_target_rate;
        underrun = s_underrun;
        dropped = s_dropped;
        buffered = s_queued;
        log_stats = true;
    }
    const bool attempt = s_init_requested && s_profile_ready && s_connected &&
#if FAKEPOD_A2DP_EXTERNAL_CODEC
                         s_sep_ready &&
#endif
                         !s_streaming && !s_media_ctrl_pending &&
                         static_cast<int32_t>(now - s_retry_due) >= 0;
    if (attempt) {
        s_media_ctrl_pending = true;
        s_retry_due = now + kMediaRetryTicks;
    }
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    const bool need_worker = s_init_requested && s_streaming && s_connected &&
        s_sep_ready && s_mcc_ready && s_conn_hdl != 0U && s_audio_mtu > 100U &&
        (s_encoder_retry_due == 0U ||
         static_cast<int32_t>(now - s_encoder_retry_due) >= 0);
    const esp_a2d_conn_hdl_t conn = s_conn_hdl;
    const uint16_t mtu = s_audio_mtu;
    const uint32_t codec_rate = s_target_rate;
    const esp_a2d_mcc_t codec = s_negotiated_mcc;
#endif
    portEXIT_CRITICAL(&s_lock);
#if FAKEPOD_A2DP_EXTERNAL_CODEC
    if (need_worker && !bt_a2dp_ext_encoder_is_running()) {
        const esp_err_t err = bt_a2dp_ext_encoder_start(
            conn, mtu, &codec, codec_rate, a2dp_pcm_callback);
        // If codec initialization fails inside the new worker, it can exit
        // immediately after task creation. Always rate-limit relaunches.
        portENTER_CRITICAL(&s_lock);
        s_encoder_retry_due = now + pdMS_TO_TICKS(err == ESP_OK ? 250 : 1500);
        portEXIT_CRITICAL(&s_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "A2DP Source编码器启动失败：%s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "A2DP_TX encoder task created conn=%u mtu=%u codec=%s rate=%lu",
                static_cast<unsigned>(conn), static_cast<unsigned>(mtu),
                codec.type == ESP_A2D_MCT_M24 ? "AAC" : "SBC",
                static_cast<unsigned long>(codec_rate));
        }
    }
#endif
    if (log_stats) {
        ESP_LOGI(TAG, "A2DP_TX rate=%luHz queued=%u/%u frames underrun=%lu drop=%lu DMA_free=%u DMA_largest=%u",
                 static_cast<unsigned long>(rate),
                 static_cast<unsigned>(buffered),
                 static_cast<unsigned>(kRingFrames),
                 static_cast<unsigned long>(underrun),
                 static_cast<unsigned long>(dropped),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    }
    if (attempt) {
        const esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_media_ctrl_pending = false;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "A2DP_TX CHECK_SRC_RDY request failed: %s", esp_err_to_name(err));
        }
    }
#endif
}

bool bt_audio_source_get_snapshot(BtAudioSourceSnapshot *out)
{
    if (out == nullptr) return false;
    BtAudioSourceSnapshot s = {};
#if FAKEPOD_A2DP_SUPPORTED
    s.available = true;
    portENTER_CRITICAL(&s_lock);
    s.initialized = s_profile_ready && s_init_requested;
    s.scanning = s_scanning;
    s.candidate_found = s_candidate_found;
    s.connecting = s_connecting;
    s.connected = s_connected;
    s.streaming = s_streaming;
    s.sample_rate_hz = s_target_rate;
    s.negotiated_codec = s_connected ? s_negotiated_codec : 0xFFU;
    s.underrun_frames = s_underrun;
    s.dropped_frames = s_dropped;
    s.queued_frames = static_cast<uint32_t>(s_queued);
    memcpy(s.peer_name, s_connected || s_connecting ? s_peer_name : s_candidate_name,
           sizeof(s.peer_name));
    portEXIT_CRITICAL(&s_lock);
#endif
    *out = s;
    return true;
}

void bt_audio_source_flush()
{
#if FAKEPOD_A2DP_SUPPORTED
    portENTER_CRITICAL(&s_lock);
    flush_locked();
    portEXIT_CRITICAL(&s_lock);
#endif
}

void bt_audio_source_feed_pcm32(const int32_t *pcm, size_t frames,
                                uint32_t source_rate_hz, uint8_t volume_percent)
{
#if FAKEPOD_A2DP_SUPPORTED
    if (pcm == nullptr || frames == 0U || source_rate_hz < 8000U ||
        source_rate_hz > 192000U) return;
    if (volume_percent > 100U) volume_percent = 100U;
    // Producer: AudioTask. 不分配内存、不等待 BT、只在锁内做有界的 PCM 拷贝。
    portENTER_CRITICAL(&s_lock);
    if (!s_init_requested || !s_streaming || s_pcm_ring == nullptr) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (s_source_rate != source_rate_hz) {
        flush_locked();
        s_source_rate = source_rate_hz;
    }
    const size_t max_frames = static_cast<size_t>(s_target_rate * kMaxQueueMs / 1000U);
    for (size_t i = 0U; i < frames; ++i) {
        s_phase += s_target_rate;
        while (s_phase >= source_rate_hz) {
            s_phase -= source_rate_hz;
            if (s_queued >= kRingFrames || s_queued >= max_frames) {
                s_read = (s_read + 1U) % kRingFrames;
                --s_queued;
                ++s_dropped;
            }
            const size_t dst = (s_read + s_queued) % kRingFrames;
            // PCM32 left-justified: arith shift yields S16; 64-bit avoids signed overflow.
            for (size_t ch = 0U; ch < 2U; ++ch) {
                int32_t sample = (pcm[i * 2U + ch] >> 16);
                sample = static_cast<int32_t>((static_cast<int64_t>(sample) *
                                               volume_percent) / 100);
                const int16_t s16 = static_cast<int16_t>(sample);
                memcpy(s_pcm_ring + dst * 4U + ch * 2U, &s16, 2U);
            }
            ++s_queued;
        }
    }
    portEXIT_CRITICAL(&s_lock);
#else
    (void)pcm; (void)frames; (void)source_rate_hz; (void)volume_percent;
#endif
}
