/* NVN formats -> Vulkan, the counterpart of third_party/d2r_gfx/gfx/tex/nvn_fmt_mtl.c (same table, same notes).
   Metal names packed formats from the least significant bits, Vulkan from the most significant: Metal RGB10A2Unorm is
   Vulkan A2B10G10R10, Metal B5G6R5Unorm is Vulkan R5G6B5, BGR5A1 is A1R5G5B5, A1BGR5 is R5G5B5A1, ABGR4 is R4G4B4A4.
   The r/b swaps and "no alpha" reads of the Metal table are kept as component swizzles. */
#include "vk_fmt.h"
#include <stdlib.h>
#include "../../third_party/d2r_gfx/gfx/nvn/include/nvn.h"

typedef struct { uint16_t vk; uint8_t kind, noalpha, rb; } ent;   /* kind 1 exact, 2 substitute; noalpha: a reads 1; rb: swap r / b */
#define E(f) {f, 1, 0, 0}
#define X(f) {f, 1, 1, 0}
#define S(f) {f, 2, 0, 0}
#define SX(f) {f, 2, 1, 0}
#define SR(f) {f, 2, 0, 1}
#define SXR(f) {f, 2, 1, 1}
#define N {0, 0, 0, 0}

static const ent T[0x9D] = {
    [0x01] = E(VK_FORMAT_R8_UNORM), E(VK_FORMAT_R8_SNORM), E(VK_FORMAT_R8_UINT), E(VK_FORMAT_R8_SINT),
    [0x05] = E(VK_FORMAT_R16_SFLOAT), E(VK_FORMAT_R16_UNORM), E(VK_FORMAT_R16_SNORM), E(VK_FORMAT_R16_UINT), E(VK_FORMAT_R16_SINT),
    [0x0A] = E(VK_FORMAT_R32_SFLOAT), E(VK_FORMAT_R32_UINT), E(VK_FORMAT_R32_SINT),
    [0x0D] = E(VK_FORMAT_R8G8_UNORM), E(VK_FORMAT_R8G8_SNORM), E(VK_FORMAT_R8G8_UINT), E(VK_FORMAT_R8G8_SINT),
    [0x11] = E(VK_FORMAT_R16G16_SFLOAT), E(VK_FORMAT_R16G16_UNORM), E(VK_FORMAT_R16G16_SNORM), E(VK_FORMAT_R16G16_UINT), E(VK_FORMAT_R16G16_SINT),
    [0x16] = E(VK_FORMAT_R32G32_SFLOAT), E(VK_FORMAT_R32G32_UINT), E(VK_FORMAT_R32G32_SINT),
    /* 0x19-0x24 three-component RGB8 / RGB16 / RGB32: as on Metal, no texture format (rare as images; vertex formats exist) */
    [0x19] = N, N, N, N, N, N, N, N, N, N, N, N,
    [0x25] = E(VK_FORMAT_R8G8B8A8_UNORM), E(VK_FORMAT_R8G8B8A8_SNORM), E(VK_FORMAT_R8G8B8A8_UINT), E(VK_FORMAT_R8G8B8A8_SINT),
    [0x29] = E(VK_FORMAT_R16G16B16A16_SFLOAT), E(VK_FORMAT_R16G16B16A16_UNORM), E(VK_FORMAT_R16G16B16A16_SNORM), E(VK_FORMAT_R16G16B16A16_UINT), E(VK_FORMAT_R16G16B16A16_SINT),
    [0x2E] = E(VK_FORMAT_R32G32B32A32_SFLOAT), E(VK_FORMAT_R32G32B32A32_UINT), E(VK_FORMAT_R32G32B32A32_SINT),
    [0x31] = E(VK_FORMAT_S8_UINT), E(VK_FORMAT_D16_UNORM),
    [0x33] = S(VK_FORMAT_X8_D24_UNORM_PACK32), E(VK_FORMAT_D32_SFLOAT),          /* DEPTH24: see vk_fmt_depth */
    [0x35] = S(VK_FORMAT_D24_UNORM_S8_UINT), E(VK_FORMAT_D32_SFLOAT_S8_UINT),
    [0x37] = X(VK_FORMAT_R8G8B8A8_SRGB), E(VK_FORMAT_R8G8B8A8_SRGB),
    [0x39] = S(VK_FORMAT_R4G4B4A4_UNORM_PACK16), SXR(VK_FORMAT_A1R5G5B5_UNORM_PACK16), SR(VK_FORMAT_A1R5G5B5_UNORM_PACK16), SR(VK_FORMAT_R5G6B5_UNORM_PACK16),
    [0x3D] = E(VK_FORMAT_A2B10G10R10_UNORM_PACK32), E(VK_FORMAT_A2B10G10R10_UINT_PACK32), E(VK_FORMAT_B10G11R11_UFLOAT_PACK32), E(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    [0x41] = X(VK_FORMAT_BC1_RGBA_UNORM_BLOCK), E(VK_FORMAT_BC1_RGBA_UNORM_BLOCK), E(VK_FORMAT_BC2_UNORM_BLOCK), E(VK_FORMAT_BC3_UNORM_BLOCK),
             X(VK_FORMAT_BC1_RGBA_SRGB_BLOCK), E(VK_FORMAT_BC1_RGBA_SRGB_BLOCK), E(VK_FORMAT_BC2_SRGB_BLOCK), E(VK_FORMAT_BC3_SRGB_BLOCK),
    [0x49] = E(VK_FORMAT_BC4_UNORM_BLOCK), E(VK_FORMAT_BC4_SNORM_BLOCK), E(VK_FORMAT_BC5_UNORM_BLOCK), E(VK_FORMAT_BC5_SNORM_BLOCK),
             E(VK_FORMAT_BC7_UNORM_BLOCK), E(VK_FORMAT_BC7_SRGB_BLOCK), E(VK_FORMAT_BC6H_SFLOAT_BLOCK), E(VK_FORMAT_BC6H_UFLOAT_BLOCK),
    /* *_UI2F / *_I2F: integer storage read as float. As on Metal: the integer format (the translator reads it as integer) */
    [0x51] = S(VK_FORMAT_R8_UINT), S(VK_FORMAT_R8_SINT), S(VK_FORMAT_R16_UINT), S(VK_FORMAT_R16_SINT), S(VK_FORMAT_R32_UINT), S(VK_FORMAT_R32_SINT),
    [0x57] = S(VK_FORMAT_R8G8_UINT), S(VK_FORMAT_R8G8_SINT), S(VK_FORMAT_R16G16_UINT), S(VK_FORMAT_R16G16_SINT), S(VK_FORMAT_R32G32_UINT), S(VK_FORMAT_R32G32_SINT),
    [0x5D] = N, N, N, N, N, N,
    [0x63] = S(VK_FORMAT_R8G8B8A8_UINT), S(VK_FORMAT_R8G8B8A8_SINT), S(VK_FORMAT_R16G16B16A16_UINT), S(VK_FORMAT_R16G16B16A16_SINT), S(VK_FORMAT_R32G32B32A32_UINT), S(VK_FORMAT_R32G32B32A32_SINT),
    [0x69] = S(VK_FORMAT_A2B10G10R10_SNORM_PACK32), S(VK_FORMAT_A2B10G10R10_SINT_PACK32), S(VK_FORMAT_A2B10G10R10_UINT_PACK32), S(VK_FORMAT_A2B10G10R10_SINT_PACK32),
    [0x6D] = X(VK_FORMAT_R8G8B8A8_UNORM), X(VK_FORMAT_R8G8B8A8_SNORM), X(VK_FORMAT_R8G8B8A8_UINT), X(VK_FORMAT_R8G8B8A8_SINT),
    [0x71] = X(VK_FORMAT_R16G16B16A16_SFLOAT), X(VK_FORMAT_R16G16B16A16_UNORM), X(VK_FORMAT_R16G16B16A16_SNORM), X(VK_FORMAT_R16G16B16A16_UINT), X(VK_FORMAT_R16G16B16A16_SINT),
    [0x76] = X(VK_FORMAT_R32G32B32A32_SFLOAT), X(VK_FORMAT_R32G32B32A32_UINT), X(VK_FORMAT_R32G32B32A32_SINT),
    [0x95] = S(VK_FORMAT_R5G6B5_UNORM_PACK16), SX(VK_FORMAT_A1R5G5B5_UNORM_PACK16), S(VK_FORMAT_A1R5G5B5_UNORM_PACK16), S(VK_FORMAT_R5G5B5A1_UNORM_PACK16),
    [0x99] = X(VK_FORMAT_B8G8R8A8_UNORM), E(VK_FORMAT_B8G8R8A8_UNORM), X(VK_FORMAT_B8G8R8A8_SRGB), E(VK_FORMAT_B8G8R8A8_SRGB),
};

static VkFormat g_d24 = VK_FORMAT_X8_D24_UNORM_PACK32, g_d24s8 = VK_FORMAT_D24_UNORM_S8_UINT;
void vk_fmt_set_depth(VkFormat d24, VkFormat d24s8) { g_d24 = d24; g_d24s8 = d24s8; }

int vk_fmt(int f, VkFormat *out, VkComponentMapping *swz) {
    ent e = {0, 0, 0, 0};
    if (f >= 0x79 && f <= 0x94) {   /* ASTC: NVN and Vulkan list the 14 block sizes in the same order, UNORM / SRGB pairs */
        int k = (f - 0x79) % 14, srgb = f >= 0x87;
        e = (ent){(uint16_t)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + srgb), 1, 0, 0};
    } else if (f > 0 && f < (int)(sizeof T / sizeof *T)) e = T[f];
    {   /* TOTK_VK_FMT_RB=3b,3c (diagnostic): flip the r / b swap of these NVN formats */
        static const char *rbs = (const char *)1; if (rbs == (const char *)1) rbs = getenv("TOTK_VK_FMT_RB");
        if (rbs && e.kind) for (const char *p = rbs; *p; ) { char *z; long k = strtol(p, &z, 16); if (z == p) break; if (k == f) e.rb ^= 1; p = *z ? z + 1 : z; }
    }
    VkFormat v = e.kind ? (VkFormat)e.vk : VK_FORMAT_UNDEFINED;
    if (f == 0x33) v = g_d24;
    if (f == 0x35) v = g_d24s8;
    if (out) *out = v;
    if (swz) {
        swz->r = e.rb ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_R; swz->g = VK_COMPONENT_SWIZZLE_G;
        swz->b = e.rb ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_B; swz->a = e.noalpha ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_A;
    }
    return e.kind;
}

