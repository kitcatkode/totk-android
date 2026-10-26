#!/usr/bin/env python3
"""Run the TOTK Android app on the connected device (AYN Thor, rooted) under test settings and collect the results.

    devrun.py run --name NAME [--secs 120] [--env KEY=VALUE ...] [--unset KEY ...] [--script FILE] [--perf SECS@AT]
                  [--shots EVERY] [--keep-running]
    devrun.py summary LOG [LOG ...]          the perf numbers of finished runs (their totk.log), side by side
    devrun.py env                            print the device's totk.env
    devrun.py awake on|off                   keep the screen on while plugged in (a test session), then restore the setting

`run`:
  1. saves the device's totk.env, writes it back with --env overrides (KEY=VALUE replaces any KEY line) and --unset
     removals, plus TOTK_INPUT_FILE (the command channel) and TOTK_AUTOLOAD=1 (the newest save loads by itself) unless
     overridden;
  2. force-stops and starts the app, then for --secs seconds sends the --script commands at their times (lines
     "<seconds> <command>", commands as in DEBUGGING.md: key <code> <ms>, down <code>, up <code>, passdump <n>,
     flag <name> <0|1>, threads; "#" lines and blank lines are skipped), takes a screen capture every --shots seconds,
     and records a simpleperf profile of --perf SECS seconds starting at AT seconds; a script line "sh <command>" runs
     <command> as root on the device instead (e.g. thor-tune task ... once the game's threads exist);
  3. collects totk.log, the captures, the profile reports and passdump/ into runs/NAME/, writes runs/NAME/summary.json
     (and prints it), stops the app (unless --keep-running) and ALWAYS restores the saved totk.env.

--tune PROFILE (repeatable) applies a thor-tune profile (thor-tools/tune, installed at THOR_TUNE) before the app starts
and restores every knob it touched at the end, with totk.env. Install it first with thor-tools/tune/install.sh.

Everything on the device goes through `adb shell su -c` (the game folder needs root to write). Python 3.8+, adb in PATH.
"""
import argparse, json, re, shlex, statistics, subprocess, sys, time
from pathlib import Path

PKG = 'dev.kitcatkode.totk'
DIR = f'/sdcard/Android/data/{PKG}/files'
TMP = '/data/local/tmp/devrun'
THOR_TUNE = '/data/local/tmp/thor-tune/thor-tune'


def adb(*args, check=True, capture=True, binary=False):
    r = subprocess.run(['adb', *args], capture_output=capture)
    if check and r.returncode:
        raise SystemExit(f'adb {" ".join(args)} failed ({r.returncode}): {r.stderr.decode(errors="replace") if capture else ""}')
    if not capture: return None
    # the log is read while the app writes it: a multi-byte character can be cut off at the end
    return r.stdout if binary else r.stdout.decode(errors='replace')


def app_pid():
    return adb('shell', 'pidof', PKG, check=False).strip()


def su(cmd, check=True):
    return adb('shell', f'su -c {shlex.quote(cmd)}', check=check)


def read_env():
    return su(f'cat {DIR}/totk.env 2>/dev/null', check=False) or ''


def write_env(text):
    tmp = Path('.devrun_env.tmp'); tmp.write_text(text)
    adb('push', str(tmp), f'{TMP}/totk.env'); tmp.unlink()
    su(f'cp {TMP}/totk.env {DIR}/totk.env')


def merged_env(orig, sets, unsets):
    keys = {k for k, _ in sets} | set(unsets)
    lines = [l for l in orig.splitlines() if l.strip() and l.split('=', 1)[0].strip() not in keys]
    lines += [f'{k}={v}' for k, v in sets]
    return '\n'.join(lines) + '\n'


def pull_dir(device_path, local):
    """copy a device folder (root-owned) out through /data/local/tmp"""
    name = Path(device_path).name
    if su(f'[ -e {device_path} ] && echo yes', check=False).strip() != 'yes': return False
    su(f'rm -rf {TMP}/{name} && cp -r {device_path} {TMP}/ && chmod -R a+rX {TMP}/{name}')
    adb('pull', f'{TMP}/{name}', str(local), capture=True)
    return True


