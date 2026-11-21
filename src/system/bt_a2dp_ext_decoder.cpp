#include "bt_a2dp_ext_decoder.h"
#include "sdkconfig.h"

#if defined(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC) && CONFIG_BT_A2DP_USE_EXTERNAL_CODEC

#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"
#include "esp_aac_dec.h"
#include "esp_sbc_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace {
static const char *TAG = "BT_AAC";
static constexpr UBaseType_t kDecodePriority = 6;
static constexpr uint32_t kDecodeStackBytes = 8192U;
static constexpr BaseType_t kDecodeCore = 1;
static constexpr UBaseType_t kRawQueueSlots = 24U;
static constexpr uint32_t kInitialOutputBytes = 16384U;
static constexpr uint32_t kMaxOutputBytes = 65536U;

struct PacketItem {
    uint32_t generation = 0;
    esp_a2d_audio_buff_t *packet = nullptr;
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue = nullptr;
static SemaphoreHandle_t s_finished = nullptr;
static TaskHandle_t s_task = nullptr;
static BtA2dpExtPcmCallback s_sink = nullptr;
static bool s_running = false;
static bool s_playing = false;
static bool s_have_codec = false;
static uint32_t s_generation = 1U;
static esp_a2d_mcc_t s_codec = {};
static BtA2dpExtStats s_stats = {};

static int get_rate(const esp_a2d_mcc_t &codec)
{
    if (codec.type == ESP_A2D_MCT_SBC) {
        const uint8_t x = codec.cie.sbc_info.samp_freq;
        if (x & ESP_A2D_SBC_CIE_SF_48K) return 48000;
        if (x & ESP_A2D_SBC_CIE_SF_44K) return 44100;
    } else if (codec.type == ESP_A2D_MCT_M24) {
        const auto &x = codec.cie.m24_info;
        if (x.samp_freq2 & ESP_A2D_M24_CIE_SF2_48K) return 48000;
        if (x.samp_freq1 & ESP_A2D_M24_CIE_SF1_44K) return 44100;
    }
    return 0;
}
static int get_channels(const esp_a2d_mcc_t &codec)
{
    if (codec.type == ESP_A2D_MCT_SBC)
        return (codec.cie.sbc_info.ch_mode &
                ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
    if (codec.type == ESP_A2D_MCT_M24)
        return (codec.cie.m24_info.ch & ESP_A2D_M24_CIE_CH_1) ? 1 : 2;
    return 0;
}

static void report_error(const char *reason, int code)
{
    portENTER_CRITICAL(&s_mux);
    ++s_stats.decode_errors;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "A2DP_EXT decode %s err=%d", reason, code);
}

static void post_decoded_pcm(const uint8_t *buffer, uint32_t bytes, int channels)
{
    if (!buffer || bytes == 0U || !s_sink) return;
    if (channels == 2) {
        const size_t frames = bytes / 4U;
        if (frames == 0U) return;
        s_sink(buffer, frames * 4U);
        portENTER_CRITICAL(&s_mux);
        s_stats.decoded_frames += frames;
        portEXIT_CRITICAL(&s_mux);
    } else if (channels == 1) {
        // Defensive mono compatibility; the AAC capability advertises stereo
        // only but some SBC peers may choose mono.
        int16_t stereo[256U * 2U] = {};
        const size_t samples = bytes / sizeof(int16_t);
        size_t used = 0U;
        while (used < samples) {
            const size_t n = samples - used > 256U ? 256U : samples - used;
            for (size_t i = 0; i < n; ++i) {
                int16_t v = 0;
                memcpy(&v, buffer + (used + i) * 2U, sizeof(v));
                stereo[2U * i] = v;
                stereo[2U * i + 1U] = v;
            }
            s_sink(reinterpret_cast<const uint8_t *>(stereo), n * 4U);
            used += n;
        }
        portENTER_CRITICAL(&s_mux);
        s_stats.decoded_frames += samples;
        portEXIT_CRITICAL(&s_mux);
    }
}

static void decode_worker(void *)
{
    esp_audio_dec_handle_t decoder = nullptr;
    uint32_t local_generation = 0U;
    int channels = 0;
    uint32_t output_capacity = kInitialOutputBytes;
    uint8_t *output = static_cast<uint8_t *>(heap_caps_malloc(
        output_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!output)
        output = static_cast<uint8_t *>(heap_caps_malloc(
            output_capacity, MALLOC_CAP_8BIT));
    if (!output) {
        report_error("output alloc", -1);
        // Caller still owns queue and must receive the worker completion.
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        portEXIT_CRITICAL(&s_mux);
    }
    while (output != nullptr) {
        PacketItem item = {};
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(30)) != pdTRUE) {
            portENTER_CRITICAL(&s_mux);
            const bool still_running = s_running;
            portEXIT_CRITICAL(&s_mux);
            if (!still_running) break;
            continue;
        }

        esp_a2d_mcc_t chosen = {};
        uint32_t current_generation = 0U;
        bool configured = false, play = false, running = false;
        portENTER_CRITICAL(&s_mux);
        chosen = s_codec;
        current_generation = s_generation;
        configured = s_have_codec;
        play = s_playing;
        running = s_running;
        portEXIT_CRITICAL(&s_mux);
        if (!running) {
            esp_a2d_audio_buff_free(item.packet);
            break;
        }
        if (!play || !configured || item.generation != current_generation) {
            esp_a2d_audio_buff_free(item.packet);
            continue;
        }
        if (local_generation != current_generation) {
            if (decoder != nullptr) {
                esp_audio_dec_close(decoder);
                decoder = nullptr;
            }
            local_generation = current_generation;
            channels = get_channels(chosen);
            const int rate = get_rate(chosen);
            if ((rate != 44100 && rate != 48000) || channels < 1 || channels > 2) {
                report_error("unsupported negotiated format", chosen.type);
            } else if (chosen.type == ESP_A2D_MCT_SBC) {
                esp_sbc_dec_cfg_t cfg = ESP_SBC_DEC_CONFIG_DEFAULT();
                cfg.sbc_mode = ESP_SBC_MODE_STD;
                cfg.ch_num = channels;
                cfg.enable_plc = true;
                esp_audio_dec_cfg_t settings = {};
                settings.type = ESP_AUDIO_TYPE_SBC;
                settings.cfg = &cfg;
                settings.cfg_sz = sizeof(cfg);
                esp_sbc_dec_register();
                const esp_audio_err_t err = esp_audio_dec_open(&settings, &decoder);
                if (err != ESP_AUDIO_ERR_OK) {
                    decoder = nullptr;
                    report_error("SBC open", static_cast<int>(err));
                }
            } else if (chosen.type == ESP_A2D_MCT_M24) {
                esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
                cfg.sample_rate = rate;
                cfg.channel = channels;
                cfg.bits_per_sample = ESP_AUDIO_BIT16;
                cfg.no_adts_header = true; // A2DP transports raw AAC access units
                cfg.aac_plus_enable = false; // Only advertise AAC-LC
                esp_audio_dec_cfg_t settings = {};
                settings.type = ESP_AUDIO_TYPE_AAC;
                settings.cfg = &cfg;
                settings.cfg_sz = sizeof(cfg);
                esp_aac_dec_register();
                const esp_audio_err_t err = esp_audio_dec_open(&settings, &decoder);
                if (err != ESP_AUDIO_ERR_OK) {
                    decoder = nullptr;
                    report_error("AAC open", static_cast<int>(err));
                }
            }
            portENTER_CRITICAL(&s_mux);
            s_stats.decoder_open = decoder != nullptr;
            s_stats.codec_type = chosen.type;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "A2DP_EXT decoder codec=%s %dHz/%dch open=%u",
                chosen.type == ESP_A2D_MCT_M24 ? "AAC-LC" : "SBC",
                rate, channels, decoder != nullptr ? 1U : 0U);
        }

