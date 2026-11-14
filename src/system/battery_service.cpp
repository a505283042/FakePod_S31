#include "battery_service.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs.h"

#include "axp2101.h"
#include "board_pins.h"
#include "persistent_state.h"

#ifndef FAKEPOD_BATTERY_DIAGNOSTIC_LOG
#define FAKEPOD_BATTERY_DIAGNOSTIC_LOG 0
#endif

namespace {

static const char *TAG = "电池";

// ESP32-S31 硬件：电池电压与电量由 AXP2101 内部 ADC / 电量计采集，
// 主控不再使用 GPIO ADC 分压。仍保留 2 秒采样与 EMA 滤波，避免电压抖动直接映射到 UI。
static constexpr TickType_t BATTERY_SAMPLE_INTERVAL = pdMS_TO_TICKS(2000);

// 每 2 秒一个样本，alpha=1/8，等效慢滤波时间尺度约十几秒；首样本直接建立基线。
static constexpr uint32_t BATTERY_EMA_OLD_WEIGHT = 7U;
static constexpr uint32_t BATTERY_EMA_DIVISOR = 8U;

// RELEASE 日志只记录低电量状态边界；退出阈值留 2% 回差，避免临界点反复刷日志。
static constexpr uint8_t BATTERY_LOW_ENTER_PERCENT = 15U;
static constexpr uint8_t BATTERY_LOW_EXIT_PERCENT = 17U;
static constexpr uint8_t BATTERY_CRITICAL_ENTER_PERCENT = 5U;
static constexpr uint8_t BATTERY_CRITICAL_EXIT_PERCENT = 7U;

enum class BatteryLogState : uint8_t
{
    Unknown = 0,
    Normal,
    Low,
    Critical,
};

struct BatterySocPoint
{
    uint16_t mv;
    uint8_t percent;
};

// 1S Li-ion/LiPo 的 V1 经验 LUT。实机稳定满电端约 4.188~4.192V，
// 因此把 UI 的 100% 显示阈值收在 4.190V；不通过全局增益硬拉整条电压曲线。
static constexpr BatterySocPoint BATTERY_SOC_LUT[] = {
    {4190U, 100U},
    {4100U, 90U},
    {4000U, 80U},
    {3920U, 70U},
    {3850U, 60U},
    {3790U, 50U},
    {3730U, 40U},
    {3680U, 30U},
    {3620U, 20U},
    {3500U, 10U},
    {3350U, 5U},
    {3250U, 0U},
};

static bool g_ready = false;
static TickType_t g_next_sample_tick = 0;
static uint32_t g_filtered_mv = 0U;
static BatteryLogState g_log_state = BatteryLogState::Unknown;
static BatterySnapshot g_snapshot = {};
static portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;

// R46.0.116：NVS 中单独保存 50% 的 ADC 实测电压，不动播放状态持久化 schema。
// armed 持久化，保证充满电后在 50% 之前意外重启也不会失去本次记录机会。
static constexpr char BATTERY_HALF_NVS_NAMESPACE[] = "bat50";
static constexpr char BATTERY_HALF_NVS_KEY[] = "checkpoint";
static constexpr uint8_t BATTERY_HALF_SCHEMA = 1U;
static constexpr uint8_t BATTERY_HALF_REARM_PERCENT = 95U;
static constexpr uint8_t BATTERY_HALF_INITIAL_ARM_PERCENT = 55U;
static constexpr uint8_t BATTERY_HALF_CAPTURE_PERCENT = 50U;
static constexpr uint8_t BATTERY_HALF_STABLE_SAMPLES = 3U;
static constexpr TickType_t BATTERY_HALF_RETRY_INTERVAL = pdMS_TO_TICKS(30000);

struct BatteryHalfStored
{
    uint8_t schema;
    uint8_t valid;
    uint8_t armed;
    uint8_t percent;
    uint16_t measured_mv;
    uint16_t filtered_mv;
};
static_assert(sizeof(BatteryHalfStored) == 8U, "battery NVS format changed");

static BatteryHalfStored g_half_stored = {BATTERY_HALF_SCHEMA, 0U, 0U, 0U, 0U, 0U};
static portMUX_TYPE g_half_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_half_init_attempted = false;
static bool g_half_storage_ready = false;
static bool g_half_seen_above = false;
static uint8_t g_half_high_count = 0U;
static uint8_t g_half_low_count = 0U;
static TickType_t g_half_retry_due = 0U;

static bool battery_half_voltage_valid(uint32_t mv)
{
    return mv >= 2500U && mv <= 5000U;
}

static void battery_half_load_once()
{
    if (g_half_init_attempted) return;
    g_half_init_attempted = true;
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(BATTERY_HALF_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        g_half_storage_ready = true;
        return;
    }
    if (opened != ESP_OK) return;

    BatteryHalfStored loaded = {};
    size_t bytes = sizeof(loaded);
    const esp_err_t ret = nvs_get_blob(handle, BATTERY_HALF_NVS_KEY, &loaded, &bytes);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        g_half_storage_ready = true;
        return;
    }
    if (ret != ESP_OK || bytes != sizeof(loaded) || loaded.schema != BATTERY_HALF_SCHEMA ||
        loaded.valid > 1U || loaded.armed > 1U ||
        (loaded.valid && (loaded.percent > BATTERY_HALF_CAPTURE_PERCENT ||
            !battery_half_voltage_valid(loaded.measured_mv) ||
            !battery_half_voltage_valid(loaded.filtered_mv)))) {
        // 旧/损坏/未知记录不自动覆盖；原有电池采样继续正常运行。
        return;
    }
    portENTER_CRITICAL(&g_half_mux);
    g_half_stored = loaded;
    portEXIT_CRITICAL(&g_half_mux);
    g_half_storage_ready = true;
}

