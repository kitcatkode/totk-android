# Performance plan after "playable" (2026-10-23 evening)

The question was whether the frame is CPU- or GPU-bound. The answer from the measurements in DEBUGGING.md is neither:
the CPU and GPU take turns, and the frame time comes from how they depend on each other. Work through this file in
order. Every step is a devrun A/B (Lookout Landing save, 40 cap, 180 s with the camera-pan script, two runs per side)
read through `devrun.py summary`.

## Status (2026-10-26)

Lookout Landing, 60 cap, camera pans: about 41 fps on 2026-10-25, about 46 now. What was done and what it showed:
- **The replay gates the frame through the fence.** hle_nvn.c fence_wait waits for the translation of submit k
  (g_xsub_done), so the game's SyncWait returns only after that frame's replay. Frame = submit call (~1.5 ms) +
  replay (~16.5 ms CPU) + ~1 ms. Pinning the replay to the prime core is worth ~2.8 ms of frame.
- **Invalidate drains skipped** (bc806c9, `TOTK_VK_OFF=invalskip`): ~93 % of mtl_pool_invalidate calls had no
  write-back in range but still drained the replay (~10 ms each). 40.8 -> 45.2 fps.
- **Replay CPU** (0bac518): snapshot lookups, the uniform rename memo, the RB_SUPPORT cache. 17.3 -> 16.5 ms a frame.
- **Barrier splits** (2e85338, `TOTK_VK_OFF=barriersmart`): a barrier inside a pass splits it only when the next draw
  samples the pass's targets or follows a storage write. Splits 141 -> 21 a frame, GPU -0.8 ms. This is step 4
  without the GMEM half: GMEM is a dead end (TU_DEBUG=gmem 29 ms GPU even unsplit; Turnip's sysmem is faster here).
- **Overlapping the fence** (64cfb89, `TOTK_SYNC_AHEAD=1`, off): the fence waits for the previous frame only. Nets
  ~0: the replay becomes 100 % busy and ~2 ms slower when it overlaps the game.
- **Shadow maps** (5a40e0f, `TOTK_SHADOW_RES`): geometry-bound, 512 costs what 720 does. Quality option only.
- **Occlusion queries**: ~32 a frame gating ~10-20 small draws; all of them off saves at most ~0.9 ms of GPU. Real
  queries are not worth the GPU-side conditional rendering they would need.
- Ruled out: GPU clock pin (already at 680 MHz under load), THP for guest memory (c461ca0, off: fewer TLB walks, no
  fps), translating at nvnCommandBufferEndRecording (buffers end <3 ms before the submit).

The GPU (~18.7 ms busy of ~21.7) and the replay are now co-limits. Left: render resolution (540p ~49.5 fps), the
replay's hot spots (bind_stage, vk_ub_rename, texture lookups, driver.so ~20 %), parallel recording (step 5).

## Where the frame goes (Lookout Landing, ~31 fps, from "What sets the frame time")

| | ms per frame |
| --- | --- |
| replay (translate thread recording frame N) | 22.8 |
| GPU busy | 24.7, starting 3.1 ms into the replay, with ~6 ms of gaps where it waits for the replay |
| GPU still running after the replay ends | 11.1 (the late passes: lighting, effects, post, composite; cheap to record, expensive to draw) |
| GPU done -> the game's next submit | ~1.7 |
| frame | ~36-38 |

The game submits frame N+1 only after a SyncWait on frame N's fence returns. Here SyncWait means "translated AND GPU
done" (hle_nvn.c `fence_wait`). So **frame = replay + GPU tail + ~1.7 ms**, with no overlap between frames:
- a faster replay stops paying off once the GPU stops waiting on it: an infinitely fast replay would still give ~29 ms;
- GPU savings in the tail (the late passes) come off the frame 1:1. Savings earlier in the frame mostly turn into
  longer gaps, which is why pinning the GPU clock to 680 MHz took GPU time from 21.7 to 20.5 ms but fps only from
  31.0 to 31.1;
- letting frame N+1's replay overlap frame N's GPU tail would bring the frame close to max(replay, GPU), about
  25-27 ms (37-40 fps), with no shader or recording changes at all.

So the **GPU route is the cleaner one**: its first steps are measurements with no code, and its likely wins land in
the tail. The overlap experiment is the biggest single lever, and step 2 below is cheap to try. The CPU route (the
replay) has a flat profile (bind_stage 37 % inclusive, no single hot spot; push descriptors changed nothing) and an
upper bound of ~29 ms, so it comes last.

## Step 1: GPU measurements (no code, one evening)

1. **UBWC (Adreno framebuffer compression) audit.**
   - The problem: vk_res.c creates every non-depth colour image with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and no
     `VkImageFormatListCreateInfo`. Current Turnip turns UBWC off for mutable-format images unless the format list is
     UBWC-compatible, and old Turnip even made them linear. That covers the G-buffer (6 colour targets at 720p), the
     HDR lighting target and post, all rendered in sysmem mode, which is exactly where bandwidth (and so UBWC) matters
     most.
   - Check:
     - `--env TU_DEBUG=perf` for a short run, then `adb logcat -d | grep -i ubwc | sort | uniq -c | sort -rn | head -40`.
       Turnip's perf_debug lines name each image that lost UBWC and why.
     - A/B `--env TU_DEBUG=noubwc` against the default. If the GPU time does not move, UBWC is effectively off today,
       and step 3 is the fix.
   - Also look at storage usage (vk_res.c:280 adds STORAGE for NVN image-flagged textures), which disables UBWC for
     some formats.
2. **GMEM for the big passes.** One run of `--env TOTK_VK_OFF=passbarrier --env TU_DEBUG=gmem`. It renders wrongly;
   it is a measurement. Forced GMEM with the barrier splits was 41.7 ms; without them, `TOTK_VK_PASSTIME=1` shows
   whether the G-buffer and lighting passes get cheaper in GMEM. If they do, step 4.
3. **Extensions** for later steps: `--env TOTK_VK_EXTLIST=1`. Check for `VK_EXT_graphics_pipeline_library` (pipeline
   hitches, step 6) and `shaderFloat16` in the features.
4. Keep `TOTK_VK_PASSTIME=1` on in every GPU A/B. Its per-pass top list shows where a change landed, which a single
   GPU number hides.

## Step 2: overlap frames (small code, off by default)

`TOTK_SYNC_EARLY=1` in hle_nvn.c `fence_wait`: for token k, wait as now until submit k is translated, then wait for
the GPU fence of the last submit of the previous frame (keep the submit count at each XJ_PRESENT in a small ring)
instead of k's own. The game then submits N+1 while N's tail is still on the GPU. That is one frame more in flight,
the way a GPU that keeps up with its CPU behaves.

Hazards, each with a check:
- **The CPU rewriting memory the GPU of frame N still reads.** Pools are imported host pointers (nvn_vk.c
  POOLS_HOST), so vertex, index and storage data the game rewrites goes straight to the GPU. Uniforms are snapshotted
  at submit and command memory is read by the replay, so both are safe. The game already writes N+1's data while the
  GPU runs N (its logic overlaps the async replay), so its dynamic memory is at least double-buffered. One more frame
  ahead is only safe if it is triple-buffered or writes late. Watch for flicker in particles, UI, grass and
  skinned / cloth meshes in the captures (`--shots 10`).
- **GPU -> CPU results.**
  - Counter reports already have their values fixed at record time (vk_draw.c `vk_counter_samples`). In early mode,
    write them into guest memory at translation time, and write timestamps from the CPU clock in GPU ticks.
  - Check that the pool-invalidate write-back waits for the real completion of the submission that wrote the image,
    not for the game's fence.
- **Capacity.** Two frames in flight on RING 24 (6 submissions a frame with the 2 ms split). Count ring-slot waits; the
  SPLIT_MS=-1 hang was a ring wrap.
- **Latency.** About one more frame of input latency. Frame generation already adds some.

Success means roughly 25-28 ms frames at Lookout Landing with clean captures. If it glitches, keep the flag as a
measurement: it still tells us how much the serialization costs.

**Step 2 result (2026-10-25, committed, on in the user's totk.env, off by default).** Lookout Landing, 40 cap, high
performance mode, two 180 s runs a side: off 28.1 / 28.2 fps (p90 ~40 ms), `TOTK_SYNC_EARLY=1` 33.9 / 33.5 fps (p90
~34 ms), the same log errors, no flicker in play (user). Counter reports are written at translation time in early mode
(nvn_vk.c mtl_sub_counter).

## Step 3: UBWC back on (if step 1 says it is off)

**Done 2026-10-25.** `TU_DEBUG=perf` (Turnip logs under the logcat tag `TU`) showed UBWC off on every colour image,
"mutable formats (fmt list missing)", the 1280x720 G-buffer included. A survey of view formats (3 minutes at Lookout
Landing) found only sRGB / UNORM twins on uncompressed images, so vk_res.c img_create now lists the format and its
twin (no MUTABLE without a twin); any other view format is refused once and makes that NVN format MUTABLE again.
`TOTK_VK_OFF=fmtlist` is the old rule. Lookout Landing, 60 cap, early sync, 180 s, two runs a side: GPU 19.4 / 19.5
-> 18.7 / 18.5 ms a frame, 39.0 / 38.8 -> 39.3 / 39.7 fps, no UBWC losses left, captures identical. The frame (p50
~25 ms) is now above both recording (~15 ms) and GPU (~18.5 ms): the ~6.5 ms between is the next question.
Step 1.1's LRZ side note: Turnip drops LRZ for fragment shaders with storage writes (2,659 of 12,064 in the seed, e.g.
`fp_s0[i] = 1u` visibility flags) and for the 81 that write depth: the game's own behaviour, left alone.

- Record which view formats each image really gets: log the first view of a different format per image.
- Create images with a format list holding the image's own format and its sRGB / UNORM twin (Turnip treats these as
  UBWC-compatible pairs), or with no MUTABLE at all for render targets that never get another view.
