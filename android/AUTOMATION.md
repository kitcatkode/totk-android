# Automating TOTK Android testing (for an agent on the user's PC)

This is the working manual for a coding agent (Claude Code) that builds, installs, runs and measures the TOTK Android
port on the user's AYN Thor over adb, so experiments and regressions do not need a human to type every command.
`DEBUGGING.md` (same folder) is the project's memory: what was found, fixed and tried. Read its Status section and the
section you are working on before changing anything, and add to it when you learn something.

## Setup facts

- **All work happens inside the `dev` distrobox** (prompt `📦[may@dev]`): the NDK, Python, adb with the Thor's
  authorization, the build tree and `~/totkdump` are set up there, not on the host. Start the agent inside it
  (`distrobox enter dev`, then run it from the repository). If a session finds itself on the host (no
  `📦` prompt, `echo $CONTAINER_ID` empty), run each command through `distrobox enter dev -- zsh -lc '<command>'`
  instead of installing tools on the host.
- Repo: this repository (`totk-android`). Other sessions push to
  the same branch: `git pull --rebase` before starting and before pushing.
- Shell: zsh inside the `dev` distrobox. Commands meant for the user to paste must not carry inline `#` comments.
- Device: AYN Thor (Snapdragon 8 Gen 2, Adreno 740), Android 13, rooted (Magisk). Turnip driver in the game folder as
  `driver.so` (Balemuni Apex; do not swap drivers unless asked).