static bool battery_half_commit(const BatteryHalfStored &new_value)
{
    nvs_handle_t handle = 0;
    if (nvs_open(BATTERY_HALF_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t ret = nvs_set_blob(handle, BATTERY_HALF_NVS_KEY, &new_value, sizeof(new_value));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) return false;
    portENTER_CRITICAL(&g_half_mux);
    g_half_stored = new_value;
    portEXIT_CRITICAL(&g_half_mux);
    return true;
}

static void battery_half_sample_update(const BatterySnapshot &snapshot)
{
    if (!g_half_storage_ready || !snapshot.valid || !snapshot.calibrated ||
        !battery_half_voltage_valid(snapshot.battery_mv) ||
        !battery_half_voltage_valid(snapshot.filtered_mv)) return;

    const TickType_t now = xTaskGetTickCount();
    if (g_half_retry_due != 0U && static_cast<int32_t>(now - g_half_retry_due) < 0) return;

    const bool need_arm = !g_half_stored.armed &&
        (snapshot.percent >= BATTERY_HALF_REARM_PERCENT ||
         (!g_half_stored.valid && snapshot.percent >= BATTERY_HALF_INITIAL_ARM_PERCENT));
    if (need_arm) {
        if (g_half_high_count < BATTERY_HALF_STABLE_SAMPLES) ++g_half_high_count;
        if (g_half_high_count >= BATTERY_HALF_STABLE_SAMPLES) {
            BatteryHalfStored armed = g_half_stored;
            armed.armed = 1U;
            if (!battery_half_commit(armed)) {
                g_half_retry_due = now + BATTERY_HALF_RETRY_INTERVAL;
            } else {
                g_half_retry_due = 0U;
            }
            g_half_high_count = 0U;
        }
    } else {
        g_half_high_count = 0U;
    }

    // 即使 NVS 显示 armed=1，若本次开机直接处于 <=50%，也不能覆盖旧记录。
    if (snapshot.percent > BATTERY_HALF_CAPTURE_PERCENT) {
        g_half_seen_above = true;
    }
    if (!g_half_stored.armed || !g_half_seen_above ||
        snapshot.percent > BATTERY_HALF_CAPTURE_PERCENT) {
        g_half_low_count = 0U;
        return;
    }
    if (g_half_low_count < BATTERY_HALF_STABLE_SAMPLES) ++g_half_low_count;
    if (g_half_low_count < BATTERY_HALF_STABLE_SAMPLES) return;

    BatteryHalfStored captured = g_half_stored;
    captured.valid = 1U;
    captured.armed = 0U;
    captured.percent = snapshot.percent;
    captured.measured_mv = snapshot.battery_mv;
    captured.filtered_mv = snapshot.filtered_mv;
    if (battery_half_commit(captured)) {
        g_half_low_count = 0U;
        g_half_retry_due = 0U;
    } else {
        g_half_retry_due = now + BATTERY_HALF_RETRY_INTERVAL;
    }
}

static uint8_t battery_soc_from_mv(uint32_t mv)
{
    if (mv >= BATTERY_SOC_LUT[0].mv) {
        return BATTERY_SOC_LUT[0].percent;
    }

    const size_t point_count = sizeof(BATTERY_SOC_LUT) / sizeof(BATTERY_SOC_LUT[0]);
    if (mv <= BATTERY_SOC_LUT[point_count - 1U].mv) {
        return BATTERY_SOC_LUT[point_count - 1U].percent;
    }

    for (size_t i = 0U; i + 1U < point_count; ++i) {
        const BatterySocPoint high = BATTERY_SOC_LUT[i];
        const BatterySocPoint low = BATTERY_SOC_LUT[i + 1U];
        if (mv <= high.mv && mv >= low.mv) {
            const uint32_t voltage_span = static_cast<uint32_t>(high.mv - low.mv);
            const uint32_t percent_span = static_cast<uint32_t>(high.percent - low.percent);
            const uint32_t voltage_from_low = mv - low.mv;
            const uint32_t interpolated = static_cast<uint32_t>(low.percent) +
                (voltage_from_low * percent_span + voltage_span / 2U) / voltage_span;
            return static_cast<uint8_t>(interpolated > 100U ? 100U : interpolated);
        }
    }

    return 0U;
}

static void battery_update_log_state(const BatterySnapshot &snapshot)
{
    BatteryLogState next = BatteryLogState::Normal;
    if (g_log_state == BatteryLogState::Critical && snapshot.percent < BATTERY_LOW_EXIT_PERCENT) {
        next = snapshot.percent >= BATTERY_CRITICAL_EXIT_PERCENT
            ? BatteryLogState::Low
            : BatteryLogState::Critical;
    } else if (g_log_state == BatteryLogState::Low && snapshot.percent < BATTERY_LOW_EXIT_PERCENT) {
        next = snapshot.percent <= BATTERY_CRITICAL_ENTER_PERCENT
            ? BatteryLogState::Critical
            : BatteryLogState::Low;
    } else if (snapshot.percent <= BATTERY_CRITICAL_ENTER_PERCENT) {
        next = BatteryLogState::Critical;
    } else if (snapshot.percent <= BATTERY_LOW_ENTER_PERCENT) {
        next = BatteryLogState::Low;
    }

    if (next == g_log_state) {
        return;
    }

    const BatteryLogState previous = g_log_state;
    g_log_state = next;

    if (next == BatteryLogState::Critical) {
        ESP_LOGW(TAG, "进入严重低电量：%u%%，%umV",
            static_cast<unsigned>(snapshot.percent),
            static_cast<unsigned>(snapshot.filtered_mv));

        // R44.3：严重低电量只在状态边沿触发一次持久化，尽量在 Brownout 前保存最新播放状态。
        // 不做自动关机，避免改变现有产品行为；这里只缩小异常掉电丢状态窗口。
        const esp_err_t flush_ret = persistent_state_flush();
        if (flush_ret != ESP_OK && flush_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "严重低电量保存运行状态失败：%s", esp_err_to_name(flush_ret));
        }
    } else if (next == BatteryLogState::Low) {
        if (previous == BatteryLogState::Critical) {
            ESP_LOGI(TAG, "电量已离开严重低电量区间：%u%%，%umV",
                static_cast<unsigned>(snapshot.percent),
                static_cast<unsigned>(snapshot.filtered_mv));
        } else {
            ESP_LOGW(TAG, "进入低电量：%u%%，%umV",
                static_cast<unsigned>(snapshot.percent),
                static_cast<unsigned>(snapshot.filtered_mv));
        }
    } else if (previous != BatteryLogState::Unknown) {
        ESP_LOGI(TAG, "低电量状态解除：%u%%，%umV",
            static_cast<unsigned>(snapshot.percent),
            static_cast<unsigned>(snapshot.filtered_mv));
    }
}

