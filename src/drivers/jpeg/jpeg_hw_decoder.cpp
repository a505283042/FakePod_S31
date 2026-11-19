#include "drivers/jpeg/jpeg_hw_decoder.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>

namespace {

constexpr const char *TAG = "JPEG_HW";
// 单帧 460x460 在 4:2:0 下会填充到 464x464，解码耗时远小于该值。
constexpr int kEngineTimeoutMs = 800;
// 失败原因只打印前几次，避免视频逐帧回退时刷屏。
constexpr int kMaxFailureLogs = 3;

jpeg_decoder_handle_t g_engine = nullptr;
bool g_engine_init_failed = false;
uint8_t *g_pad_buffer = nullptr;
size_t g_pad_capacity = 0;
int g_failure_logs = 0;
bool g_logged_first_success = false;

SemaphoreHandle_t lock()
{
    static SemaphoreHandle_t s_lock = xSemaphoreCreateMutex();
    return s_lock;
}

// 调用方需持有 lock() 返回的互斥量。
bool ensure_engine_locked()
{
    if (g_engine != nullptr) return true;
    if (g_engine_init_failed) return false;

    jpeg_decode_engine_cfg_t cfg = {};
    cfg.intr_priority = 0;
    cfg.timeout_ms = kEngineTimeoutMs;

    esp_err_t err = jpeg_new_decoder_engine(&cfg, &g_engine);
    if (err != ESP_OK || g_engine == nullptr) {
        g_engine_init_failed = true;
        g_engine = nullptr;
        ESP_LOGW(TAG, "硬件 JPEG 引擎初始化失败(%s)，固定回退软件解码", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "硬件 JPEG 解码引擎就绪");
    return true;
}

// 调用方需持有 g_lock。
bool ensure_pad_buffer_locked(size_t bytes)
{
    if (g_pad_buffer != nullptr && g_pad_capacity >= bytes) return true;

    if (g_pad_buffer != nullptr) {
        heap_caps_free(g_pad_buffer);
        g_pad_buffer = nullptr;
        g_pad_capacity = 0;
    }

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {};
    mem_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t allocated = 0;
    g_pad_buffer = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(bytes, &mem_cfg, &allocated));
    if (g_pad_buffer == nullptr) {
        ESP_LOGW(TAG, "硬件 JPEG 输出缓冲分配失败: %uB", static_cast<unsigned>(bytes));
        return false;
    }
    g_pad_capacity = allocated;
    return true;
}

// MCU 对齐步长：4:2:0 → 16/16，4:2:2 → 16/8，4:4:4/灰度 → 8/8。
void sampling_to_mcu(jpeg_down_sampling_type_t sampling, uint32_t *mcux, uint32_t *mcuy)
{
    switch (sampling) {
    case JPEG_DOWN_SAMPLING_YUV420:
        *mcux = 16U;
        *mcuy = 16U;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        *mcux = 16U;
        *mcuy = 8U;
        break;
    default:
        *mcux = 8U;
        *mcuy = 8U;
        break;
    }
}

uint32_t align_up(uint32_t value, uint32_t align)
{
    return (value + align - 1U) / align * align;
}

void log_failure(const char *reason)
{
    if (g_failure_logs < kMaxFailureLogs) {
        ++g_failure_logs;
        ESP_LOGW(TAG, "硬件 JPEG 解码回退：%s", reason);
    }
}

} // namespace

bool jpeg_hw_available(void)
{
    if (g_engine_init_failed) return false;
    if (g_engine != nullptr) return true;

    SemaphoreHandle_t l = lock();
    if (l == nullptr) return false;
    xSemaphoreTake(l, portMAX_DELAY);
    const bool ok = ensure_engine_locked();
    xSemaphoreGive(l);
    return ok;
}

