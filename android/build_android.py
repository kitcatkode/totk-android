#!/usr/bin/env python3
"""Build the headless Android (arm64) runtime of Tears of the Kingdom from your own game files, on a Linux PC.

    python3 android/build_android.py <game.xci|game.nsp> -k <prod.keys> --ndk <android-ndk> [--update <update.nsp>]
                                                [--work <folder>] [-o <output folder>] [--equiv]

What it makes: a folder (default "<game file folder>/TOTK_android") with the game's code rewritten for Android, its data,
the runtime built with the Android NDK, and run.sh / push.sh to put it on a device and start it from adb shell. This is
the headless stage of the port: the game boots and runs its frame loop with the GPU replaced by a null backend (no
picture, no input). It exists to bring the game up on the AYN Thor before the Vulkan backend and the Android app.

Steps:
  1. The extraction and checks of buildlib.py (from the original Mac build): ExeFS and the three code modules, rewritten for Apple silicon and
     checked against assets/expected-v<version>.json. That rewrite is not used on Android; matching the
     verified hashes proves the dump and the extraction.
  2. The modules rewritten again for Linux / Android (DM_HOST_TP=tpidr_el0, third_party/d2r/hostreg.py). Every rewrite
     site, cut and gate must be the same set as in the verified rewrite; the structural checker (structck.py) checks the
     new images word by word; --equiv also runs the emulator equivalence check of every site (hours, as on the Mac).
  3. RomFS (17 GB), shared fonts and RomFS overlays (buildlib.py).
  4. libopus and libvpx for Android (pinned sources), then the runtime (android/Makefile, TARGET=android).

Needs: Linux (or macOS) with Python 3.12+ (3.14 for NSZ updates), git, rsync, make, cmake, clang or gcc (hactool, for
updates), llvm-objdump / llvm-objcopy (LLVM_BIN, default /usr/bin), the Android NDK r26 or newer, and about 45 GB free.
Nothing from the game is in the repository; key values are never printed.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(REPO))
import buildlib as lib   # extraction, checks and helpers (buildlib.py)
from buildlib import BuildError, say, run, sha256, check

OPUS_SRC = ('https://github.com/xiph/opus.git', 'v1.5.2')
VPX_SRC = ('https://chromium.googlesource.com/webm/libvpx', 'v1.15.0')
MODULES = ('main', 'subsdk0', 'sdk')


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', type=Path, help='the game file (.xci or .nsp), base game 1.0.0')
    ap.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('--update', type=Path, help='separate supported cumulative update NSP/NSZ (1.1.2, 1.2.1 or 1.4.3)')
    ap.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'),
                    help='Android NDK directory (default $ANDROID_NDK_HOME)')
    ap.add_argument('--api', type=int, default=33, help='Android API level (the AYN Thor runs Android 13 = 33)')
    ap.add_argument('-o', '--out', type=Path, help='output folder (default: TOTK_android next to the game file)')
    ap.add_argument('--work', type=Path, help='work folder (default: TOTK_build next to the game file, shared with the Mac build)')
    ap.add_argument('--equiv', action='store_true', help='also run the emulator equivalence check of every rewrite site (hours)')
    ap.add_argument('--no-vpx', action='store_true', help='leave out libvpx (cutscenes then abort, as on iOS)')
    ap.add_argument('--gpu', choices=('vk', 'null'), default='vk', help='GPU backend: vk (Vulkan, default) or null (no GPU)')
    ap.add_argument('--shaders', action='store_true',
                    help='translate the game\'s shaders for Vulkan (about an hour the first time; needs the Linux shader translator, '
                         'tools/totk_xlate/publish_native.sh). With --runtime-only: also (re)make the shaders')
    ap.add_argument('--translator', type=Path, help='the shader translator (default: prebuilt/linux-x64/totk_xlate)')
    ap.add_argument('--apk', action='store_true',
                    help='also build the Android app (totk.apk: the runtime as a NativeActivity, picture and controls) and install_app.sh')
    ap.add_argument('--runtime-only', action='store_true',
                    help='after a full build: rebuild only totk-native from the current sources (minutes); push it with sh push.sh runtime')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA directory with the shared fonts (NSP input)')
    fonts.add_argument('--shared-fonts', type=Path, help='previously extracted shared-font directory (NSP input)')
    return ap.parse_args(argv)


def prerequisites(args):
    if sys.version_info < (3, 12):
        raise BuildError('needs Python 3.12 or later')
    for tool in ('git', 'rsync', 'make', 'cmake'):
        if not shutil.which(tool):
            raise BuildError(f'{tool} not found')
    llvm = Path(os.environ.get('LLVM_BIN', '/usr/bin'))
    if not (llvm / 'llvm-objdump').exists() or not (llvm / 'llvm-objcopy').exists():
        raise BuildError(f'llvm-objdump / llvm-objcopy not found in {llvm} (install LLVM, or set LLVM_BIN)')
    if not args.ndk or not (Path(args.ndk) / 'build/cmake/android.toolchain.cmake').is_file():
        raise BuildError('--ndk must point at an Android NDK (r26 or newer), or set ANDROID_NDK_HOME')
    return Path(sys.executable), llvm


# ---- step 2: rewrite for Linux / Android ----
def site_lists(folder, name):
    """What must be the same in both rewrites: the sites (text addresses and kinds, window and call extents), cuts,
    parameter gates, observation gates, and each gate's kind / argument / original address (its return point lies in
    the patch area, whose layout differs)."""
    out = {}
    for kind in ('sites', 'cuts', 'params', 'obs'):
        p = folder / f'{name}.{kind}.tsv'
        out[kind] = sorted(p.read_text().splitlines()) if p.exists() else []
    out['gates'] = sorted(' '.join(l.split()[1:]) for l in (folder / f'{name}.gates.tsv').read_text().splitlines())
    return out


def android_rewrite(tree, version, env, logs, llvm, *, equiv=False, darwin=None):
    """Rewrite main / subsdk0 / sdk of `version` for Linux / Android into rewritten-android/<version>; returns that folder.
    darwin: the verified Darwin rewrite to compare the site sets with (default rewritten/<version>)."""
    py = tree / '.venv/bin/python'
    exefs = tree / f'extracted/{version}/exefs'
    darwin = darwin or tree / f'rewritten/{version}'
    out = tree / f'rewritten-android/{version}'
    out.mkdir(parents=True, exist_ok=True)
    renv = dict(env, DM_RULESET='native', DM_PRIVATE_GATES='0', DM_HOST_TP='tpidr_el0', LLVM_BIN=str(llvm))
    stamp = out / 'verified.json'
    rewriter = sha256(tree / 'third_party/d2r/patch_nso.py') + sha256(tree / 'third_party/d2r/hostreg.py')
    known = json.loads(stamp.read_text()) if stamp.exists() else {}
    for name in MODULES:
        image = out / f'{name}.patched.bin'
        k = known.get(name, {})
        if image.exists() and k.get('source') == sha256(exefs / name) and k.get('image') == sha256(image) \
                and k.get('rewriter') == rewriter and (k.get('equiv') or not equiv):
            continue
        say(f'2/4 code: rewriting {name} for Android' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite_android_{version}_{name}.log', cwd=tree, env=renv)
        a, b = site_lists(darwin, name), site_lists(out, name)
        for kind in a:
            if a[kind] != b[kind]:
                raise BuildError(f'{name}: the Android rewrite has different {kind} than the verified rewrite '
                                 f'({len(a[kind])} vs {len(b[kind])}); please report this')
        say(f'2/4 code: structural check of {name} (every rewritten word)')
        run([py, 'third_party/d2r/structck.py', out, name, exefs / name], logs / f'structck_android_{version}_{name}.log', cwd=tree, env=renv)
        if equiv:
            say(f'2/4 code: emulator equivalence check of every {name} site (long)')
            run([py, 'third_party/d2r/equiv.py', out, name, exefs / name], logs / f'equiv_android_{version}_{name}.log', cwd=tree, env=renv)
        known[name] = {'source': sha256(exefs / name), 'image': sha256(image), 'rewriter': rewriter, 'equiv': equiv}
        stamp.write_text(json.dumps(known, indent=2) + '\n')
    report = dict(format=1, status='pass', rom_version=version, host='linux-android (tpidr_el0)',
                  scope='same sites as the verified Darwin rewrite; structck' + (' + equiv' if all(known[n].get('equiv') for n in MODULES) else ''),
                  module_file_sha256={**{f'extracted/{version}/exefs/{n}': sha256(exefs / n) for n in MODULES},
                                      **{str((out / f'{n}.patched.bin').relative_to(tree)): sha256(out / f'{n}.patched.bin') for n in MODULES}})
    (out / 'phase12-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    return out


# ---- step 4: libraries and the runtime ----
def fetch(url, ref, dst, log):
    marker = dst / '.fetched-ref'
    if marker.is_file() and marker.read_text() == ref:
        return dst
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, ref], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)
    marker.write_text(ref)
    return dst


def ndk_tools(ndk, api):
    host = next((p for p in (Path(ndk) / 'toolchains/llvm/prebuilt').iterdir() if p.is_dir()), None)
    if not host:
        raise BuildError('no prebuilt LLVM toolchain in the NDK')
    b = host / 'bin'
    return dict(CC=str(b / f'aarch64-linux-android{api}-clang'), CXX=str(b / f'aarch64-linux-android{api}-clang++'),
                AR=str(b / 'llvm-ar'), NM=str(b / 'llvm-nm'), RANLIB=str(b / 'llvm-ranlib'), STRIP=str(b / 'llvm-strip'), AS=str(b / f'aarch64-linux-android{api}-clang'))


def build_opus(work, ndk, api, logs):
    prefix = work / 'android-libs/opus'
    if (prefix / 'lib/libopus.a').is_file():
        return prefix
    say('4/4 libraries: libopus for Android')
    src = fetch(*OPUS_SRC, work / 'src/opus', logs / 'fetch-opus.log')
    build = work / 'android-libs/opus-build'
    shutil.rmtree(build, ignore_errors=True)
    run(['cmake', '-S', src, '-B', build, f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', '-DANDROID_ABI=arm64-v8a',
         f'-DANDROID_PLATFORM=android-{api}', '-DCMAKE_BUILD_TYPE=Release', '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_BUILD_TESTING=OFF',
         '-DBUILD_TESTING=OFF', f'-DCMAKE_INSTALL_PREFIX={prefix}'], logs / 'opus-cmake.log')
    run(['cmake', '--build', build, '--target', 'install', '-j', str(os.cpu_count() or 4)], logs / 'opus-build.log')
    return prefix


def build_vpx(work, ndk, api, logs):
    """libvpx's VP9 decoder only, configured as generic arm64 Linux with the NDK's compilers (its own android target
    expects the old standalone toolchains)."""
    prefix = work / 'android-libs/vpx'
    if (prefix / 'lib/libvpx.a').is_file():
        return prefix
    say('4/4 libraries: libvpx (VP9 decoder) for Android')
    src = fetch(*VPX_SRC, work / 'src/libvpx', logs / 'fetch-libvpx.log')
    build = work / 'android-libs/vpx-build'
    shutil.rmtree(build, ignore_errors=True); build.mkdir(parents=True)
    env = dict(os.environ, **ndk_tools(ndk, api), LD=ndk_tools(ndk, api)['CC'])
    run([src / 'configure', '--target=arm64-linux-gcc', f'--prefix={prefix}', '--enable-pic', '--disable-examples', '--disable-tools',
         '--disable-docs', '--disable-unit-tests', '--disable-vp8', '--disable-vp9-encoder', '--enable-vp9-decoder',
         '--disable-webm-io', '--disable-libyuv', '--enable-runtime-cpu-detect'], logs / 'vpx-configure.log', cwd=build, env=env)
    run(['make', '-j', str(os.cpu_count() or 4), 'install'], logs / 'vpx-build.log', cwd=build, env=env)
    return prefix


def generated_headers(tree, version, rewritten, gen, logs):
    py = tree / '.venv/bin/python'
    gen.mkdir(parents=True, exist_ok=True)
    run([py, 'scripts/prepare_runtime.py', '--version', version, '--rewritten', rewritten, '--report', rewritten / 'phase12-validation.json',
         '--out', gen], logs / f'prepare_runtime_android_{version}.log', cwd=tree)
    run([py, 'scripts/prepare_romfs_backend.py', '--version', version], logs / f'prepare_romfs_{version}.log', cwd=tree)
    run([py, 'scripts/prepare_shared_fonts.py'], logs / 'prepare_shared_fonts.log', cwd=tree)
    shutil.copy2(tree / ('build/romfs_config.h' if version == 'v0' else f'build/{version}/romfs_config.h'), gen / 'romfs_config.h')
    shutil.copy2(tree / 'build/shared_fonts_config.h', gen / 'shared_fonts_config.h')


# ---- the app ----
APP_PACKAGE = 'dev.kitcatkode.totk'
INSTALL_SH = '''#!/bin/sh
# The Android app (android/APP.md). Its game folder is the app's folder on shared storage:
#   /sdcard/Android/data/{package}/files   (rewritten/, extracted/, shaders/, driver.so, user-data/, totk.env, totk.log)
#   sh install_app.sh              install or update the app (keeps its data)
#   sh install_app.sh from-device  copy the game folder from /data/local/tmp/totk (the adb shell build) on the device (~17 GB, minutes)
#   sh install_app.sh data         copy the game folder from this computer (USB; the folders are made first: adb push
#                                  cannot make folders under Android/data on Android 13)
#   sh install_app.sh log          the app's log (totk.log)
#   sh install_app.sh saves        copy the app's saves (kept in its private storage) to saves-<date>.tar here
set -e
cd "$(dirname "$0")"
PKG={package}
DIR=/sdcard/Android/data/$PKG/files
case "$1" in
  "") adb install -r totk.apk; echo "installed; start it from the launcher, or: adb shell am start -n $PKG/android.app.NativeActivity" ;;
  from-device)
    adb shell mkdir -p "$DIR"
    for f in rewritten extracted shaders user-data driver.so totk.env; do
      adb shell "if [ -e /data/local/tmp/totk/$f ]; then echo copying $f; cp -r /data/local/tmp/totk/$f $DIR/; fi"
    done ;;
  data)
    adb shell mkdir -p "$DIR"
    find rewritten extracted $([ -d shaders ] && echo shaders) -type d | sed "s|^|$DIR/|" | adb shell xargs mkdir -p
    adb push --sync rewritten extracted "$DIR/"
    if [ -d shaders ]; then adb push --sync shaders "$DIR/"; fi
    if [ -f driver.so ]; then adb push driver.so "$DIR/"; fi ;;
  log) adb shell cat "$DIR/totk.log" ;;
  saves) f="saves-$(date +%Y%m%d-%H%M%S).tar"; adb exec-out run-as $PKG tar cf - files/user-data > "$f"; echo "saved $f" ;;
  *) echo "usage: sh install_app.sh [from-device|data|log|saves]"; exit 2 ;;
esac
'''


def build_app(tree, version, gen, ndk, api, opus, vpx, gpu, work, out, logs, env):
    """libtotk.so (the runtime for the NativeActivity) and totk.apk, signed with work/apk-key.pem (made on first use)."""
    say('app: building libtotk.so and totk.apk')
    make = ['make', '-C', tree / 'android', 'TARGET=android', 'APP=1', f'NDK={ndk}', f'API={api}', f'VERSION={version}',
            f'GEN={gen}', f'OUT={gen}', f'OPUS={opus}', *([f'VPX={vpx}'] if vpx else []), f'GPU={gpu}', f'-j{os.cpu_count() or 4}']
    run(make, logs / f'make_android_app_{version}.log', env=env)
    libs = [gen / 'libtotk.so', gen / 'libdl_android.so', gen / 'liblsfg.so']
    host = f"{'linux' if sys.platform.startswith('linux') else 'darwin'}-x86_64"
    cxx = ndk / f'toolchains/llvm/prebuilt/{host}/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
    if cxx.is_file(): libs.append(cxx)   # for Vulkan drivers built against the NDK's C++ library
    run([tree / '.venv/bin/python', tree / 'android/apk/build_apk.py', *sum((['--lib', str(l)] for l in libs), []),
         '--out', out / 'totk.apk', '--key', work / 'apk-key.pem', '--package', APP_PACKAGE, '--min-sdk', str(api)],
        logs / 'build_apk.log', env=env)
    (out / 'install_app.sh').write_text(INSTALL_SH.format(package=APP_PACKAGE)); (out / 'install_app.sh').chmod(0o755)


# ---- output folder ----
RUN_SH = '''#!/system/bin/sh
# Start the game from adb shell (headless: no picture or input yet; the log shows how far it boots).
#   adb shell sh /data/local/tmp/totk/run.sh          (TOTK_* variables can be set in front)
cd "$(dirname "$0")" || exit 1
mkdir -p user-data state
export TOTK_PROFILE_DIR="$PWD/user-data" TOTK_ROMFS_PROOF_DIR="$PWD/state" TOTK_SHARED_FONTS="$PWD/extracted/firmware/shared-fonts"
export TOTK_OPERATION_MODE="${{TOTK_OPERATION_MODE:-handheld}}" TOTK_ROMFS_MMAP="${{TOTK_ROMFS_MMAP:-1}}"
# Vulkan driver: driver.so in this folder (e.g. a Turnip build) when present, else the system one.
# TOTK_VK_DRIVER=/path/to/driver.so picks another file, TOTK_VK_DRIVER=system the system driver.
if [ -z "$TOTK_VK_DRIVER" ] && [ -f driver.so ]; then export TOTK_VK_DRIVER="$PWD/driver.so"; fi
if [ -d shaders/{version} ]; then export TOTK_SHADER_SEED_DIR="$PWD/shaders/{version}"; fi
exec ./totk-native rewritten/{version} boot 2>&1 | tee totk.log
'''
PUSH_SH = '''#!/bin/sh
# Copy this folder to the device (about {gb:.0f} GB; USB 3 takes several minutes): sh push.sh [device folder]
# After build_android.py --runtime-only: sh push.sh runtime   (only totk-native and run.sh)
# After build_android.py --runtime-only --shaders: sh push.sh shaders   (also the shaders folder)
set -e
cd "$(dirname "$0")"
DEST=/data/local/tmp/totk
if [ "$1" = runtime ]; then adb push totk-native run.sh "$DEST/"
elif [ "$1" = shaders ]; then adb push totk-native run.sh shaders "$DEST/"; else
  [ -n "$1" ] && DEST="$1"
  adb shell mkdir -p "$DEST"
  adb push --sync totk-native run.sh rewritten extracted "$DEST/"
  if [ -d shaders ]; then adb push --sync shaders "$DEST/"; fi
fi
adb shell chmod 755 "$DEST/totk-native" "$DEST/run.sh"
echo "now: adb shell sh $DEST/run.sh"
'''


# ---- shaders: the Mac's seed preparation, keeping the Vulkan SPIR-V ----
def build_spirv_cross_linux(tree, logs):
    """SPIRV-Cross with the host C++ compiler (the Mac build used Xcode's)."""
    src, out = tree / 'tooling/spirv-cross', tree / 'build/spirv-cross'
    cxx = shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        raise BuildError('SPIRV-Cross needs clang++ or g++')
    flags = list(lib.SPIRV_CROSS_FLAGS)
    if '-arch' in flags:
        i = flags.index('-arch'); del flags[i:i + 2]
    objs = out / 'obj'; objs.mkdir(parents=True, exist_ok=True)
    import concurrent.futures
    def one(name):
        run([cxx, *flags, '-c', src / name, '-o', objs / (name + '.o')], logs / 'spirv-cross' / (name + '.log'), quiet=False)
    (logs / 'spirv-cross').mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        list(pool.map(one, lib.SPIRV_CROSS_SOURCES))
    run([cxx, *[objs / (n + '.o') for n in lib.SPIRV_CROSS_SOURCES], '-o', out / 'spirv-cross'], logs / 'spirv-cross' / 'link.log', quiet=False)


def translator_inputs_hash(tree):
    """What publish_native.sh compiles into the translator: its sources, project and patches (TOTK, 2026-10-22).
    The seed's reuse check keys on the translator binary's hash, so a stale binary silently reuses an old seed and
    translator changes never reach the device (the gather bias was lost this way)."""
    import hashlib
    h = hashlib.sha256()
    src = tree / 'tools/totk_xlate'
    for p in sorted(list(src.glob('*.cs')) + list(src.glob('*.csproj')) + list((src / 'patches').glob('*.patch'))):
        h.update(p.name.encode()); h.update(p.read_bytes())
    return h.hexdigest()


def translator_stale(args, tree):
    """True when the translator binary predates the sources publish_native.sh compiles into it (no stamp, or a hash
    that no longer matches). The seed's reuse check keys on the binary's hash, so a stale binary reuses a stale seed
    and translator changes never reach the device (the gather bias was lost this way)."""
    if args.translator:
        t = args.translator.expanduser().resolve()
    else:
        import platform
        t = tree / f"prebuilt/linux-{'arm64' if platform.machine() in ('aarch64', 'arm64') else 'x64'}/totk_xlate"
    stamp = t.parent / 'totk_xlate.inputs.sha256'
    return not stamp.is_file() or stamp.read_text().strip() != translator_inputs_hash(tree)


def translator_path(args, tree, logs=None, auto=False):
    if args.translator:
        t = args.translator.expanduser().resolve()
    else:
        import platform
        t = tree / f"prebuilt/linux-{'arm64' if platform.machine() in ('aarch64', 'arm64') else 'x64'}/totk_xlate"
    if not t.is_file():
        raise BuildError(f'no shader translator at {t}. Build it once with:  zsh tools/totk_xlate/publish_native.sh   '
                         '(needs the .NET 10 SDK, clang and zlib: on Fedora  sudo dnf install dotnet-sdk-10.0 clang zlib-devel), '
                         'then run this again')
    stamp = t.parent / 'totk_xlate.inputs.sha256'
    want = translator_inputs_hash(tree)
    if not stamp.is_file() or stamp.read_text().strip() != want:
        # publish_native.sh copies $REPO/vendor/ryujinx with REPO four levels above its own directory: only the source
        # checkout resolves that correctly (the build tree's work folder has no vendor). It writes the binary into the
        # checkout's prebuilt/<platform>; from there it is copied next to the one this build uses.
        script = lib.REPO / 'tools/totk_xlate/publish_native.sh'
        if auto and not args.translator and script.is_file() and shutil.which('dotnet'):
            say('shaders: the translator predates its sources, rebuilding it (publish_native.sh; a stale binary reuses a stale seed)')
            run(['zsh', str(script)], logs / 'publish-native-translator.log', cwd=tree, quiet=False)
            fresh = script.parent.parent.parent / f"prebuilt/linux-{'arm64' if platform.machine() in ('aarch64', 'arm64') else 'x64'}/totk_xlate"
            shutil.copy2(fresh, t)
            if (fresh.parent / 'totk_xlate.sha256').is_file():
                shutil.copy2(fresh.parent / 'totk_xlate.sha256', t.parent)
            stamp.write_text(want + '\n')
        else:
            raise BuildError(f'the shader translator {t} predates its sources (totk_xlate): a seed built with it silently '
                             'misses translator changes. Rebuild it:  zsh tools/totk_xlate/publish_native.sh   '
                             '(needs the .NET 10 SDK, clang and zlib), then run this again')
    return t


def step_shaders(args, tree, version, logs):
    """Every shader of the game for its default bind state and every bind state of the shared list, as SPIR-V for
    Vulkan with the reflection the runtime uses (scripts/prepare_shader_seed.py --keep-spirv). Reused when unchanged."""
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        say('shaders: building SPIRV-Cross')
        build_spirv_cross_linux(tree, logs)
    translator = translator_path(args, tree, logs=logs, auto=True)
    seed = tree / f'build/shader-seed-android/{version}'
    say('shaders: translating the game\'s shaders for Vulkan (about an hour the first time, then reused)')
    run([tree / '.venv/bin/python', 'scripts/prepare_shader_seed.py', '--romfs', tree / f'extracted/{version}/romfs',
         '--output', seed, '--translator', translator, '--spirv-cross', tree / 'build/spirv-cross/spirv-cross',
         '--version', version, '--modules', tree / f'rewritten/{version}', '--keep-spirv'],
        logs / f'prepare-shader-seed-android-{version}.log', cwd=tree, quiet=False)
    return seed


def place_shaders(seed, out, version):
    """The seed for the device in out/shaders/<version>: each variant's SPIR-V (hard links) with its Vulkan reflection
    (<variant>.vk) and each shader's variant list (<key>.vv), made by android/vk_reflect.py. The MSL and the Mac
    reflection stay in the work folder."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import vk_reflect
    dst = out / f'shaders/{version}'
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    n, bad, unknown = vk_reflect.convert(seed, dst)
    for f in seed.glob('*.spv'):
        try: os.link(f, dst / f.name)
        except OSError: shutil.copy2(f, dst / f.name)
    say(f'shaders: {n} variants reflected for Vulkan into {dst}' + (f', {bad} incomplete' if bad else ''))
    for k, c in sorted(unknown.items(), key=lambda kv: -kv[1])[:10]:
        say(f'  unclassified descriptor {k}: {c} variants (bound as empty)')


def assemble(tree, version, rewritten, binary, out):
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, out / 'totk-native')
    dst = out / f'rewritten/{version}'
    dst.mkdir(parents=True, exist_ok=True)
    for n in MODULES:
        shutil.copy2(rewritten / f'{n}.patched.bin', dst / f'{n}.patched.bin')
    def place(src, rel):   # hard links: no second 17 GB copy on the same file system
        d = out / rel; d.parent.mkdir(parents=True, exist_ok=True)
        if d.exists(): d.unlink()
        try: os.link(src, d)
        except OSError: shutil.copy2(src, d)
    romfs = tree / f'extracted/{version}/romfs'
    for f in ('romfs.bin', 'runtime-index.bin'):
        place(romfs / f, f'extracted/{version}/romfs/{f}')
    for folder in (f'extracted/{version}/romfs-patches', 'extracted/firmware/shared-fonts'):
        for f in (tree / folder).rglob('*'):
            if f.is_file(): place(f, f'{folder}/{f.relative_to(tree / folder)}')
    gb = sum(f.stat().st_size for f in out.rglob('*') if f.is_file()) / 2**30
    (out / 'run.sh').write_text(RUN_SH.format(version=version)); (out / 'run.sh').chmod(0o755)
    (out / 'push.sh').write_text(PUSH_SH.format(gb=gb)); (out / 'push.sh').chmod(0o755)
    return gb


def main(argv):
    args = parse_args(argv)
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    for p in (game, keys):
        if not p.is_file():
            raise BuildError(f'no such file: {p}')
    if lib.input_kind(game) not in ('xci', 'nsp'):
        raise BuildError('the base game must be an uncompressed XCI or NSP; NSZ is supported for update packages')
    update = args.update.expanduser().resolve() if args.update else None
    work = (args.work.expanduser().resolve() if args.work else game.parent / 'TOTK_build')
    out = (args.out.expanduser().resolve() if args.out else game.parent / 'TOTK_android')
    firmware = args.firmware.expanduser().resolve() if args.firmware else None
    shared_fonts = args.shared_fonts.expanduser().resolve() if args.shared_fonts else None
    py, llvm = prerequisites(args)
    ndk = Path(args.ndk).expanduser().resolve()
    logs = work / 'logs'
    env = dict(os.environ)
    t0 = time.time()
    tree = lib.prepare_work(work, py, logs)
    if args.runtime_only:
        return runtime_only(args, tree, work, out, ndk, logs, env, t0)
    if lib.identify_game(tree, game, keys, logs) != 'totk':
        raise BuildError('the Android build is for Tears of the Kingdom only')
    version = 'v0'
    if update:
        version, update = lib.identify_update(tree, update, keys, logs)
    say('1/4 code: extracting and checking the code modules (the verified Apple-silicon rewrite proves the dump)')
    lib.step_code(tree, game, keys, env, logs)
    if update:
        lib.step_code(tree, update, keys, env, logs, version=version)
    rewritten = android_rewrite(tree, version, env, logs, llvm, equiv=args.equiv)
    say('3/4 data: RomFS, shared fonts and overlays')
    lib.step_data(tree, game, keys, logs, firmware=firmware, shared_fonts=shared_fonts)
    if update:
        lib.step_update_data(tree, game, update, keys, logs, version=version)
    seed = step_shaders(args, tree, version, logs) if args.shaders else None
    gen = tree / f'build/android-{version}'
    generated_headers(tree, version, rewritten, gen, logs)
    opus = build_opus(work, ndk, args.api, logs)
    vpx = None if args.no_vpx else build_vpx(work, ndk, args.api, logs)
    say('4/4 runtime: building totk-native with the NDK')
    make = ['make', '-C', tree / 'android', 'TARGET=android', f'NDK={ndk}', f'API={args.api}', f'VERSION={version}',
            f'GEN={gen}', f'OUT={gen}', f'OPUS={opus}', *([f'VPX={vpx}'] if vpx else []), f'GPU={args.gpu}', f'-j{os.cpu_count() or 4}']
    run(make, logs / f'make_android_{version}.log', env=env)
    if seed:
        place_shaders(seed, out, version)
    gb = assemble(tree, version, rewritten, gen / 'totk-native', out)
    if args.apk:
        build_app(tree, version, gen, ndk, args.api, opus, vpx, args.gpu, work, out, logs, env)
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out} ({gb:.0f} GB)')
    say('Connect the device with USB debugging on, then: sh push.sh   and   adb shell sh /data/local/tmp/totk/run.sh')


def runtime_only(args, tree, work, out, ndk, logs, env, t0):
    """Rebuild totk-native against the generated headers and rewritten modules of an earlier full build."""
    gens = sorted(p for p in (tree / 'build').glob('android-*') if (p / 'runtime_tables.h').is_file())
    if not gens:
        raise BuildError('--runtime-only needs a full build first (no build/android-*/runtime_tables.h in the work folder)')
    if len(gens) > 1:
        gens = [max(gens, key=lambda p: (p / 'runtime_tables.h').stat().st_mtime)]
    gen = gens[0]; version = gen.name.removeprefix('android-')
    if not (out / 'push.sh').is_file():
        raise BuildError(f'--runtime-only: no earlier output folder at {out}')
    if args.shaders:
        place_shaders(step_shaders(args, tree, version, logs), out, version)
    opus = build_opus(work, ndk, args.api, logs)
    vpx = None if args.no_vpx else build_vpx(work, ndk, args.api, logs)
    say(f'runtime: building totk-native ({version}, GPU {args.gpu}) with the NDK')
    make = ['make', '-C', tree / 'android', 'TARGET=android', f'NDK={ndk}', f'API={args.api}', f'VERSION={version}',
            f'GEN={gen}', f'OUT={gen}', f'OPUS={opus}', *([f'VPX={vpx}'] if vpx else []), f'GPU={args.gpu}', f'-j{os.cpu_count() or 4}']
    run(make, logs / f'make_android_{version}.log', env=env)
    shutil.copy2(gen / 'totk-native', out / 'totk-native')
    if args.apk:
        build_app(tree, version, gen, ndk, args.api, opus, vpx, args.gpu, work, out, logs, env)
    (out / 'run.sh').write_text(RUN_SH.format(version=version)); (out / 'run.sh').chmod(0o755)
    gb = sum(f.stat().st_size for f in out.rglob('*') if f.is_file()) / 2**30
    (out / 'push.sh').write_text(PUSH_SH.format(gb=gb)); (out / 'push.sh').chmod(0o755)
    say(f'done in {time.time() - t0:.0f} s: {out / "totk-native"}. Then: sh {out / "push.sh"} {"shaders" if args.shaders else "runtime"}')
    if args.apk:
        say(f'app: {out / "totk.apk"}. Install: sh {out / "install_app.sh"}   (the game folder once: sh {out / "install_app.sh"} from-device)')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nbuild stopped: {e}', file=sys.stderr)
        sys.exit(1)
