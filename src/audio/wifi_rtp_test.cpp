#include "wifi_rtp_test.h"

#include <errno.h>

// Existing generated sdkconfig files may predate the discovery-port option.
// Keep the test build source-compatible until PlatformIO regenerates sdkconfig.
#ifndef CONFIG_FAKEPOD_WIFI_RTP_TEST_DISCOVERY_PORT
#define CONFIG_FAKEPOD_WIFI_RTP_TEST_DISCOVERY_PORT 5005
#endif

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
static constexpr size_t kRtpExtensionBytes = 8U;
static constexpr size_t kRtpPayloadOffset = kRtpHeaderBytes + kRtpExtensionBytes;
static constexpr uint16_t kRtpAudioFormatExtensionProfile = 0xF0D1U;
static constexpr size_t kRtpPacketFrames = 240U;
static constexpr size_t kChannels = 2U;
static constexpr size_t kBytesPerSample = 3U;
static constexpr size_t kRtpPayloadBytes =
    kRtpPacketFrames * kChannels * kBytesPerSample;
static constexpr size_t kRtpPacketBytes = kRtpPayloadOffset + kRtpPayloadBytes;
static constexpr uint8_t kRtpPayloadTypeL24 = 96U;
static constexpr uint16_t kDiscoveryPort =
    CONFIG_FAKEPOD_WIFI_RTP_TEST_DISCOVERY_PORT;
static constexpr int64_t kDiscoveryIntervalUs = 2000000LL;
static constexpr int64_t kReceiverLeaseUs = 7000000LL;
static constexpr size_t kDiscoveryPacketBytes = 12U;
static constexpr uint32_t kTaskStackBytes = 4096U;
static constexpr UBaseType_t kTaskPriority = 4U;
// Core 0 is reserved for AudioTask (P5): keeping RTP there at P2 starves
// packetization during CPU-heavy FLAC decode, especially at 96 kHz.
// Core 1 P4 is above LVGL P3 and shares priority with FlacPrefetch P4.
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
static uint32_t g_queue_peak = 0U;
static int32_t g_last_send_error_errno = 0;
static int32_t g_last_send_error_result = 0;
static uint32_t g_last_send_error_us = 0U;
static uint32_t g_max_send_us = 0U;
static int g_socket_send_buffer_bytes = 0;
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

static uint16_t read_be16(const uint8_t *src)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(src[0]) << 8U) |
        static_cast<uint16_t>(src[1]));
}

static uint32_t read_be32(const uint8_t *src)
{
    return (static_cast<uint32_t>(src[0]) << 24U) |
        (static_cast<uint32_t>(src[1]) << 16U) |
        (static_cast<uint32_t>(src[2]) << 8U) |
        static_cast<uint32_t>(src[3]);
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

static int create_rtp_socket()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "创建 RTP UDP socket 失败：errno=%d", errno);
        return -1;
    }

    int send_buffer_bytes = 8192;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
        &send_buffer_bytes, sizeof(send_buffer_bytes));
    int actual_send_buffer = 0;
    socklen_t actual_send_buffer_len = sizeof(actual_send_buffer);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                   &actual_send_buffer, &actual_send_buffer_len) == 0) {
        g_socket_send_buffer_bytes = actual_send_buffer;
    }

    // Socket back-pressure is isolated to this worker task.
    struct timeval timeout = {};
    timeout.tv_usec = 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "RTP UDP socket 已就绪：port=%u，等待自动发现接收端",
        static_cast<unsigned>(CONFIG_FAKEPOD_WIFI_RTP_TEST_PORT));
    return fd;
}

static int create_discovery_socket()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "创建 RTP 发现 socket 失败：errno=%d", errno);
        return -1;
    }

    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                   &enabled, sizeof(enabled)) != 0) {
        ESP_LOGE(TAG, "启用 UDP 广播失败：errno=%d", errno);
        close(fd);
        return -1;
    }

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kDiscoveryPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<const sockaddr *>(&local),
             sizeof(local)) != 0) {
        ESP_LOGE(TAG, "绑定 RTP 发现端口失败：port=%u errno=%d",
            static_cast<unsigned>(kDiscoveryPort), errno);
        close(fd);
        return -1;
    }

    ESP_LOGI(TAG, "RTP 自动发现已就绪：UDP broadcast/response port=%u",
        static_cast<unsigned>(kDiscoveryPort));
    return fd;
}

