#include "bt_a2dp_ext_encoder.h"
#include "sdkconfig.h"

#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC

#include <algorithm>
#include <limits.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_audio_enc.h"
#include "esp_aac_enc.h"
#include "esp_sbc_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace {
static const char *TAG = "BT_TX_ENC";
static constexpr uint32_t kTaskStack = 8192U;
static constexpr UBaseType_t kTaskPriority = 6U;
static constexpr BaseType_t kTaskCore = 1;
static constexpr uint32_t kMaxPcmBytes = 16384U;
static constexpr uint32_t kMaxEncodedBytes = 8192U;

struct Stream {
    esp_a2d_conn_hdl_t connection = 0U;
    uint16_t mtu = 0U;
    esp_a2d_mcc_t codec = {};
    uint32_t sample_rate = 0U;
    BtA2dpPcmReader reader = nullptr;
};
static Stream s_stream = {};
static TaskHandle_t s_task = nullptr;
static SemaphoreHandle_t s_finished = nullptr;
static volatile bool s_run = false;

static bool open_codec(const Stream &stream, void **out_handle, int *in_bytes,
                       int *out_bytes, uint16_t *frames_per_packet)
{
    if (stream.codec.type == ESP_A2D_MCT_M24) {
        esp_aac_enc_config_t config = {};
        config.sample_rate = static_cast<int>(stream.sample_rate);
        config.channel = ESP_AUDIO_DUAL;
        config.bits_per_sample = ESP_AUDIO_BIT16;
        config.bitrate = 128000;
        config.adts_used = false; // A2DP inserts LATM; no ADTS header.
        if (esp_aac_enc_open(&config, sizeof(config), out_handle) != ESP_AUDIO_ERR_OK)
            return false;
        if (esp_aac_enc_get_frame_size(*out_handle, in_bytes, out_bytes) != ESP_AUDIO_ERR_OK)
            return false;
        *frames_per_packet = 1U;
    } else if (stream.codec.type == ESP_A2D_MCT_SBC) {
        const esp_a2d_cie_sbc_t &peer = stream.codec.cie.sbc_info;
        esp_sbc_enc_config_t config = {};
        config.sbc_mode = ESP_SBC_MODE_STD;
        config.allocation_method = (peer.alloc_mthd & ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS)
            ? ESP_SBC_ALLOC_LOUDNESS : ESP_SBC_ALLOC_SNR;
        config.ch_mode = (peer.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO)
            ? ESP_SBC_CH_MODE_JOINT_STEREO :
            (peer.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_STEREO)
                ? ESP_SBC_CH_MODE_STEREO : ESP_SBC_CH_MODE_DUAL;
        config.sample_rate = stream.sample_rate;
        config.bits_per_sample = 16U;
        config.block_length = (peer.block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_16) ? 16U :
            (peer.block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_12) ? 12U :
            (peer.block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_8) ? 8U : 4U;
        config.sub_bands_num = (peer.num_subbands & ESP_A2D_SBC_CIE_NUM_SUBBANDS_8)
            ? 8U : 4U;
        if (peer.max_bitpool < peer.min_bitpool) return false;
        config.bitpool = static_cast<uint16_t>(std::min<unsigned>(
            peer.max_bitpool, std::max<unsigned>(peer.min_bitpool, 35U)));
        if (esp_sbc_enc_open(&config, sizeof(config), out_handle) != ESP_AUDIO_ERR_OK)
            return false;
        if (esp_sbc_enc_get_frame_size(*out_handle, in_bytes, out_bytes) != ESP_AUDIO_ERR_OK)
            return false;
        // Pack several SBC frames per RTP payload, but never exceed the
        // negotiated audio MTU or the 4-bit SBC A2DP frame-count field.
        const int max_packet = static_cast<int>(stream.mtu) - 8;
        if (*out_bytes <= 0 || max_packet < *out_bytes) return false;
        *frames_per_packet = static_cast<uint16_t>(
            std::min(4, max_packet / *out_bytes));
    } else {
        return false;
    }
    return *in_bytes > 0 && *out_bytes > 0 && *frames_per_packet > 0U;
}

static void close_codec(uint8_t codec_type, void *handle)
{
    if (handle == nullptr) return;
    if (codec_type == ESP_A2D_MCT_M24) esp_aac_enc_close(handle);
    else if (codec_type == ESP_A2D_MCT_SBC) esp_sbc_enc_close(handle);
}