        if (decoder != nullptr && item.packet != nullptr &&
            item.packet->data != nullptr && item.packet->data_len > 0U) {
            esp_audio_dec_in_raw_t in = {};
            in.buffer = item.packet->data;
            in.len = item.packet->data_len;
            // A packet may have multiple access units. Decode until consumed.
            while (in.len > 0U) {
                esp_audio_dec_out_frame_t out = {};
                out.buffer = output;
                out.len = output_capacity;
                const esp_audio_err_t ret = esp_audio_dec_process(decoder, &in, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    if (output_capacity >= kMaxOutputBytes) {
                        report_error("output exceeded cap", static_cast<int>(ret));
                        break;
                    }
                    const uint32_t bigger = output_capacity * 2U;
                    uint8_t *replacement = static_cast<uint8_t *>(heap_caps_malloc(
                        bigger, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (!replacement) {
                        report_error("output realloc", -1);
                        break;
                    }
                    heap_caps_free(output);
                    output = replacement;
                    output_capacity = bigger;
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    report_error("process", static_cast<int>(ret));
                    break;
                }
                // A SUSPEND, track change or codec reconfigure invalidates any
                // PCM decoded from a previous buffer without locking Bluedroid.
                portENTER_CRITICAL(&s_mux);
                const bool current = s_running && s_playing &&
                    s_generation == current_generation;
                portEXIT_CRITICAL(&s_mux);
                if (current && out.decoded_size != 0U)
                    post_decoded_pcm(out.buffer, out.decoded_size, channels);
                if (in.consumed == 0U || in.consumed > in.len) {
                    report_error("decoder made no forward progress", -1);
                    break;
                }
                in.buffer += in.consumed;
                in.len -= in.consumed;
            }
        }
        esp_a2d_audio_buff_free(item.packet);
    }

    if (decoder != nullptr) esp_audio_dec_close(decoder);
    if (output != nullptr) heap_caps_free(output);
    PacketItem pending = {};
    while (xQueueReceive(s_queue, &pending, 0) == pdTRUE)
        esp_a2d_audio_buff_free(pending.packet);
    portENTER_CRITICAL(&s_mux);
    s_stats.decoder_open = false;
    s_task = nullptr;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_finished);
    vTaskDelete(nullptr);
}
} // namespace

