# Android port: progress and open problems

A working log for whoever picks this up next, human or agent. Keep it current: when something is ruled out, fixed or
found, update the sections below in the same commit. Background docs: `README.md` (the port), `VULKAN.md` (the
Vulkan backend), `APP.md` (the APK). The Mac backend this one follows is `third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`;
its comments record the Mac's fixes (search for a date like `2026-10-15`), and most picture bugs here have a Mac
counterpart there.

## Setup

- Device: AYN Thor (Adreno 740, Android 13). Drivers tried: Turnip "Apex" and Purple T23 (`driver.so` in the game
  folder); no difference for any bug below.
- Game: 1.4.3 (base 1.0.0 dump + `--update`). 1.0.0 behaves the same.
- Build (Linux, inside the user's dev distrobox), from the repo root:

  ```
  python3 android/build_android.py ~/totkdump/totk.nsp --update ~/totkdump/update.nsp -k ~/totkdump/prod.keys --firmware ~/totkdump/Firmware.22.5.0/ --ndk ~/Android/ndk/android-ndk-r27c --apk --shaders
  ```

  After a full build, code changes only need `--runtime-only --apk` (minutes), then `sh ~/totkdump/TOTK_android/install_app.sh`.
- Game folder on the device: `/sdcard/Android/data/dev.kitcatkode.totk/files` (`totk.env`, `totk.log`, `shots/`).
  `install_app.sh log` prints the log. Options go in `totk.env` (`NAME=value` lines), read at start; force-stop the app
  (`adb shell am force-stop dev.kitcatkode.totk`) after editing.
- Desktop tests (lavapipe): `make -C android vktest vktest-app TARGET=linux CC=clang OUT=<dir>`, then run
  `vktest`, `vkdrawtest`, `vkdrawtest-app` with `TOTK_SHADER_SEED_DIR=<dir>/vkdraw-seed VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`.
  All must pass before pushing.

## Status

**2026-10-23 midday: Lookout Landing 32 fps with the 40 cap (user: "feels pretty good", heavier areas "a bit laggier
... not horrible"); the field save 33 fps (was 28.5)**: the replay thread spent ~11 ms a frame
blocked in vkQueueSubmit2 behind the driver's emulated timeline semaphore; completion now uses fences (Frame pacing,
"Sixth round"). Also fixed: TOTK_VK_SPLIT_MS=-1 GPU hang (uniform snapshot overwritten when a replay wrapped the ring).

**2026-10-23 ~02:00: playable (user: "not perfect, but I would fully consider this playable").** 30 fps cap holds in
normal play (p50 33.3 ms); fights 22-30 fps with first-time pipeline-compile hitches that fade as the on-disk
pipeline cache fills. Fixed today: grey lighting squares (Turnip tu_use_tex_coord_round_nearest_even_mode, set by
the app), uncapped frame pacing (30 fps cap), whole-frame submissions (split), the replay thread's top hot spots,
and two split side effects (stale uniforms after a mid-replay submit; textures re-read mid-frame - see Frame pacing).
2026-10-23 night (Claude, with devrun.py): the split-only terrain bug was write-back records aging out after 3000
submissions (grass and leaf litter gone for good ~40 s into standing still with the split, ~3 min without); fixed,
see "Flat, wrongly coloured terrain tiles" under Frame pacing.
Open: hitches from new pipelines (graphics pipeline library or pre-building would help), the replay thread
(~15-20 ms a frame in fights), occasional skipped draws for shader variants missing from the seed (fill_shaders.py).

Boots, plays, saves. As of 2026-10-22 (late) the overworld renders correctly on the Thor (user-confirmed): the black
flicker (`condorder`), the dark water (`d16tie`), the overworld hangs (scheduler), the mapping-limit crash (64 KiB
protection units) and the memory leak (descriptor pools) are fixed. Performance: 27-28 fps with async replay on
(recording 17-20 ms, GPU 12-15 ms), hitches reduced by the pipeline worker but still frequent; the frame ceiling
(~43 ms median) points at the game's fence waits, whose `completion` / `wake` split is not yet read from a device log.
The gather bias is confirmed working and in the device seed (1119 biased SPIR-Vs; the old 0 marker was a marker bug,
opcodes 103/104, fixed). The black squares are traced to the missing sun term in the lighting target (see "passdump
forensics" below), not to the gather chain. Open: the squares' missing sun term (skipped draw or depth rejection),
the ~43 ms ceiling, the grass / texture glitch, occasional shadow weirdness.

### Overworld hangs and crash (2026-10-21/22) (fixed?)

Symptom: loading into the overworld, graphics stop, audio goes on. In order:

1. **No preemption.** A `threads` dump showed thread 0x1040 (priority 53) holding virtual core 1 for 5+ s in guest code
   without an SVC while 0x1004 (priority 30, core 1 only) waited: the TOTK scheduler only preempted at SVC entry.
   Fix: asynchronous preemption (`TOTK_ASYNC_PREEMPT`, on by default on Android; `=0` off): a waiter signals worse-priority
   owners every millisecond and the owner gives its core up in the handler (the Breath of the Wild mechanism,
   runtime/threads.c).
2. **Priority inversion.** 0x1040 (53) held a mutex that 0x1037 (44) waited for while 48-52 kept core 1 busy. Fix:
   priority inheritance (`tk_prio`: a mutex owner competes at its best blocked waiter's priority, as Horizon does).
3. **Preempted inside an SDK lock.** Host stacks (`su -c 'debuggerd -b <pid>'`, Magisk) showed two threads spinning at
   sdk+0x13f034 while a third, preempted at sdk+0x13f06c inside the same lock, waited for a core. Fix: priority
   preemption only in the game's own code (`main`).
4. **Starvation in SDK code.** The `threads` dump prints where the signal last interrupted each thread (`last interrupted
   ... at <module>+<offset>`; live registers cannot be read on Android). 0x1040 held core 1 for 53 s at sdk+0x5b2518
   (from main+0x2ef77c), refused 161k times. Fix: a starvation rule (`TOTK_STARVE=0`: off): a thread that waited 20 ms
   for a core takes one whose owner held it 20 ms, in any guest code, and keeps it 2 ms.
5. **Mapping limit.** Then it loaded into the overworld and froze or crashed (Android aborted creating a thread). The
   process had 92,673 mappings (`adb shell "su -c 'wc -l /proc/\$(pidof dev.kitcatkode.totk)/maps'"`), past
   `vm.max_map_count` (65530): dirty-page tracking write-protected single 4 KiB pages, each a separate mapping. Raising
   the limit with root (`sysctl -w vm.max_map_count=1048576`) made it run. Fix: nvn_dirty.c protects in aligned
   64 KiB units on Android (`TOTK_DIRTY_UNIT`, pages; 1 = the old behaviour). A write to one page makes its unit
   writable and new, so textures sharing a unit re-upload once more. Desktop test: 1054 mappings per page vs 31 per unit.

The app reads only commands appended to TOTK_INPUT_FILE after it starts (old `threads` lines were replayed at every start).

### Memory leak (2026-10-22, fixed, to confirm)

The app's memory grew ~45 MB/s in the world (the Thor's monitor: up to ~200 MB/s) until Android ran out: textures dropped
to their lowest detail at ~15 GB, then surfaceflinger aborted ("no suitable EGLConfig found") - a system-wide
out-of-memory, not a crash in the game. `dumpsys meminfo` 10 s apart: Native Heap +107 MB, Gfx dev +366 MB. Ruled out:
write-back, pipelines (flat at ~830). The per-300-frame `memory:` line showed it: descriptor pools 8 -> 212 -> 512 ->
812, one more per frame. `vk_alloc_set` took sets only from the slot's last pool; a submission needing more than one
pool (the world: ~4,400 sets a frame, 4,096 per pool) made a new pool each time and never used the reset ones again.
Fixed: a slot's pools are used in turn (sets counted per pool as well; lavapipe never reports a full pool, so the
desktop test needs the count). Watch `descriptor pools` in the `memory:` line: it should stay small.

First world numbers (Apex, 1.4.3): 17 fps, ~3,400 draws and ~168 passes per frame, ~108 of those split by NVN
barriers inside a pass (each split stores and reloads every attachment on a tiled GPU). `per frame:` in the world: recording
32-36 ms, GPU 21 ms, one submission - CPU-bound on recording first (~4,400 descriptor sets written a frame, ~2,000
reused), then the GPU.

### Dark water (2026-10-22, fixed)

The dark "patches" are water (the user's observation): water surfaces render dark and unfogged. Passdump of a hilltop
frame with a lake (1280x720 internal; pass numbers vary per frame):
- the water material pass (fs 9175cf1e, ~31 draws, after the terrain) writes the G-buffer correctly over the lake:
  material flags c0 = (0.2, 0, 0), dark teal albedo, upward normals, linear depth;
- the deferred lighting pass (fs 233dfb93, 12 draws, HDR target 2955, its own DEPTH16 target cleared in the pass)
  first writes a per-pixel class depth (the "class marker", nvn_mtl.m: from per-tile flags the material shaders store
  the frame before), then lights each class with EQUAL depth tests. The lake's pixels get the water class depth (D16
  ~39154) but stay black where the far lake is: a contiguous region (60% of the class), while the near water (same
  class) is lit. So the water lighting draw does not cover those tiles: tile feedback, not depth precision.
Fix 1 (to test): storage-buffer stores were never marked rw (the translator's flag; nvn_mtl.m msl_stores_through), so
buffers the shaders write could be bound from uniform snapshots taken at submit (older CPU bytes). vk_shader.c now reads
each SPIR-V for OpStore / atomics / calls through storage bindings and marks them rw (log: "shader ... writes N storage
binding(s)"). Result: no change; neither did `TOTK_VK_OFF=snap` nor `TOTK_VK_SYNC=1` (so not a race with the GPU or a stale
snapshot). Next: passdump now lists every draw of passes with up to 32 draws (shaders, counts, depth / stencil state,
the class uniform vp c11[25], and draws skipped with the reason), and `TOTK_VK_PD_SPLIT=d16` dumps the lighting pass
after each draw, to see which draw should light the far lake and why it does not.

Result (per-draw dump): the lighting pass lights each material class with a full-screen quad at depth
`0.5 + 0.5 * nibble_swap(class) / 255`, EQUAL against the marker's class depth in a DEPTH16 target. The water quad
(class 35, vs 04c4bc6f, fs f7641da0) lit 0 of the lake's 7,065 pixels; classes 8 / 9 / 11 / 12 lit theirs. Every such
depth lands exactly on a DEPTH16 rounding tie (32767.5 + 128.5 * n, n even), so an ulp of difference between the
marker's vertex shader and the water quad's decides the step. `TOTK_VK_D16TIE=1` (bias every DEPTH16 draw a fifth of
a step off the tie) lit the water - confirmed - but turned Link's clothes black and stair-stepped the horizon: depth a
fragment shader writes gets no depth bias, so markers that export depth stay on the tie. Fix (to test, with
TOTK_VK_D16TIE=1): fragment shaders that write FragDepth are patched at load (`spirv_depth_nudge`: each store adds a
specialization constant, set to the same fifth of a step in pipelines with a DEPTH16 target). Confirmed on the Thor: water, clothes and
horizon all right. d16tie is now on by default (`TOTK_VK_D16TIE=0` or `TOTK_VK_OFF=d16tie`: off).

### Performance (2026-10-22)

simpleperf (10 s in the world, all threads): ub_overlaps 8.9%, bionic mutex unlock swap 7.9% + lock 3.1% + unlock 1.3%
+ kernel futex 5.1%, bind_stage 5.4%, memcpy 4%, vk_treg_get 1.8%, resolve_span 1.6%. Fixed: ub_overlaps scanned the
whole uniform log whenever one long entry was early in it (now bounded by the longest entry); texture ids resolve once
per submission (`texcache`: no registry / texture / view locks or registry copies for repeated ids; samplers cached
until a sampler registration). After: recording 32 -> 24 ms a frame, 17 -> 21-22 fps (GPU 18 ms). Next: descriptor sets (~3,000 written a frame),
the pool lock in resolve_span, pass splits (~165 passes a frame, ~105 of them split by NVN barriers). Second profile (with call stacks, after
d16tie): replay 40% of all CPU; ub_overlaps still 7.3% (the long-entry window did not bound it: the updates sit
together), vk_treg_get 7.4% from stage_sig (every bound texture id's format looked up through the registry lock on
every draw), mutex unlock 9.5%. Fixed: uniform renaming memoised per (range, snapshot, submission, log generation);
stage_sig reads a per-id format cache (registration generation); repeated GPU-write marks of one range in a
submission (each material draw's rw tile-flag buffer) skip the snapshot / log work; write-back supersede checks have
an unlocked range reject. `shader missing` counts draws skipped for variants not in the seed: fill them with
android/fill_shaders.py (--device-dir /sdcard/Android/data/dev.kitcatkode.totk/files). After that round: 27-28 fps, recording 17-20 ms, GPU 12-15
ms a frame. The replay still runs inside the game's submit call; `TOTK_ASYNC_REPLAY=1` (hle_nvn.c translation thread,
the iPhone default) queues submits and presents to a separate thread so the game's next frame overlaps the replay.
First device run with it (2026-10-22): fps 16-27 depending on the window, "hitches a LOT" (the user), a patch of
grass disappeared while sitting still and textures/polygons shifted.

### Frame pacing and the 30 fps push (2026-10-23)

The user: 27-28 fps but "not stable", not playable yet. The frame budget math says the bottleneck is not the replay
(recording 17-20 ms) or the GPU (12-15 ms) alone: 27 fps is 37 ms. Suspects, from reading the code:
- **Presentation has no pacing.** Each frame is blitted to a host buffer, read back, and a presenter thread memcpys it
  into the window (ANativeWindow_lock / unlockAndPost) whenever it is ready; nothing aligns posts to the display's
  vsync, so even a steady 30 fps can judder. A real fix is zero-copy AHardwareBuffer + ASurfaceControl with desired
  present times (or a loader swapchain).
- **GPU waits keep the virtual core.** runtime/threads.c runs guest threads on NCORE = 3 virtual cores (core_take).
  mtl_wait (fences, present waits) does not give the core up (runtime_core_away is only used by applets / swkbd),
  so a game thread waiting on the GPU blocks one of the three cores for other guest threads.
- The game threads themselves (guest code + HLE) may be the limit; nothing measured them per frame before.

Instrumentation (every 300 frames): `frame time p50/p90/p99/max, N over 34 ms, guest GPU waits X ms (of it holding a
core), guest core queueing` (vk_draw.c perf_summary), `CPU per frame ... top: <thread>/<tid> ms` (from
/proc/self/task; guest threads are named `g<handle> p<prio> c<core>`, the replay thread `totk-translate`), and
`window posts, last 300: interval p50/p90/p99/max, lock + copy + post ms` (presenter). Experiment switch:
`TOTK_FREE_CORES=1` (no virtual-core limit; may hang where the game relies on Horizon's scheduling).

First capture with it (2026-10-23, perf_a; the yuzu name + rounding option on):
- Menus / light scenes: 120 fps (8.3 ms frames), the world 46-53 fps in lighter spots, heavy areas 17-20 fps
  (p50 46 ms). **The game ran uncapped**: TOTK_UNCAPPED (default on) + the 60 FPS patch + the real-time frame step,
  ceiling 120. So the cadence swung 8 / 16 / 45 ms: the "not stable" feel. Fix: the Android app defaults
  `TOTK_UNCAPPED_HZ=30` **and `TOTK_UNCAPPED_INTERVAL=1`** (frontend_android.c), the shipping rate; totk.env can raise
  it. The first try set only the HZ (perf_c, "feels worse by a good bit"): the uncapped period is the game's own
  present interval (2 or 3) / TOTK_UNCAPPED_HZ, so 30 gave 67 / 100 ms frames (heavy area 13.6 fps, p50 74 ms, the
  hitch median exactly 100 ms). TOTK_UNCAPPED_INTERVAL=1 makes the period 1 / HZ. Same capture: heavy-area recording
  20.9 ms, GPU 13.6 ms, 2.5 splits a frame (the split itself not judged yet: the cap hid it).
- Heavy areas: recording 22-30 ms + GPU ~20 ms = the 46 ms frame, with **1.0 submissions a frame**: TOTK submits a
  frame at once, so the GPU waited for the whole replay. Fix: submission split (vk_draw.c split_maybe, before a new
  pass once a submission has recorded TOTK_VK_SPLIT_MS = 4 ms; 0 off, -1 every pass for the tests, which all pass
  that way). RING 8 -> 24 for the extra submissions in flight; descriptor pools 4x smaller (1024 sets) so 24 slots do
  not hold hundreds of MB (lavapipe ran out of host memory at 14 slots with the old size).
- CPU per frame (heavy): totk-translate 22-29 ms, the main thread ~7, two workers ~6-7 each; guest GPU waits 0 and
  virtual-core queueing 10-30 ms summed over all threads. NVN's waits already give the core up (NVN_BLOCKING).
- `TOTK_FREE_CORES=1`: froze (run B). The virtual cores stay.
- Window posts: 0.8-8 ms per copy on the presenter thread; it tracks the game's frame times (no extra judder of its
  own measured yet).

Third round (2026-10-23, perf_d with the cap fixed): normal play holds 30 fps (p50 33.3 ms), "super playable" until
a fight (stone talus + bokoblins): 15-24 fps. Two causes:
- **First-use pipeline builds**: every big hitch line is pipelines (22 in 878 ms, 7 in 403 ms; 40-100 ms each on
  Turnip), about one new shader per new pipeline, so they are first-ever compiles; the on-disk pipeline cache should
  make the second encounter cheap.
- **Recording** (simpleperf of totk-translate, 15 s in a fight): bind_stage 13.9% (its own time: the descriptor-set
  cache hash, FNV-1a byte by byte over ~1 KB per stage per draw), ub_overlaps 12.8% (the vk_ub_rename memo was voided
  by every one of the 100+ inline updates per submission), bionic mutex unlock 9% + unlock 2.2% (the pool lock on
  every buffer binding, shared with the game threads), resolve_span 4.2%, memcpy 4.2%, vk_ub_rename 3%, memcmp 2.6%,
  vk_treg_fmt 2.5%. Fixed: an eight-byte-step hash; the memo checks the log changes since it was made and stays valid
  unless one overlaps its range (TOTK_VK_OFF=ubmemo: the old rule); vk_ub_touch / vk_ub_pending reject by the
  unflushed entries' bounds first; a per-thread resolve cache keyed by a pool-table generation (TOTK_VK_OFF=rcache).
  New test: an unrelated update between draws keeps the memo, an overlapping one after it does not.

Fourth round (perf_e / perf_translate2, 2026-10-23): "very playable, feels really nice 90% of the time". The pipeline
cache works on the Thor (25.5 MB loaded at start, saved and growing; 362 cached pipelines in 153 ms). 2,740 draws
skipped as `shader missing` in a fight: new variants, filled with fill_shaders.py. The profile after the first fixes:
ub_overlaps 19.9%, bind_stage 7.7%, memcpy 5%, mutex unlock swap 5% (was 9%), vk_ub_rename 4.6% (the change-ring
scan per lookup), vk_treg_fmt 3.8%, memcmp 3.5%, vk_prog_resolve 2.7%, scudo realloc 2.5%. Fixed: the memo's validity
check is per 4 KiB page generation (O(pages) instead of a scan of every change); ub_overlaps sorts larger results
with qsort (the insertion sort was quadratic for a 64 KiB binding over dozens of updates); the descriptor-set cache
grows its info copy instead of a realloc per new set.

**Stale uniforms after a mid-replay submit (2026-10-23, fixed).** After the split went in: the ground near Link lost
its material for a while (dark, smeared, grass gone; `shader missing` 0) and the save-select screen showed a green
backdrop. Cause: with a uniform snapshot, ub_rename_slow / vk_ub_peek kept only log entries with no fence, meaning
"this replay's updates" (the snapshot holds earlier replays'). But a submit in the middle of a replay (the split,
also a CPU step or the window copy) flushes and seals this replay's updates too; every binding after it then skipped
them and read the snapshot's older bytes. Entries now carry the replay they were logged in (`rep`, bumped at each
snapshot), and that decides. Test: an update, a CPU step (an odd-sized update forces a mid-replay submit), then a draw
of the same range with a snapshot - it read 200 (stale) before the fix, 100 after. Present before the split too, in
the rarer CPU-step / present cases.

**Flat, wrongly coloured terrain tiles with the split (2026-10-23, fixed: user-confirmed with the split on).** Still there after the
stale-uniform fix, gone with TOTK_VK_SPLIT_MS=0. vk_tex_get checked a mirrored texture's memory for CPU changes once
per Vulkan submission; with the split that ran again in the middle of a replay, and with async replay the game is
already writing its next frame: a later chunk uploaded next-frame bytes (e.g. a recycled terrain tile) and the rest of
the frame drew with them. The check is now once per replay (vk_replay_serial: mtl_sub_begin, and every CPU step, whose
writes are in command order).
  Later the same night: the user saw it again with the split on (the check above was right in itself but not, or not
  all of, the bug). Diagnostic: `TOTK_VK_SPLIT_DEFER=1` records the splits as usual but hands their command buffers to
  the GPU only with the replay's next real submission (one vkQueueSubmit2 with several submits). Bug gone with it ->
  a timing issue (the GPU running part of frame N while the game writes frame N+1 into memory the GPU reads unsnapped:
  vertex / storage / shadow-cache data); bug still there -> a recording-state issue of the split itself.
  **A/B (2026-10-23, Claude with devrun.py; standing still at the save in slot_03, autumn field with a wooden
  scaffold, 210 s, captures every 20 s): a recording-state issue, not timing.** About 20-40 s after the world appears
  (in-game ~3:55 PM) the ground around Link turns dark and smeared, its grass is gone, and Link casts no shadow; it
  stays that way. Split on (4 ms): 2 of 2 runs. `TOTK_VK_SPLIT_DEFER=1`: 2 of 2, at the same time. `TOTK_VK_SPLIT_MS=0`:
  0 of 2 (ground and grass intact until the real dusk shadow of the mountain sweeps over at ~5:35 PM, with a straight
  edge, which the split runs show as well). So the GPU running early is not it; something the split does to the
  recording is. DEFER still does everything of the split but the vkQueueSubmit2: ub_flush / ub_seal, gw_seal, a new
  submission serial (every per-submission cache starts over) and a new ring slot (transient memory, descriptor pools,
  its own `pre` command buffer for image inits and uploads). The numbers (fps 24 split / 21 DEFER / 21 off) are this
  spot's.
  **Cause found and fixed (2026-10-23, same night): write-back records aged out after 3000 Vulkan submissions.**
  Bisection, 150 s runs at the same spot, split on: `TOTK_VK_OFF=writeback` broken; the per-submission caches off
  (`ubmemo,rcache,pfront,texcache,block`) broken; a split without its uniform flush / seal, or without gw_seal,
  broken; a split that keeps the submission serial (diagnostic, since removed) intact for 140 s and broken later. The
  onsets line up in submissions, not seconds: ~3400 submissions after the world appears with the split (~3.5 a frame)
  and without it (the "dusk shadow" of the no-split runs at ~160-180 s was the same bug). vk_res.c wb_age_locked
  dropped every write-back record not invalidated within 3000 submissions. The game renders some maps on the GPU
  into CPU pools and reads them back (invalidate) much later; with the record gone the invalidate wrote nothing back,
  the CPU read the old bytes, and the grass and leaf litter vanished for good (passdump of a good and a bad frame at
  the spot: the same passes and draw counts, the 2202-draw G-buffer pass without grass and leaves in albedo and
  normals; the occlusion depth identical). Confirmed both ways with `TOTK_VK_WB_AGE` (new): no split with age 300 broke
  at 60 s like the split; split with age 0 did not stay broken. Fix: only records whose image is an orphan (its
  texture object gone) age; a record a live texture or a shared image holds costs no memory and stays (log: `GPU
  write-back: ... N orphans aged out (M live kept)`; at the spot 0 orphans, ~100 live kept per 3000 submissions).
  Device: split 2 of 2 clean for 150 s, no split clean for 240 s (only the real dusk shadow, grass intact inside it).
  Test: vkdrawtest renders into a CPU-pool texture, runs 50 submissions with the age at 20, invalidates: the
  rendering is written back (it read the old bytes before the fix).
  Not explained yet: with the age at 0 (the fix's behaviour for these records) one of two split runs still showed
  the dark ground in single captures (60 s and 100 s in one run, 120 s in the other) that were fine again 20 s later;
  the two runs with the fix itself showed none. The pass dump run (`e_pd`) also had a light ground without leaves
  for a while. If the user still sees short-lived flat tiles, that is the thread to pull (passdump at the moment).
  After the fix, camera-pan runs (Link still, right stick held in 15 s bursts, 200 s, captures every 10 s; no walking:
  moving that long would likely trigger an autosave): split 4 runs, no split 2 runs. The leaf litter was there in
  all captures but two of one split run (h_pan_1 at 50 s and 120 s: grass and pebbles present, leaves gone, ground
  darker, no sun shadow), which were fine again 10 s later. Nothing split-specific in the rest; darker frames
  (backlit, the dusk shadow) appear in both. Open and rare: the vanishing leaf litter.
  `TOTK_VK_SPLIT_MS=-1` on the device: `device lost` within ~20 s, at the moment auto-load starts loading the save.
  **Fixed 2026-10-23 (morning): the uniform snapshot was overwritten when one replay used the whole ring.** kgsl
  logged a `gpu timeout` (a hang, not a fault) on one submission; the new hang log (nvn_vk.c notes_log: what the
  unfinished submission held) showed the same one every time (5 of 5 runs, also with `TU_DEBUG=sysmem`): one 8x8 pass
  with 8 colour targets, fragment shader 0534127e..., whose loop runs `c4[52].x` times. A watch on that binding: guest
  memory 2, the GPU's view 0x3f800000 (1.0f's bits: ~10^9 iterations). The replay's snapshot lived in the transient
  memory of the slot that was current at the replay's start; a split before every pass makes ~40 submissions a frame,
  more than the 24 slots, so mid-replay that slot was begun again, its memory reset, and the next support buffer
  (73 x 1.0f) was written over the snapshot the rest of the replay still bound. Snapshots now have chunks of their
  own (snapmem_take), free once the replay's last submission is done. Device: SPLIT_MS=-1 2 of 2 runs 150 s into the
  world, intact (12.5 fps, expected for a submission per pass). Test: a replay of 40 CPU steps, then a draw reading
  the snapshot (read the support buffer's 1.0 before the fix). Normal play could hit it only with 24+ submissions in
  one replay (splits + CPU steps + presents): a 180 s camera-pan run with the 4 ms split peaked at 21 (2 of 4,200
  replays at 12 or more), so it is probably not the short-lived leaf-litter loss, but close enough to the ring size
  that loading screens or heavier scenes might have hit it.
  A GPU hang now logs `【红】submission N: D draws, ...` and its last 8 passes / dispatches / copies (shaders, sizes).
  The devrun.py runs before the device was kept awake were void: asleep / behind the lock screen the app had no
  window (black captures, then device lost).

Fifth round (perf_translate3, a fight, 2026-10-23, "already feels a lot better"): ub_overlaps 19.9% -> 0.3%. Now
flat: memcpy 9.7%, bind_stage 8.2%, vk_ub_rename 6%, mutex (swap 5.9% + unlock 2.9% + cas 0.7%), memcmp 4.6%,
vk_treg_fmt 4.4% (stage_sig: every bound texture's format on every draw), vk_prog_resolve 3.1% (the program map's
mutex), pipe_get 1.9% (the pipeline map's mutex and two hash operations). Fixed: stage_sig memo per stage (same
bound ids, no registration since: vk_treg_epoch); per-thread program cache (vk_progs are never freed, serials never
reused); per-thread pipeline front cache (TOTK_VK_OFF=pfront). memcpy's callers unknown yet (call-graph report).

**Above 30 fps and gyro (2026-10-23).** The cap is `TOTK_UNCAPPED_HZ` (with `TOTK_UNCAPPED_INTERVAL=1`, the app's
default): 40 suits a 120 Hz panel (3 refreshes a frame), 60 for light areas. The window now asks Android for that
frame rate (ANativeWindow_setFrameRate, FIXED_SOURCE; log `window frame rate N fps`), so the display picks a rate the
cap divides. Gyro: frontend_android.c motion_thread feeds the device's accelerometer + gyroscope (200 Hz) to
runtime/motion_fusion.c, which the game reads as the Switch six-axis sensor (on unless TOTK_GYRO=0; aiming etc. per
the game's own Motion Controls options). Android's accelerometer sign is flipped to the Switch's gravity vector.
Axes (fixed 2026-10-23, user-confirmed "works exactly as I would expect"): the first version assumed the sensors' natural
orientation was the screen's; the Thor's is portrait (user: steering like a wheel moved the view up and down;
`TOTK_GYRO_LOG=1` with hold still / steer / tilt / turn showed gravity on raw x in the landscape hold, tilting on y,
turning on x). The axes now follow Display.getRotation like SensorManager.remapCoordinateSystem (the Thor: 90 deg ->
"-y,x,z"; log `[motion] axes ...`); TOTK_GYRO_AXES overrides (it was never read on Android before).
Uniform copies: 617 KiB of snapshot per submission (copied twice: ucap_build on the game thread, then into transient
memory on the replay thread) and ~98 renamed copies a submission - the likely bulk of the replay's memcpy (9.7%).

**Sixth round (2026-10-23 morning, Claude with devrun.py; the field save, 40 cap, 180 s with camera pans): the replay
thread was blocked in the driver, not slow.** Recording 25-27 ms a frame (28.5 fps, p50 35 ms) at a quiet spot, but
the thread's CPU time was 11.6-13.9 ms a frame. Pinning it to the prime core (taskset 0x80 mid-run) changed nothing;
schedstat: 0.04 s of run-queue wait in 10 s, ~7 voluntary sleeps a frame. An off-CPU profile with DWARF stacks
(`simpleperf record --trace-offcpu -e cpu-clock --call-graph dwarf`; fp stacks stop in the kernel): 37% of the
thread's time in slot_submit -> vkQueueSubmit2 -> a mutex in driver.so, while our completion thread sat in a kgsl
waittimestamp ioctl inside the driver. Turnip on kgsl emulates timeline semaphores (Mesa vk_sync_timeline); waiting on
ours (even polling vkGetSemaphoreCounterValue every 250 us: no change) holds the lock the submit needs to signal it,
so each submit waited for the GPU. **Fix: completion by a VkFence per submission** (nvn_vk.c fences_on; the timeline
stays behind `TOTK_VK_FENCES=0`). Two runs each: recording 25.0 -> 14.0 ms, 28.7 -> 33.0 fps, p50 34.7 -> 30.5 ms,
frames over 34 ms 180-190 -> 26-38 of 300; captures intact, the same log errors. The frame is now bounded by the GPU
(~20 ms) and the game's own threads, not the replay.
Also (no fps change on its own): uniform bindings trimmed to what the shader reads (vk_shader.c spirv_ubo_extent: a
block read only at constant indices binds (max index + 1) x 16 bytes, not 64 KiB): renamed copies 52.7 KiB -> 0.7 KiB
on average (~2.9 MB of memcpy a frame -> ~60 KB). `TOTK_VK_OFF=ubtrim` turns it off.

**Seventh round (2026-10-23, Lookout Landing save, slot_05; 40 cap, 180 s with pans).** First run (cold pipeline
cache for the spot) 26.5 fps; warm: 4 ms split 29.5 / 28.9 fps, recording ~19.5 ms (real CPU now: 20.8 ms), GPU
22.2 ms, the game's GPU waits ~5.4 ms a frame. Split interval (two warm runs each): 2 ms 30.9 / 30.8 fps (p50 32.2 ms,
GPU waits 4.3 ms), 1 ms 31.3 / 30.4 (nothing more): **default 4 -> 2 ms** (a submit is cheap since the fence fix).
With it, 32.0 fps, p50 30.8 ms. GPU side, each one run: `TOTK_VK_OFF=passbarrier` (no pass splits at barriers, ~105
of ~156 passes a frame: not correct, a measurement) GPU 22.0 ms - the splits cost nothing measurable (Turnip renders
these passes in sysmem); `TU_DEBUG=sysmem` the same as default; `TU_DEBUG=gmem` GPU 41.7 ms, 18.9 fps. The GPU's
~21.5 ms is the game's shading; the large lever left is an internal resolution scale. Replay thread there (perf):
bind_stage 8.9%, vk_ub_rename 7.6%, mutex unlock (swp2_rel) 7.1% (g_tex_mu per texture per draw: vk_tex_get,
vk_tex_by_id, view_get), memcpy 6.9%, memcmp 5.3%.
Render resolution there (`TOTK_RENDER_RES`, the game's own scene size, upscaled by the game; one run each): 1280x720
32.0 fps (GPU 21.6 ms), 1088x612 33.2 (19.8 ms), 960x540 34.6 (18.8 ms): 44% fewer pixels saved 13% of the GPU time
(the 720x720 shadow maps, geometry and compute do not scale), and at 540p the replay (~18 ms) is close to the limit.
A modest lever; left to the user's taste in totk.env.

### What sets the frame time: the game waits for each frame's GPU completion (2026-10-23)

Lookout Landing, 40 cap, no frame generation (~31 fps). Measured with two new diagnostics:
- `TOTK_VK_PASSTIME=1` (GPU timestamp after every pass / dispatch, top passes logged every 10 s): the GPU is busy
  ~670 ms a second, ~22 ms a frame, spread over the game's real work (G-buffer passes, 1280x720 x6 colour, ~6.4 ms;
  8-colour lighting ~3.5; 720x720 shadow maps ~2.2; 640x360 effects ~2.2; ~125 smaller kinds ~7.5). No single
  broken pass.
- `TOTK_TIMELINE=<file>:<first frame>:<frames>` now also gets Vulkan events (commit at vkQueueSubmit2; "gpu" from the
  submit to the completion thread seeing the fence). 119 frames: replay 21.6 ms, GPU still busy 11.9 ms after the replay
  ends, submit -> GPU done 35.4 ms, **GPU done -> the game's next submit 1.3 ms**, frame 36.8 ms (that window ran
  slower than without the timeline). The game's render thread submits frame N+1 only after another game thread's
  nvnSyncWait on frame N's fence returns (NVN fences here are queue-level; we wait exactly as hardware would). On a
  Switch that latency is GPU time alone; here it is our replay plus the GPU work left when it ends. The last part of a
  frame (lighting, post) is cheap to record and expensive to draw, so the GPU sits ~10 ms a frame waiting for the
  replay to reach it. **Every ms of replay saved comes off the frame**; ~10 ms of replay would be ~25 ms frames here.
**Correction with the GPU's own spans** (the timeline now records each submission's GPU start / end from its
timestamps, aligned by android/tl_frames.py; `seen` is when the completion thread noticed): replay 22.8 ms, the GPU
starts 3.1 ms into it, is **busy 24.7 ms** and finishes 11.1 ms after the replay ends; seen 0.2 ms later; the next
submit 1.5 ms after that; frame 37.8 ms (timeline overhead included). So the GPU is the larger part, with ~6 ms of
idle gaps waiting for the replay: an infinitely fast replay would still give ~29 ms frames here (~34 fps). GPU and
replay are roughly balanced, which is why single-sided changes moved little:
- push descriptors (`TOTK_VK_PUSH=1`, fragment / compute sets pushed; vertex keeps sets): replay CPU 18.0-18.8 vs
  18.6 ms, no change (building the bindings is the cost, not allocating sets); kept, off by default;
