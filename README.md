# totk-android

The Legend of Zelda: Tears of the Kingdom (1.4.3), running natively on Android arm64. The game's own ARM64 code runs on
the CPU, a host runtime answers the Switch OS, and NVN graphics are rebuilt on Vulkan. The target device is the
AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13, two screens), with the Mesa Turnip driver.

**You must own the game.** Everything is built on your PC from files you dumped from your own console and your own
copy of the game. Nothing from the game is in this repository, and nothing the build makes from your files may be
shared.

## What it does

- Rewrites the game's code modules for Android (`third_party/d2r`), checked word by word and against the verified
  hashes in `assets/expected-v*.json`, so a damaged dump stops the build at the first wrong file.
- Runs them in the host runtime (`runtime/`): loader, threads, Horizon services, filesystem, audio (AAudio), input.
- Draws through a Vulkan backend for NVN (`android/gfx/`, under the NVN layer in `third_party/d2r_gfx`), with the
  game's shaders translated ahead of time (`tools/totk_xlate`, built on Ryujinx's shader translator).
- Ships as an APK (`android/apk/`) with a NativeActivity, controller and gyro input, frame generation
  (lsfg-vk with the player's own `Lossless.dll`), and a second-screen app with settings, map and inventory.

About 46 fps in the open world at 720p on the Thor; details and the performance work are in
[android/PERF_PLAN.md](android/PERF_PLAN.md) and [android/DEBUGGING.md](android/DEBUGGING.md).

## What you need

- From your own Switch and your own copy of the game: the base game 1.0.0 (XCI or NSP), the 1.4.3 update, your
  console's keys (`prod.keys`) and its firmware (for the system's shared fonts).
- A Linux PC with Python 3.12+ (3.14 for NSZ updates), git, rsync, make, cmake, clang, LLVM (`llvm-objdump`,
  `llvm-objcopy`; `LLVM_BIN` if not in /usr/bin), the Android NDK r26 or newer, the .NET 10 SDK (once, for the shader
  translator) and about 45 GB free.
- The device: Android 13+ arm64 with Vulkan. A Turnip build as `driver.so` in the game folder is recommended on Adreno.
- Optional: `Lossless.dll` from your copy of Lossless Scaling, for frame generation.

This project does not provide, link to or help anyone obtain games, updates, keys or firmware, and issues or
messages asking for them will be closed. Do not share the build's output: the work and output folders contain the
game's code and data.

## Build

```
zsh tools/totk_xlate/publish_native.sh        # once: the shader translator -> prebuilt/linux-x64/totk_xlate
python3 android/build_android.py <game.nsp|game.xci> --update <update.nsp> -k <prod.keys> \
    --firmware <firmware dir> --ndk <android-ndk> --apk --shaders
```

The work folder (`TOTK_build`) and the output (`TOTK_android`) go next to the game file unless `--work` / `-o` say
otherwise. A second run reuses what is done and correct; after changing the runtime, `--runtime-only --apk` rebuilds in
minutes. Installing and the game folder on the device: [android/APP.md](android/APP.md).

## On the AYN Thor

The port is built and tuned on the Thor; other Android devices may work, but only this one is tested.