- An image that later needs a cross-format view keeps today's path, so recreate it MUTABLE on its first such view, or
  keep a list of known cases.
- Depth already has no MUTABLE.
- Desktop tests with lavapipe catch only correctness; the gain shows only on the Thor.

**What sets the frame after step 3 (2026-10-25 evening).** Lookout Landing, 60 cap, early sync, high performance mode.
- GPU pinned at 680 MHz (thor-tools `gpu-pin-680`): no change. Under load the GPU already runs at 680, and AYN's mode
  floors it at 615.
- When the game ends its command buffers (timeline `end_rec` / `sub_h` / `exec_h`, tl_frames.py): one top-level
  handle per submit, calling ~70 buffers recorded on 3 threads. 45 % of the recorded bytes end < 1 ms before the
  submit, 31 % 1-3 ms, 23 % 3-6 ms. Translating at EndRecording would gain little, so it was not built.
- The GPU sat idle ~4.4 ms a frame 6-10 ms into the replay, while the G-buffer (one pass, ~2,200 draws) was being
  recorded. **Splits inside passes** (vk_draw.c pass_split_due, `TOTK_VK_SPLIT_PASS`, default 64 draws; 0 = off):
  ~3.7 a frame, the game's GPU waits 1.8 -> 0.3 ms, the GPU finishes 7.4 ms after the replay instead of 10.5 (~3 ms
  less latency), captures clean, fps unchanged (41.3 / 40.0 off, 40.8 / 40.8 on). With it the frame (24.6 ms) is
  the game's own submit-to-submit interval, and the next submit arrives ~0.5 ms before the GPU finishes. **The frame
  is set by the game's threads**, not the GPU or the replay. Next: their critical path between two submits.

