"""Per-frame critical path from a TOTK_TIMELINE file (Android Vulkan backend).

    python3 android/tl_frames.py tl.tsv

Run the game with TOTK_TIMELINE=<file>:<first frame>:<frames> (e.g. via devrun.py --env), pull the file. The GPU spans
are in the GPU's own clock (Turnip on kgsl has no calibrated timestamps); they are aligned to the host clock by the
smallest GPU-start-after-submit over all submissions. Per replay (one game submission): its length, when the GPU starts
and finishes relative to it, the GPU's busy time, and the game's next submit after the GPU finished."""
import sys, statistics as st
ev = []
for line in open(sys.argv[1]):
    if line.startswith('#'): continue
    t, k, tid, fr, a, b = line.rstrip('\n').split('\t'); ev.append((int(t), k, int(tid), int(fr), int(a), int(b)))
ev.sort()
commit = {e[4]: e[0] for e in ev if e[1] == 'commit'}
gpu = {e[4]: (e[0], e[5]) for e in ev if e[1] == 'gpu'}
seen = {e[4]: e[5] for e in ev if e[1] == 'seen'}
off = min(gpu[c][0] - commit[c] for c in gpu if c in commit)   # GPU domain -> host: the smallest start-after-submit
G = {c: ((s - off), (e - off)) for c, (s, e) in gpu.items()}
subs = [e[0] for e in ev if e[1] == 'sub_in']
waits = [(e[0]) for e in ev if e[1] == 'wait_out']
reps = []; cur = None
for e in ev:
    if e[1] == 'rep_begin': cur = [e[0], None, []]
    elif e[1] == 'commit' and cur: cur[2].append(e[4])
    elif e[1] == 'rep_end' and cur: cur[1] = e[0]; reps.append(cur); cur = None
ms = lambda x: x / 1e6
rows = []
for rb, re_, cs in reps:
    nxt = [c for c in sorted(commit) if commit[c] >= re_][:1]   # the commit just after rep_end belongs to this replay
    cs = cs + nxt
    if not cs or any(c not in G for c in cs): continue
    sub = max([s for s in subs if s <= rb], default=None); nsub = min([s for s in subs if s > re_], default=None)
    if sub is None or nsub is None: continue
    gend = max(G[c][1] for c in cs); gstart = min(G[c][0] for c in cs)
    busy = sum(G[c][1] - G[c][0] for c in cs)
    last = max(cs, key=lambda c: G[c][1])
    sn = seen.get(last)
    w = min([x for x in waits if x >= gend], default=None)
    rows.append(dict(rep=ms(re_ - rb), first_gpu=ms(gstart - rb), gpu_busy=ms(busy), gpu_end_after_rep=ms(gend - re_),
                     seen_after_gpu=ms(sn - gend) if sn else 0, wait_after_gpu=ms(w - gend) if w else 0, next_sub_after_gpu=ms(nsub - gend),
                     frame=ms(nsub - sub), last_commit_after_rep=ms(commit[last] - re_), last_span=ms(G[last][1] - G[last][0])))
print(f'clock offset fit from {len(G)} spans; {len(rows)} frames')
for k in rows[0]:
    v = sorted(r[k] for r in rows); print(f'{k:24s} median {st.median(v):7.2f}  p10 {v[len(v)//10]:7.2f}  p90 {v[len(v)*9//10]:7.2f} ms')

# How early the game ends its command buffers (end_rec / sub_h / exec_h events): for each submitted handle, the time
# from its EndRecording to the submit that carries it, weighted by the replay time the handle takes (exec_h to the next
# exec_h or rep_end). Replay work recorded well before its submit could be translated ahead, off the frame's path.
ends, subh, exech = {}, [], []
for e in ev:
    if e[1] == 'end_rec': ends.setdefault(e[4], []).append(e[0])
    elif e[1] == 'sub_h': subh.append(e)
    elif e[1] == 'exec_h': exech.append(e)
if subh and exech:
    import bisect
    for v in ends.values(): v.sort()
    sub_in_t = sorted(e[0] for e in ev if e[1] == 'sub_in')
    rep_ends = sorted(e[0] for e in ev if e[1] == 'rep_end')
    # replay time of each executed handle, keyed by (handle, the submit it belongs to)
    exec_ms = {}
    for i, e in enumerate(exech):
        nxt = exech[i + 1][0] if i + 1 < len(exech) else None
        re_ = rep_ends[bisect.bisect_right(rep_ends, e[0])] if bisect.bisect_right(rep_ends, e[0]) < len(rep_ends) else None
        end = min(x for x in (nxt, re_) if x is not None) if (nxt or re_) else None
        if end is None: continue
        s = sub_in_t[bisect.bisect_right(sub_in_t, e[0]) - 1]   # the latest submit before this replay step
        exec_ms[(e[4], s)] = exec_ms.get((e[4], s), 0) + (end - e[0])
    nframes = len({e[3] for e in ev if e[1] == 'pres_in'}) or 1
    bins = [(0, 1), (1, 3), (3, 6), (6, 10), (10, 20), (20, 40), (40, 1e9)]
    by = {b: [0, 0.0] for b in bins}; nolead = [0, 0.0]; per_sub = {}
    for e in subh:
        s = sub_in_t[bisect.bisect_right(sub_in_t, e[0]) - 1]; per_sub[s] = per_sub.get(s, 0) + 1
        r = exec_ms.get((e[4], s), 0) / 1e6
        v = ends.get(e[4]); j = bisect.bisect_right(v, e[0]) - 1 if v else -1
        if j < 0: nolead[0] += 1; nolead[1] += r; continue
        lead = (e[0] - v[j]) / 1e6
        for b in bins:
            if b[0] <= lead < b[1]: by[b][0] += 1; by[b][1] += r; break
    tot = sum(x[1] for x in by.values()) + nolead[1]
    print(f'\nsubmits {len(per_sub)} ({len(per_sub) / nframes:.1f} a frame), handles per submit median {st.median(per_sub.values())}; '
          f'replay {tot / nframes:.2f} ms a frame over submitted handles')
    print('EndRecording -> submit lead, per frame: handles, replay ms (share)')
    for b in bins:
        n, r = by[b]; lab = f'{b[0]:>2}-{b[1]:<3} ms' if b[1] < 1e9 else f'{b[0]:>2}+     ms'
        print(f'  {lab}  {n / nframes:6.1f}  {r / nframes:6.2f} ms  ({100 * r / tot if tot else 0:4.1f} %)')
    print(f'  no EndRecording seen  {nolead[0] / nframes:6.1f}  {nolead[1] / nframes:6.2f} ms  (recorded before the window, or re-submitted)')