static void worker(void *)
{
    const Stream stream = s_stream;
    void *encoder = nullptr;
    uint8_t *pcm = nullptr;
    uint8_t *encoded = nullptr;
    int in_bytes = 0, out_bytes = 0;
    uint16_t frames_per_packet = 0U;
    uint32_t timestamp = 0U;
    uint32_t sent = 0U, failed = 0U, underrun = 0U;
    uint32_t max_lateness_us = 0U, late_over_40ms = 0U, late_over_80ms = 0U;
    uint32_t max_encode_us = 0U, max_send_us = 0U;
    const bool ready = open_codec(stream, &encoder, &in_bytes, &out_bytes,
                                  &frames_per_packet);
    if (!ready) {
        ESP_LOGE(TAG, "Codec open failed type=%u rate=%lu", unsigned(stream.codec.type),
            static_cast<unsigned long>(stream.sample_rate));
        goto finish;
    }

    {
        const uint32_t pcm_size = static_cast<uint32_t>(in_bytes) * frames_per_packet;
        const uint32_t encoded_size = static_cast<uint32_t>(out_bytes) * frames_per_packet;
        if (pcm_size > kMaxPcmBytes || encoded_size > kMaxEncodedBytes ||
            encoded_size >= USHRT_MAX) {
            ESP_LOGE(TAG, "Invalid frame sizing: pcm=%lu encoded=%lu",
                static_cast<unsigned long>(pcm_size),
                static_cast<unsigned long>(encoded_size));
            goto finish;
        }
        pcm = static_cast<uint8_t *>(heap_caps_malloc(pcm_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        encoded = static_cast<uint8_t *>(heap_caps_malloc(encoded_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pcm == nullptr || encoded == nullptr) {
            ESP_LOGE(TAG, "Encoder PSRAM buffer allocation failed");
            goto finish;
        }
        const uint32_t pcm_frames = pcm_size / 4U; // stereo S16
        const int64_t frame_us = std::max<int64_t>(1000,
            static_cast<int64_t>(pcm_frames) * 1000000LL / stream.sample_rate);
        int64_t next_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Encoder running codec=%s rate=%lu mtu=%u pcm=%lu packet_frames=%u",
            stream.codec.type == ESP_A2D_MCT_M24 ? "AAC" : "SBC",
            static_cast<unsigned long>(stream.sample_rate),
            unsigned(stream.mtu), static_cast<unsigned long>(pcm_size),
            unsigned(frames_per_packet));

        while (s_run) {
            const int64_t due_us = next_us;
            const int64_t now = esp_timer_get_time();
            if (now < next_us) {
                const uint32_t wait_ms = static_cast<uint32_t>(
                    (next_us - now + 999LL) / 1000LL);
                // At 100Hz RTOS tick, pdMS_TO_TICKS(1..9ms)==0. A P6
                // zero-tick loop would starve LVGL(P3)/FLAC prefetch(P4)
                // on Core1. Always block for >= 1 tick when waiting.
                const TickType_t ticks = pdMS_TO_TICKS(wait_ms);
                vTaskDelay(ticks > 0 ? ticks : 1);
            } else if (now - next_us > frame_us * 2) {
                next_us = now;
            }
            next_us += frame_us;
            if (!s_run) break;
            // Time the actual task wakeup without per-packet logging overhead.
            const int64_t started_us = esp_timer_get_time();
            if (started_us > due_us) {
                const uint32_t late = static_cast<uint32_t>(
                    std::min<int64_t>(started_us - due_us, UINT32_MAX));
                if (late > max_lateness_us) max_lateness_us = late;
                if (late > 40000U) ++late_over_40ms;
                if (late > 80000U) ++late_over_80ms;
            }
            memset(pcm, 0, pcm_size);
            const int32_t got = stream.reader(pcm, static_cast<int32_t>(pcm_size));
            if (got != static_cast<int32_t>(pcm_size)) ++underrun;
            esp_audio_enc_in_frame_t input = {};
            input.buffer = pcm;
            input.len = pcm_size;
            esp_audio_enc_out_frame_t output = {};
            output.buffer = encoded;
            output.len = encoded_size;
            const int64_t encode_start_us = esp_timer_get_time();
            // Espressif esp_sbc_enc_process supports multi-frame input.
            // Keep the negotiated A2DP SBC packet grouping intact.
            esp_audio_err_t status = stream.codec.type == ESP_A2D_MCT_M24
                ? esp_aac_enc_process(encoder, &input, &output)
                : esp_sbc_enc_process(encoder, &input, &output);
            const uint32_t encode_us = static_cast<uint32_t>(
                esp_timer_get_time() - encode_start_us);
            if (encode_us > max_encode_us) max_encode_us = encode_us;
            if (status != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0U) {
                ++failed;
                timestamp += pcm_frames;
                continue;
            }
            const uint16_t payload_limit = stream.codec.type == ESP_A2D_MCT_M24
                ? (stream.mtu > 20U ? stream.mtu - 20U : 0U) : stream.mtu;
            if (output.encoded_bytes > payload_limit) {
                ++failed;
                timestamp += pcm_frames;
                continue;
            }
            auto *packet = esp_a2d_audio_buff_alloc(
                static_cast<uint16_t>(output.encoded_bytes));
            if (packet == nullptr) {
                ++failed;
                timestamp += pcm_frames;
                continue;
            }
            packet->data_len = static_cast<uint16_t>(output.encoded_bytes);
            packet->number_frame = frames_per_packet;
            packet->timestamp = timestamp;
            memcpy(packet->data, encoded, packet->data_len);
            bool accepted = false;
            const int64_t send_start_us = esp_timer_get_time();
            for (unsigned tries = 0U; tries < 4U && s_run; ++tries) {
                const esp_err_t ret = esp_a2d_source_audio_data_send(stream.connection, packet);
                if (ret == ESP_OK) { accepted = true; break; }
                if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE ||
                    ret == ESP_ERR_INVALID_STATE) break;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            const uint32_t send_us = static_cast<uint32_t>(
                esp_timer_get_time() - send_start_us);
            if (send_us > max_send_us) max_send_us = send_us;
            if (accepted) ++sent;
            else { ++failed; esp_a2d_audio_buff_free(packet); }
            timestamp += pcm_frames;
            if (((sent + failed) % 500U) == 0U && (sent + failed) > 0U) {
                ESP_LOGI(TAG,
                    "A2DP_TX_EXT sent=%lu failed=%lu short_pcm=%lu late_max=%luus late40=%lu late80=%lu encode_max=%luus send_max=%luus",
                    static_cast<unsigned long>(sent),
                    static_cast<unsigned long>(failed),
                    static_cast<unsigned long>(underrun),
                    static_cast<unsigned long>(max_lateness_us),
                    static_cast<unsigned long>(late_over_40ms),
                    static_cast<unsigned long>(late_over_80ms),
                    static_cast<unsigned long>(max_encode_us),
                    static_cast<unsigned long>(max_send_us));
                max_lateness_us = 0U;
                late_over_40ms = 0U;
                late_over_80ms = 0U;
                max_encode_us = 0U;
                max_send_us = 0U;
            }
        }
    }
finish:
    heap_caps_free(pcm);
    heap_caps_free(encoded);
    close_codec(stream.codec.type, encoder);
    s_run = false;
    s_task = nullptr;
    if (s_finished != nullptr) xSemaphoreGive(s_finished);
    vTaskDelete(nullptr);
}
} // namespace

esp_err_t bt_a2dp_ext_encoder_start(esp_a2d_conn_hdl_t connection, uint16_t audio_mtu,
    const esp_a2d_mcc_t *codec, uint32_t sample_rate_hz, BtA2dpPcmReader reader)
{
    if (s_task != nullptr) return ESP_ERR_INVALID_STATE;
    if (connection == 0U || audio_mtu < 100U || codec == nullptr || reader == nullptr ||
        (sample_rate_hz != 44100U && sample_rate_hz != 48000U) ||
        (codec->type != ESP_A2D_MCT_M24 && codec->type != ESP_A2D_MCT_SBC))
        return ESP_ERR_INVALID_ARG;
    if (s_finished == nullptr) s_finished = xSemaphoreCreateBinary();
    if (s_finished == nullptr) return ESP_ERR_NO_MEM;
    (void)xSemaphoreTake(s_finished, 0);
    s_stream = {connection, audio_mtu, *codec, sample_rate_hz, reader};
    s_run = true;
    const BaseType_t ret = xTaskCreatePinnedToCore(
        worker, "bt_tx_enc", kTaskStack, nullptr, kTaskPriority, &s_task, kTaskCore);
    if (ret != pdPASS) {
        s_run = false;
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void bt_a2dp_ext_encoder_request_stop()
{
    s_run = false;
}

bool bt_a2dp_ext_encoder_stop()
{
    if (s_task == nullptr) return true;
    s_run = false;
    if (s_finished == nullptr) return false;
    if (xSemaphoreTake(s_finished, pdMS_TO_TICKS(2500)) != pdTRUE) {
        ESP_LOGE(TAG, "Encoder task stop timeout; do not deinit A2DP while still sending");
        return false;
    }
    return true;
}

bool bt_a2dp_ext_encoder_is_running()
{
    return s_task != nullptr;
}
#endif