PATS = {
    'fps': re.compile(r'frame (\d+): ([\d.]+) fps over the last 300'),
    'pace': re.compile(r'frame (\d+): frame time p50 ([\d.]+) p90 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms, (\d+) of (\d+) over 34 ms, (\d+) over 50'),
    'rec': re.compile(r'frame (\d+), per frame: recording ([\d.]+) ms, waiting for the GPU ([\d.]+) ms, GPU ([\d.]+) ms, ([\d.]+) submissions(?: \(([\d.]+) splits(?:, [\d.]+ inside passes)?\))?'),
    'skips': re.compile(r'frame (\d+): [\d.]+ fps over the last 300, .*shader missing (\d+)'),
    'hitch': re.compile(r'hitch at frame (\d+): \+([\d.]+) ms .*pipelines (\d+) \(([\d.]+) ms'),
    'cpu': re.compile(r'frame (\d+): CPU per frame ([\d.]+) ms over \d+ threads; top: (.*)'),
    'mem': re.compile(r'memory: malloc (\d+) MiB'),
}


def summarize(log_text, warmup_frames=600):
    """averages over the 300-frame windows after warmup_frames (loading), plus worst values and counts"""
    out = {'windows': 0}
    fps = [(int(m[0]), float(m[1])) for m in PATS["fps"].findall(log_text)]
    pace = [tuple(float(x) for x in m) for m in PATS['pace'].findall(log_text)]
    rec = [m for m in PATS['rec'].findall(log_text)]
    late = [f for f in fps if f[0] > warmup_frames]
    if late:
        out['windows'] = len(late)
        out['fps_mean'] = round(statistics.mean(v for _, v in late), 1)
        out['fps_min'] = min(v for _, v in late)
    lp = [p for p in pace if p[0] > warmup_frames]
    if lp:
        out['p50_ms_mean'] = round(statistics.mean(p[1] for p in lp), 1)
        out['p90_ms_mean'] = round(statistics.mean(p[2] for p in lp), 1)
        out['p99_ms_worst'] = max(p[3] for p in lp)
        out['max_ms'] = max(p[4] for p in lp)
        out['frames_over_34ms_pct'] = round(100 * sum(p[5] for p in lp) / max(1, sum(p[6] for p in lp)), 1)
    lr = [r for r in rec if int(r[0]) > warmup_frames]
    if lr:
        out['recording_ms_mean'] = round(statistics.mean(float(r[1]) for r in lr), 1)
        out['gpu_ms_mean'] = round(statistics.mean(float(r[3]) for r in lr), 1)
        sp = [float(r[5]) for r in lr if r[5]]
        if sp: out['splits_per_frame'] = round(statistics.mean(sp), 1)
    sk = PATS['skips'].findall(log_text)
    if sk: out['shader_missing_total'] = int(sk[-1][1])
    hs = PATS['hitch'].findall(log_text)
    out['hitches'] = len(hs); out['hitch_pipeline_ms'] = round(sum(float(h[3]) for h in hs))
    out['red_lines'] = log_text.count('【红】')
    out['crashed'] = bool(re.search(r'Fatal signal|SIGSEGV|DIE\b|device lost|abort', log_text))
    return out


def awake_check():
    """the screen must be on and unlocked: asleep or behind the lock screen the app loses its window (captures go black,
    then the GPU reports device lost). A secure lock screen cannot be dismissed from adb: the user unlocks it."""
    adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP', check=False); time.sleep(0.5)
    adb('shell', 'wm', 'dismiss-keyguard', check=False); time.sleep(1)
    win = adb('shell', 'dumpsys', 'window', check=False)
    if re.search(r'isKeyguardShowing=true', win):
        raise SystemExit('[devrun] the device is locked: unlock it, and keep it awake for a session with "devrun.py awake on"')


AWAKE_SAVE = Path('runs/.stay_on_before')