static esp_err_t battery_take_sample(BatterySnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!axp2101_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!axp2101_is_battery_connected()) {
        // 无电池（仅 USB 供电）不是故障，UI 维持“无电量数据”状态。
        return ESP_ERR_NOT_FOUND;
    }

    const int measured_mv = axp2101_battery_mv();
    if (measured_mv <= 0) {
        return ESP_FAIL;
    }

    const uint32_t battery_mv = static_cast<uint32_t>(measured_mv);
    if (g_filtered_mv == 0U) {
        g_filtered_mv = battery_mv;
    } else {
        g_filtered_mv = (g_filtered_mv * BATTERY_EMA_OLD_WEIGHT + battery_mv +
            BATTERY_EMA_DIVISOR / 2U) / BATTERY_EMA_DIVISOR;
    }

    const int gauge_percent = axp2101_battery_percent();

    BatterySnapshot snapshot = {};
    snapshot.valid = true;
    snapshot.calibrated = true;
    // PMU 直读，不再有 ADC 原始码；这三个字段保留是为了兼容既有快照结构。
    snapshot.raw_average = 0U;
    snapshot.raw_min = 0U;
    snapshot.raw_max = 0U;
    snapshot.adc_mv = static_cast<uint16_t>(battery_mv);
    snapshot.battery_mv = static_cast<uint16_t>(battery_mv);
    snapshot.filtered_mv = static_cast<uint16_t>(g_filtered_mv);
    // 优先采用 AXP2101 电量计；读取失败时退回电压 LUT。
    snapshot.percent = gauge_percent >= 0
        ? static_cast<uint8_t>(gauge_percent)
        : battery_soc_from_mv(g_filtered_mv);

    portENTER_CRITICAL(&g_snapshot_mux);
    snapshot.sequence = g_snapshot.sequence + 1U;
    g_snapshot = snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);

    *out_snapshot = snapshot;
    return ESP_OK;
}

} // namespace

