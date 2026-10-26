# Android port (AYN Thor): status and how to test

Goal: run Tears of the Kingdom natively on Android arm64 the way the Mac build does. The game's own ARM64 code runs
on the CPU, a host answers the Switch OS, and NVN graphics are rebuilt on Vulkan instead of Metal. The first target is
the AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13, 8–16 GB).

Current progress, open bugs, what has been ruled out and the diagnostic switches: [DEBUGGING.md](DEBUGGING.md).

You must own the game. Everything is built on your machine from your own cartridge dump and keys. Nothing from the
game goes in this repository.

## Where it stands

| Part | State |
| --- | --- |
| Code rewriter (`third_party/d2r`) | **Done.** `DM_HOST_TP=tpidr_el0` rewrites for Linux / Android. The Mac output is byte-identical to before. |
| Loader, threads, Horizon services, filesystem, audio mixing (`runtime/`) | **Ported.** Builds for arm64 Linux and Android. |
| Audio output | AAudio (low-latency float stereo, reopened on device loss) |
| NVN API layer (`gfx/nvn/hle_nvn.c` and helpers) | **Ported.** Builds unchanged through `android/compat`. |
| GPU backend | **Vulkan, drawing** (`android/gfx/`): the game draws on the Thor with Turnip, about 30 fps in the world. Details: [VULKAN.md](VULKAN.md). `--gpu null` keeps the old null backend. |
| Frontend | **Headless** (`runtime/frontend_headless.c`): runs from `adb shell`, with no window and no input. |
| Android app (APK, window, touch, controller) | **First version** (`--apk`, [APP.md](APP.md)): NativeActivity, picture in the window, gamepad. No touch controls yet. |

What has been verified here, without game data:

- **Fixture modules.** `android/fixture/` builds synthetic main / subsdk0 / sdk modules that use every instruction
  kind the rewriter replaces: x18 reads, writes, branches and `adr`, guest TPIDR_EL0 and TPIDRRO_EL0, `svc`,
  `cntpct` / `cntvct`, an exclusive load/store window through x18, and a `blr x18` call window.
- **Rewriter checks.** In both modes, the structural checker and the emulator equivalence check pass, and the
  mutation negative controls are all caught.
- **The real loader on arm64 Linux (qemu).** It maps, relocates, seals and checks permissions. Its cross-module probe
  passes (4 threads × 1000 calls). The fixture harness passes 168,000 per-thread slot checks on 6 threads with 0
  failures. An image rewritten for the Mac is refused.
- **Page aliasing for virtual pools.** This is `mremap` on shared anonymous memory, tested on a real Linux kernel,
  including its negative control.
- **Build checks.** The whole headless `totk-native` links for arm64 Linux. Every source file compiles for Android
  API 33 against bionic's headers. The NDK link itself has to happen on your PC.

**On the AYN Thor with the real game (1.4.3):** it boots headless and stays in the game loop. One run went 18 minutes and
43,500 frames, at about 39 fps on the null backend with roughly 1,730 draws per frame. Audio plays through AAudio, and
resident memory is about 1.9 GB.

## Build it (Linux PC)

Needs:

