#include "wifi_rtp_test.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sys/time.h"

#include "esp_err.h"

#if defined(CONFIG_FAKEPOD_WIFI_RTP_TEST_ENABLE) && CONFIG_FAKEPOD_WIFI_RTP_TEST_ENABLE

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wifi_service.h"

namespace {

static const char *TAG = "WiFi_RTP_Test";

static constexpr size_t kInputFrames = 256U;
static constexpr size_t kRingBlockCount = 64U;
static constexpr size_t kRtpHeaderBytes = 12U;
static constexpr size_t kRtpPacketFrames = 240U;
static constexpr size_t kChannels = 2U;
static constexpr size_t kBytesPerSample = 3U;
static constexpr size_t kRtpPayloadBytes =
    kRtpPacketFrames * kChannels * kBytesPerSample;
static constexpr size_t kRtpPacketBytes = kRtpHeaderBytes + kRtpPayloadBytes;
static constexpr uint8_t kRtpPayloadTypeL24 = 96U;
static constexpr uint32_t kTaskStackBytes = 4096U;
static constexpr UBaseType_t kTaskPriority = 2U;
static constexpr BaseType_t kTaskCore = 1;
static constexpr int64_t kPartialPacketFlushUs = 20000LL;

struct PcmBlock {
    uint32_t sample_rate_hz;
    uint16_t frames;
    uint16_t reserved;
    int32_t samples[kInputFrames * kChannels];
};

static PcmBlock *g_ring = nullptr;
static uint8_t *g_packet = nullptr;
static TaskHandle_t g_task = nullptr;

// Single producer: AudioTask. Single consumer: wifi_rtp_task.
// Release/acquire ordering publishes a fully copied block without a mutex.
static uint32_t g_head = 0U;
static uint32_t g_tail = 0U;
static uint32_t g_dropped_blocks = 0U;
static uint32_t g_send_errors = 0U;
static uint32_t g_packets_sent = 0U;
static bool g_network_ready = false;
static bool g_overflowed = false;

static void write_be16(uint8_t *dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    dst[1] = static_cast<uint8_t>(value & 0xFFU);
}

static void write_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    dst[1] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    dst[3] = static_cast<uint8_t>(value & 0xFFU);
}

static void discard_queued_audio()
{
    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_tail, head, __ATOMIC_RELEASE);
}

static void reset_rtp_session(
    uint16_t *sequence,
    uint32_t *timestamp,
    uint32_t *ssrc,
    bool *marker,
    uint16_t *packet_frames)
{
    *sequence = 0U;
    *timestamp = 0U;
    *ssrc = esp_random();
    if (*ssrc == 0U) *ssrc = 1U;
    *marker = true;
    *packet_frames = 0U;
}

static int create_rtp_socket(sockaddr_in *destination)
{
    if (destination == nullptr) return -1;

    memset(destination, 0, sizeof(*destination));
    destination->sin_family = AF_INET;
    destination->sin_port = htons(CONFIG_FAKEPOD_WIFI_RTP_TEST_PORT);

    if (inet_pton(AF_INET, CONFIG_FAKEPOD_WIFI_RTP_TEST_TARGET_IP,
                  &destination->sin_addr) != 1) {
        ESP_LOGE(TAG, "无效的接收端 IPv4 地址：%s",
            CONFIG_FAKEPOD_WIFI_RTP_TEST_TARGET_IP);
        return -1;
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "创建 RTP UDP socket 失败：errno=%d", errno);
        return -1;
    }

    int send_buffer_bytes = 8192;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
        &send_buffer_bytes, sizeof(send_buffer_bytes));

    // Socket back-pressure is isolated to this low-priority worker task.
    struct timeval timeout = {};
    timeout.tv_usec = 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "RTP L24 接收端：%s:%u",
        CONFIG_FAKEPOD_WIFI_RTP_TEST_TARGET_IP,
        static_cast<unsigned>(CONFIG_FAKEPOD_WIFI_RTP_TEST_PORT));
    return fd;
}

static bool send_rtp_packet(
    int fd,
    const sockaddr_in &destination,
    uint16_t *sequence,
    uint32_t *timestamp,
    uint32_t ssrc,
    bool *marker,
    uint16_t frames)
{
    if (frames == 0U || frames > kRtpPacketFrames) return false;

    g_packet[0] = 0x80U; // RTP version 2, no padding/extension/CSRC.
    g_packet[1] = static_cast<uint8_t>(
        (*marker ? 0x80U : 0U) | kRtpPayloadTypeL24);
    write_be16(g_packet + 2U, *sequence);
    write_be32(g_packet + 4U, *timestamp);
    write_be32(g_packet + 8U, ssrc);

    const size_t packet_bytes =
        kRtpHeaderBytes + static_cast<size_t>(frames) * kChannels * kBytesPerSample;
    const int sent = sendto(
        fd,
        g_packet,
        packet_bytes,
        0,
        reinterpret_cast<const sockaddr *>(&destination),
        sizeof(destination));

    // Advance RTP time even when a datagram is lost. The receiver detects the
    // missing sequence and can insert silence without shifting the timeline.
    *sequence = static_cast<uint16_t>(*sequence + 1U);
    *timestamp += frames;
    *marker = false;

    if (sent != static_cast<int>(packet_bytes)) {
        __atomic_add_fetch(&g_send_errors, 1U, __ATOMIC_RELAXED);
        return false;
    }

    __atomic_add_fetch(&g_packets_sent, 1U, __ATOMIC_RELAXED);
    return true;
}