- App package `dev.kitcatkode.totk`, activity `android.app.NativeActivity`.
- Game folder (root-owned; write with `su -c`): `/sdcard/Android/data/dev.kitcatkode.totk/files`, holding `totk.env`
  (NAME=value settings, read at app start), `input.txt` (the command channel), `totk.log` (this run's log),
  `pipeline_cache.bin`, `passdump/`, `shaders/`, `driver.so`.

## Build, install, test

```
git pull --rebase && python3 android/build_android.py ~/totkdump/totk.nsp -k ~/totkdump/prod.keys --firmware ~/totkdump/Firmware.22.5.0/ --ndk ~/Android/ndk/android-ndk-r27c --runtime-only --apk && sh ~/totkdump/TOTK_android/install_app.sh
```

- Build output and logs: `~/totkdump/TOTK_build/logs/` (a make failure names the log).
- `sh ~/totkdump/TOTK_android/install_app.sh log` prints the last run's log; `... saves` backs up the saves to a tar.
- Missing shader variants (the frame line's `shader missing N` > 0) are filled with
  `python3 android/fill_shaders.py --work ~/totkdump/TOTK_build --out ~/totkdump/TOTK_android`, then restart.
- Desktop tests (lavapipe, if installed in the distrobox), required before pushing renderer changes:
  `make -C android vktest vktest-app TARGET=linux CC=clang OUT=/tmp/vkt` then run `/tmp/vkt/vktest`,
  `/tmp/vkt/vkdrawtest`, `/tmp/vkt/vkdrawtest-app` with
  `TOTK_SHADER_SEED_DIR=/tmp/vkt/vkdraw-seed VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`; also run the two
  draw tests with `TOTK_VK_SPLIT_MS=-1` (a submission split before every pass). All must report `0 failed`.
  The `dev` distrobox (Fedora 44) has the Vulkan loader and lavapipe but, as of 2026-10-23, no Vulkan headers and no
  glslangValidator, and the ICD file is `lvp_icd.x86_64.json`. Without installing anything: copy `vulkan/` and
  `vk_video/` from the NDK's `toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include` into a scratch folder and build
  with `CC="clang -I<that folder>"`; put a `glslangValidator` shim first in PATH that turns
  `-V --target-env vulkan1.2 IN -o OUT` into the NDK's `shader-tools/linux-x86_64/glslc --target-env=vulkan1.2 IN -o OUT`
  (the seed script's only call).

## The harness: `android/devrun.py`

```
python3 android/devrun.py run --name NAME --secs 180 [--env KEY=VALUE ...] [--unset KEY ...] \
        [--script FILE] [--shots 20] [--perf 15@120]
python3 android/devrun.py summary runs/A runs/B      side-by-side numbers of finished runs
python3 android/devrun.py env                         the device's totk.env
python3 android/devrun.py awake on|off                screen on while plugged in for a session / restore
```

`run` saves the device's totk.env, applies the overrides for this run (plus `TOTK_AUTOLOAD=1`, which loads the newest
save by itself, and the command channel), starts the app, sends the script's commands at their times, takes screen
captures (`--shots N`: every N s) and a simpleperf profile of the replay thread (`--perf SECS@AT`), then writes
`runs/NAME/` (totk.log, screen_*.png, perf_*.txt, passdump/, summary.json) and restores totk.env even on failure.
The app is force-stopped at the end. Reaching the world from launch takes roughly a minute; the summary skips the
first 600 frames.

Look at the screen captures yourself (they are PNGs you can open): visual bugs are not in the numbers.

**The Thor's performance mode changes every number.** AYN's standard mode caps the CPU at ~1.84 GHz and the GPU at
401 MHz; high performance allows 3.19 GHz / 680 MHz. Ask the user which mode the Thor is in and keep it fixed for a
session; compare runs only within one mode.

### System tuning: `--tune` and `sh` (thor-tools)

`~/Projects/thor-tools/tune` holds `thor-tune`, a root script on the device that changes scheduler / CPU / GPU knobs
reversibly (it records each knob's first value and `restore` puts them all back). Install it once per device boot
with `~/Projects/thor-tools/tune/install.sh`.
- `--tune PROFILE` (repeatable) applies `tune/profiles/PROFILE.prof` before the app starts and restores at the end,
  like totk.env. Profiles: `gpu-floor-550`, `gpu-floor-615` (GPU minimum clock), `totk-bigcores` (all TOTK threads off
  the efficiency cores).
- Script line `<seconds> sh <command>` runs `<command>` as root on the device (output and errors in runs/NAME/sh.log),
  for per-thread settings once the threads exist, e.g.
  `95 sh /data/local/tmp/thor-tune/thor-tune threads dev.kitcatkode.totk` (every thread: CPU, affinity, WALT boost) or
  `... thor-tune task dev.kitcatkode.totk totk-translate boost 2`. Ready scripts: `~/Projects/thor-tools/bench/scripts`.
- `~/Projects/thor-tools/bench/session.sh` runs interleaved A/B sessions unattended (DEBUGGING.md, "The replay on the
  prime core", has the first one).

### Script commands (lines `<seconds> <command>`)

`key <code> <ms>` press (ms <= 5000), `down <code>` / `up <code>` hold and release, `passdump <n>` dump every pass of
n frames into passdump/, `flag <name> <0|1>` switch a runtime behaviour live, `threads` dump every guest thread (hangs),
`shot <path>` the next game frame as PNG (path relative to the game folder).

Key codes (the frontend's Mac key codes):

| Switch | code | | Switch | code |
| --- | --- | --- | --- | --- |
| left stick up / down / left / right | 13 / 1 / 0 / 2 | | A / B / X / Y | 38 / 40 / 34 / 32 |
| right stick up / down / left / right | 126 / 125 / 123 / 124 | | L / R / ZL / ZR | 12 / 14 / 58 / 56 |
| D-pad up / down / left / right | 17 / 5 / 3 / 4 | | Plus / Minus | 36 / 48 |
| left / right stick press | 6 / 8 | | | |

Example (`walk_pan.txt`): run forward 20 s, then turn the camera slowly, then dump 2 frames.

```
90 down 13
110 up 13
112 down 124
120 up 124
125 passdump 2
```

The world a run starts in is the newest save: ask the user to make a save at a spot that shows the problem under test (a
busy area for performance, the place a bug appeared) and keep that save for repeatable runs. Real button presses on the
Thor take over from auto-load and scripts.

The Thor's lock screen is secure (adb cannot dismiss it): asleep or locked, the app has no window, captures come out
black and the GPU soon reports device lost. `run` refuses to start then. Run `devrun.py awake on` at the start of a
session (the user unlocks once) and `devrun.py awake off` at the end, then turn the screens off
(`adb shell input keyevent KEYCODE_SLEEP`).

## What the log tells you (every 300 frames)

- `frame N: X fps over the last 300, ... skipped: ... shader missing N, pipeline N ...`
- `frame N, per frame: recording X ms, waiting for the GPU, GPU Y ms, S submissions (P splits); descriptor sets ...`
- `frame N: frame time p50 / p90 / p99 / max, K of 300 over 34 ms ...; guest GPU waits ..., guest core queueing ...`
- `frame N: CPU per frame X ms over T threads; top: <thread>/<tid> ms ...` (totk-translate = the replay thread)
- `window posts, last 300: interval p50 ...` (what the screen gets), `hitches, last 300 frames: ...`
- `hitch at frame N: +X ms ...; pipelines P (Q ms ...)`: first-time pipeline compiles cause most hitches
- `【红】` marks errors; `[motion]`, `window frame rate`, `pipeline cache` lines at start.

## Settings worth A/B-testing (totk.env)

| setting | meaning |
| --- | --- |
| `TOTK_UNCAPPED_HZ=30/40/60` | frame cap (app default 30; 40 suits the 120 Hz panel) |
| `TOTK_VK_SPLIT_MS=2 / 4 / 0 / -1` | split a frame's submission every N ms of recording (default 2; 0 off, -1 every pass) |
| `TOTK_VK_SPLIT_DEFER=1` | keep the splits but submit them together at the end (timing diagnostic) |
| `TOTK_VK_OFF=a,b` | turn backend features off: `rcache, ubmemo, pfront, cmpfix, writeback, texcache, ds, d16tie, ...` (DEBUGGING.md's Diagnostics table) |
| `TOTK_VK_FENCES=0` | submission completion via the timeline semaphore (the old, slower way) |
| `TOTK_GYRO=0` | motion off (`TOTK_GYRO_AXES` overrides the axes, `TOTK_GYRO_LOG=1` logs raw axes) |
| `TOTK_RENDER_RES=960x540` | the game's scene render size (default its own 1280x720) |
| `TOTK_REPLAY_CPU=7 / -1` | CPU the replay thread is pinned to, re-checked every frame (default the last CPU, the X3; -1 off) |
| `TOTK_LSFG=2/3/4` | frame generation multiplier (needs Lossless.dll in the game folder; `TOTK_LSFG_PERF=1`, `TOTK_LSFG_FLOW=0.5`; `TOTK_LSFG_HUD=0` mixes the HUD in) |

## How to work

1. One question per experiment. Change one setting (or one code change) against a baseline run of the same save, the
   same script and the same length; repeat each side at least twice (the scenes vary).
2. Compare with `devrun.py summary` and the screen captures; keep raw runs in `runs/` (not committed).
3. Write what you learned into DEBUGGING.md (the numbers, what it rules in or out), commit, push.
4. Code changes: smallest change that answers the question; desktop tests pass; the device run confirms; then commit
   with a message that says why. Never weaken a test to make it pass.
5. Never delete the saves or the game folder's data; back up the saves (`install_app.sh saves`) before anything risky.
   If the app hangs, send `threads` before stopping it and keep that log.

## Open work (2026-10-23)

1. **Terrain / shadow tiles with the split.** Fixed 2026-10-23 (write-back records aging out; DEBUGGING.md, Frame
   pacing). Left: leaf litter gone in single captures, back 10 s later (rare, not split-specific). Next: long runs
   moving over open terrain (needs a save the user does not mind being overwritten: autosaves); passdump at a bad capture.
2. **Replay-thread cost: the main lever for frame rate** (DEBUGGING.md "What sets the frame time": the game waits for
   each frame's GPU completion, and the replay is in that latency). Earlier notes: 2026-10-23 morning: the replay was blocked ~11 ms a frame behind the timeline semaphore
   (fixed: fences); recording is now ~14 ms a frame at the field save with ~14 ms of CPU, and the frame is bounded by
   the GPU (~20 ms) and the game threads. Profile a fight with `--perf 15@120` for what is left (bind_stage,
   vk_ub_rename, memcmp). For waits, profile off-CPU with DWARF stacks (DEBUGGING.md "Sixth round").
2b. **GPU time** (~21.5 ms a frame at Lookout Landing, the limit now with the replay at ~18 ms). Ruled out
   2026-10-23: pass splits at barriers (no GPU change without them) and GMEM rendering (twice as slow); the render
   resolution (`TOTK_RENDER_RES`) helps only a little (540p: +2.6 fps). Left: per-shader cost from a GPU profile.
3. **First-use pipeline hitches**: candidates are VK_EXT_graphics_pipeline_library or prebuilding pipelines from the
   on-disk cache's keys at startup.
4. **40 fps**: heavy areas fall to ~27 fps; measure what a 40 cap does to p90 / p99 vs 30.
5. **Frame generation**: works (2026-10-23, `TOTK_LSFG`, lsfg-vk v1 in liblsfg.so; DEBUGGING.md "Frame generation").
   Next: the user's verdict on feel and latency; 3x at a 40 cap (120 Hz); performance mode and flow scale vs GPU cost.
