#pragma once

#include <stdbool.h>
#include "esp_err.h"

// ============================================================
// AXP2101 电源管理
// 总线：I2C0  地址：0x34（7-bit）
// 负责：供电轨配置、电池电压/电量、POWER 按键事件
// ============================================================

typedef enum {
    AXP2101_PWRKEY_NONE = 0,
    AXP2101_PWRKEY_SHORT,
    AXP2101_PWRKEY_LONG,
} axp2101_pwrkey_event_t;

// 初始化 PMU：校验芯片 ID、打开电池 ADC / 电量计、使能电源键中断
esp_err_t axp2101_init(void);

// 是否初始化成功
bool axp2101_is_ready(void);

// 电池是否在位
bool axp2101_is_battery_connected(void);

// 电池电压（mV）；读取失败返回 -1
int axp2101_battery_mv(void);

// 电池电量（0~100）；读取失败返回 -1
int axp2101_battery_percent(void);

// 读取并清除一次电源按键事件（短按 / 长按）
axp2101_pwrkey_event_t axp2101_take_pwrkey_event(void);

// 清除 PMU 中断状态
esp_err_t axp2101_clear_irq(void);
