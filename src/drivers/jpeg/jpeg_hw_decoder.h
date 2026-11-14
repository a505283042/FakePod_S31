#pragma once

#include <stddef.h>
#include <stdint.h>

// ESP32-S31 片内硬件 JPEG 解码（esp_driver_jpeg）封装。
// 仅供本项目内部使用：调用方失败时必须回退到 esp_new_jpeg 软件解码。
// 约束（来自 IDF 驱动）：
// - 只支持基线 JPEG（SOF0），progressive 会解析失败并回退；
// - 宽高乘积必须能被 8 整除；
// - 输出按 MCU 对齐（4:2:0/4:2:2 水平 16、垂直 16/8），本封装负责裁掉填充区，
//   对调用方始终输出紧凑的 width * height RGB565。

typedef enum {
    JPEG_HW_ORDER_LE = 0, // RGB565 小端字节序（LVGL / 封面缓存使用）
    JPEG_HW_ORDER_BE = 1, // RGB565 大端字节序（视频直写 CO5300 使用）
} jpeg_hw_order_t;

// 硬件 JPEG 引擎是否可用（懒初始化，失败后固定返回 false）。
bool jpeg_hw_available(void);

// 仅解析 JPEG 头，不占用硬件；padded_width/padded_height 可为 nullptr。
bool jpeg_hw_probe(const uint8_t *data, size_t size,
    uint32_t *width, uint32_t *height,
    uint32_t *padded_width, uint32_t *padded_height);

// 硬件解码为紧凑 RGB565（width * height * 2 字节），dst 容量必须足够。
// 失败（非基线、尺寸超限、硬件超时等）返回 false，调用方应回退软件解码。
bool jpeg_hw_decode_rgb565(const uint8_t *data, size_t size,
    uint8_t *dst, size_t dst_capacity, jpeg_hw_order_t order,
    uint32_t *width, uint32_t *height);
