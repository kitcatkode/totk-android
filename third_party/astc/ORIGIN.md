# ASTC decoder

From K0lb3/texture2ddecoder (https://github.com/K0lb3/texture2ddecoder, commit 3ebc3b758bd6b1a3108b50148f7998ee34f058c6),
src/Texture2DDecoder/: astc.cpp, astc.h, color.h, endianness.h, fp16.h, fp16/fp16.h, fp16/bitcasts.h. License: MIT (LICENSE; fp16 from
Maratyszcza/FP16, MIT, notice in fp16.h).

Unmodified. Ours: `astc_c.cpp`, compiled alone (it includes astc.cpp) to give C callers
`totk_decode_astc(blocks, w, h, block_w, block_h, out)`: linear ASTC blocks (row-major) to 32-bit pixels, bytes B G R A.
Used by runtime/bottom_assets.c for the game's map tiles and item icons.