bool bt_a2dp_ext_decoder_start(BtA2dpExtPcmCallback on_pcm)
{
    if (!on_pcm || s_queue || s_task) return false;
    s_queue = xQueueCreate(kRawQueueSlots, sizeof(PacketItem));
    s_finished = xSemaphoreCreateBinary();
    if (!s_queue || !s_finished) {
        if (s_queue) vQueueDelete(s_queue);
        if (s_finished) vSemaphoreDelete(s_finished);
        s_queue = nullptr;
        s_finished = nullptr;
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_sink = on_pcm;
    s_generation = 1U;
    s_running = true;
    s_playing = false;
    s_have_codec = false;
    s_codec = {};
    s_stats = {};
    portEXIT_CRITICAL(&s_mux);
    if (xTaskCreatePinnedToCore(
        decode_worker, "A2DP_Decode", kDecodeStackBytes, nullptr,
        kDecodePriority, &s_task, kDecodeCore) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        portEXIT_CRITICAL(&s_mux);
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_finished);
        s_queue = nullptr;
        s_finished = nullptr;
        s_task = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "A2DP_EXT worker started core=%d queue=%u",
        static_cast<int>(kDecodeCore), static_cast<unsigned>(kRawQueueSlots));
    return true;
}

void bt_a2dp_ext_decoder_configure(const esp_a2d_mcc_t *codec)
{
    if (!codec) return;
    portENTER_CRITICAL(&s_mux);
    ++s_generation;
    s_codec = *codec;
    s_have_codec = true;
    s_playing = false;
    s_stats.codec_type = codec->type;
    portEXIT_CRITICAL(&s_mux);
}

void bt_a2dp_ext_decoder_stream_state(bool playing)
{
    portENTER_CRITICAL(&s_mux);
    // Always invalidate queued compressed data on suspend; this prevents
    // stale AAC/SBC frames crossing pause, disconnect or role switches.
    if (!playing) ++s_generation;
    s_playing = playing && s_running && s_have_codec;
    portEXIT_CRITICAL(&s_mux);
}

void bt_a2dp_ext_decoder_receive(esp_a2d_conn_hdl_t, esp_a2d_audio_buff_t *packet)
{
    if (!packet) return;
    PacketItem item = {};
    bool accept = false;
    portENTER_CRITICAL(&s_mux);
    ++s_stats.raw_packets;
    accept = s_running && s_playing && s_have_codec && s_queue != nullptr;
    item.generation = s_generation;
    portEXIT_CRITICAL(&s_mux);
    item.packet = packet;
    if (accept && xQueueSend(s_queue, &item, 0) == pdTRUE) return;
    portENTER_CRITICAL(&s_mux);
    ++s_stats.raw_drop_packets;
    portEXIT_CRITICAL(&s_mux);
    esp_a2d_audio_buff_free(packet);
}

void bt_a2dp_ext_decoder_stop()
{
    if (!s_queue) return;
    portENTER_CRITICAL(&s_mux);
    s_playing = false;
    s_running = false;
    ++s_generation;
    portEXIT_CRITICAL(&s_mux);
    // Never free the queue while its worker still has access to it.
    const bool finished = xSemaphoreTake(s_finished, pdMS_TO_TICKS(2500)) == pdTRUE;
    if (!finished) {
        ESP_LOGE(TAG, "A2DP_EXT worker stop timed out; retaining queue for safety");
        return;
    }
    vQueueDelete(s_queue);
    vSemaphoreDelete(s_finished);
    s_queue = nullptr;
    s_finished = nullptr;
    s_sink = nullptr;
}

BtA2dpExtStats bt_a2dp_ext_decoder_stats()
{
    portENTER_CRITICAL(&s_mux);
    const BtA2dpExtStats stats = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return stats;
}
#endif
