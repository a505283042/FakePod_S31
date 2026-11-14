#include "ppa_ops.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "PPA";

// PSRAM 输出缓冲需按 cache line 对齐，S31 为 64B。
static constexpr size_t PPA_PSRAM_ALIGN = 64U;
// 与 cover_surface_cache 的 COVER_DIM_SCALE_NUM/SHIFT 保持一致：13/32 压暗。
static constexpr uint8_t PPA_DIM_ALPHA = 151U;

static ppa_client_handle_t s_blend_client = nullptr;
static SemaphoreHandle_t s_lock = nullptr;
static uint8_t *s_alpha_mask = nullptr;
static uint32_t s_alpha_mask_bytes = 0U;
static uint16_t s_alpha_mask_w = 0U;
static uint16_t s_alpha_mask_h = 0U;
static bool s_init_failed = false;

static bool ppa_ensure_client()
{
    if (s_blend_client != nullptr) {
        return true;
    }
    if (s_init_failed) {
        return false;
    }

    ppa_client_config_t config = {};
    config.oper_type = PPA_OPERATION_BLEND;
    config.max_pending_trans_num = 1;
    if (ppa_register_client(&config, &s_blend_client) != ESP_OK || s_blend_client == nullptr) {
        s_blend_client = nullptr;
        s_init_failed = true;
        ESP_LOGW(TAG, "PPA blend 客户端注册失败，封面压暗回退软件实现");
        return false;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        s_init_failed = true;
        return false;
    }

    ESP_LOGI(TAG, "PPA blend 客户端就绪");
    return true;
}

static bool ppa_ensure_alpha_mask(uint16_t width, uint16_t height)
{
    if (s_alpha_mask != nullptr && s_alpha_mask_w == width && s_alpha_mask_h == height) {
        return true;
    }
    heap_caps_free(s_alpha_mask);
    s_alpha_mask = nullptr;
    s_alpha_mask_w = 0U;
    s_alpha_mask_h = 0U;

    const uint32_t bytes = static_cast<uint32_t>(width) * height;
    uint8_t *mask = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        PPA_PSRAM_ALIGN, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (mask == nullptr) {
        ESP_LOGW(TAG, "PPA alpha 蒙版分配失败：%ux%u", width, height);
        return false;
    }
    memset(mask, PPA_DIM_ALPHA, bytes);

    s_alpha_mask = mask;
    s_alpha_mask_bytes = bytes;
    s_alpha_mask_w = width;
    s_alpha_mask_h = height;
    return true;
}

bool ppa_dim_rgb565(const void *src, void *dst, uint16_t width, uint16_t height,
                    size_t dst_bytes, int64_t *out_elapsed_us)
{
    if (src == nullptr || dst == nullptr || width == 0U || height == 0U) {
        return false;
    }
    if (!ppa_ensure_client() || !ppa_ensure_alpha_mask(width, height)) {
        return false;
    }

    const int64_t started_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    ppa_blend_oper_config_t config = {};
    config.in_bg.buffer = src;
    config.in_bg.pic_w = width;
    config.in_bg.pic_h = height;
    config.in_bg.block_w = width;
    config.in_bg.block_h = height;
    config.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    config.in_fg.buffer = s_alpha_mask;
    config.in_fg.pic_w = width;
    config.in_fg.pic_h = height;
    config.in_fg.block_w = width;
    config.in_fg.block_h = height;
    config.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8;
    config.fg_fix_rgb_val.val = 0U;  // 前景固定为黑色，只由 A8 提供 alpha。

    config.out.buffer = dst;
    config.out.buffer_size = static_cast<uint32_t>(dst_bytes);
    config.out.pic_w = width;
    config.out.pic_h = height;
    config.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    config.mode = PPA_TRANS_MODE_BLOCKING;

    const esp_err_t ret = ppa_do_blend(s_blend_client, &config);
    xSemaphoreGive(s_lock);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PPA 封面压暗失败：%s，回退软件实现", esp_err_to_name(ret));
        return false;
    }

    if (out_elapsed_us != nullptr) {
        *out_elapsed_us = esp_timer_get_time() - started_us;
    }

    static bool s_first_dim_logged = false;
    if (!s_first_dim_logged) {
        s_first_dim_logged = true;
        ESP_LOGI(TAG, "PPA 压暗完成：%ux%u 耗时=%lldus",
            width, height, static_cast<long long>(esp_timer_get_time() - started_us));
    }
    return true;
}