- the texture lookup cache per replay instead of per Vulkan submission (6 submissions a frame with the 2 ms split):
  no measurable change; kept (correct, fewer misses);
- GPU clock pinned at its 680 MHz top (the governor holds 615 at ~66 % busy): GPU 21.7 -> 20.5 ms, fps 31.0 -> 31.1;
- replay profile at Lookout Landing: bind_stage 37 % inclusive (uniform bindings 15, texture lookups ~15-20 with the
  lock), address resolution ~13 %, memcpy 7, memcmp 5; no single hot spot.
Turnip on kgsl lacks VK_EXT_calibrated_timestamps (`TOTK_VK_EXTLIST=1` lists the 139 device extensions).
Ruled out the same day: `spirv-opt -O` on all 21,011 seed shaders (GPU 22.9 vs 22.3 ms: Mesa already optimises);
a 4th virtual core (TOTK keys per-core data by core number for Horizon's three application cores; game threads'
masks stop at core 2); the replay drains before CPU-pool invalidates (~300 a 300 frames, ~16 ms each, but on a thread
off the critical path: waiting only for fenced submissions removed them and changed nothing; reverted). Note: the
game's "NVN 提交+出图" counter adds up the replay thread's recording, not game-thread blocking.

### The replay on the prime core (2026-10-25, thor-tools bench, Lookout Landing)