- Python 3.12+ (3.14 if the update is an NSZ)
- git, rsync, make, cmake
- clang or gcc
- LLVM tools (`llvm-objdump`, `llvm-objcopy`)
- the Android NDK r26+ (Android Studio's SDK manager or the NDK zip)
- about 45 GB free

On Arch / CachyOS: `sudo pacman -S llvm cmake rsync`, plus the `android-ndk` AUR package or the zip.

1. **Dump the cartridge on your Switch.** Use nxdumptool (XCI with the update partition, which carries the system
   fonts), Lockpick_RCM for `prod.keys`, and the update NSP if you want 1.4.3.
2. **Build:**

   ```
   python3 android/build_android.py ~/Games/totk.xci -k ~/keys/prod.keys --ndk ~/Android/ndk/27.2.12479018 \
       --update ~/Games/totk-1.4.3.nsp
   ```

   It runs the same extraction and checks as the Mac build, including its verified hashes. It then rewrites the code
   again for Android and checks that every site matches the verified rewrite. After that it extracts the data, builds
   libopus / libvpx and the runtime with the NDK, and writes `TOTK_android/` next to the game file.
3. **Run it on the Thor.** Enable Developer options and USB debugging, then:

   ```
   sh TOTK_android/push.sh                       # about 17 GB to /data/local/tmp/totk
   adb shell sh /data/local/tmp/totk/run.sh      # log in the terminal and /data/local/tmp/totk/totk.log
   ```

   What to look for:
   - The three `[totk] mapped …` lines.
   - The RomFS check, which takes a while the first time.
   - `ENTER original nn::init::Start`, then the module constructors.
   - `[nvn-null] frame N: … fps` lines once the game presents frames.

   Whatever line it stops at, send `totk.log`. That line is the next thing to fix.

## Test without game data (developers)

```
DM_HOST_TP=tpidr_el0 python3 android/fixture/make_fixture.py /tmp/fx      # rewrite + structck + equiv
# loader harness (arm64 Linux or qemu-aarch64):
aarch64-linux-gnu-gcc -O2 -D_GNU_SOURCE -I/tmp/fx -Iruntime -Ithird_party/d2r_gfx/gfx/nvn \
  -Ithird_party/d2r_gfx/gfx/nvn/include android/fixture/harness.c runtime/host_tp.c \
  runtime/host_os.c runtime/hle_stack.c runtime/gate.S runtime/hle_stack.S \
  -lpthread -ldl -o /tmp/harness && qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/harness /tmp/fx
```

## How the Linux / Android differences are handled

- **Thread slots.** Darwin's patched code finds its per-thread values (virtual x18, guest TLS, Horizon TLR, the host
  gate) through `TPIDRRO_EL0`, which points at the TSD array. On Linux that register reads 0. The Android rewrite emits
  `mrs xN, tpidr_el0; ldr xN, [xN, #16]` instead. TP + 16 is bionic's `TLS_SLOT_APP`, free for apps from API 29. It
  holds a per-thread slot block (`runtime/host_tp.c`). Under glibc it is the executable's first TLS word, checked at
  startup. See `third_party/d2r/hostreg.py`.
- **No red zone.** The rewrite parks scratch registers below sp. Every signal handler is `SA_ONSTACK` on a per-thread
  alternate stack (already true for the Mac), so signals never land there.
- **Guest memory.** Guest memory is shared anonymous memory reserved with `MAP_FIXED_NOREPLACE` below 2^39.
  Android's arm64 user space is 39-bit, the same size as Horizon's. Being shared lets NVN virtual pools alias pages
  with `mremap(old_size 0)`; Darwin uses `vm_remap` for this. Freed pages are returned with `MADV_REMOVE`.
- **Other Darwin services** (Mach VM reads, `mach_absolute_time`, QoS, CommonCrypto, Core Audio) have Linux
  counterparts in `runtime/host_os.*`, `runtime/host_thread.h` and the AAudio branch of `audio_mixer.c`. The graphics
  files get theirs from `android/compat/include`.

## Next steps

1. ~~Boot to the game loop headless on the Thor~~ (done).
2. **Vulkan backend** (stages in [VULKAN.md](VULKAN.md)). Replace `nvn_null.c` with an implementation of the `nvn_mtl.h` interface on Vulkan 1.3 (Adreno
   740, with Turnip as a fallback driver):
   - textures and pools via `VK_EXT_external_memory_host` so pools stay zero-copy
   - the Ryujinx translator's SPIR-V used directly (no SPIRV-Cross)
   - render passes as dynamic rendering
   - real occlusion queries; the game culls with them
   - BC textures: Turnip has BC, but Qualcomm's driver may not, so the iOS BC→ASTC path comes along

   `nvn_mtl.m` is the reference. A lot of its 14k lines are Mac-specific tuning and diagnostics.
3. **Shaders.** The translator ships as a macOS NativeAOT program. For Android, publish it for `linux-bionic-arm64`
   (NativeAOT) to translate on the device, or pre-translate the shared shader list on the PC into the build.
4. **The app.** A GameActivity / NativeActivity APK: an `ANativeWindow` for Vulkan, touch controls (the iOS layout),
   gamepad input, the dual-screen bottom panel later, and the game data in the app's external files directory.

Known risks to watch:

- **VMA limits.** Linux limits mappings per process (`vm.max_map_count`, about 65k). Per-page write protection reached
  92k mappings in the overworld and crashed; on Android pages are now protected in 64 KiB units (see DEBUGGING.md).
- **The 8 GB Thor Base:** the game uses about 4 GB.