static void wifi_rtp_task(void *)
{
    int fd = -1;
    sockaddr_in destination = {};

    PcmBlock *current = nullptr;
    size_t current_frame = 0U;
    uint16_t packet_frames = 0U;
    uint16_t sequence = 0U;
    uint32_t timestamp = 0U;
    uint32_t ssrc = 0U;
    uint32_t stream_rate_hz = 0U;
    bool marker = true;
    int64_t last_pcm_us = esp_timer_get_time();
    int64_t last_stats_us = last_pcm_us;

    reset_rtp_session(&sequence, &timestamp, &ssrc, &marker, &packet_frames);

    while (true) {
        WifiServiceSnapshot wifi = {};
        const bool connected =
            wifi_service_get_snapshot(&wifi) && wifi.connected;

        if (!connected) {
            __atomic_store_n(&g_network_ready, false, __ATOMIC_RELEASE);
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            current = nullptr;
            current_frame = 0U;
            packet_frames = 0U;
            stream_rate_hz = 0U;
            discard_queued_audio();
            reset_rtp_session(&sequence, &timestamp, &ssrc, &marker, &packet_frames);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (fd < 0) {
            fd = create_rtp_socket(&destination);
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            current = nullptr;
            packet_frames = 0U;
            stream_rate_hz = 0U;
            discard_queued_audio();
            reset_rtp_session(&sequence, &timestamp, &ssrc, &marker, &packet_frames);
            __atomic_store_n(&g_network_ready, true, __ATOMIC_RELEASE);
        }

        if (__atomic_exchange_n(&g_overflowed, false, __ATOMIC_ACQ_REL)) {
            // Drop stale queued audio rather than allowing network congestion
            // to turn into an ever-growing playback delay.
            discard_queued_audio();
            current = nullptr;
            current_frame = 0U;
            stream_rate_hz = 0U;
            reset_rtp_session(&sequence, &timestamp, &ssrc, &marker, &packet_frames);
            ESP_LOGW(TAG, "RTP 发送队列溢出：已丢弃过期 PCM 并重建会话");
        }

        const int64_t stats_now_us = esp_timer_get_time();
        if (stats_now_us - last_stats_us >= 5000000LL) {
            const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
            const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
            ESP_LOGI(TAG,
                "RTP_STATS rate=%lu packets=%lu send_errors=%lu dropped_blocks=%lu queue=%lu",
                static_cast<unsigned long>(stream_rate_hz),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_packets_sent, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_send_errors, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_dropped_blocks, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(head - tail));
            last_stats_us = stats_now_us;
        }

        if (current == nullptr) {
            const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
            const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
            if (tail == head) {
                const int64_t now_us = esp_timer_get_time();
                if (packet_frames > 0U &&
                    now_us - last_pcm_us >= kPartialPacketFlushUs) {
                    (void)send_rtp_packet(fd, destination, &sequence,
                        &timestamp, ssrc, &marker, packet_frames);
                    packet_frames = 0U;
                }
                // Wait for the producer instead of polling. A 1 ms delay
                // can round down to zero ticks on lower tick-rate builds,
                // starving IDLE1 and triggering the task watchdog.
                TickType_t wait_ticks = pdMS_TO_TICKS(20U);
                if (wait_ticks == 0U) wait_ticks = 1U;
                (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
                continue;
            }

            current = &g_ring[tail % kRingBlockCount];
            current_frame = 0U;
            if (current->frames == 0U || current->frames > kInputFrames ||
                current->sample_rate_hz == 0U) {
                current = nullptr;
                __atomic_store_n(&g_tail, tail + 1U, __ATOMIC_RELEASE);
                continue;
            }

            if (stream_rate_hz != current->sample_rate_hz) {
                packet_frames = 0U;
                stream_rate_hz = current->sample_rate_hz;
                reset_rtp_session(&sequence, &timestamp, &ssrc, &marker, &packet_frames);
                ESP_LOGI(TAG, "RTP L24 新音频流：%lu Hz / 24-bit / stereo",
                    static_cast<unsigned long>(stream_rate_hz));
            }
        }

        while (current_frame < current->frames &&
               packet_frames < kRtpPacketFrames) {
            const uint32_t left = static_cast<uint32_t>(
                current->samples[current_frame * 2U]) >> 8U;
            const uint32_t right = static_cast<uint32_t>(
                current->samples[current_frame * 2U + 1U]) >> 8U;

            // pcm_decoder_read_pcm32() stores valid PCM bits left-aligned.
            // RTP L24 uses packed 24-bit samples in network byte order.
            const size_t out = kRtpHeaderBytes +
                static_cast<size_t>(packet_frames) * kChannels * kBytesPerSample;
            g_packet[out + 0U] = static_cast<uint8_t>((left >> 16U) & 0xFFU);
            g_packet[out + 1U] = static_cast<uint8_t>((left >> 8U) & 0xFFU);
            g_packet[out + 2U] = static_cast<uint8_t>(left & 0xFFU);
            g_packet[out + 3U] = static_cast<uint8_t>((right >> 16U) & 0xFFU);
            g_packet[out + 4U] = static_cast<uint8_t>((right >> 8U) & 0xFFU);
            g_packet[out + 5U] = static_cast<uint8_t>(right & 0xFFU);

            ++current_frame;
            ++packet_frames;
        }

        if (current_frame >= current->frames) {
            // One timer read per decoded block, not per PCM frame.
            last_pcm_us = esp_timer_get_time();
            current = nullptr;
            current_frame = 0U;
            const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
            __atomic_store_n(&g_tail, tail + 1U, __ATOMIC_RELEASE);
        }

        if (packet_frames == kRtpPacketFrames) {
            (void)send_rtp_packet(fd, destination, &sequence,
                &timestamp, ssrc, &marker, packet_frames);
            packet_frames = 0U;
        }
    }
}

} // namespace

esp_err_t wifi_rtp_test_start()
{
    if (g_task != nullptr) return ESP_OK;

    g_ring = static_cast<PcmBlock *>(heap_caps_calloc(
        kRingBlockCount, sizeof(PcmBlock),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_packet = static_cast<uint8_t *>(heap_caps_malloc(
        kRtpPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (g_ring == nullptr || g_packet == nullptr) {
        if (g_ring != nullptr) heap_caps_free(g_ring);
        if (g_packet != nullptr) heap_caps_free(g_packet);
        g_ring = nullptr;
        g_packet = nullptr;
        return ESP_ERR_NO_MEM;
    }

    __atomic_store_n(&g_head, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tail, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_network_ready, false, __ATOMIC_RELAXED);
    __atomic_store_n(&g_overflowed, false, __ATOMIC_RELAXED);

    const BaseType_t created = xTaskCreatePinnedToCore(
        wifi_rtp_task,
        "WiFiRtpL24",
        kTaskStackBytes,
        nullptr,
        kTaskPriority,
        &g_task,
        kTaskCore);
    if (created != pdPASS) {
        heap_caps_free(g_ring);
        heap_caps_free(g_packet);
        g_ring = nullptr;
        g_packet = nullptr;
        g_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG,
        "PC RTP 音频测试已启用：目标=%s:%u；仅在 Wi-Fi 已连接且播放 Music PCM 时发送",
        CONFIG_FAKEPOD_WIFI_RTP_TEST_TARGET_IP,
        static_cast<unsigned>(CONFIG_FAKEPOD_WIFI_RTP_TEST_PORT));
    return ESP_OK;
}

void wifi_rtp_test_publish_pcm32(
    const int32_t *interleaved_stereo,
    size_t frames,
    uint32_t sample_rate_hz)
{
    if (interleaved_stereo == nullptr || frames == 0U ||
        sample_rate_hz == 0U ||
        !__atomic_load_n(&g_network_ready, __ATOMIC_ACQUIRE) ||
        g_ring == nullptr) {
        return;
    }

    size_t offset = 0U;
    while (offset < frames) {
        const size_t count = (frames - offset) > kInputFrames
            ? kInputFrames : (frames - offset);

        const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_RELAXED);
        const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
        if ((head - tail) >= kRingBlockCount) {
            __atomic_add_fetch(&g_dropped_blocks, 1U, __ATOMIC_RELAXED);
            __atomic_store_n(&g_overflowed, true, __ATOMIC_RELEASE);
            return;
        }

        PcmBlock *block = &g_ring[head % kRingBlockCount];
        block->sample_rate_hz = sample_rate_hz;
        block->frames = static_cast<uint16_t>(count);
        block->reserved = 0U;
        memcpy(block->samples,
            interleaved_stereo + offset * kChannels,
            count * kChannels * sizeof(int32_t));

        __atomic_store_n(&g_head, head + 1U, __ATOMIC_RELEASE);

        // Wake the sender only after the PCM block is fully published.
        // The notification is non-blocking and keeps the AudioTask independent
        // of UDP/network scheduling.
        if (g_task != nullptr) {
            xTaskNotifyGive(g_task);
        }
        offset += count;
    }
}

#else

esp_err_t wifi_rtp_test_start()
{
    return ESP_OK;
}

void wifi_rtp_test_publish_pcm32(
    const int32_t *,
    size_t,
    uint32_t)
{
}

#endif
