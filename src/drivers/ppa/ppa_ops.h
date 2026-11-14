#ifndef FAKEPOD_PPA_OPS_H
#define FAKEPOD_PPA_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// S31 的 PPA 缩放步进只有 1/16（6.25%），无法替代封面精确的 460x460 中心裁切重采样；
// 这里只提供「无损」的混色能力：把 RGB565 封面按固定 alpha 叠一层黑色，生成 dimmed 面。
// 换算是 13/32 压暗（原软件实现），即 alpha = 1 - 13/32 ≈ 151/255。

// 第一次调用会按需注册 PPA blend 客户端；失败后返回 false，调用方需回退软件实现。
bool ppa_dim_rgb565(const void *src, void *dst, uint16_t width, uint16_t height,
                    size_t dst_bytes, int64_t *out_elapsed_us);

#ifdef __cplusplus
}
#endif

#endif