esp_err_t battery_service_init()
{
    battery_half_load_once();
    if (g_ready) {
        return ESP_OK;
    }

    const esp_err_t ret = axp2101_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 初始化失败：%s；不提供电量数据", esp_err_to_name(ret));
        return ret;
    }

    g_filtered_mv = 0U;
    g_log_state = BatteryLogState::Unknown;
    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot = {};
    portEXIT_CRITICAL(&g_snapshot_mux);
    g_next_sample_tick = xTaskGetTickCount();
    g_ready = true;

    ESP_LOGI(TAG, "BatteryService就绪：AXP2101 电池电压 / 电量计，采样间隔2s");
    return ESP_OK;
}

bool battery_service_is_ready()
{
    return g_ready;
}

void battery_service_update()
{
    if (!g_ready) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<int32_t>(now - g_next_sample_tick) < 0) {
        return;
    }
    g_next_sample_tick = now + BATTERY_SAMPLE_INTERVAL;

    BatterySnapshot snapshot = {};
    const esp_err_t ret = battery_take_sample(&snapshot);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "未检测到电池，跳过采样");
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 电池采样失败：%s；2秒后重试", esp_err_to_name(ret));
        return;
    }

    battery_update_log_state(snapshot);
    battery_half_sample_update(snapshot);

#if FAKEPOD_BATTERY_DIAGNOSTIC_LOG
    // 诊断模式才恢复每 2 秒 VBAT / 滤波 / SOC 详细遥测。
    ESP_LOGI(TAG,
        "PMU：vbat=%umV filtered=%umV soc=%u%% seq=%lu",
        static_cast<unsigned>(snapshot.battery_mv),
        static_cast<unsigned>(snapshot.filtered_mv),
        static_cast<unsigned>(snapshot.percent),
        static_cast<unsigned long>(snapshot.sequence));
#endif
}

bool battery_service_get_snapshot(BatterySnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return out_snapshot->valid;
}

bool battery_service_get_half_record(BatteryHalfRecord *out_record)
{
    if (out_record == nullptr) return false;
    BatteryHalfStored stored = {};
    portENTER_CRITICAL(&g_half_mux);
    stored = g_half_stored;
    portEXIT_CRITICAL(&g_half_mux);
    if (!g_half_storage_ready || !stored.valid) return false;
    out_record->measured_mv = stored.measured_mv;
    out_record->filtered_mv = stored.filtered_mv;
    out_record->percent = stored.percent;
    return true;
}