Android has no time-constraint policy: darwin_compat's `thread_policy_set` returns KERN_FAILURE, so x_thread's
`set_time_constraint_comp_x` (worth 6.6 -> 13 ms on the Mac when missing) does nothing here, and the scheduler put
totk-translate on the A715 / A710 cores. On the Cortex-X3 (CPU 7) the replay is ~27 % faster:
- Overnight session in AYN's **standard** performance mode (CPU ~1.84 GHz, GPU 401 MHz caps; note the mode in every
  A/B, high performance is 3.19 GHz / 680 MHz): replay 23.3-24.2 ms in 8 runs with the scheduler's placement, 17.4-17.9
  ms in the 3 runs where it sat on CPU 7 (WALT per-task boost 2 via thor-tune, or taskset 80), fps 22.4 -> 23.4-25.3.
  The sixth round's "pinning to the prime core changed nothing" predates the fence fix (the thread was then blocked in
  the driver, not computing).
- Now in the runtime: `host_replay_cpu_tick` (runtime/host_os.c, Android only), called by x_thread each present.
  Same session, same mode: pinned 17.3 ms / 28.2 fps vs `TOTK_REPLAY_CPU=-1` 22.4 ms / 26.7 fps.
- **A pin set once does not last.** Pinned once at the first present: 24.8 ms, and `thor-tune threads` showed the
  thread's mask back to all CPUs (df, 5f, 3f) during the run, presumably Android re-attaching the thread to its cpuset.
  So the tick checks every frame (one sched_getaffinity) and pins again; the log line `[replay] CPU 7 over N frames:
  pinned again R times, core paused P times` counts both. Measured: 16 re-pins and 934 pauses, all while loading.