int vk_fmt_is_depth(int f) { return f >= 0x32 && f <= 0x36; }
int vk_fmt_has_stencil(int f) { return f == 0x31 || f == 0x35 || f == 0x36; }
int vk_fmt_has_depth(int f) { return f >= 0x32 && f <= 0x36; }
VkImageAspectFlags vk_fmt_aspect(int f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f >= 0x32 && f <= 0x34) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
/* value class of a format, as nvn_mtl.m fmt_class: 0 float, 1 uint, 2 sint */
int vk_fmt_class(int f) {
    switch (f) {
        case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: return 1;
        case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: return 2;
        default: return 0;
    }
}

/* NVN vertex attribute formats (the same enum) -> Vulkan vertex formats; *_UI2F / *_I2F use the scaled formats, which
   convert integers to float the way NVN does. */
VkFormat vk_vertex_fmt(int f) {
    switch (f) {
        case 0x01: return VK_FORMAT_R8_UNORM; case 0x02: return VK_FORMAT_R8_SNORM; case 0x03: return VK_FORMAT_R8_UINT; case 0x04: return VK_FORMAT_R8_SINT;
        case 0x05: return VK_FORMAT_R16_SFLOAT; case 0x06: return VK_FORMAT_R16_UNORM; case 0x07: return VK_FORMAT_R16_SNORM; case 0x08: return VK_FORMAT_R16_UINT; case 0x09: return VK_FORMAT_R16_SINT;
        case 0x0A: return VK_FORMAT_R32_SFLOAT; case 0x0B: return VK_FORMAT_R32_UINT; case 0x0C: return VK_FORMAT_R32_SINT;
        case 0x0D: return VK_FORMAT_R8G8_UNORM; case 0x0E: return VK_FORMAT_R8G8_SNORM; case 0x0F: return VK_FORMAT_R8G8_UINT; case 0x10: return VK_FORMAT_R8G8_SINT;
        case 0x11: return VK_FORMAT_R16G16_SFLOAT; case 0x12: return VK_FORMAT_R16G16_UNORM; case 0x13: return VK_FORMAT_R16G16_SNORM; case 0x14: return VK_FORMAT_R16G16_UINT; case 0x15: return VK_FORMAT_R16G16_SINT;
        case 0x16: return VK_FORMAT_R32G32_SFLOAT; case 0x17: return VK_FORMAT_R32G32_UINT; case 0x18: return VK_FORMAT_R32G32_SINT;
        case 0x19: return VK_FORMAT_R8G8B8_UNORM; case 0x1A: return VK_FORMAT_R8G8B8_SNORM; case 0x1B: return VK_FORMAT_R8G8B8_UINT; case 0x1C: return VK_FORMAT_R8G8B8_SINT;
        case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT; case 0x1E: return VK_FORMAT_R16G16B16_UNORM; case 0x1F: return VK_FORMAT_R16G16B16_SNORM; case 0x20: return VK_FORMAT_R16G16B16_UINT; case 0x21: return VK_FORMAT_R16G16B16_SINT;
        case 0x22: return VK_FORMAT_R32G32B32_SFLOAT; case 0x23: return VK_FORMAT_R32G32B32_UINT; case 0x24: return VK_FORMAT_R32G32B32_SINT;
        case 0x25: return VK_FORMAT_R8G8B8A8_UNORM; case 0x26: return VK_FORMAT_R8G8B8A8_SNORM; case 0x27: return VK_FORMAT_R8G8B8A8_UINT; case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
        case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT; case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM; case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM; case 0x2C: return VK_FORMAT_R16G16B16A16_UINT; case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
        case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT; case 0x2F: return VK_FORMAT_R32G32B32A32_UINT; case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
        case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
        case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
        case 0x51: return VK_FORMAT_R8_USCALED; case 0x52: return VK_FORMAT_R8_SSCALED; case 0x53: return VK_FORMAT_R16_USCALED; case 0x54: return VK_FORMAT_R16_SSCALED;
        case 0x55: return VK_FORMAT_R32_UINT; case 0x56: return VK_FORMAT_R32_SINT;   /* no 32-bit scaled formats: read as integer */
        case 0x57: return VK_FORMAT_R8G8_USCALED; case 0x58: return VK_FORMAT_R8G8_SSCALED; case 0x59: return VK_FORMAT_R16G16_USCALED; case 0x5A: return VK_FORMAT_R16G16_SSCALED;
        case 0x5B: return VK_FORMAT_R32G32_UINT; case 0x5C: return VK_FORMAT_R32G32_SINT;
        case 0x5D: return VK_FORMAT_R8G8B8_USCALED; case 0x5E: return VK_FORMAT_R8G8B8_SSCALED; case 0x5F: return VK_FORMAT_R16G16B16_USCALED; case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
        case 0x61: return VK_FORMAT_R32G32B32_UINT; case 0x62: return VK_FORMAT_R32G32B32_SINT;
        case 0x63: return VK_FORMAT_R8G8B8A8_USCALED; case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED; case 0x65: return VK_FORMAT_R16G16B16A16_USCALED; case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
        case 0x67: return VK_FORMAT_R32G32B32A32_UINT; case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkBlendFactor vk_blend_factor(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
VkBlendOp vk_blend_op(int e) {
    switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; }
    return VK_BLEND_OP_ADD;
}
/* NVN depth / stencil / sampler compare functions are 1..8 = NEVER..ALWAYS, Vulkan's are 0..7 in the same order */
VkCompareOp vk_compare(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
VkStencilOp vk_stencil_op(int o) {
    switch (o) {
        case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}
VkSamplerAddressMode vk_wrap(int w) {
    switch (w) {
        case 0: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
VkComponentSwizzle vk_swizzle_sel(int s) {
    switch (s) { case 0: return VK_COMPONENT_SWIZZLE_ZERO; case 1: return VK_COMPONENT_SWIZZLE_ONE; case 2: return VK_COMPONENT_SWIZZLE_R;
                 case 3: return VK_COMPONENT_SWIZZLE_G; case 4: return VK_COMPONENT_SWIZZLE_B; case 5: return VK_COMPONENT_SWIZZLE_A; }
    return VK_COMPONENT_SWIZZLE_IDENTITY;
}