static bool send_discovery_probe(int fd, uint32_t nonce)
{
    uint8_t packet[kDiscoveryPacketBytes] = {};
    memcpy(packet, "FPRD", 4U);
    packet[4] = 1U; // Discovery protocol version.
    write_be32(packet + 8U, nonce);

    sockaddr_in broadcast = {};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(kDiscoveryPort);
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const int sent = sendto(
        fd,
        packet,
        sizeof(packet),
        0,
        reinterpret_cast<const sockaddr *>(&broadcast),
        sizeof(broadcast));
    return sent == static_cast<int>(sizeof(packet));
}

static bool parse_discovery_response(
    const uint8_t *packet,
    size_t packet_bytes,
    uint32_t expected_nonce,
    uint16_t *rtp_port)
{
    if (packet == nullptr || rtp_port == nullptr ||
        packet_bytes != kDiscoveryPacketBytes ||
        memcmp(packet, "FPRR", 4U) != 0 ||
        packet[4] != 1U ||
        read_be32(packet + 8U) != expected_nonce) {
        return false;
    }

    const uint16_t port = read_be16(packet + 6U);
    if (port < 1024U) return false;
    *rtp_port = port;
    return true;
}

static void format_ipv4(uint32_t address_be, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0U) return;
    const uint32_t value = ntohl(address_be);
    (void)snprintf(out, out_size, "%u.%u.%u.%u",
        static_cast<unsigned>((value >> 24U) & 0xFFU),
        static_cast<unsigned>((value >> 16U) & 0xFFU),
        static_cast<unsigned>((value >> 8U) & 0xFFU),
        static_cast<unsigned>(value & 0xFFU));
}

