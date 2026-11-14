#pragma once

#include "esp_err.h"

// ESP32-S31 硬件：上方电源键直接接 AXP2101 PWRKEY，主控无独立 GPIO。
// 短按/长按事件通过 I2C0 读取 AXP2101 中断状态获得；长按按原有逻辑提前静音收尾并显式 NVS flush。
// 不在 ISR 中访问 NVS；也不会提前关闭 Audio/Display，关机由 PMU 硬件时序完成。
esp_err_t power_service_init();

// READY 后每轮调用；内部限频 50ms 轮询 AXP2101 电源键中断。
void power_service_update();