- **Paused cores look like interference but are not.** Qualcomm's core control pauses idle CPUs (the prime core too,
  while loading). A paused CPU is missing from sched_getaffinity's answer (masks like df = CPU 5 paused) and pinning to
  it fails with EINVAL until it is back. The overnight run whose taskset "failed" made all five attempts while loading.
- Ruled out the same night: `-march=armv8.6-a+fp16+fp16fml+dotprod -mtune=cortex-a715` for the runtime (Makefile
  CPU_FLAGS, default off; inlines LSE atomics, 222 -> 0 helper calls): no measurable change. Never `-mcpu=cortex-a715`
  / `x3`: the Thor has no SVE and those emit it. All TOTK threads off the efficiency cores (WALT sched_lib_name
  libtotk.so): no change. The 7 % `swp2_rel` in the replay profile is bionic's pthread_mutex (uint16 state), not ours.

### Frame generation: lsfg-vk (2026-10-23, works on the Thor)

the user's idea: Lossless Scaling's frame generation, which the user owns. lsfg-vk v1 (MIT; v2 is CC BY-NC-ND and cannot be
built on) has a self-contained core, `framegen/` (~10k lines C++20 plus volk, no other dependencies), whose shaders
are SPIR-V resources of Lossless.dll; the user's DLL (Oct 2026) has all 100 that v1's table names. Vendored in
third_party/lsfg-vk (ORIGIN.md), built into **liblsfg.so** (own library: its volk defines the same global vkXxx names
as the backend), loaded with dlopen by android/gfx/vk_fg.c only when `TOTK_LSFG` is set. The core makes its own
VkDevice on the same Turnip (by our vkGetInstanceProcAddr, a patch to its instance.cpp) and shares images as opaque
fds (Turnip has those). Semaphores: Turnip on kgsl exports and imports only **sync fds** (opaque-fd export failed with
VK_ERROR_INVALID_EXTERNAL_HANDLE), so the core is patched to import the input's sync fd temporarily and to hand back a
sync fd per generated frame after submitting it. Per real frame (vk_draw.c present_window): the window picture is
blitted into input 0 / 1 (RGBA16F, as the layer), the submission signals a semaphore exported as a sync fd,
lsfg_present generates, our next submission waits for the returned fds and copies each output into a presentation
buffer; the presenter shows the generated frame(s) first and the real one after, spaced by the real interval / the
multiplier. The window asks for cap x multiplier Hz.
First run (Lookout Landing, `TOTK_LSFG=2`, 30 cap, camera pans): window posts p50 19-20 ms, p99 23 ms, none over 34 ms
(without it, about half over 34 ms); the real frame rate 26-27 fps (29-30 without it: the generation's GPU time).
Captures intact; a smeared digit in the minimap's coordinates shows on generated frames (HUD text is the usual weak
spot of interpolation). Settings: `TOTK_LSFG=2/3/4`, `TOTK_LSFG_PERF=1` (3.1p, lighter), `TOTK_LSFG_FLOW=0.25..1`,
`TOTK_LSFG_DLL` (default Lossless.dll in the game folder; put there with su, owned like pipeline_cache.bin).
Latency: the real frame is shown half an interval later than without it.
(Correction: the "smeared digit" in the minimap coordinates is the game's own icon over the number; real frames show it
too.)

**The HUD kept out of frame generation (2026-10-23, on by default with TOTK_LSFG; `TOTK_LSFG_HUD=0` mixes it in).**
Pass dumps (Lookout Landing and the field) show the frame's end: the scene composite (fs 0ac9a549...) into a 1280x720
image, the HUD passes on that image (fs 309199f1..., 35 + ~145 draws: icons, hearts, minimap, text), the scale pass
(fs dd3d1e0e...) into the 1920x1080 output (top-left 1280x720 presented), then the present. vk_draw.c hud_mark copies
that image into a "scene" picture before the first HUD pass (only on the image the composite finished this frame:
menus and the title draw the HUD shader onto other targets) and into a "final" picture when the scale pass starts
reading it; lsfg-vk interpolates scenes, and a compute pass (shaders/hud_mask.comp, embedded SPIR-V; `make hud-spv`)
gives each generated picture the final pixel wherever final != scene. Things learned on the way:
- the HUD image holds linear colour and the scale pass writes it through an sRGB view: a fit of copy vs shown was
  exactly the sRGB curve, so the copies go through an sRGB staging image;
- a submission can hold the start of the next frame, so "final" must be taken at the scale pass, not at the present;
- keep the backend's texture record from the HUD pass and ask for its image at each blit (per-frame render-target
  objects; the backend can switch a texture's image mid-frame);
- a reused presentation buffer kept the "generated" flag: real frames were dropped (looked like a frame mismatch);
- the title draws effects onto the output after the HUD image: the presenter compares 49 pixels of "final" with the
  frame shown and drops the generated picture on a bias (> 6 levels).
Cost: none measurable (24.8 fps with it, 23.9 without, pictures p50 20.3 vs 20.8 ms; both on an 11 % battery, below the
26-27 fps of earlier runs). Runs between ~16:30 and the battery dying were slowed from their first frame (a launcher
and the dual-screen assistant at ~50 % CPU, the battery running out): not comparable.

### Hitch profiler and pipeline hitches (2026-10-22)

The hitch profiler (vk_draw.c `mtl_present`) logs, per frame above 40 ms and 2.5x the median of the last 64, what the
backend did in that window, and a histogram every 300 frames. First run with async replay on: hitches of 30-45 ms
every few seconds, worst 60-100 ms. The one counter that stands out: **pipeline creation** (e.g. one hitch at frame
2720 logged 47 pipelines taking 1356 ms total). `vkCreateGraphicsPipelines` on Adreno compiles a lot even with a
pipeline cache, and every new material's first draw paid it on the recording path.

Fix (vk_draw.c): pipeline creation moved off the recording thread onto a worker. On a miss `pipe_get` copies the key
to a job ring (g_pjobs, 256 entries) and returns NULL, which `draw()` already handles as a skip (the "pipeline" skip
counter): that submission misses the draw, the worker fills the cache, the next submission draws. Each material pays
its build once, off the frame path. The pipeline cache (V.pcache) is externally synchronised, so all creations
(graphics and the compute ones in vk_shader.c) take g_pipecache_mu. `TOTK_VK_OFF=pipeasync` restores synchronous
building. Draw-skip counts land in the usual frame line (`skipped: ... pipeline N`), so if geometry stays missing the
count grows - look there.

Second run's log still showed pipeline hitches with `pipeline 0` skips and submit bursts (311 submits replaying in
8 ms right after a hitch) - the shape of a recording thread that stalled on builds while the game's submits queued
up. With the async path active the first miss of every new pipeline skips at least one draw, so `pipeline 0`
alongside hundreds of created pipelines means the worker never ran: that binary predates the fix (the stale-binary
gotcha) or `TOTK_VK_OFF` contained `pipeasync`/`all`. The startup line `pipelines: built on a worker thread` now
settles it, and the hitch line counts sync-built pipelines separately.

Third run (worker active, 0 sync-built): the spikes shrank (worst 127-205 ms, was 354-719) and the user confirmed
"hitches seem much better". Fourth run: two builders (TOTK_PIPE_WORKERS) but the spikes came back with the bias
build's area transitions (worst 1870 ms, 60-90 pipeline batches of 0.5-2.7 s right there), and recording rose
(28.3 ms a frame, was 17-20): the builders starve the game's threads or stress memory (resident ~3.1 GB). Changes:
the builders run niced (+10), a key skipped for more than 90 submissions (PIPE_WAIT_MAX) builds synchronously once -
geometry must appear (a hitch, not a hole) - and mtl_wait now counts every thread blocked on GPU completion into
`game waits N (X ms)` in the hitch line: with async replay that is where the game's own stalls show.