def cmd_awake(a):
    if a.state == 'on':
        if not AWAKE_SAVE.exists():
            AWAKE_SAVE.parent.mkdir(exist_ok=True)
            AWAKE_SAVE.write_text(adb('shell', 'settings', 'get', 'global', 'stay_on_while_plugged_in').strip())
        adb('shell', 'svc', 'power', 'stayon', 'usb')   # AC / USB / wireless: on while plugged in to the PC
        print('[devrun] the screen stays on while plugged in to USB ("devrun.py awake off" restores the setting)')
    else:
        old = AWAKE_SAVE.read_text().strip() if AWAKE_SAVE.exists() else '0'
        adb('shell', 'settings', 'put', 'global', 'stay_on_while_plugged_in', old)
        AWAKE_SAVE.unlink(missing_ok=True)
        print(f'[devrun] stay_on_while_plugged_in restored to {old}')


INFLIGHT = Path('runs/.env_inflight')   # the user's totk.env while a run has its own on the device


def cmd_run(a):
    out = Path('runs') / a.name; out.mkdir(parents=True, exist_ok=True)
    awake_check()
    su(f'mkdir -p {TMP} && chmod 777 {TMP}')
    if INFLIGHT.exists():   # the last run never restored totk.env (2026-10-23: the battery died mid-run, and the runs after it
        write_env(INFLIGHT.read_text())   # took the run's env for the user's: TOTK_AUTOLOAD=1 stayed in it)
        print('[devrun] the previous run was cut off: totk.env restored from runs/.env_inflight first', flush=True)
    if a.tune:
        if su(f'[ -x {THOR_TUNE} ] && echo yes', check=False).strip() != 'yes':
            raise SystemExit(f'[devrun] --tune needs thor-tune on the device ({THOR_TUNE}): run thor-tools/tune/install.sh')
        print(su(f'{THOR_TUNE} restore', check=False).strip(), flush=True)   # a cut-off run's knobs, before recording new ones
    orig = read_env(); (out / 'totk.env.before').write_text(orig); INFLIGHT.write_text(orig)
    sets = [tuple(e.split('=', 1)) for e in a.env]
    keys = {k for k, _ in sets}
    if 'TOTK_INPUT_FILE' not in keys: sets.append(('TOTK_INPUT_FILE', f'{DIR}/input.txt'))
    if 'TOTK_AUTOLOAD' not in keys and not a.no_autoload: sets.append(('TOTK_AUTOLOAD', '1'))
    env = merged_env(orig, sets, a.unset); (out / 'totk.env.used').write_text(env)
    script = []
    if a.script:
        for line in Path(a.script).read_text().splitlines():
            line = line.strip()
            if not line or line.startswith('#'): continue
            t, cmd = line.split(None, 1); script.append((float(t), cmd))
    script.sort()
    perf_secs, perf_at = (None, None)
    if a.perf: s, at = a.perf.split('@'); perf_secs, perf_at = int(s), float(at)
    try:
        write_env(env)
        for p in a.tune:
            print(su(f'{THOR_TUNE} apply {shlex.quote(p)}').strip(), flush=True)
        su(f'touch {DIR}/input.txt; rm -rf {DIR}/passdump')
        adb('shell', 'am', 'force-stop', PKG); time.sleep(1)
        adb('shell', 'am', 'start', '-n', f'{PKG}/android.app.NativeActivity')
        # am start returns before the process exists
        for _ in range(60):
            if app_pid(): break
            time.sleep(0.5)
        else: raise SystemExit('[devrun] the app did not start')
        t0 = time.time(); gone = 0; next_shot = a.shots if a.shots else None; shot_n = 0; perf_done = False
        print(f'[devrun] {a.name}: running {a.secs} s', flush=True)
        while (now := time.time() - t0) < a.secs:
            while script and script[0][0] <= now:
                t, cmd = script.pop(0)
                if cmd.startswith('sh '):   # a device command (root), not a game command
                    res = su(f'{cmd[3:]} 2>&1', check=False).strip()   # errors too: a failed pin was silent once
                    print(f'[devrun] {now:6.1f}s  {cmd}' + (f'\n         {res}' if res else ''), flush=True)
                    with open(out / 'sh.log', 'a') as f: f.write(f'{now:.1f}s $ {cmd[3:]}\n{res}\n')
                    continue
                su(f'echo {shlex.quote(cmd)} >> {DIR}/input.txt'); print(f'[devrun] {now:6.1f}s  {cmd}', flush=True)
            if next_shot is not None and now >= next_shot:
                png = adb('exec-out', 'screencap', '-p', binary=True)
                (out / f'screen_{shot_n:03d}_{int(now)}s.png').write_bytes(png); shot_n += 1; next_shot += a.shots
            if perf_secs and not perf_done and now >= perf_at:
                print(f'[devrun] profiling {perf_secs} s', flush=True)
                su(f'simpleperf record -p $(pidof {PKG}) --duration {perf_secs} -f 2000 --call-graph fp -o {TMP}/perf.data', check=False)
                for name, args in (('perf_translate.txt', '--comms totk-translate --sort dso,symbol --percent-limit 0.3'),
                                   ('perf_tree.txt', '--comms totk-translate --children --sort symbol -g caller --percent-limit 3'),
                                   ('perf_all_threads.txt', '--sort comm --percent-limit 0.5')):
                    (out / name).write_text(su(f'simpleperf report -i {TMP}/perf.data {args} 2>/dev/null', check=False) or '')
                perf_done = True
            gone = 0 if app_pid() else gone + 1
            if gone >= 3:
                print('[devrun] the app is not running any more (crash or exit)', flush=True); break
            time.sleep(0.25)
        log = su(f'cat {DIR}/totk.log', check=False) or ''
        (out / 'totk.log').write_text(log)
        pull_dir(f'{DIR}/passdump', out)
    finally:
        if not a.keep_running: adb('shell', 'am', 'force-stop', PKG, check=False)
        write_env(orig); INFLIGHT.unlink(missing_ok=True)
        print('[devrun] totk.env restored', flush=True)
        if a.tune:
            print(su(f'{THOR_TUNE} restore', check=False).strip(), flush=True)
    s = summarize(log, a.warmup); s['name'] = a.name; s['env'] = [f'{k}={v}' for k, v in sets] + [f'-{u}' for u in a.unset]
    if a.tune: s['tune'] = a.tune
    (out / 'summary.json').write_text(json.dumps(s, indent=2))
    print(json.dumps(s, indent=2))


