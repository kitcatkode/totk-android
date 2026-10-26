# Third-party code

Code in this repository that comes from other projects keeps its license; the license file sits next to the code.

| Code | From | License | File |
|---|---|---|---|
| `third_party/d2r` (NSO rewriter, verification tools) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), adapted | MIT | `third_party/d2r/LICENSE` |
| `third_party/d2r_gfx` (the NVN layer the Vulkan backend in `android/gfx` sits under, texture tools, host headers) | d2r-mobile, extended for this game | MIT | `third_party/d2r_gfx/LICENSE` |
| texture layout sizes in `third_party/d2r_gfx/gfx/nvn/nvn_tex.c` | deko3d | zlib | `third_party/d2r_gfx/LICENSE.deko3d` |
| `third_party/d2r_gfx/third_party/bcdec` | [bcdec](https://github.com/iOrange/bcdec) 0.985 | MIT or public domain | `third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| `third_party/lsfg-vk` (frame generation core) | lsfg-vk v1, via [this mirror](https://github.com/LoveDoLove/lsfg-vk) at `62f8149`, adapted (`ORIGIN.md`) | MIT | `third_party/lsfg-vk/LICENSE.md` |
| `third_party/astc` (ASTC decoder, bottom screen map) | [texture2ddecoder](https://github.com/K0lb3/texture2ddecoder) at `3ebc3b7` (`ORIGIN.md`) | MIT | `third_party/astc/LICENSE` |
| `third_party/zstd` (decompressor, bottom screen map) | [Zstandard](https://github.com/facebook/zstd) 1.5.6 (`ORIGIN.md`) | BSD-3-Clause or GPL-2.0 | `third_party/zstd/LICENSE` |
| `third_party/stb/stb_truetype.h` (bottom screen text) | [stb](https://github.com/nothings/stb) | MIT or public domain | end of the file |
| `tools/totk_xlate` (shader translator front end) | d2r-mobile, adapted | MIT | `tools/totk_xlate/LICENSE` |
| `vendor/ryujinx` (Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator) | Ryujinx, as pinned by d2r-mobile; patched by `tools/totk_xlate/patches` | MIT | `vendor/ryujinx/LICENSE.txt` |
| audio effects in `runtime/audio_reverb.c`, `runtime/audio_effects.c` (delay, reverb, compressor) and the mixer | ported from Ryujinx.Audio | MIT | `third_party/licenses/Ryujinx-Audio.txt` |

Built locally (not in this repository): `prebuilt/linux-x64/totk_xlate`, the shader translator as one native program,
made by `tools/totk_xlate/publish_native.sh` from `vendor/ryujinx` + the patches + `tools/totk_xlate`; it contains .NET
runtime libraries compiled in by NativeAOT (MIT).

Fetched at pinned versions at build time (not in this repository):

| Code | Use | License |
|---|---|---|
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) at `aa217ae` | used by the shader translator | Apache-2.0 |
| [hactool](https://github.com/SciresM/hactool) at `1d64a83` (with explicit private title-key-file hook), including its bundled mbedTLS | base + update BKTR merge at build time only | ISC (hactool), Apache-2.0 (mbedTLS); see fetched source LICENSE files |
| [libopus](https://github.com/xiph/opus) v1.5.2, built with the NDK and linked statically | Opus audio | BSD-3-Clause |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) v1.15.0, built with the NDK and linked statically | VP9 movies | BSD-3-Clause |

Supplied by the player, never in this repository: the game files and keys, the firmware (shared fonts), and
`Lossless.dll` from the player's own copy of Lossless Scaling (the frame generation shaders).
