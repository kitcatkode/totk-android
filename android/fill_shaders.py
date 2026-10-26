#!/usr/bin/env python3
"""Fill shader variants the device met but the seed lacks (the Android counterpart of ios/tools/fill_shaders.py).

The device has no translator: a variant missing from shaders/<version> skips its draws and leaves its complete request
(variant, stage, guest code, answers) in shader-requests/ (android/gfx/vk_shader.c). This pulls the requests with adb,
translates them with the Linux translator (prebuilt/linux-x64/totk_xlate, built by
tools/totk_xlate/publish_native.sh), adds the Vulkan reflection (android/vk_reflect.py), pushes the new files into the
device's shaders/<version>, updates the local TOTK_android copy, and moves the requests to shader-requests/done/.
The next start of the game draws with them.

usage: fill_shaders.py --work <TOTK_build folder> --out <TOTK_android folder> [--translator <totk_xlate>] [--device-dir <game folder on the device>]
"""
import argparse, json, os, platform, shutil, subprocess, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vk_reflect


def adb(*args, check=True, capture=False):
    r = subprocess.run(['adb', *args], check=check, text=True, capture_output=capture)
    return r.stdout if capture else None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True, help='the TOTK_build folder of the full build')
    ap.add_argument('--out', type=Path, required=True, help='the TOTK_android folder (its shaders/<version> is updated too)')
    ap.add_argument('--translator', type=Path)
    ap.add_argument('--device-dir', default='/sdcard/Android/data/dev.kitcatkode.totk/files', help='the game folder on the device (the app\'s; /data/local/tmp/totk for the adb build)')
    a = ap.parse_args(argv)
    tree = a.work.expanduser().resolve() / 'totk'
    out = a.out.expanduser().resolve()
    versions = sorted(p.name for p in (out / 'shaders').iterdir() if p.is_dir()) if (out / 'shaders').is_dir() else []
    if len(versions) != 1:
        print(f'expected one shaders/<version> folder in {out}, found {versions}'); return 2
    version = versions[0]
    translator = (a.translator.expanduser().resolve() if a.translator else
                  tree / f"prebuilt/linux-{'arm64' if platform.machine() in ('aarch64', 'arm64') else 'x64'}/totk_xlate")
    spirv_cross = tree / 'build/spirv-cross/spirv-cross'
    for p in (translator, spirv_cross):
        if not p.is_file():
            print(f'missing {p}: run build_android.py --shaders once first'); return 2
    sys.path.insert(0, str(tree / 'scripts'))
    from prepare_shader_seed import CHAIN

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); req_dir = tmp / 'requests'; xl = tmp / 'out'; xl.mkdir()
        listing = adb('shell', f'ls {a.device_dir}/shader-requests 2>/dev/null', check=False, capture=True) or ''
        names = [n.strip() for n in listing.split() if n.strip().endswith('.json')]
        if not names:
            print('no shader requests on the device'); return 0
        req_dir.mkdir()
        for n in names:
            adb('pull', f'{a.device_dir}/shader-requests/{n}', str(req_dir / n))
        print(f'{len(names)} requests')
        proc = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross), '--out', str(xl), '--chain', CHAIN],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        done, failed = [], []
        for n in names:
            rq = json.loads((req_dir / n).read_text())
            code = tmp / f'{rq["variant"]}.code'; code.write_bytes(bytes.fromhex(rq.pop('code_hex')))
            rq['code'] = str(code)
            proc.stdin.write(json.dumps(rq) + '\n'); proc.stdin.flush()
            reply = json.loads(proc.stdout.readline())
            (done if reply.get('ok') else failed).append((n, rq, reply.get('error')))
        proc.stdin.close(); proc.wait()
        dst = out / f'shaders/{version}'
        push = []
        for n, rq, _ in done:
            variant = rq['variant']; key = variant.split('-')[0]
            stage = vk_reflect.STAGES[rq['stage']]
            refl = json.loads((xl / f'{variant}_b.json').read_text())
            r = vk_reflect.reflect((xl / f'{variant}.spv').read_bytes(), json.loads((xl / f'{variant}.res.json').read_text()), stage)
            if refl.get('threadgroup_size'):
                r['local'] = tuple(refl['threadgroup_size'])
            shutil.copy2(xl / f'{variant}.spv', dst / f'{variant}.spv')
            (dst / f'{variant}.vk').write_text(vk_reflect.vk_text(r, stage, refl.get('c1', '')))
            vv = dst / f'{key}.vv'
            rows = vv.read_text().splitlines() if vv.is_file() else []
            row = f'{variant}\t{vk_reflect.flat_deps(refl.get("deps") or {})}'
            if row not in rows:
                vv.write_text('\n'.join(rows + [row]) + '\n')
            push += [dst / f'{variant}.spv', dst / f'{variant}.vk', vv]
        for f in push:
            adb('push', str(f), f'{a.device_dir}/shaders/{version}/{f.name}')
        # The app owns its folder's shader-requests/ (adb's shell may not write there): with root, su does it.
        rq_dir = f'{a.device_dir}/shader-requests'
        moves = '; '.join(f'mv {rq_dir}/{n} {rq_dir}/done/' for n, _, _ in done)
        script = f'mkdir -p {rq_dir}/done && {moves}'
        if done and adb('shell', f'{script} 2>/dev/null && echo moved', check=False, capture=True).strip() != 'moved':
            if adb('shell', f"su -c '{script}' 2>/dev/null && echo moved", check=False, capture=True).strip() != 'moved':
                print('could not move the filled requests to shader-requests/done (no permission, no root): the next fill translates them again')
        print(f'filled {len(done)} variants, {len(failed)} failed')
        for n, _, e in failed[:20]:
            print(f'  {n}: {e}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