### Black terrain patches (2026-10-22, fix written, to confirm)

Screenshots: ground and distant terrain black in rectangular patches. The Mac had this (nvn_mtl.m 2026-10-15): the game
bakes terrain tile normals on the GPU into a CPU-visible pool and, after nvnMemoryPoolInvalidateMappedRange, copies them
with the CPU into new tiles. Our textures in CPU pools keep a GPU rendering only in their VkImage, so the CPU copied the
stale bytes. Fix (vk_res.c `wb_note` / `mtl_pool_invalidate`, `TOTK_VK_OFF=writeback` turns it off): every GPU write to
such a texture is noted (its pages protected page by page, `nvn_dirty_protect_exact`, so writes next to it in the same
64 KiB unit do not count); an invalidate over it reads every level back and swizzles it into memory, unless the CPU
wrote those pages since (then its bytes are newer) or a GPU copy into that memory came later. The log shows
`[writeback] ...` lines and, every 3000 submissions, a `GPU write-back:` summary. If patches remain, check those
counts first (no write-backs at all = the game reads them another way).

First device run: 423 write-backs (mostly 1x1 RGBA8 / R32F, every frame), no change to the patches, and
`TOTK_VK_OFF=writeback` looked the same. A closer look at the screenshots: the patches are pieces of terrain with their
grass and trees, dark and unfogged, floating at the wrong place (above the hill, in the sky next to a village) - tiles
with wrong heights and normals, not just wrong shading. The first version skipped block-compressed textures, but TOTK
renders into BC textures through a one-texel-per-block view (vk_draw.c blk_rt; nvn_mtl.m blk_aux) and the Mac writes
those back (it only skips BC on GPUs without BC). Now BC textures are written back too, and so are unshared images
(BC textures never share storage: a write-back record keeps the image after its texture object goes, `orphan`).
Log: `[writeback] block-compressed ...`. If that line never appears, the heights come from somewhere else.

## Fixed: black flicker (lit geometry went dark for one frame)

**Cause:** the game clears its shadow / AO mask under conditional rendering (two counter reports compared). Our
conditionals were evaluated at record time from counter memory, which reports only write when their submission
completes, so the decision depended on timing; on some frames (camera cuts most of all) the clears ran and wiped the
mask. **Fix:** `condorder` in `vk_draw.c` - report values fixed at record time and read by later conditionals in GPU
order; SAMPLES_PASSED is a cumulative counter. Confirmed on the Thor: no flicker. The investigation log below is kept
for reference (several hypotheses in it were wrong; the final runs are what matter).

Symptom: for single frames, lit geometry goes dark while emissive things and the UI stay. 20-30 times a minute on the
title screen and in dialogue. **The title screen flickers very consistently on every camera cut to another angle of the
background, and not during fades** (user, 2026-10-21) - the cut is the reproducible trigger.

