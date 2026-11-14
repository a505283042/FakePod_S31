#include "power_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lvgl.h"

#include "axp2101.h"
#include "audio_service.h"
#include "persistent_state.h"
#include "player/player_control.h"
#include "device_settings.h"
#include "screen_lock_simple.h"
#include "app/app_manager.h"
#include "app/video_app.h"
#include "usb_storage_service.h"

static const char *TAG = "电源";

// ESP32-S31 硬件：K2 直接接 AXP2101 PWRKEY，主控无独立 GPIO。
// 短按/长按事件由 AXP2101 中断上报，system_loop 每轮轮询寄存器（内部限频 50ms）。
// 关机时序由 PMU 自行完成，固件在长按事件时尽快静音收尾并显式保存 NVS。
static constexpr TickType_t POWER_KEY_POLL_INTERVAL = pdMS_TO_TICKS(50);

// AXP2101 的 PWRKEY 长按中断在按住约 1 秒时上报。上电时用户可能仍按着开机键，
// 因此在 READY 后的短时间内忽略事件，避免把“开机长按”误判为关机保存请求。
static constexpr TickType_t POWER_KEY_BOOT_GUARD_TICKS = pdMS_TO_TICKS(2000);

static bool g_ready = false;
static TickType_t g_next_poll_tick = 0;
static TickType_t g_armed_tick = 0;

// 与 GPIO61 辅助键一致：实体电源键只负责投递媒体动作。
// 真正的 Playlist/NAS transport 放到 6KB LVGL task 执行，避免上一曲路径继续压 SystemLoop 栈。
static void power_previous_track_async(void *)
{
    const AppId foreground = app_manager_foreground();
    if (foreground != AppId::Music && foreground != AppId::Ebook) {
        ESP_LOGI(TAG, "电源键上一曲异步请求取消：前台=%s", app_manager_name(foreground));
        return;
    }
    if (!player_control_previous()) {
        ESP_LOGW(TAG, "电源键上一曲异步执行失败");
    }
}

static void power_handle_short_press()
{
    // USB MSC active 时 TF owner 属于电脑，短按媒体动作不能重新提交后台 Music 请求。
    if (usb_storage_service_is_active()) {
        ESP_LOGI(TAG, "电源键短按：USB MSC活动中，忽略媒体动作");
        return;
    }

    // 短按功能由“辅助键模式”统一决定；长按关机保存逻辑完全不变。
    DeviceSettingsSnapshot settings = {};
    const bool track_mode = device_settings_get_snapshot(&settings) &&
        settings.aux_key_mode == DeviceAuxKeyMode::Track;

    if (track_mode) {
        // 切歌仅在音乐或电子书前台时生效，避免在其它 APP 误切歌。
        const AppId foreground = app_manager_foreground();
        if (foreground == AppId::Music || foreground == AppId::Ebook) {
            ESP_LOGI(TAG, "电源键短按 → 上一曲（投递LVGL任务）");
            (void)lv_async_call(power_previous_track_async, nullptr);
        } else if (foreground == AppId::Video && video_app_remote_control_active()) {
            // 视频播放中复用同一“辅助键模式”：切歌档位切换上一个视频。
            ESP_LOGI(TAG, "电源键短按 → 上一个视频");
            (void)video_app_remote_submit(VideoRemoteCommand::Previous);
        } else {
            ESP_LOGI(TAG, "电源键短按 → 非音乐/电子书/视频播放前台，切歌不生效");
        }
    } else {
        ESP_LOGI(TAG, "电源键短按 → 音量+1");
        (void)player_control_volume_up(1U);
    }
}

static void power_handle_long_press()
{
    PersistentStateStatus status = {};
    (void)persistent_state_get_status(&status);
    ESP_LOGI(TAG, "电源键长按：dirty=0x%08lX，开始音频静音收尾 + 显式NVS保存",
        static_cast<unsigned long>(status.dirty_bits));

    // PMU 后续会自行断电。确认关机意图后立即异步提交 Stop，
    // 让 AudioTask 与 NVS flush 并行执行淡出 -> DAC soft mute -> 零PCM -> 耳放掉电。
    // 这里不 wait，优先把时间留给 Flash 保存。
    if (!audio_service_stop(false)) {
        ESP_LOGW(TAG, "关机音频安全收尾提交失败；继续优先保存NVS");
    } else {
        ESP_LOGI(TAG, "关机音频安全收尾已提交：AudioTask后台执行pop-free Stop");
    }

    const TickType_t flush_begin = xTaskGetTickCount();
    const esp_err_t ret = persistent_state_flush();
    const uint32_t flush_ms = static_cast<uint32_t>(
        (xTaskGetTickCount() - flush_begin) * portTICK_PERIOD_MS
    );

    if (ret == ESP_OK) {
        PersistentStateStatus after = {};
        (void)persistent_state_get_status(&after);
        ESP_LOGI(TAG, "关机NVS保存完成：耗时=%lums remaining_dirty=0x%08lX；等待PMU断电",
            static_cast<unsigned long>(flush_ms),
            static_cast<unsigned long>(after.dirty_bits));
    } else {
        ESP_LOGE(TAG, "关机NVS保存失败：耗时=%lums %s；仍等待PMU断电",
            static_cast<unsigned long>(flush_ms),
            esp_err_to_name(ret));
    }
}

esp_err_t power_service_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    const esp_err_t ret = axp2101_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 初始化失败：%s；电源键事件不可用", esp_err_to_name(ret));
        return ret;
    }

    const TickType_t now = xTaskGetTickCount();
    g_next_poll_tick = now + POWER_KEY_POLL_INTERVAL;
    g_armed_tick = now + POWER_KEY_BOOT_GUARD_TICKS;
    g_ready = true;

    ESP_LOGI(TAG,
        "电源键就绪：AXP2101 短按/长按事件，长按触发NVS保存，上电%dms内忽略事件",
        static_cast<int>(POWER_KEY_BOOT_GUARD_TICKS * portTICK_PERIOD_MS));
    return ESP_OK;
}

void power_service_update()
{
    if (!g_ready) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<int32_t>(now - g_next_poll_tick) < 0) {
        return;
    }
    g_next_poll_tick = now + POWER_KEY_POLL_INTERVAL;

    const axp2101_pwrkey_event_t event = axp2101_take_pwrkey_event();
    if (event == AXP2101_PWRKEY_NONE) {
        return;
    }

    if (static_cast<int32_t>(now - g_armed_tick) < 0) {
        ESP_LOGI(TAG, "上电保护窗口内忽略电源键事件（可能是开机长按尾巴）");
        return;
    }

    screen_lock_simple_notify_user_activity();

    if (event == AXP2101_PWRKEY_SHORT) {
        power_handle_short_press();
    } else if (event == AXP2101_PWRKEY_LONG) {
        power_handle_long_press();
    }
}