static bool send_rtp_packet(
    int fd,
    const sockaddr_in &destination,
    uint16_t *sequence,
    uint32_t *timestamp,
    uint32_t ssrc,
    bool *marker,
    uint32_t sample_rate_hz,
    uint16_t frames)
{
    if (frames == 0U || frames > kRtpPacketFrames || sample_rate_hz == 0U) return false;

    // RTP v2 + extension. The one-word FakePod audio-format extension carries
    // the PCM sample rate so receivers can configure their I2S clock exactly.
    g_packet[0] = 0x90U; // RTP version 2, extension present, no CSRC.
    write_be16(g_packet + kRtpHeaderBytes, kRtpAudioFormatExtensionProfile);
    write_be16(g_packet + kRtpHeaderBytes + 2U, 1U); // one 32-bit extension word
    write_be32(g_packet + kRtpHeaderBytes + 4U, sample_rate_hz);
    g_packet[1] = static_cast<uint8_t>(
        (*marker ? 0x80U : 0U) | kRtpPayloadTypeL24);
    write_be16(g_packet + 2U, *sequence);
    write_be32(g_packet + 4U, *timestamp);
    write_be32(g_packet + 8U, ssrc);

    const size_t packet_bytes =
        kRtpPayloadOffset + static_cast<size_t>(frames) * kChannels * kBytesPerSample;
    const int64_t send_started_us = esp_timer_get_time();
    const int sent = sendto(
        fd,
        g_packet,
        packet_bytes,
        0,
        reinterpret_cast<const sockaddr *>(&destination),
        sizeof(destination));
    const uint32_t send_elapsed_us = static_cast<uint32_t>(
        esp_timer_get_time() - send_started_us);
    uint32_t previous_max_us = __atomic_load_n(&g_max_send_us, __ATOMIC_RELAXED);
    while (send_elapsed_us > previous_max_us &&
           !__atomic_compare_exchange_n(
               &g_max_send_us, &previous_max_us, send_elapsed_us, false,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }

    // Advance RTP time even when a datagram is lost. The receiver detects the
    // missing sequence and can insert silence without shifting the timeline.
    *sequence = static_cast<uint16_t>(*sequence + 1U);
    *timestamp += frames;
    *marker = false;

    if (sent != static_cast<int>(packet_bytes)) {
        const int send_errno = sent < 0 ? errno : 0;
        __atomic_store_n(&g_last_send_error_errno, send_errno, __ATOMIC_RELAXED);
        __atomic_store_n(&g_last_send_error_result, sent, __ATOMIC_RELAXED);
        __atomic_store_n(&g_last_send_error_us, send_elapsed_us, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_send_errors, 1U, __ATOMIC_RELAXED);
        return false;
    }

    __atomic_add_fetch(&g_packets_sent, 1U, __ATOMIC_RELAXED);
    return true;
}

static void wifi_rtp_task(void *)
{
    int fd = -1;
    int discovery_fd = -1;
    sockaddr_in destination = {};
    bool receiver_found = false;
    uint32_t pending_nonce = 0U;
    int64_t next_probe_us = 0;
    int64_t last_receiver_seen_us = 0;
    int64_t last_wait_log_us = 0;

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
            if (discovery_fd >= 0) {
                close(discovery_fd);
                discovery_fd = -1;
            }
            g_socket_send_buffer_bytes = 0;
            receiver_found = false;
            pending_nonce = 0U;
            next_probe_us = 0;
            last_receiver_seen_us = 0;
            destination = {};
            current = nullptr;
            current_frame = 0U;
            packet_frames = 0U;
            stream_rate_hz = 0U;
            discard_queued_audio();
            reset_rtp_session(&sequence, &timestamp, &ssrc, &marker, &packet_frames);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (discovery_fd < 0) {
            discovery_fd = create_discovery_socket();
            if (discovery_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            next_probe_us = 0;
        }

        if (fd < 0) {
            fd = create_rtp_socket();
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_probe_us) {
            pending_nonce = esp_random();
            if (pending_nonce == 0U) pending_nonce = 1U;
            if (!send_discovery_probe(discovery_fd, pending_nonce)) {
                ESP_LOGW(TAG, "发送 RTP 接收端发现广播失败：errno=%d", errno);
            }
            next_probe_us = now_us + kDiscoveryIntervalUs;
        }

        // The PC receiver replies by unicast to this socket's UDP source port.
        // Drain all currently queued discovery replies without blocking RTP work.
        while (true) {
            uint8_t response[kDiscoveryPacketBytes] = {};
            sockaddr_in sender = {};
            socklen_t sender_len = sizeof(sender);
            const int received = recvfrom(
                discovery_fd,
                response,
                sizeof(response),
                MSG_DONTWAIT,
                reinterpret_cast<sockaddr *>(&sender),
                &sender_len);
            if (received < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ESP_LOGW(TAG, "接收 RTP 发现响应失败：errno=%d", errno);
                }
                break;
            }

            uint16_t receiver_rtp_port = 0U;
            if (!parse_discovery_response(
                    response, static_cast<size_t>(received),
                    pending_nonce, &receiver_rtp_port)) {
                continue;
            }

            if (!receiver_found) {
                destination = sender;
                destination.sin_port = htons(receiver_rtp_port);
                receiver_found = true;
                last_receiver_seen_us = esp_timer_get_time();

                current = nullptr;
                current_frame = 0U;
                packet_frames = 0U;
                stream_rate_hz = 0U;
                discard_queued_audio();
                reset_rtp_session(
                    &sequence, &timestamp, &ssrc, &marker, &packet_frames);
                __atomic_store_n(&g_network_ready, true, __ATOMIC_RELEASE);

                char receiver_ip[16] = {};
                format_ipv4(destination.sin_addr.s_addr, receiver_ip, sizeof(receiver_ip));
                ESP_LOGI(TAG,
                    "自动发现 RTP 接收端：%s:%u（discovery UDP %u）",
                    receiver_ip,
                    static_cast<unsigned>(receiver_rtp_port),
                    static_cast<unsigned>(kDiscoveryPort));
            } else if (sender.sin_addr.s_addr == destination.sin_addr.s_addr) {
                // Renew the active receiver lease. Other responders are ignored
                // while this receiver remains alive to avoid target flapping.
                destination.sin_port = htons(receiver_rtp_port);
                last_receiver_seen_us = esp_timer_get_time();
            }
        }

        now_us = esp_timer_get_time();
        if (receiver_found &&
            now_us - last_receiver_seen_us > kReceiverLeaseUs) {
            char receiver_ip[16] = {};
            format_ipv4(destination.sin_addr.s_addr, receiver_ip, sizeof(receiver_ip));
            ESP_LOGW(TAG, "RTP 接收端离线：%s:%u；重新发现中",
                receiver_ip,
                static_cast<unsigned>(ntohs(destination.sin_port)));

            receiver_found = false;
            __atomic_store_n(&g_network_ready, false, __ATOMIC_RELEASE);
            destination = {};
            last_receiver_seen_us = 0;
            next_probe_us = 0;
            current = nullptr;
            current_frame = 0U;
            packet_frames = 0U;
            stream_rate_hz = 0U;
            discard_queued_audio();
            reset_rtp_session(
                &sequence, &timestamp, &ssrc, &marker, &packet_frames);
        }

        if (!receiver_found) {
            __atomic_store_n(&g_network_ready, false, __ATOMIC_RELEASE);
            if (now_us - last_wait_log_us >= 5000000LL) {
                ESP_LOGI(TAG, "等待 RTP 接收端：broadcast=255.255.255.255:%u",
                    static_cast<unsigned>(kDiscoveryPort));
                last_wait_log_us = now_us;
            }
        }

        if (now_us - last_stats_us >= 5000000LL) {
            const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
            const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
            const uint32_t queue_blocks = head - tail;
            const uint32_t queue_ms = stream_rate_hz != 0U
                ? static_cast<uint32_t>(
                    (static_cast<uint64_t>(queue_blocks) * kInputFrames * 1000ULL) /
                    stream_rate_hz)
                : 0U;
            ESP_LOGI(TAG,
                "RTP_STATS rate=%lu packets=%lu send_errors=%lu dropped_blocks=%lu "
                "queue=%lu/%u queue_ms=%lu peak=%lu send_errno=%ld send_ret=%ld "
                "send_err_us=%lu max_send_us=%lu sndbuf=%d receiver=%u",
                static_cast<unsigned long>(stream_rate_hz),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_packets_sent, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_send_errors, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_dropped_blocks, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(queue_blocks),
                static_cast<unsigned>(kRingBlockCount),
                static_cast<unsigned long>(queue_ms),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_queue_peak, __ATOMIC_RELAXED)),
                static_cast<long>(
                    __atomic_load_n(&g_last_send_error_errno, __ATOMIC_RELAXED)),
                static_cast<long>(
                    __atomic_load_n(&g_last_send_error_result, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_last_send_error_us, __ATOMIC_RELAXED)),
                static_cast<unsigned long>(
                    __atomic_load_n(&g_max_send_us, __ATOMIC_RELAXED)),
                g_socket_send_buffer_bytes,
                receiver_found ? 1U : 0U);
            last_stats_us = now_us;
        }

        if (!receiver_found) {
            TickType_t wait_ticks = pdMS_TO_TICKS(100U);
            if (wait_ticks == 0U) wait_ticks = 1U;
            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
            continue;
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

        if (current == nullptr) {
            const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
            const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
            if (tail == head) {
                const int64_t idle_now_us = esp_timer_get_time();
                if (packet_frames > 0U &&
                    idle_now_us - last_pcm_us >= kPartialPacketFlushUs) {
                    (void)send_rtp_packet(fd, destination, &sequence,
                        &timestamp, ssrc, &marker, stream_rate_hz, packet_frames);
                    packet_frames = 0U;
                }
                const uint32_t wait_ms = (packet_frames > 0U) ? 20U : 200U;
                TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
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
            const size_t out = kRtpPayloadOffset +
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
            last_pcm_us = esp_timer_get_time();
            current = nullptr;
            current_frame = 0U;
            const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
            __atomic_store_n(&g_tail, tail + 1U, __ATOMIC_RELEASE);
        }

        if (packet_frames == kRtpPacketFrames) {
            (void)send_rtp_packet(fd, destination, &sequence,
                &timestamp, ssrc, &marker, stream_rate_hz, packet_frames);
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
    __atomic_store_n(&g_queue_peak, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_send_error_errno, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_send_error_result, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_send_error_us, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_max_send_us, 0U, __ATOMIC_RELAXED);
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
        "PC RTP 音频测试已启用：自动发现 UDP=%u，RTP UDP=%u；task core=%d priority=%u",
        static_cast<unsigned>(kDiscoveryPort),
        static_cast<unsigned>(CONFIG_FAKEPOD_WIFI_RTP_TEST_PORT),
        static_cast<int>(kTaskCore),
        static_cast<unsigned>(kTaskPriority));
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
        const uint32_t queued_after_publish = head + 1U - tail;
        uint32_t peak = __atomic_load_n(&g_queue_peak, __ATOMIC_RELAXED);
        while (queued_after_publish > peak &&
               !__atomic_compare_exchange_n(
                   &g_queue_peak, &peak, queued_after_publish, false,
                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }

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