The dark frames are what the game rendered (the presenter's copy is fine): draws, passes and uploads on a dark frame
match a normal one.

Ruled out (the flicker survives each):
- every `TOTK_VK_OFF` switch, and `TOTK_VK_OFF=all`
- `TOTK_VK_SYNC=1` (wait for each submission: no CPU/GPU race)
- `TOTK_VK_SERIAL=1` (every draw its own pass: no in-pass overlap)
- another driver (Purple T23); `TOTK_UNCAPPED=0`; game version 1.0.0 vs 1.4.3
- compressible storage shared across formats (fixed anyway, `compalias`: the Mac's 2026-10-15 flash cause)
- NVN barriers dropped inside a pass (fixed anyway, `passbarrier`)
- non-compressible storage shared across formats: the log counts 0 such shares
- dynamic resolution: the scene stays 1280x720 (`depth target sizes` log lines only show shadow / cube passes
  alternating)

Found with the pass probe (2026-10-21, title screen, three flashes): every pass up to about pass 52 matches the good
frame. The first pass that goes black is a **640x360 RGBA8 (NVN 0x25) pass with 3 draws**: mean 0.0000 in the flash,
~0.72-0.75 in the good frame. The passes after it that read it (640x360 / 320x180 RGBA8, 1 draw each: a blur chain) are 0
too, then the 1280x720 R11G11B10F (0x3f) scene lighting (12 draws) comes out near 0 and everything after it is dim. A
half-resolution mask around 0.75 multiplied into the lighting is most likely the shadow / ambient-occlusion mask. An
RGBA8 target writes 0 for NaN, so the pass may compute NaN rather than draw nothing. Next: what that pass reads
(the probe report now lists each differing pass's fragment shader, the textures it samples and whether this frame drew
them, and missing textures / buffers), especially inputs not drawn this frame (history from the previous view).

Second probe run: that pass is fragment shader `679c5257f9a55f7f`. It reads the scene depth (1280x720 DEPTH32F 0x34),
G-buffer targets (1280x720 0x25 / 0x38), the linear depth (1280x720 R32F, drawn this frame) and a **DEPTH16 (0x32) map
that is 1024x1024 in the flash frame but 512x512 in the good frame** - almost certainly the shadow map; the probe said
"not drawn this frame" because depth passes were not tracked yet. So on a camera cut the shadow pass samples a different
(bigger) shadow map, and that one gives "fully shadowed". Next run tracks depth passes, copies, clears and image stores
too, so the report says what wrote that 1024x1024 map in the flash frame (or that nothing did). Suspects: a shadow map
the game rebuilds on a cut through a texture copy / blit between depth formats or sizes (`copy_t2t` falls back to a
blit, which Vulkan does not allow between different depth formats), or one never rendered in our replay.

Third probe run: both frames' shadow maps are written by a depth pass with **1 draw** right before the colour pass 30
(1024x1024 in the flash frame, 512x512 in the good one). One draw is not a shadow render of the scene: it is a
full-screen pass writing depth - most likely the game copying a cached shadow map into the one it samples. Next run
samples depth values (8x8 points) of every depth pass and lists what that 1-draw pass reads, to see whether its source
is empty or never written on a cut.

Fourth probe run (3 flashes): **the shadow map is not it.** In two flashes the good frame read the same 1024x1024
map, in the third both read 512x512, and the depth values match (0..1, mean ~0.27). The flash and good passes have the
same shader, the same textures, the same depth inputs - yet one outputs 0 and the other ~0.75. What is left is the
non-texture input: uniform / storage buffers (shadow matrices, cascade splits, light parameters). Next run reads the
first 256 bytes of every uniform / storage buffer each probed pass of up to 8 draws binds (on the CPU at record time; a
first version copied them on the GPU after each pass and the game stalled 30-40 s per frame) and the report prints the ones that differ, with the floats. NaN or zero matrices on the flash frame would explain it.

Fifth probe run (buffers): the uniforms of the shadow-mask pass (`679c5257f9a55f7f`) match between flash and good
frame up to camera jitter (shadow / view matrices fine, no NaN). The one real difference is **fragment storage buffer 4**
(0x3a4e0f000 in that session): at byte 72 the flash frame has 0 where the good frame has 2 (read as float bits: small
integers). That buffer is GPU-written, so this is likely the per-tile feedback the Mac describes (nvn_mtl.m
`msl_stores_through`: the lighting quads store a per-tile word the next frame reads; material shaders store a per-tile
class-group flag). Caveat: the probe reads it on the CPU at record time, so it shows what earlier frames' GPU work left
in memory, not exactly what the GPU sees. Next: read the shader (`679c5257f9a55f7f`, from the build's shader seed) to see
how storage 4 gates the output; find which passes write storage 4 and whether a camera cut clears or skips them.

The shader (`679c5257f9a55f7f`, decompiled with the build's spirv-cross): 17 textures, 5 uniform banks, one storage
buffer `fp_s0` (binding 18) that it both reads (word 100) and writes (`fp_s0[...] = 1` at a per-tile index when a
condition holds: the per-tile feedback), 6 colour outputs. **Output 0 is (1.0, y, 1.0, w): x and z are constant 1**, so a
mean of 0 on colour 0 means the draws wrote no fragments at all there - the pass is not computing a bad value, its
fragments are rejected (depth / stencil test against a class-depth or stencil the marker writes from last frame's
per-tile flags, scissor, or write mask). The probe now logs each differing pass's depth / stencil / colour-mask state
and the depth passes right before it (its own depth attachment too, with depth samples), and up to 16 inputs.

Sixth probe run (state): the flash pass has a **640x360 DEPTH16 attachment that holds 0.5 everywhere** (written by a
1-draw depth pass just before - the marker), and its draws use **depth test EQUAL (NVN func 3), no depth write, no
stencil, full colour mask, no blend - identical in the flash and the good frame**, with the same 0.5 depth. So the 3
draws' fragments arrive with a depth that is not exactly 0.5 on the flash frame, and with 0.5 on good frames. EQUAL on
DEPTH16 needs the bit-exact same depth from rasterization as from the marker. What sets the draws' depth: their vertex
positions (vertex shader, vertex data, vs uniform 1 - camera matrices that differ by jitter), the viewport depth range,
depth bias, depth clamp. Next run logs per draw: vertex shader key, primitive / count / instances, viewport, depth
range, bias, clamp, cull, scissor and the first vertex's data.

Seventh probe run (draws): the 3 draws are also identical in both frames (vertex shaders 345bb4abe6b6bbf4 /
97c2cb79ae8c75a5, 6-vertex quads, viewport 640x360, depth range 0..1, bias off, no clamp). Everything the game controls
matches. **Hypothesis (being tested): a DEPTH16 rounding tie.** 0.5 * 65535 = 32767.5 is exactly halfway between two
steps, so whether the marker's 0.5 and the shading draws' 0.5 land on the same step depends on an ulp of
interpolation error, which on Adreno can change sign with the jittered vertex positions; on a cut frame they land
on different steps and EQUAL rejects everything. The fix `d16tie` (on by default, `TOTK_VK_OFF=d16tie` to compare)
adds a fifth of a depth step of constant bias to every draw into a DEPTH16 target and nudges DEPTH16 clears that sit
on a tie. The probe now prints DEPTH16 samples as raw steps (32767 vs 32768 would confirm it).

Eighth run: the d16tie fix did not help, and the probe shows the marker stores step **32768** in both frames - no
tie. Ruled out. The shading draws' vertex shaders (345bb4abe6b6bbf4, 97c2cb79ae8c75a5) draw a full-screen quad from
gl_VertexID, and its depth is `k + (1 - k) * nibble_swap(class) / 255` with `class = max(trunc(vp_c11[25].x), 0)`
(uniform slot 8, byte 400) and `k = vp_c4[147].z` (slot 1, byte 2360) - both beyond the 256 bytes the buffer probe
reads. So on a flash frame these draws are given a different class (or k) than the marker: the uniform words for the
class are what to check. The probe now logs, per draw, slot 8 [25], slot 1 [147] and slot 1 [7] as the GPU sees them
(snapshot / memory plus this submission's inline updates, `vk_ub_peek`), for the shading draws and the marker.

Ninth run: the class words are identical in flash and good frames too (draws with classes 16, 24, 48, k = 0.5;
the marker's vs is 9a2cc1e8547cf9f1). And by the vertex shader's formula those classes give depths 0.502 / 0.753 /
0.506 (D16 steps ~32896 / ~49344 / ~33153), while the stored depth at all 64 probe points is 32768 (0.5) in both
frames - so EQUAL should fail at those points in the good frame as well, yet the good frame's colour there is ~0.75.
So the good frame's 0.75 may not come from this pass's fragments at all, but from what the colour target already held
(the pass loads it). Hypothesis: on a camera cut the target (or the one feeding it) is a fresh image with no content,
where on hardware the memory still holds the previous content. Next run logs, per differing pass, whether colour 0 was
cleared or loaded, its image handle and depth image handle, whether the same images appear in the other frame, and
where they were last drawn earlier in the frame.

Tenth run: **the first real difference.** In the flash frame the shadow-mask pass's colour target is *cleared at the
pass start*; in the good frame it is *loaded* (it then keeps the ~0.75 content that pass 51/54 drew into it earlier in
the same frame). The passes after it (the blur chain, 59 / 61 / 65) show the same: cleared in the flash, loaded in the
good frame. So the shading draws never contribute at the probe points on either frame (EQUAL against the 0.5 marker
fails there); the mask lives on carried content, and on a cut frame something clears it. Either the game issues the
clear only on cut frames (then on hardware the shading draws must fill the mask, and our marker / EQUAL path is what is
really broken: the marker writes 0.5 everywhere - per-tile class feedback not working?), or the game issues it every
frame and we skip it on normal frames (`clear_color` returns when `S.render_disabled` - conditional rendering, whose
counters we fake as "all visible"). Next run counts, per pass, the colour clears issued / skipped / done inside a pass
since the previous pass, the last clear colour, SetRenderTargets and conditional-render ops, and the render-enable state.

Eleventh run: **found the switch.** Between the passes, the game issues its colour clears of the mask under
conditional rendering (SetRenderEnableConditional, comparing two counter reports). Flash frame: 7 conditionals, 0
disabled, 7 clears done. Good frame: 6 conditionals, all 6 disabled, 6 clears skipped. Our conditionals were evaluated
at record time from memory, but a report's memory is only written when its submission completes - so they compared
whatever older, already completed reports had left there, and the outcome depended on timing (a camera cut changes
the sequence). Fix `condorder` (on; `TOTK_VK_OFF=condorder` for the old behaviour): each report's value is fixed when
it is recorded and kept per address; conditionals read those first (GPU order, as nvn_mtl.m `cond_eval` does with its
pending ring). SAMPLES_PASSED became a real cumulative counter (ResetCounter zeroes, each recorded draw adds its
vertices x instances; "everything visible" still, but two reports with no draw between them are now equal). Desktop
test: both cases pass with the fix, fail with it off. To verify on the device: the flicker should be gone; if the
picture is instead dark all the time, the conditional decision is now consistently "clear" and the EQUAL shading
path is the next thing to fix. Watch also for objects vanishing (occlusion queries now return per-draw counts).

The probe makes frames slow and the game froze a few seconds after each probe run (cause unknown; the completion
thread now logs "the GPU has not finished submission ..." every 5 s if the GPU hangs).

How the probe works: `TOTK_VK_PASSPROBE=1`. Every pass's colour target is sampled at 8x8; when the flash detector confirms a dark
frame it logs which passes' output differs from the good frame right after it (the same camera angle) (`pass probe, flash frame ...`). Passes are paired by size and format (the frames' pass lists differ: shadow / cube passes take turns). The first pass that
goes dark or NaN is where to look. Multisampled targets are not probed (blitting them is invalid; the first version did, and the game froze after a few reports - maybe that, unconfirmed). Ideas for what a camera cut changes: temporal history reset (TAA / exposure /
reprojection weights; NaN * 0 is NaN), occlusion results of the old view (we report every query as visible), the
per-tile material-class feedback the next frame reads (nvn_mtl.m `msl_stores_through`).

Other known gaps (may or may not matter here):
- occlusion queries: all visible; timestamps: the CPU clock at completion.

## Open: grass glitch under async replay; shadow glitches

Two picture bugs observed with `TOTK_ASYNC_REPLAY=1` (first run, 2026-10-22):

- **A patch of grass disappeared while the user sat still, and textures/polygons shifted a bit.** Likely a staleness
  race the async replay opens: the game may rewrite per-frame data (terrain bake results, per-tile flags) right after
  a submit that the translation thread is still replaying. The write-back path drains the queue before it copies, but
  other consumers (uploads, texture copies) do not. Check whether the patch pops back when the camera moves (fresh
  upload) or stays gone until the tile is rebuilt.
- **"Shadows are acting weird"** (user, no screenshot yet): capture one with `TOTK_VK_SHOT=N` or a photo plus the
  frame number. The black-squares fix below touches the sun shadow mask, so retest after it lands before chasing this
  separately.

### Loading-screen map black (2026-10-22, after the async pipelines)

The map on the loading screen renders fully black; it appeared with the pipeline-worker build. The bias run proved
the seed was old (`shader seed: 0 gather-biased`), so the bias is not it, and no build failed (`0 failed` in every
hitch line): the likely mechanism is plain skips - the map's draws fall in the skip window while the worker chews
through an area's pipeline batch (60-90 builds, seconds, right at a load). Fix: a key skipped past PIPE_WAIT_MAX
builds synchronously, so the screen fills within ~90 frames of loading at worst. If it is still black, check
`graphics pipeline (vs ... fs ...): <result>` lines (a failed build is cached as a permanent skip).

### Async replay was never on (2026-10-22, found via the frame arithmetic)

The steady-state numbers settled it: median frame 33.5 ms = recording 17.5 + GPU 15.8 exactly. With the translation
thread those two overlap, so a frame that equals their sum means submits replay inside the game's submit call:
`TOTK_ASYNC_REPLAY` was off in every device run so far, including the ones meant to test it (the env line had not
survived in totk.env). The new `game waits` counter showed the cost: at an area load the game's threads paid 29.9 s
of fence waits inside a 2.1 s window (1810 submits, ~8 ms of completion latency each, all serialized). Now the app
defaults it on (frontend_android.c env_default; totk.env's `TOTK_ASYNC_REPLAY=0` still wins). The grass / texture
shift happened in these sync-mode runs, so it is not an async-replay race after all - back to the write-back
suspects when it reproduces.

Fifth run, the translation thread genuinely on (its startup line in the log): the map is back, but the frame STILL
equals recording + GPU (21.4 + 21.0 = 43.7 median). So the game thread is not free-running behind the replay: its
own fence waits serialize it - `game waits` averages 8-10 ms per wait, hundreds per window. The suspect is the
completion path's latency (GPU signals the timeline semaphore, the completion thread wakes, applies the posts,
broadcasts, the waiter wakes). The hitch line now splits each wait into `completion` (driver signal to g_done) and
`wake` (completion to the waiter running): the first points at the driver, the second at scheduling.

### Review of the pipeline worker (2026-10-22 night, Claude)

Three problems in the worker build, fixed:
- **A race that bakes wrong blending into cached pipelines.** `pipe_build` decided blend enable from `R.dclass[i]`,
  the *recording thread's current pass*, not the key. On the worker that is whatever pass the recording thread is in
  at that moment, and the result is cached for good under the key. `dclass` is now part of the pipe key.
- **Draws skipped for up to 90 misses** (and the counter counted draws, not submissions, despite its comment). Terrain,
  shadow-caster and material draws missing for seconds are exactly the new symptoms: tiles without their sun term
  (the black squares, per the forensics below: G = 0 in ser 4120 where the terrain draw did not land) and shadow maps
  losing casters ("shadow map deterioration"). Now a key is skipped for at most 2 submissions (TOTK_PIPE_WAIT_MAX),
  then built synchronously.
- **No pipeline cache on disk**, so every start rebuilt every pipeline. The cache is now loaded at start and saved
  every 300 frames on a background thread (pipeline_cache.bin in the game folder; TOTK_VK_PCACHE=<path>,
  TOTK_VK_OFF=pcache). After an area has been seen once its pipelines come from the cache, so the synchronous
  fallback costs little.
- The desktop tests failed with the worker on (draws skipped in single-submission checks); they set
  TOTK_PIPE_WAIT_MAX=0 now and pass.

Also: the gather bias defaults on in the translation service, which the Mac build shares (build_app.py ->
prepare_shader_seed.py -> totk_xlate --service). build_app.py now passes TOTK_GATHER_BIAS=0, so the Mac seed is
unchanged when its prebuilt translator is next rebuilt.

### Degradation while standing still: the idle-image sweep (2026-10-22 night, fixed)

The real cause. Screenshots over ~10 in-game minutes at one spot: the mountains' moss and the hills' grass detail turn
into blotchy grey rock, patch by patch - terrain surface textures losing their content. vk_res_sweep dropped shared
images nobody held (first after 600 idle submissions, then "superseded by a newer image over overlapping memory");
such an image is the only copy of what the GPU rendered there, and the game samples that storage again later through
new texture objects. `TOTK_VK_OFF=sweep` stopped the degradation on the Thor. The sweep is off by default now: shared
images go only with their pool (`TOTK_VK_SWEEP=1` turns it back on). Watch the `memory:` line's image blocks over long
sessions for growth. (The earlier "fixed by turning the pipeline worker off" was a lucky spot: the degradation is
spot-dependent.)

### Degradation while standing still: the pipeline worker's skips (2026-10-22 night, NOT the cause - see above)

`TOTK_VK_OFF=pipeasync` (no worker, no skipped draws) fixed the slow degradation and the black loading-screen map on
the Thor; hitching felt no worse (the on-disk pipeline cache), frame rate maybe slightly lower. The lighting keeps
itself alive through per-tile flags that each frame's draws leave for the next; a tile whose draws were skipped for
even one frame drops out and stays out until new geometry flags it again, so a still camera accumulates holes.
Synchronous building is the default now (`TOTK_PIPE_ASYNC=1` brings the worker back). The idle-image sweep change
below is kept (it is right in itself) but was not this bug.

### Things vanishing after 20-30 s of standing still (2026-10-22 night, fix to test)

User report: after about 20-30 s without moving, objects start popping out of view. Suspect: vk_res_sweep dropped a
shared image after 600 idle submissions (~20 s at 30 fps). Such an image is the only copy of what the GPU rendered
into that storage; when the game later makes a new texture object over the same memory and samples it, the new image
is empty (GPU-only) or re-uploaded from stale pool bytes (CPU pools). Now an idle image is dropped only when a newer
image overlaps its memory, or after TOTK_VK_SWEEP_IDLE submissions (default 36000; 0 = never). Watch the `memory:`
line's image blocks for growth.

### Grey 64 px squares floating in the air (2026-10-22 night, FIXED 2026-10-23)

Screenshot: a flat, semi-transparent grey square, screen-aligned, ~96 px at 1080p (= one 64 px tile at the 1280x720
internal resolution), in mid-air over grass. The lighting pass works in 64 px tiles (the marker and the tiled lighting
draws: 240 instances = 20x12 tiles), so one tile got a wrong class / lighting. Next: passdump with
TOTK_VK_PD_SPLIT=d16 while a square is on screen (the per-draw lighting dumps show which draw paints it).

Later findings (2026-10-22/23):
- They are half-res 32 px tiles (640x360 lighting, 240 instances) left unlit (fs 37bb03a9, marker fs 5218c5e5,
  DEPTH16 class depth). They depend on the position and camera angle, stay while still, and flash around while moving.
- Raw .d16 of dump f2470: the class-0 tiles all read 32768, consistent, so the D16 rounding-tie theory is unlikely.
- Not fixed by: the Balemuni Apex V2 driver (slower, squares remain), async replay off.
- Eden had the same squares on the Thor until PR 4324 (commit c3bdfd14). Its video_core diff against the current
  Eden tree is mostly refactoring. The one rendering change is `Sampler::VariantKey::drop_depth_comparison`
  (vk_texture_cache.cpp): when the sampler has depth compare and the view's format lacks
  VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT, the sampler is created with compareEnable off.
- Fix written (vk_draw.c `cmp_fix`, vk_res.c `vk_samp_by_id_nocmp`, vk_shader.c `spirv_mark_shadow`). Maxwell
  applies a sampler's compare only in TEX.DC, while Vulkan applies it, undefined, to every sample through that
  sampler. So a compare sampler is bound without compare when:
  1. the shader samples the binding plainly, i.e. the SPIR-V image is not Depth = 1. Lavapipe shows this is real:
     the test's compare-NEVER sampler on a plain RGBA8 texture reads 255 128 instead of the texels;
  2. Eden's rule applies: the view's format has no depth-compare support.

  `TOTK_VK_OFF=cmpfix` restores the old binding. The log prints the first few cases ("compare sampler on ...") and
  the counts every 300 frames ("compare samplers on plainly sampled textures N, on formats without depth compare N").
- 2026-10-23 result: the squares remain with the cmpfix, `TU_DEBUG=sysmem` and `TU_DEBUG=nolrz` (user, all tests).
- Research (2026-10-23):
  - Eden PR 4565 ("Fix TOTK dark squares", open, Oct 21) only adds QUALCOMM_PROPRIETARY / MESA_TURNIP to
    `need_gather_subpixel_offset` (vk_pipeline_cache.cpp, +3 -1). Its nudge is 2^-9 / size, identical to our
    Ryujinx GatherBiasPrecision with 8 bits, so we already have it. PR 4324's tree does not enable it for Qualcomm
    either, so that build's fix was not the gather.
  - Eden PR 4520 (merged Oct 11, branch `totk-black-squeres-driver`) reverted PR 4320, which had masqueraded Eden's
    VkApplicationInfo as PUBG Mobile + UE, because with it Balemuni's driver "stopped fixing the black squares in
    TotK". So **the fix Eden users see lives in the driver and is keyed on app detection**.
  - The Balemuni Aurora README states the TOTK fix: driconf `tu_dont_care_as_load = true` and
    `tu_allow_oob_indirect_ubo_loads = true`, plus emulator rules for border colour, OOB indirect UBOs and
    "D3D-compatible texture coordinate rounding". Our app is not recognised, so none of them applied, which explains
    why the V2 driver changed nothing.
  - Our passes never use LOAD/STORE_OP_DONT_CARE, so `tu_dont_care_as_load` is a no-op for us. The candidate is
    `tu_allow_oob_indirect_ubo_loads`: it stops Turnip from lowering indirectly indexed UBO loads to constants, where an
    out-of-range index reads something else. It is independent of GMEM, which fits sysmem and nolrz not helping.
  - Mesa lets an environment variable of the same name override any driconf option, and totk.env is applied before
    vkCreateInstance, so both can be tried from totk.env without a rebuild.
  - New: `TOTK_VK_APP_NAME` / `TOTK_VK_ENGINE_NAME` set the VkApplicationInfo names. Eden uses "yuzu Emulator" for
    both, which may trigger the driver's whole emulator rule set. Rules matched on the executable (Android: the
    package name) cannot be reached this way.
- **FIXED (user-confirmed 2026-10-23, 00:42): `TOTK_VK_APP_NAME=yuzu Emulator` + `TOTK_VK_ENGINE_NAME=yuzu Emulator`**
  (test 2) removes the squares on the old Apex driver (not only V2; user, 2026-10-23): the driver's emulator rules
  now apply to us. Test 1 (`tu_allow_oob_indirect_ubo_loads` + `tu_dont_care_as_load` via totk.env) did NOT fix
  it, so the fix is another rule keyed on the name, or those options are not read from the environment there.
- **Root cause found (2026-10-23): `tu_use_tex_coord_round_nearest_even_mode=true`.** On the old name with only this
  option set in totk.env, the squares stay gone (user-confirmed). The driver's strings carry the emulator regex
  `eden|citron|yuzu|suyu|sudachi|skyline|strato|winlator|box64|fex|mobox|hangover` (driconf); this is the
  README's "D3D-compatible texture coordinate rounding". Maxwell rounds texture coordinates to nearest-even;
  Adreno's default picks the neighbouring texel at exact half-texel coordinates. That is the same class of bug as
  the gather bias, but for ordinary samples, which the gather bias does not touch. It also confirms that Mesa
  driconf options can be set from the environment on this driver.
- Now set by the app at startup (runtime/frontend_android.c, `env_default`, before the game thread creates the
  Vulkan instance), so any Turnip with the option gets it whatever the name. totk.env `...=false` overrides. The
  "yuzu Emulator" name stays the default too, which brings the driver's whole emulator rule set, as Eden gets. The name is the default since this
  commit (nvn_vk.c, vkCreateInstance); `TOTK_VK_APP_NAME=totk-native` brings back the old name for A/B. Still to
  learn: which driver was installed for the test, whether test 1 (the two tu_* options from totk.env alone) failed,
  and which rule does it (a `strings` dump of the driver's embedded driconf). Once that rule is known it could be set
  directly, so the fix would not depend on a driver that recognises the name.
- If the counts stay 0 or the squares remain: test Turnip with `TU_DEBUG=sysmem` (no GMEM tiling), then
  `TU_DEBUG=nolrz`. The lighting pass samples its own attachment (c7 ser 30187 is both input and output), so a
  feedback loop is the next suspect. The fix there would be VK_EXT_attachment_feedback_loop_layout, or LRZ off for
  those passes.

### Black squares: passdump forensics (2026-10-22 night, f2713)

The first passdump (4 frames: f2713 / f4461 / f4494 / f5813) traced one square (f2713's blotch, ~1/3 of the screen
over sunlit terrain, hard stair-step edges = whole 64 px tiles). The chain, bottom up:

- The final black is painted by the class-shading pass f2713 pass 100 (fs c23ef1f842fcfba5, 240 instances, depth
  test EQUAL write 0 against the D16 class map ser 53008 1280x720, the "fully shadowed" class value 0.5): it just
  obeys the class map.
- The class map is written by pass 99 (fs 233dfb9398efe63b, 240 tile instances, depth ALWAYS write 1, colour mask
  0: the class goes into the DEPTH, c11[25] = 48, 0.5). Its only input is the lighting target ser 4120.
- In ser 4120 (RGBA8) the blotch region has G = 0 while the sunlit grass right of it has G = 253 (raw bytes, the
  PNG of a unorm8 image is unscaled): **the sun term is missing over the whole blotch region**. The classifier
  correctly reads "no sun here" and marks the tiles fully shadowed; the squares are a symptom, not the decision.
- The gather-bias angle is closed for this artifact: every gather fs in the frame is biased (seed scan), the
  cascade masks the shadow pass reads (45164-67, 640x360) show no matching dark patch, and the linearized depth
  (41898, R32F) is smooth there. No fp64 in these shaders either (OpTypeFloat 64 count 0).
- ser 4120's G is written by the deferred terrain passes (34-45, e.g. pass 83: terrain chunks, depth LESSEQUAL
  write 1 into the scene depth 52906 D32F, G-buffer channels with colour mask 0xf0f0f0ff). So the sun term went
  missing because the terrain draw covering the blotch either did not run that submission or its fragments were
  rejected (a stale nearer depth in that rectangle rejects everything: depth test func 4).

Suspects, in order: (1) a draw skipped by the backend in that submission (async pipeline build / shader missing -
the frame line's cumulative `skipped:` counters; passdump's index.txt now ends with the counters at frame end, so
one capture shows both the artifact and the skip state); (2) a stale depth rectangle rejecting the terrain draws
(func 4 LESSEQUAL against 52906); (3) the game's own chunk streaming. Pass numbering is NOT stable across frames
(the same pass id is a different pass in f4461), so only same-frame comparisons are meaningful.

Next probe (device): play until a square shows, then immediately
`adb shell "echo 'passdump 1' >> /sdcard/Android/data/dev.kitcatkode.totk/files/input.txt"`
and pull: the index's `skips at frame end` line vs the numbers from the startup log tells whether that frame
skipped draws; and the terrain pass's draw list shows whether a chunk is missing.

### Black squares: the gather bias (2026-10-22, fix written, to confirm)

Occasional black squares on lit terrain. Screenshots (2026-10-22): a semi-transparent grey square, screen-aligned,
about 64 game pixels across, floating in the air ABOVE the terrain (over a crater), plus blotchy/smeared ground
shadows up close and patches of grass blades gone pale. The Ryujinx_Neo fork fixed the same artefact
(github.com/Sunshinin11/Ryujinx_Neo, commit 734fe55f8, docs/TOTK-turnip-gather-bias.md): TOTK's quarter-resolution
sun shadow mask (guest shader e751605e) reconstructs each pixel's position with textureGather (TLD4) at coordinates
that fall essentially on a texel centre, and the choice of 2x2 gather block there is implementation-defined
(subTexelPrecisionBits). NVIDIA rounds to 1/256 texel first; Adreno does not, so a tiny float difference selects the
neighbouring texel, the reconstructed position shifts on sloped terrain, dRef rises 100-150 D16 steps, and the game's
64 px tile classifier marks those tiles "fully in shadow": the black squares. Same save shows nothing on a desktop
GPU. Our squares floating above the ground is a twist: the marked tile is also drawn at a wrong class depth, so the
DEPTH16 class-marker path (d16tie) is in scope too, not only the gather bias.

Fix: Ryujinx's GatherBiasPrecision transform (vendor/ryujinx TexturePass) adds half a subtexel,
1 / (size * 2^(bits+1)), to every gather coordinate from a TextureQuerySize; it was gated off for our host
(`QueryHostGatherBiasPrecision` defaulted to 0). Now `totk_xlate` answers 8 (Vulkan SubTexelPrecisionBits) when the
job says `gather_bias`, and the translation service stamps `gather_bias` on every job from `TOTK_GATHER_BIAS`
(default on, `=0` for A/B). Both the bulk seed build and the device fill flow go through that one service, so every
translation of a build agrees. **The seed must be rebuilt**: a full `build_android.py ... --apk --shaders`
(a `--runtime-only` build changes nothing here - the bias is in the translated SPIR-V).

First two runs with the fix: squares still present, and the marker read `shader seed: 0 gather-biased shaders
loaded so far` both times - the seed on the device was never rebuilt. `--runtime-only --apk` does not remake or
push the seed: the seed is remade by `--shaders` (also on `--runtime-only --shaders`) and reaches the device through
the push script's `shaders` step (`sh push.sh shaders` from the build output folder; `adb push --sync shaders`). The
marker line (0 = old seed) is the check that it landed. If N > 0 and the squares remain, the bias is not the (only)
cause here: next step is a passdump of a square frame.

Third run with the marker: still 0. The build pipeline was fixed first (the prebuilt translator binary; a stale
binary made the seed reuse kick in and nothing retranslated at all; build_android.py now hashes the translator's
sources and rebuilds it itself, running the checkout's publish_native.sh since the build tree has no vendor).
After the binary was rebuilt and the seed retranslated (20,961 SPIR-Vs, fresh), the marker was STILL 0.

**Resolution of the whole marker saga (2026-10-22 night): the bias had been working all along - the MARKER was
wrong.** Two compounding mistakes, both mine:

1. My opcode constants were from memory, not the vendored SPIR-V tables. OpImageGather is **96** (not 105) and
   OpImageQuerySizeLod / OpImageQuerySize are **103 / 104** (not 117 / 118). Opcode 105 is OpImageQueryLod, which
   every TOTK shader that samples with LOD has - so "1282 files contain gathers" was really "1282 contain LOD
   queries", and the marker's 117/118 never matched the real bias output.
2. With the right opcodes: 657 seed shaders contain real gathers; **1119 of 20961 seed SPIR-Vs carry the bias**
   (the query ops), i.e. the seed on the device is already biased, and a live retranslation of a gather shader
   through the fresh binary logs "bias inserted" and gains the 103/104 ops. The earlier "the transform never sees
   a gather" claim came from my synthetic test jobs (empty tex answers produce one logger note per handle, which
   blew through the 300-note cap and dropped the later logs) plus the wrong opcodes. The transform sees gathers
   fine; no bindless-flag patch is needed.

So the squares were seen WITH the bias active - the gather bias alone is not the (whole) fix, consistent with the
floating-squares observation: the DEPTH16 class-marker path (d16tie / class depth) is next, via a passdump of a
square frame. vk_shader.c's marker now counts 103/104 (commit of 2026-10-22, late), so the startup line finally
reports ~1000 on the next runtime-only build.

Also in the third run the reason the earlier marker kept reading 0 was the **prebuilt translator
binary** (`prebuilt/linux-x64/totk_xlate`, made by `tools/totk_xlate/publish_native.sh`); the seed's reuse
check keys on that binary's hash, so the old binary did not just translate without the bias - it made the seed
*reuse* kick in and nothing was retranslated at all. build_android.py now hashes the translator's sources
(totk_xlate_inputs) and, when the binary predates them, rebuilds it itself (publish_native.sh, needs dotnet + clang)
or fails with the command. After that the binary hash changes, so the next `--shaders` build retranslates
everything (~1 h).

### The Depths terrain as spikes (2026-10-25, FIXED)

The whole Depths ground drew as huge stray triangles (same every run, with and without `TOTK_SYNC_EARLY`). The terrain
vertex shader (`44a5a5bdde089e90…`) has no vertex inputs: it builds the grid from `gl_VertexIndex` and reads heights
from a storage buffer whose address is in uniform slot 3 (c6) at offset 0. The game rewrites that slot inline between
its terrain draws, one storage address per patch.

- Cause: `bind_stage` (and `draw_touch`) read the 8-byte pointer from guest memory with `memcpy`. Inline uniform updates
  are logged and laid over memory per draw; memory already holds the *last* update of the submission, so each patch
  was bound to another patch's heights. Fix: read it with `vk_ub_peek`, the view the draw's uniforms give the GPU.
- How it was found: `TOTK_VK_DRAWCAP` captured four terrain draws; the seed's `_b.metal` compiled as C++ (a small
  Metal shim) ran on the PC over the captured inputs and gave clean ground, so the translation and the data were fine.
  `IR3_SHADER_DEBUG=vs,disasm` with `MESA_SHADER_CACHE_DISABLE=true` and `TOTK_VK_OFF=pcache` (Mesa logs to logcat,
  stream it with `adb logcat -v raw -s MESA:I`) showed Turnip's code was right too. The capture then compared guest
  memory with the GPU view of each uniform slot: slot 3 differed on every draw.
- Ruled out on the way: the 0x3b/0x3c textures and their rb swap, block height, the 128 MiB storage range, Maxwell
  BFE/BFI corner cases (translator patch 0006: more correct, not committed yet), Turnip reading the buffer through `isam`.
- Lesson: anything the backend reads *out of* a uniform block at record time (pointers, handles) must go through
  `vk_ub_peek`, never memory.

## Diagnostics

| Setting (`totk.env`) | What it does |
| --- | --- |
| `TOTK_VK_OFF=a,b` | turn off backend features: `rename,ds,dyn,block,ts,span,snap,mirshare,present_wait,compalias,passbarrier,condorder,writeback,texcache,d16tie,pipeasync,pcache` or `all` |
| `TOTK_VK_D16TIE=0` | turn off the DEPTH16 nudge (rasterized: depth bias; fragment-shader written: a patched specialization constant; a fifth of a step off the rounding tie the class-depth EQUAL tests sit on). On by default: without it water renders black |
| `TOTK_VK_SYNC=1` | wait for every submission |
| `TOTK_VK_DRAWCAP=<vs key prefix>[:<skip frames>]` | write the inputs of the first 4 indexed draws of that vertex shader (draw parameters, indices, uniform slots as the GPU sees them, guest memory where it differs, storage buffers) to `drawcap/` in the game folder, to run the shader on the PC |
| `TOTK_VK_SERIAL=1` | end the pass after every draw |
| `TOTK_VK_FMT_SPLIT=1` | no storage sharing across format classes at all |
| `passdump <n>` (a line appended to `TOTK_INPUT_FILE`) | the n frames after the next: every pass's colour targets and depth / stencil as PNG (384x216 samples, float formats scaled to their own range, NaN magenta) in `passdump/f<frame>/`, with `index.txt` (targets: serial, size, NVN format, level / layer; draws; first fragment shader; value ranges). Pull with `adb pull /sdcard/Android/data/dev.kitcatkode.totk/files/passdump` |
| `TOTK_VK_PD_SPLIT=<fs prefix>,...` or `d16` | with passdump: end the pass after every matching draw (d16: every draw of passes with colour targets and a DEPTH16 depth), so each draw's output is dumped |
| `TOTK_ASYNC_REPLAY=0` | replay submits and presents inside the game's submit call (the old behaviour; the app defaults the translation thread on) |
| `TOTK_GATHER_BIAS=0` (build time) | no half-subtexel bias on gather coordinates (the black-squares fix); the seed must then be rebuilt without it |
| `TOTK_VK_PASSPROBE=1` | per-pass colour probe, reported on each detected flash (slow); also compares the uniform / storage buffers (read on the CPU) of passes with up to 8 draws |
| `TOTK_VK_FRAMES_AHEAD=1` | one frame in flight |
| `TOTK_UNCAPPED_HZ=30` | frame cap (the app's default; 60 / 120 raise it) |
| `TOTK_VK_SPLIT_MS=2` | submit what is recorded before a new pass after this many ms of recording (default 2, was 4; 0 = one submission per game submit; -1 = before every pass, a test mode; on the Thor -1 loses the device when a save loads) |
| `TOTK_VK_SPLIT_PASS=64` | also split a pass once it has drawn this many draws and its submission is due (end, submit, reopen with load); 0 = only between passes. The G-buffer is one ~2,200-draw pass: without it the GPU waited for all of it to be recorded |
| `TOTK_VK_OFF=invalskip` | a pool invalidate drains the translation queue even when no GPU rendering lies in its range (before 2026-10-25). ~93 % of invalidates find none; each drain blocked a recording thread ~10 ms on the replay, ~6 ms before the frame's submit. Skipping them: 40.8/39.9 -> 45.2/44.8 fps (runs/1026_inv_*), write-backs unchanged |
| `TOTK_SYNC_EARLY=1` / `=2` | a fence waits for the previous frame's GPU only (one frame more in flight). `=1` turns itself off while frame generation is on (followed live), `=2` keeps it. 2026-10-26 (runs/1026_lat_*; the `latency, last 300 real frames` line: the game's present call to its real picture posted): no frame generation 43.7 fps / 41 ms with it, 37.5 / 26 ms without, so it stays; 2x 69 -> 47 ms and 3x 112 -> 62 ms without it at the same fps. Input to screen is about that plus one real frame plus the compositor |
| `TOTK_SYNC_AHEAD=1` | experiment, off by default (needs TOTK_ASYNC_REPLAY and TOTK_SYNC_EARLY): a fence wait needs only the previous frame's translation and GPU, so the game runs one frame ahead of the replay; retired command copies are freed once the replays queued before are done. 2026-10-25: no gain (46.0/45.8 vs 46.0/44.7 fps, runs/1026_ah_*): the replay is then busy all the time and ~2 ms slower per frame. Hazards not handled: counter reports and texture re-uploads may see the next frame's data |
| `TOTK_VK_OFF=barriersmart` | end the pass at every NVN barrier inside it (before 2026-10-26). By default a barrier waits for the next draw, which splits the pass only if it samples one of the pass's attachments or a draw of the pass wrote storage since it opened (the material tile flags); otherwise the barrier is dropped. Barrier splits ~141 -> ~21 a frame, GPU 19.6 -> 18.75 ms, fps 45.4 -> 46.2 (runs/1026_bs_*), captures clean. Forced GMEM (TU_DEBUG=gmem) is no lever here: 29 ms GPU even without splits |
| `TOTK_SHADOW_RES=1024` | the cascade shadow maps' size (the game's 720; runtime/game_patches_v143.c patch_shadow_res, applied at load; bottom screen: Shadow resolution). The shadow passes are geometry-bound on the Thor: 512 costs the same GPU time as 720, 1024 ~0.2 ms more a frame (sharper shadows), 1536 ~2.3 ms more (-4 fps) (runs/1026_sh*) |
| `TOTK_THP=1` (or `heap`, `code`) | experiment: transparent huge pages for the guest heap (madvise on the reservation) and the game's modules (mapped 2 MiB aligned). Needs the kernel's THP at madvise / advise (thor-tools `thp` tune profile). No fps change at Lookout Landing (PERF_PLAN.md) |
| `TOTK_VK_FENCES=0` | submission completion through the timeline semaphore again (default: a fence per submission; the timeline blocked every vkQueueSubmit2 on Turnip/kgsl) |
| `TOTK_VK_OFF=ubtrim` | bind uniform blocks whole (default: only the bytes a shader reads at constant indices) |
| `TOTK_VK_WB_AGE=3000` | submissions after which a write-back record whose image is an orphan is dropped (0 = never); records of live or shared images never age (the split terrain bug) |
| `TOTK_VK_BLACK=0` | turn off the flash detector (on by default; logs `dark flash at frame`, saves `shots/flash_*.png`; misses some) |
| `TOTK_VK_SHOT=N` | save every N-th frame |
| `TOTK_INPUT_FILE=<path>` | diagnostic commands, one per line, read as they are appended; `threads` dumps every guest thread's wait state and return chain to the log (`[totk-dump]`): use it on a hang. On the app: `/sdcard/Android/data/dev.kitcatkode.totk/files/input.txt` |

Log lines worth grepping: `dark flash`, `pass probe`, `depth target sizes`, `textures:` (sharing counters),
`frame N:` (fps, draws, passes).

## Build gotchas

- Run builds inside the dev distrobox; builds outside it used stale binaries more than once.
- `install_app.sh from-device` copies from `/data/local/tmp/totk` (the old adb-shell layout); use `data` to copy
  from the computer. The app needs exactly one version folder under `rewritten/`.
- adb cannot create folders under `Android/data` on Android 13; `install_app.sh data` makes them first.
- After installing, an old instance can hold the game-slot lock: clear the app from recents.

### Frame generation's cost at Lookout Landing (2026-10-23, 180 s with pans, 30 cap, x2)

| setting | real fps (thirds) | pictures p50 |
| --- | --- | --- |
| off (40 cap, for reference) | 28.2 / 30.6 / 31.2 | - |
| quality, flow 1.0 | 23.8 / 24.3 / 24.8 | 20.3 ms |
| quality, flow 0.5 | 27.6 / 29.5 / 29.9 | 16.7 ms |
| performance, flow 1.0 | 26.9 / 28.0 / 29.7 | 16.6 ms |
| quality, flow 1.0, 612p | 23.9 / 24.6 / 26.0 | 19.3 ms |

Frame generation costs the replay CPU too (translate 18.7 -> 22.6 ms at full flow), so a lower render resolution
does not buy it back; a half flow scale does (the second screen's "Frame generation flow scale" row, live).
