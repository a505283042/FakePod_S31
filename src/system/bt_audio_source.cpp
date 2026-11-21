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
    defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE && \
    !defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC)
#define FAKEPOD_A2DP_SUPPORTED 1
#elif defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED && \
      defined(CONFIG_BT_CLASSIC_ENABLED) && CONFIG_BT_CLASSIC_ENABLED && \
      defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE && \
      defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && !CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#define FAKEPOD_A2DP_SUPPORTED 1
#else
#define FAKEPOD_A2DP_SUPPORTED 0
#endif

// This experimental branch requires A2DP. A stale sdkconfig must fail the build
// instead of flashing a firmware that silently displays 'not supported'.
#if !FAKEPOD_A2DP_SUPPORTED
#error "FakePod S31 A2DP source is disabled in generated sdkconfig. Check CONFIG_BT_CLASSIC_ENABLED=y, CONFIG_BT_A2DP_ENABLE=y, and CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=n; rebuild with sdkconfig.fakepod_s31_a2dp."
#endif

#if FAKEPOD_A2DP_SUPPORTED
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_gap_bt_api.h"
#endif

#if FAKEPOD_A2DP_SUPPORTED
static const char *TAG = "BT_AUDIO";
namespace {

// PSRAM 占用 32KB。高水位 120ms，既减缓抖动又避免无限制增加无线延迟。
static constexpr size_t kRingFrames = 8192U;  // 170ms @48kHz
static constexpr uint32_t kMaxQueueMs = 120U;
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
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const esp_a2d_connection_state_t state = param->conn_stat.state;
        portENTER_CRITICAL(&s_lock);
        s_connecting = state == ESP_A2D_CONNECTION_STATE_CONNECTING;
        s_connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        s_streaming = false;
        s_media_ctrl_pending = false;
        s_retry_due = xTaskGetTickCount() + kMediaRetryTicks;
        if (s_connected) {
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(s_peer_bda));
            if (s_candidate_found &&
                memcmp(s_candidate_bda, s_peer_bda, sizeof(s_peer_bda)) == 0) {
                set_peer_name_locked(s_candidate_name);
            }
        } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            memset(s_peer_bda, 0, sizeof(s_peer_bda));
            s_candidate_found = false; // 用户断开后下一次点击可重新搜索其它耳机。
            flush_locked();
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "A2DP connection state=%d", static_cast<int>(state));
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
        portENTER_CRITICAL(&s_lock);
        s_target_rate = rate;
        flush_locked();
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "A2DP negotiated PCM %luHz/S16/stereo", static_cast<unsigned long>(rate));
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        const bool playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        portENTER_CRITICAL(&s_lock);
        s_streaming = playing && s_connected;
        if (!s_streaming) flush_locked();
        portEXIT_CRITICAL(&s_lock);
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
        if (!ok) ESP_LOGW(TAG, "A2DP media ctrl %d failed", static_cast<int>(command));
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
    portENTER_CRITICAL(&s_lock);
    if (s_init_requested && s_streaming && s_pcm_ring != nullptr) {
        const size_t read_frames = requested < s_queued ? requested : s_queued;
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
    return len;
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
        if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) return;
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
        ESP_LOGE(TAG, "A2DP启动失败：无法分配32KB PSRAM PCM队列");
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
    err = esp_a2d_source_register_data_callback(a2dp_pcm_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP PCM回调注册失败：%s", esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_init_requested = true;
    s_profile_ready = false;
    s_deinit_confirmed = false;
    s_scanning = false;
    s_candidate_found = false;
    s_connecting = false;
    s_connected = false;
    s_streaming = false;
    s_target_rate = 44100U;
    s_best_rssi = -127;
    s_stats_due = 0;
    s_underrun = 0;
    s_dropped = 0;
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
    flush_locked();
    portEXIT_CRITICAL(&s_lock);
    if (!old_init) return;
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
        memset(s_candidate_name, 0, sizeof(s_candidate_name));
    }
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 4, 0);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_scanning = false;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
#else
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
                         !s_streaming && !s_media_ctrl_pending &&
                         static_cast<int32_t>(now - s_retry_due) >= 0;
    if (attempt) {
        s_media_ctrl_pending = true;
        s_retry_due = now + kMediaRetryTicks;
    }
    portEXIT_CRITICAL(&s_lock);
    if (log_stats) {
        ESP_LOGI(TAG, "A2DP_TX rate=%luHz queued=%u/%u frames underrun=%lu drop=%lu",
                 static_cast<unsigned long>(rate),
                 static_cast<unsigned>(buffered),
                 static_cast<unsigned>(kRingFrames),
                 static_cast<unsigned long>(underrun),
                 static_cast<unsigned long>(dropped));
    }
    if (attempt) {
        const esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_media_ctrl_pending = false;
            portEXIT_CRITICAL(&s_lock);
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
    s.underrun_frames = s_underrun;
    s.dropped_frames = s_dropped;
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
