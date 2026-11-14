#include "axp2101.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"

static const char *TAG = "AXP2101";

namespace {

// AXP2101 寄存器（与 XPowersLib 定义一致）
constexpr uint8_t REG_STATUS1 = 0x00;
constexpr uint8_t REG_IC_TYPE = 0x03;
constexpr uint8_t REG_ADC_CHANNEL_CTRL = 0x30;
constexpr uint8_t REG_BAT_VOLT_H = 0x34;
constexpr uint8_t REG_BAT_VOLT_L = 0x35;
constexpr uint8_t REG_INTEN2 = 0x41;
constexpr uint8_t REG_INTSTS1 = 0x48;
constexpr uint8_t REG_INTSTS2 = 0x49;
constexpr uint8_t REG_INTSTS3 = 0x4A;
constexpr uint8_t REG_BAT_DET_CTRL = 0x68;
constexpr uint8_t REG_BAT_PERCENT = 0xA4;

constexpr uint8_t EXPECTED_CHIP_ID = 0x4A;
constexpr uint8_t STATUS1_BAT_PRESENT = 1U << 3;

// INTEN2/INTSTS2：bit2 = 电源键长按，bit3 = 电源键短按
constexpr uint8_t INT2_PWRKEY_LONG = 1U << 2;
constexpr uint8_t INT2_PWRKEY_SHORT = 1U << 3;

// 电池电压 ADC：13bit，单位 mV（高 5 位 + 低 8 位）
constexpr uint8_t BAT_VOLT_H_MASK = 0x1F;
constexpr int BAT_VOLT_MIN_MV = 2000;
constexpr int BAT_VOLT_MAX_MV = 5000;

i2c_master_dev_handle_t g_device = nullptr;
bool g_ready = false;

esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    if (g_device == nullptr || value == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_read_reg8(g_device, reg, value);
}

esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    if (g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_write_reg8(g_device, reg, value);
}

esp_err_t set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t value = 0;
    const esp_err_t read_ret = read_reg(reg, &value);
    if (read_ret != ESP_OK) {
        return read_ret;
    }
    return write_reg(reg, static_cast<uint8_t>(value | mask));
}

} // namespace

esp_err_t axp2101_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    esp_err_t ret = i2c_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }

    if (g_device == nullptr) {
        ret = i2c_bus_add_device(FAKEPOD_ADDR_AXP2101, &g_device);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AXP2101 挂载失败（0x%02X）：%s",
                FAKEPOD_ADDR_AXP2101, esp_err_to_name(ret));
            return ret;
        }
    }

    uint8_t chip_id = 0;
    ret = read_reg(REG_IC_TYPE, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 芯片 ID 读取失败：%s", esp_err_to_name(ret));
        (void)i2c_bus_remove_device(&g_device);
        return ret;
    }
    if (chip_id != EXPECTED_CHIP_ID) {
        ESP_LOGE(TAG, "AXP2101 芯片 ID 不匹配：读到 0x%02X，期望 0x%02X",
            chip_id, EXPECTED_CHIP_ID);
        (void)i2c_bus_remove_device(&g_device);
        return ESP_ERR_NOT_FOUND;
    }

    // 打开电池电压 ADC 与电池在位检测
    ret = set_bits(REG_ADC_CHANNEL_CTRL, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "电池电压 ADC 使能失败：%s", esp_err_to_name(ret));
    }

    ret = set_bits(REG_BAT_DET_CTRL, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "电池在位检测使能失败：%s", esp_err_to_name(ret));
    }

    // 使能电源键短按 / 长按中断；开关机时序仍由 PMU 默认配置负责
    ret = set_bits(REG_INTEN2,
        static_cast<uint8_t>(INT2_PWRKEY_SHORT | INT2_PWRKEY_LONG));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "电源键中断使能失败：%s", esp_err_to_name(ret));
        (void)i2c_bus_remove_device(&g_device);
        return ret;
    }

    // 清除上电期间遗留的中断状态，避免开机即被当成按键事件
    ret = axp2101_clear_irq();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "清除遗留中断失败：%s", esp_err_to_name(ret));
    }

    g_ready = true;
    ESP_LOGI(TAG, "AXP2101 就绪：I2C0 0x%02X，电源键短按/长按中断已使能",
        FAKEPOD_ADDR_AXP2101);
    return ESP_OK;
}

bool axp2101_is_ready()
{
    return g_ready;
}

bool axp2101_is_battery_connected()
{
    uint8_t status = 0;
    if (!g_ready || read_reg(REG_STATUS1, &status) != ESP_OK) {
        return false;
    }
    return (status & STATUS1_BAT_PRESENT) != 0;
}

int axp2101_battery_mv()
{
    if (!g_ready || !axp2101_is_battery_connected()) {
        return -1;
    }

    uint8_t raw[2] = {};
    if (i2c_bus_read_bytes(g_device, REG_BAT_VOLT_H, raw, sizeof(raw)) != ESP_OK) {
        return -1;
    }

    // AXP2101 电池电压为 13bit：0x34 低 5 位为高位，0x35 为低位，单位为 mV
    const int mv = static_cast<int>(
        ((raw[0] & BAT_VOLT_H_MASK) << 8) | raw[1]);

    if (mv < BAT_VOLT_MIN_MV || mv > BAT_VOLT_MAX_MV) {
        return -1;
    }
    return mv;
}

int axp2101_battery_percent()
{
    if (!g_ready || !axp2101_is_battery_connected()) {
        return -1;
    }

    uint8_t percent = 0;
    if (read_reg(REG_BAT_PERCENT, &percent) != ESP_OK) {
        return -1;
    }
    if (percent > 100) {
        return -1;
    }
    return static_cast<int>(percent);
}

axp2101_pwrkey_event_t axp2101_take_pwrkey_event()
{
    if (!g_ready) {
        return AXP2101_PWRKEY_NONE;
    }

    uint8_t status = 0;
    if (read_reg(REG_INTSTS2, &status) != ESP_OK) {
        return AXP2101_PWRKEY_NONE;
    }

    axp2101_pwrkey_event_t event = AXP2101_PWRKEY_NONE;
    if ((status & INT2_PWRKEY_LONG) != 0) {
        event = AXP2101_PWRKEY_LONG;
    } else if ((status & INT2_PWRKEY_SHORT) != 0) {
        event = AXP2101_PWRKEY_SHORT;
    }

    if (event != AXP2101_PWRKEY_NONE) {
        (void)axp2101_clear_irq();
    }
    return event;
}

esp_err_t axp2101_clear_irq()
{
    if (g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // 三个中断状态寄存器写 1 清除
    esp_err_t ret = write_reg(REG_INTSTS1, 0xFF);
    const esp_err_t ret2 = write_reg(REG_INTSTS2, 0xFF);
    const esp_err_t ret3 = write_reg(REG_INTSTS3, 0xFF);
    if (ret == ESP_OK) ret = ret2;
    if (ret == ESP_OK) ret = ret3;
    return ret;
}