**Kernel-side checks (2026-10-25 night).** The stock kernel has THP compiled in but set to `never`; deep idle (C4, 1.3-1.9
ms exit) is never entered during play; CPU and GPU clocks sit at their maximum. The game's worker threads run at IPC
0.4-0.8 with 40-44 % memory stalls and ~1.7 page-table walks per 1000 cycles (simpleperf raw-dtlb-walk / raw-itlb-walk;
the replay thread half that). `TOTK_THP=1` with the `thp` tune profile: the code is 83 % on 2 MiB pages, instruction-TLB
walks 4x fewer, data-TLB walks -25-30 %, worker IPC +3-5 %, fps unchanged (off 41.5 / 40.7, on 40.7). Most guest data
cannot be huge: the NVN pools are dma-buf mappings over guest memory (~1.2 GiB in 30,000 mappings, always 4 KiB) and the
alias region is cut into 12,500 mappings by its 16 KiB commit units. Kept as an option, off by default.

## Step 4: GMEM where it pays (if step 1.2 says so)

- About 105 of ~156 passes a frame are split at NVN barriers, and each split stores and reloads every attachment.
- Split only when the barrier can matter to the open pass: the pass samples one of its own attachments, or a
  storage write feeds it.
- Leave other barriers as pipeline barriers outside a render pass (or `VK_EXT_attachment_feedback_loop` where it
  applies), and let Turnip's autotune pick GMEM for the big unsplit passes.

## Step 5: the replay (CPU), in the gaps

Do this after steps 2 to 4, when the replay is the larger side again:
- `g_tex_mu` per texture per draw (vk_tex_get / vk_tex_by_id / view_get; mutex unlock ~7 %): a lock-free read path
  (registration epoch + per-replay copies, like the texture lookup cache).
- **Pre-walk:** when the game ends a command buffer's recording (nvnCommandBufferEndRecording), a worker walks it to
  warm the pipeline front cache, the texture lookups and stage_sig, so the replay at submit finds them hot. First
  check with TOTK_TIMELINE how long before the submit the game ends recording.
- **Parallel recording:** replay chunks (the 2 ms split points) recorded on 2-3 threads into separate command buffers,
  submitted in order. This is the large one, because every chunk needs the bind state as of its start. Worth doing
  only if the frame is replay-bound after step 2.

## Step 6: stable frame times (hitches, not averages)

- First-use pipelines: if Turnip lists `VK_EXT_graphics_pipeline_library`, link a fast unoptimised pipeline from
  libraries at first use and swap in the optimised one from the pipeline worker when it is ready.
- Shader variants missing from the seed: keep running fill_shaders.py after new areas.

## Smaller items

- Frame generation: default `TOTK_LSFG_FLOW=0.5` when TOTK_LSFG is on. Quality at flow 0.5 costs about 1 real fps;
  flow 1.0 costs about 6 ("Frame generation's cost").
- fp16: check whether totk_xlate emits native Float16 for Maxwell's half-precision ops (HADD2 / HMUL2 / HFMA2) or
  emulates them in fp32 with pack/unpack. Adreno runs fp16 at twice the rate. This needs `shaderFloat16` from step 1.3.
- Render resolution is already measured: 44 % fewer pixels saved 13 % of the GPU time. It stays a user setting.