**Two screens.** The game runs on the top screen. When the app starts, it opens its own screen on the bottom display
(the manifest's activity alias `.Bottom`; `TOTK_BOTTOM=0` turns it off). Touch goes to the bottom screen, and the
built-in controller always drives the game, whichever screen has focus. The bottom screen has three tabs:

- **Settings:** frame generation (off / 2x / 3x, mode, HUD, flow scale), frame cap (30 / 40 / 60), render resolution
  (720p / 612p / 540p), shadow resolution (720 / 1024) and gyro aiming, plus the live frame rate. A tap saves the value
  to `totk.env`. Frame generation changes apply at once; the rest say "applies after a restart".
- **Map:** the Sky, Surface and Depths maps from your game files, with Link's position and heading live. Pinch, drag,
  double-tap, "Whole map" and "Follow Link". It shows the fully revealed map for now, not your save's reveal state.
- **Inventory:** the pouches from your newest save, with counts, durability, fused materials, names and icons (English
  only). Read-only: it never changes the save.
  
612p 40fps locked without framegen has been the "smoothest" experience for me.

Details: [android/APP.md](android/APP.md) and [android/BOTTOM_SCREEN.md](android/BOTTOM_SCREEN.md).

**Refresh rate.** The Thor's screens run at 60 or 120 Hz only, with no variable refresh. The frame caps are 30, 40 and 60
because they divide 120, so every frame stays up for the same number of refreshes. 45 would alternate between 2 and 3
refreshes and judder. The app asks Android for the matching rate (cap x frame-generation multiplier).

**Performance.**
- Use AYN's high-performance mode. Standard mode caps the CPU at ~1.84 GHz and the GPU at 401 MHz, against
  3.19 GHz / 680 MHz in high performance, and every figure here is measured in high performance.
- The thread that replays the game's graphics commands is pinned to the Cortex-X3 prime core (`TOTK_REPLAY_CPU`),
  where it runs about 27 % faster.
- A Turnip build as `driver.so` in the game folder is what this is developed against.
- With frame generation on, the app turns off early sync (one frame more in flight). That keeps input latency down:
  about 77 ms to the screen at 2x, against about 97 ms with it on.

**Controls.** The Thor's built-in controller, and gyro aiming from its motion sensors (Settings, or `TOTK_GYRO=0` to turn it off).

**If the game won't close from recents:** the Thor's launcher can lock a recents card, and a locked card bounces back
when you swipe it. Unlock it on the card (a downward drag toggles the lock), then swipe it away.

## Layout

| | |
| --- | --- |
| `android/` | the Android side: build driver, Vulkan backend (`gfx/`), APK builder, device tools (`devrun.py`), docs |
| `runtime/` | the host runtime the game runs in, and the second screen (`bottom_*.c`) |
| `buildlib.py`, `scripts/` | game identification, extraction, checks, RomFS and shader seed preparation |
| `third_party/`, `vendor/ryujinx` | code from other projects ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) |
| `tools/totk_xlate` | the shader translator front end and its patches to Ryujinx |
| `assets/` | verified hashes of each supported game version and the shader list (identifiers only) |

This started as the Android port inside MacNativeGames; the shared files keep their other platform branches, but only
what the Android build uses is here.

## Credits

This project is heavily based on **MacNativeGames**, the native Mac and iPhone/iPad build of Tears of the Kingdom. The
code rewriter setup, the host runtime, the NVN layer, the build pipeline and its verification all come from it; this
repository adds the Android side (Vulkan backend, APK, second screen, frame generation, Thor tuning) and keeps only
what that needs. Thanks to the MacNativeGames contributors.

It also builds on:

- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile): the NSO rewriter, the original NVN-on-Metal layer and the
  shader translator front end
- [Ryujinx](https://github.com/Ryujinx/Ryujinx): the Maxwell shader translator and the audio effects
- lsfg-vk v1 and [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/): frame generation
- [Mesa](https://mesa3d.org/) Turnip, the Vulkan driver this is developed against on Adreno
- deko3d, bcdec, texture2ddecoder, Zstandard, stb, hactool, SPIRV-Cross, libopus and libvpx
  ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md))

The Legend of Zelda: Tears of the Kingdom is © Nintendo. This project is not affiliated with or endorsed by Nintendo.

## AI disclaimer

Much of the Android port's code, its performance work and its documentation was written with AI coding assistants
(Anthropic's Claude, GLM 5.2 Flash, Deepseek 4.1 Flash), directed and tested on the device by the maintainer. Treat it like any other community project:
read before you trust, and report what breaks.

## License

MIT ([LICENSE](LICENSE)); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
