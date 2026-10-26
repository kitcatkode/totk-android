/* NVN format and state enums -> Vulkan (vk_fmt.c). */
#pragma once
#include "vk_load.h"
#include <stdint.h>

/* 1 exact, 2 substitute, 0 none (*out = VK_FORMAT_UNDEFINED); swz: the format's own component remap (r/b swap, a = 1) */
int vk_fmt(int nvn_format, VkFormat *out, VkComponentMapping *swz);
void vk_fmt_set_depth(VkFormat d24, VkFormat d24s8);   /* what DEPTH24 / DEPTH24_STENCIL8 become on this device */
int vk_fmt_is_depth(int f);
int vk_fmt_has_depth(int f);
int vk_fmt_has_stencil(int f);
VkImageAspectFlags vk_fmt_aspect(int f);
int vk_fmt_class(int f);   /* 0 float, 1 uint, 2 sint */
VkFormat vk_vertex_fmt(int nvn_format);
VkBlendFactor vk_blend_factor(int nvn_blend_func);
VkBlendOp vk_blend_op(int nvn_blend_equation);
VkCompareOp vk_compare(int nvn_func);
VkStencilOp vk_stencil_op(int nvn_op);
VkSamplerAddressMode vk_wrap(int nvn_wrap);
VkComponentSwizzle vk_swizzle_sel(int nvn_swizzle);
