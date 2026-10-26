// TOTK: decode_astc for C callers (runtime/bottom_assets.c); astc.cpp is unmodified.
#include "astc.cpp"
extern "C" int totk_decode_astc(const uint8_t *data, long w, long h, int bw, int bh, uint32_t *image) { return decode_astc(data, w, h, bw, bh, image); }