def cmd_summary(a):
    rows = []
    for p in a.logs:
        p = Path(p); log = (p / 'totk.log' if p.is_dir() else p).read_text(errors='replace')
        s = summarize(log, a.warmup); s['name'] = p.name; rows.append(s)
    keys = ['name', 'windows', 'fps_mean', 'fps_min', 'p50_ms_mean', 'p90_ms_mean', 'p99_ms_worst', 'frames_over_34ms_pct',
            'recording_ms_mean', 'gpu_ms_mean', 'splits_per_frame', 'hitches', 'hitch_pipeline_ms', 'shader_missing_total', 'red_lines', 'crashed']
    print('\t'.join(keys))
    for r in rows: print('\t'.join(str(r.get(k, '-')) for k in keys))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--name', required=True); r.add_argument('--secs', type=float, default=120)
    r.add_argument('--env', action='append', default=[], help='KEY=VALUE for totk.env (repeatable)')
    r.add_argument('--unset', action='append', default=[], help='remove KEY from totk.env for this run')
    r.add_argument('--script', help='timed input-channel commands: lines "<seconds> <command>"')
    r.add_argument('--perf', help='SECS@AT: a simpleperf profile of SECS seconds starting AT seconds into the run')
    r.add_argument('--shots', type=float, help='a screen capture every N seconds')
    r.add_argument('--warmup', type=int, default=600, help='frames ignored by the summary (loading)')
    r.add_argument('--no-autoload', action='store_true'); r.add_argument('--keep-running', action='store_true')
    r.add_argument('--tune', action='append', default=[], help='thor-tune profile for this run (repeatable), restored at the end')
    s = sub.add_parser('summary'); s.add_argument('logs', nargs='+'); s.add_argument('--warmup', type=int, default=600)
    sub.add_parser('env')
    w = sub.add_parser('awake'); w.add_argument('state', choices=['on', 'off'])
    a = ap.parse_args()
    if a.cmd == 'run': cmd_run(a)
    elif a.cmd == 'summary': cmd_summary(a)
    elif a.cmd == 'awake': cmd_awake(a)
    else: print(read_env(), end='')


if __name__ == '__main__':
    main()