bool jpeg_hw_probe(const uint8_t *data, size_t size,
    uint32_t *width, uint32_t *height,
    uint32_t *padded_width, uint32_t *padded_height)
{
    if (data == nullptr || size == 0U) return false;

    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(data, static_cast<uint32_t>(size), &info) != ESP_OK) return false;
    if (info.width == 0U || info.height == 0U) return false;

    if (width != nullptr) *width = info.width;
    if (height != nullptr) *height = info.height;

    uint32_t mcux = 8U;
    uint32_t mcuy = 8U;
    sampling_to_mcu(info.sample_method, &mcux, &mcuy);
    if (padded_width != nullptr) *padded_width = align_up(info.width, mcux);
    if (padded_height != nullptr) *padded_height = align_up(info.height, mcuy);
    return true;
}

bool jpeg_hw_decode_rgb565(const uint8_t *data, size_t size,
    uint8_t *dst, size_t dst_capacity, jpeg_hw_order_t order,
    uint32_t *width, uint32_t *height)
{
    if (data == nullptr || size == 0U || dst == nullptr) return false;

    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(data, static_cast<uint32_t>(size), &info) != ESP_OK) {
        log_failure("头部解析失败(非基线SOF0或码流不完整)");
        return false;
    }
    if (info.width == 0U || info.height == 0U) return false;
    if ((static_cast<uint64_t>(info.width) * info.height) % 8U != 0U) {
        log_failure("宽高乘积不能被8整除");
        return false;
    }

    uint32_t mcux = 8U;
    uint32_t mcuy = 8U;
    sampling_to_mcu(info.sample_method, &mcux, &mcuy);
    const uint32_t padded_w = align_up(info.width, mcux);
    const uint32_t padded_h = align_up(info.height, mcuy);
    const size_t visible_bytes = static_cast<size_t>(info.width) * info.height * 2U;
    const size_t padded_bytes = static_cast<size_t>(padded_w) * padded_h * 2U;
    if (visible_bytes > dst_capacity) {
        log_failure("目标缓冲过小");
        return false;
    }

    SemaphoreHandle_t l = lock();
    if (l == nullptr) return false;
    xSemaphoreTake(l, portMAX_DELAY);

    if (!ensure_engine_locked()) {
        xSemaphoreGive(l);
        return false;
    }
    if (!ensure_pad_buffer_locked(padded_bytes)) {
        xSemaphoreGive(l);
        return false;
    }

    jpeg_decode_cfg_t decode_cfg = {};
    decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    decode_cfg.rgb_order = (order == JPEG_HW_ORDER_BE)
        ? JPEG_DEC_RGB_ELEMENT_ORDER_RGB
        : JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

    const int64_t started_us = esp_timer_get_time();
    uint32_t decoded_bytes = 0;
    esp_err_t err = jpeg_decoder_process(g_engine, &decode_cfg,
        data, static_cast<uint32_t>(size),
        g_pad_buffer, static_cast<uint32_t>(g_pad_capacity), &decoded_bytes);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (err != ESP_OK) {
        xSemaphoreGive(l);
        log_failure(esp_err_to_name(err));
        return false;
    }
    if (decoded_bytes != padded_bytes) {
        xSemaphoreGive(l);
        log_failure("输出尺寸与MCU对齐推断不一致");
        return false;
    }

    // 裁掉 MCU 填充列/行，输出紧凑 RGB565。
    const size_t row_bytes = static_cast<size_t>(info.width) * 2U;
    const size_t src_stride = static_cast<size_t>(padded_w) * 2U;
    for (uint32_t y = 0; y < info.height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * row_bytes,
            g_pad_buffer + static_cast<size_t>(y) * src_stride,
            row_bytes);
    }
    xSemaphoreGive(l);

    if (!g_logged_first_success) {
        g_logged_first_success = true;
        ESP_LOGI(TAG, "首帧硬件JPEG解码完成：%ux%u(填充%ux%u) 耗时%lldus",
            static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
            static_cast<unsigned>(padded_w), static_cast<unsigned>(padded_h),
            static_cast<long long>(elapsed_us));
    }

    if (width != nullptr) *width = info.width;
    if (height != nullptr) *height = info.height;
    return true;
}
