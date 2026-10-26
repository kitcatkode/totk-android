#!/usr/bin/env python3
"""The build steps android/build_android.py shares with the original Mac build: game identification, ExeFS and code
module extraction checked against assets/expected-v<version>.json (the Apple-silicon rewrite is made as a check: matching
the verified hashes proves the dump and the extraction), RomFS, the shared fonts and update merging (hactool, fetched at
a pinned commit). Nothing from the game is in this repository; key values are never printed."""
import argparse, concurrent.futures, hashlib, json, os, platform, shutil, struct, subprocess, sys, threading, time
from pathlib import Path

REPO = Path(__file__).resolve().parent

GAME = REPO   # the game folder: this repository's root (the work folder gets a copy of it)

EXPECTED = json.loads((GAME / 'assets/expected-v1.0.0.json').read_text())

UPDATED = json.loads((GAME / 'assets/expected-v1.1.2.json').read_text())

UPDATED121 = json.loads((GAME / 'assets/expected-v1.2.1.json').read_text())

UPDATED143 = json.loads((GAME / 'assets/expected-v1.4.3.json').read_text())

VERSIONS = {'v0': EXPECTED, 'v112': UPDATED, 'v121': UPDATED121, 'v143': UPDATED143}

UPDATES = ('v112', 'v121', 'v143')

TITLES = {'0100F2C0115B6000': 'totk'}

HACTOOL = ('https://github.com/SciresM/hactool.git', '1d64a83450e025622f3468c28fc4164dad2c5ef6')

# Pinned upstream sources fetched at build time (repository, commit)
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165')

# The spirv-cross program: the sources and flags of SPIRV-Cross's own CMake Release build of it, so no cmake is needed
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp',
                       'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp',
                       'spirv_reflect.cpp')

SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')

# Nintendo's public ACID signing key (generation 0): a dump's main.npdm is genuine when this key verifies
# its ACID signature, which is the check the Horizon loader itself makes before it starts a title
# (Atmosphere libraries/libstratosphere/source/fssystem/fssystem_crypto_configuration.cpp).
NINTENDO_ACID_MODULUS = bytes.fromhex(
    'DDC8DDF24E6DF0CA9EC75DC77BADFE7D238969B6F206A20288E15591ABCB4D50'
    '2EFC9D9476D64CD8FF10FA5E930AB457AC51C71666F41A54C2C5043D1BFE3020'
    '8AAC6F6FF5C7B668B8C9406B42AD1121E78BE9750186E4489B0A0AF87FE887F2'
    '8201E6A30FE466AE833F4E9F5E0130A400B99AAE5F03CC1860E5EF3B5E1516FE'
    '1C8278B52F477C0666885D35A2672010E76C4368D3E45A682A5AE26D73B03153'
    '1C200944F51A9D22BE12A17711E2A1CD409AA28B609BEFA0D34863A2F8A32C08'
    '56522E6019675AA79FDC3F3F692B316AB7884A148480333C9D44B73F4CE175EA'
    '37EAE81E7C77B7C61AA2F09F1061CD7B5B324C37EFB17168530AED517D3522FD'
)

class BuildError(Exception):
    pass

def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)

def run(cmd, log, cwd=None, env=None, quiet=True):
    """Run a step with its whole output in log; on failure show the end of it. quiet: a line a minute instead of the output."""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    done = threading.Event()

    def beat():
        t0 = time.time()
        while not done.wait(60):
            say(f'   ... still running ({(time.time() - t0) / 60:.0f} min)')
    if quiet:
        threading.Thread(target=beat, daemon=True).start()
    try:
        with open(log, 'w') as f:
            p = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    finally:
        done.set()
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-25:]
        raise BuildError(f'{Path(cmd[1] if Path(cmd[0]).name.startswith("python") else cmd[0]).name} failed (exit {p.returncode}); '
                         f'full output: {log}\n' + '\n'.join(tail))

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()

def check(what, got, want):
    if got != want:
        raise BuildError(f'{what}: SHA-256 {got[:16]}... is not the expected {want[:16]}... '
                         f'(is this the supported game version, and the dump complete?)')

# ---- genuine dumps ----
def _mgf1_sha256(seed, length):
    blocks = b''
    for counter in range((length + 31) // 32):
        blocks += hashlib.sha256(seed + counter.to_bytes(4, 'big')).digest()
    return blocks[:length]

def _pss_sha256_valid(signature, modulus, message):
    """RSA-2048 PSS/SHA-256 (32-byte salt, MGF1-SHA256), the scheme of an ACID block's signature."""
    size = len(modulus)
    if size < 256 or len(signature) != size:
        return False
    modulus_number = int.from_bytes(modulus, 'big')
    number = int.from_bytes(signature, 'big')
    if number >= modulus_number:
        return False
    encoded = pow(number, 0x10001, modulus_number).to_bytes(size, 'big')
    if encoded[-1] != 0xBC:
        return False
    hashed, mask = encoded[size - 33:size - 1], _mgf1_sha256(encoded[size - 33:size - 1], size - 33)
    db = bytes([(encoded[0] ^ mask[0]) & 0x7F]) + bytes(a ^ b for a, b in zip(encoded[1:size - 33], mask[1:]))
    if db[:-33].strip(b'\x00') or db[-33] != 0x01:
        return False
    return hashlib.sha256(b'\x00' * 8 + hashlib.sha256(message).digest() + db[-32:]).digest() == hashed

def npdm_signed(path):
    """True when the npdm's ACID block carries a valid Nintendo signature over its own access control.

    The pinned SHA-256 identifies the reference dump; the signature identifies every genuine dump of the
    version, which is why this is the one ExeFS file that does not have to match byte for byte.
    """
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b'META' or int.from_bytes(data[0x04:0x08], 'little') != 0:
        return False
    acid, acid_size = struct.unpack_from('<II', data, 0x78)
    if acid_size < 0x200 or acid + acid_size > len(data) or data[acid + 0x200:acid + 0x204] != b'ACID':
        return False
    size = struct.unpack_from('<I', data, acid + 0x204)[0]
    if size < 0x100 or acid + 0x100 + size > len(data):
        return False
    return _pss_sha256_valid(data[acid:acid + 0x100], NINTENDO_ACID_MODULUS, data[acid + 0x100:acid + 0x100 + size])

def npdm_program_id(path):
    """The program ID in a well-formed npdm's ACI0 block (16 upper-case hex digits), else None."""
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b'META':
        return None
    aci, aci_size = struct.unpack_from('<II', data, 0x70)
    if aci_size < 0x20 or aci + aci_size > len(data) or data[aci:aci + 4] != b'ACI0':
        return None
    return f'{struct.unpack_from("<Q", data, aci + 0x10)[0]:016X}'

def npdm_kind(path, title):
    """How a main.npdm that is not the reference dump's file is accepted: 'signed' (Nintendo's ACID signature: another
    genuine dump), 'resigned' (a well-formed npdm of this title whose ACID a dump tool signed again, as tools that change
    an NCA's key generation do), else None. The game never reads main.npdm here, and the code modules are checked by
    hash, so a re-signed one is accepted only alongside matching code (exefs_matches, step_code)."""
    if npdm_signed(path):
        return 'signed'
    return 'resigned' if npdm_program_id(path) == title.upper() else None

def exefs_matches(exefs, expected, title):
    """True when the extracted ExeFS holds the expected code modules and a main.npdm of this version (npdm_kind)."""
    for name, digest in expected.items():
        if name != 'main.npdm' and (not (exefs / name).is_file() or sha256(exefs / name) != digest):
            return False
    npdm = exefs / 'main.npdm'
    return 'main.npdm' not in expected or (npdm.is_file() and (sha256(npdm) == expected['main.npdm'] or npdm_kind(npdm, title) is not None))

def _bfttf_valid(path):
    """A structurally valid shared font, by the same size check scripts/extract_shared_fonts.py makes."""
    data = path.read_bytes()
    if len(data) < 12:
        return False
    magic, size = struct.unpack_from('>II', data)
    key = magic ^ 0x7f9a0218
    if (size ^ key) + 8 != len(data):
        return False
    return struct.unpack_from('>I', data, 8)[0] ^ key in (0x00010000, 0x4F54544F)

# ---- work folder ----
def git_fetch(url, commit, dst, log):
    if (dst / '.git').is_dir() and subprocess.run(['git', '-C', dst, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() == commit:
        return
    if dst.is_symlink():
        dst.unlink()   # replace only the cache link, never its external target
    elif dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, commit], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)

def link(path, target):
    if path.is_symlink() or path.exists():
        if path.is_symlink() and path.resolve() == Path(target).resolve():
            return
        path.unlink() if path.is_symlink() else shutil.rmtree(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.symlink_to(target)

def prepare_work(work, py, logs):
    """work/totk: the game folder of this repository with the generated files inside it (the build scripts write
    extracted/, rewritten/, build/ and analysis/ next to themselves); its tooling/ points at the fetched sources."""
    tree = work / 'totk'
    tree.mkdir(parents=True, exist_ok=True)
    # Match both directories AND symlinks to directories. A trailing slash
    # allows rsync -a to copy a tooling symlink from a development checkout.
    excludes = [f'--exclude=/{name}' for name in ('extracted', 'rewritten', 'build', 'analysis', '.venv', 'tooling', 'user-data', '.git', 'runs', 'shots', 'passdump')]
    run(['rsync', '-a', *excludes, f'{GAME}/', f'{tree}/'], logs / 'sources.log')
    tooling = tree / 'tooling'
    # Migrate a work folder made by the old copier before writing child links:
    # link(tooling/spirv-cross, ...) must not traverse into a shared checkout.
    if tooling.is_symlink():
        tooling.unlink()
    tooling.mkdir(exist_ok=True)
    src = work / 'src'
    say('fetching the pinned SPIRV-Cross sources')
    git_fetch(*SPIRV_CROSS, src / 'SPIRV-Cross', logs / 'fetch-spirv-cross.log')
    link(tooling / 'spirv-cross', src / 'SPIRV-Cross')
    venv = work / 'venv'
    if not (venv / 'bin/python').exists():
        say('Python environment for the build tools')
        run([py, '-m', 'venv', venv], logs / 'venv.log')
    run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', tree / 'requirements.txt'], logs / 'pip.log')
    link(tree / '.venv', venv)
    (tree / 'analysis').mkdir(exist_ok=True)
    (tree / 'build').mkdir(exist_ok=True)
    return tree

# ---- steps ----
def input_kind(game):
    with game.open('rb') as f:
        if f.read(4) == b'PFS0':
            return 'nsz' if game.suffix.lower() == '.nsz' else 'nsp'
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            return 'xci'
    raise BuildError('expected an XCI or NSP (PFS0) game file, or a supported NSZ update')

def source_matches(manifest, game):
    try:
        data = json.loads(manifest.read_text())
        stat = game.stat()
        return (data['source'] == str(game.resolve()) and data['source_size'] == stat.st_size
                and data['source_mtime_ns'] == stat.st_mtime_ns)
    except (OSError, ValueError, KeyError):
        return False

def prepare_update_container(tree, game, keys, logs):
    """Expand and cache a compressed update using NSZ's hash-verified NCA conversion."""
    if game.suffix.lower() != '.nsz':
        return game
    folder = tree / 'extracted/containers'
    output = folder / f'{game.stem}.nsp'
    manifest = folder / f'{game.stem}.json'
    if source_matches(manifest, game) and output.is_file():
        return output
    folder.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    if manifest.exists():
        manifest.unlink()
    say('1/4 code: decompressing the NSZ update and verifying its NCA hashes')
    run([tree / '.venv/bin/nsz', '-D', '--keys', keys, '-o', folder, game],
        logs / f'decompress-{game.stem}.log', cwd=tree)
    generated = folder / f'{game.stem}.nsp'
    if not generated.is_file():
        raise BuildError(f'NSZ decompressor did not produce an NSP: {generated}')
    if generated != output:
        generated.replace(output)
    stat = game.stat()
    manifest.write_text(json.dumps(dict(source=str(game.resolve()), source_size=stat.st_size,
                                        source_mtime_ns=stat.st_mtime_ns), indent=2) + '\n')
    return output

def identify_update(tree, game, keys, logs):
    """Choose the runtime by verified module bytes, never by the package filename."""
    source = prepare_update_container(tree, game, keys, logs)
    for version in UPDATES:
        exefs = tree / f'extracted/{version}/exefs'
        if source_matches(exefs.parent / 'extraction-manifest.json', source) and exefs_matches(
                exefs, VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id']):
            return version, source
    staging = tree / 'extracted/update-detection'
    if staging.exists(): shutil.rmtree(staging)
    run([tree / '.venv/bin/python', 'scripts/extract_exefs.py', source, '--keys', keys,
         '--update', '--output', staging / 'exefs'], logs / 'identify-update.log', cwd=tree)
    version = next((version for version in UPDATES
                    if exefs_matches(staging / 'exefs', VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id'])), None)
    if version is None:
        raise BuildError('unsupported update module hashes; expected 1.1.2, 1.2.1 or 1.4.3 (the file name is not used to choose a version)')
    target = tree / f'extracted/{version}'
    target.mkdir(parents=True, exist_ok=True)
    if (target / 'exefs').exists(): shutil.rmtree(target / 'exefs')
    (staging / 'exefs').rename(target / 'exefs')
    (staging / 'extraction-manifest.json').replace(target / 'extraction-manifest.json')
    staging.rmdir()
    say(f'update identified by verified ExeFS hashes: {VERSIONS[version]["display_version"]}')
    return version, source

def font_report(folder):
    """The fonts bound into the runtime: the pinned hashes identify the reference dump, and a valid font from
    another firmware version is bound by the SHA-256 of its actual bytes (the manifest keeps that value, so
    scripts/prepare_shared_fonts.py writes the real bytes into the runtime)."""
    rows = []
    for kind, (name, expected) in enumerate(EXPECTED['shared_fonts_sha256'].items()):
        path = folder / f'{kind}-{name}.bfttf'
        if not path.is_file():
            raise BuildError(f'missing shared font: {path}')
        digest = sha256(path)
        if digest != expected:
            if not _bfttf_valid(path):
                raise BuildError(f'shared font {name}: not the expected bytes and not a valid BFTTF font '
                                 f'(SHA-256 {digest[:16]}...)')
            say(f'2/4 data: shared font {name} comes from another firmware version; its bytes are bound instead')
        size = path.stat().st_size
        rows.append(dict(type=kind, name=name, bfttf_bytes=size, font_bytes=size - 8, sha256=digest))
    return dict(status='pass', integrity='Each shared font is a valid BFTTF; the reference dump matches its pinned SHA-256',
                fonts=rows)

def validate_fonts(folder):
    report = font_report(folder)
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        rows = manifest['fonts']
        if manifest['status'] != 'pass' or len(rows) != 6 or any(
                any(row.get(k) != value for k, value in expected.items())
                for row, expected in zip(rows, report['fonts'])):
            raise ValueError('font manifest differs from its files')
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        raise BuildError(f'invalid shared-font manifest in {folder}: {e}') from None

def fonts_valid(folder):
    try:
        validate_fonts(folder)
        return True
    except (BuildError, OSError):
        return False

def step_code(tree, game, keys, env, logs, *, version="v0"):
    expected = VERSIONS[version]
    update_args = [] if version == "v0" else ["--update"]
    py = tree / '.venv/bin/python'
    exefs = tree / f'extracted/{version}/exefs'
    want = expected['exefs_sha256']
    manifest = exefs.parent / 'extraction-manifest.json'
    if not source_matches(manifest, game) or not exefs_matches(exefs, want, expected['title_id']):
        say('1/4 code: extracting the ExeFS from the game file')
        if exefs.exists():
            shutil.rmtree(exefs)
        source = prepare_update_container(tree, game, keys, logs) if update_args else game
        run([py, 'scripts/extract_exefs.py', source, '--keys', keys, '--output', exefs, *update_args], logs / f'extract_exefs-{version}.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing; not the supported version')
        got = sha256(exefs / n)
        if got == h:
            continue
        kind = npdm_kind(exefs / n, expected['title_id']) if n == 'main.npdm' else None
        if kind == 'signed':
            say(f"1/4 code: main.npdm is a Nintendo-signed {expected['display_version']} file from another dump; using it")
            continue
        if kind == 'resigned':   # the code modules are checked in this same loop: any mismatch there still stops the build
            say(f"1/4 code: main.npdm of this dump was signed again by a dump tool (not Nintendo's signature, e.g. a changed "
                f"key generation); the game does not use it and the code must match {expected['display_version']}; using it")
            continue
        check(f'ExeFS {n}', got, h)
    if not (tree / f'extracted/{version}/elf/main-inventory.json').exists():
        run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / f'extracted/{version}/elf'], logs / 'prepare_modules.log', cwd=tree)
    out = tree / f'rewritten/{version}'
    out.mkdir(parents=True, exist_ok=True)
    for name in ('sdk', 'subsdk0', 'main'):
        image, h = out / f'{name}.patched.bin', expected['rewritten_sha256'][f'{name}.patched.bin']
        if image.exists() and sha256(image) == h:
            continue
        say(f'1/4 code: rewriting {name} for Apple silicon' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite_{version}_{name}.log', cwd=tree,
            env=dict(env, DM_RULESET='native', DM_PRIVATE_GATES='0'))
        check(f'rewritten {name}', sha256(image), h)
    # The loader binds itself to these images (scripts/prepare_runtime.py): they are the same bytes as the rewrite that
    # was verified site by site (python3 scripts/verify_rewrites.py re-runs that verification, hours).
    report = dict(format=1, status='pass', rom_version=version, scope=f'hash match with verified {expected["display_version"]} rewrite',
                  module_file_sha256={**{f'extracted/{version}/exefs/{n}': want[n] for n in ('main', 'sdk', 'subsdk0')},
                                      **{f'rewritten/{version}/{n}.patched.bin': expected['rewritten_sha256'][f'{n}.patched.bin'] for n in ('main', 'sdk', 'subsdk0')}})
    report_path = tree / ('analysis/phase12-validation.json' if version == 'v0' else f'analysis/{version}-port/phase12-validation.json')
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')

def step_data(tree, game, keys, logs, *, firmware=None, shared_fonts=None):
    py = tree / '.venv/bin/python'
    romfs = tree / 'extracted/v0/romfs'
    index = romfs / 'index.json'
    ok = index.exists() and (romfs / 'romfs.bin').is_file() and json.loads(index.read_text()).get('romfs_sha256') == EXPECTED['romfs']['sha256'] and \
        (romfs / 'romfs.bin').stat().st_size == EXPECTED['romfs']['size']
    if not ok:
        say('2/4 data: extracting and verifying the RomFS (17 GB, a minute or two)')
        if romfs.exists():
            shutil.rmtree(romfs)
        run([py, 'scripts/extract_romfs.py', '--keys', keys], logs / 'extract_romfs.log', cwd=tree)
        r = json.loads(index.read_text())
        check('RomFS', r['romfs_sha256'], EXPECTED['romfs']['sha256'])
    fonts = tree / 'extracted/firmware/shared-fonts'
    if not fonts_valid(fonts):
        if shared_fonts:
            # Validate the actual bytes before copying, not just the source's manifest.
            report = font_report(shared_fonts)
            if shared_fonts.resolve() != fonts.resolve():
                if fonts.exists():
                    shutil.rmtree(fonts)
                fonts.mkdir(parents=True)
                for f in report['fonts']:
                    name = f"{f['type']}-{f['name']}.bfttf"
                    shutil.copy2(shared_fonts / name, fonts / name)
            (fonts / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        else:
            if not firmware and input_kind(game) == 'nsp':
                raise BuildError('NSP needs --firmware or --shared-fonts because no valid shared fonts are cached')
            say('2/4 data: extracting the system shared fonts')
            if fonts.exists():
                shutil.rmtree(fonts)
            source_args = ['--firmware', firmware] if firmware else ['--xci', game]
            run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, *source_args, '--output', fonts],
                logs / 'shared_fonts.log', cwd=tree)
        validate_fonts(fonts)
    if not (tree / 'extracted/v0/romfs-patches/manifest.tsv').exists():
        say('2/4 data: RomFS overlays (quick-menu capture timing, game-over retry)')
        run([py, 'scripts/make_romfs_patches.py'], logs / 'romfs_patches.log', cwd=tree)
    if not (tree / 'extracted/v0/romfs-patches-out/2560x1440/manifest.tsv').exists():
        say('2/4 data: RomFS overlay for the 2560x1440 output (key 3 in the game)')
        run([py, 'scripts/make_output_res_overlay.py', '2560x1440'], logs / 'output_res_overlay.log', cwd=tree)

def build_hactool(tree, logs):
    source = tree.parent / 'src/hactool'
    git_fetch(*HACTOOL, source, logs/'fetch-hactool.log')
    # Add an explicit private title-key file, so base NSP and update tickets do not
    # need command-line keys or changes to the player's HOME/.switch files.
    main = source/'main.c'
    text = main.read_text()
    if 'static FILE *mng_open_base(const char *path)' in text and 'fopencookie' not in text:
        run(['git', 'checkout', '--', 'main.c'], logs/'hactool-reset.log', cwd=source)   # an earlier macOS-only patch: redo it portably
        text = main.read_text()
    old = 'FILE *titlekeyfile = open_key_file("title");'
    new = 'FILE *titlekeyfile = getenv("MNG_TITLE_KEYS") ? fopen(getenv("MNG_TITLE_KEYS"), "rb") : open_key_file("title");'
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool title-key hook changed')
        main.write_text(text.replace(old, new))
    # Expose only the bounded Program NCA inside the original XCI/NSP as a
    # seekable FILE. This avoids a second 17 GB base copy during BKTR merging.
    text = main.read_text()
    if '#define _DARWIN_C_SOURCE 1' not in text:
        text = '#define _DARWIN_C_SOURCE 1\n#define _GNU_SOURCE 1\n' + text
        main.write_text(text)
    hook = 'static FILE *mng_open_base(const char *path)'
    if hook not in text:
        helper = r'''
typedef struct { FILE *file; off_t start, size, position; } mng_base_range;
static int mng_base_read(void *cookie, char *buffer, int count) {
    mng_base_range *range = cookie;
    off_t left = range->size - range->position;
    if (left < count) count = (int)left;
    if (!count) return 0;
    if (fseeko(range->file, range->start + range->position, SEEK_SET)) return -1;
    int got = (int)fread(buffer, 1, count, range->file);
    if (!got && ferror(range->file)) return -1;
    range->position += got;
    return got;
}
static off_t mng_base_seek_to(mng_base_range *range, off_t offset, int whence) {
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? range->position : whence == SEEK_END ? range->size : -1;
    if (base < 0 || offset < -base || offset > range->size - base) return -1;
    return range->position = base + offset;
}
#ifdef __APPLE__
static fpos_t mng_base_seek(void *cookie, fpos_t offset, int whence) { return (fpos_t)mng_base_seek_to(cookie, (off_t)offset, whence); }
#else
static ssize_t mng_base_read_gnu(void *cookie, char *buffer, size_t count) { return mng_base_read(cookie, buffer, count > 0x40000000 ? 0x40000000 : (int)count); }
static int mng_base_seek_gnu(void *cookie, off64_t *offset, int whence) { off_t r = mng_base_seek_to(cookie, (off_t)*offset, whence); if (r < 0) return -1; *offset = r; return 0; }
#endif
static int mng_base_close(void *cookie) {
    mng_base_range *range = cookie;
    int result = fclose(range->file); free(range); return result;
}
static FILE *mng_open_base(const char *path) {
    const char *start = getenv("MNG_BASE_RANGE_OFFSET"), *size = getenv("MNG_BASE_RANGE_SIZE");
    if (!start && !size) return fopen(path, "rb");
    if (!start || !size) return NULL;
    char *end1, *end2;
    unsigned long long a = strtoull(start, &end1, 10), n = strtoull(size, &end2, 10);
    if (!*start || !*size || *end1 || *end2 || a > INT64_MAX || !n || n > INT64_MAX - a) return NULL;
    mng_base_range *range = calloc(1, sizeof(*range));
    if (!range) return NULL;
    range->file = fopen(path, "rb"); range->start = (off_t)a; range->size = (off_t)n;
    if (!range->file) { free(range); return NULL; }
    if (fseeko(range->file, 0, SEEK_END) || ftello(range->file) < (off_t)(a + n)) { mng_base_close(range); return NULL; }
#ifdef __APPLE__
    FILE *view = funopen(range, mng_base_read, NULL, mng_base_seek, mng_base_close);
#else
    FILE *view = fopencookie(range, "rb", (cookie_io_functions_t){mng_base_read_gnu, NULL, mng_base_seek_gnu, mng_base_close});
#endif
    if (!view) mng_base_close(range);
    return view;
}
'''
        marker = 'int main(int argc, char **argv)'
        if text.count(marker) != 1 or text.count('nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")') != 2:
            raise BuildError('pinned hactool bounded base-file hook changed')
        text = text.replace(marker, helper + '\n' + marker)
        text = text.replace('nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")',
                            'nca_ctx.tool_ctx->base_file = mng_open_base(optarg)')
        main.write_text(text)
    # Some newer cumulative update NCAs leave sector-aligned padding after the
    # BKTR subsection table. The relocation and subsection tables still have to
    # be contiguous and remain inside the section; IVFC verification below
    # validates the reconstructed RomFS data. Keep hactool's strict default for
    # the table boundary and permit only this unused trailing section space.
    nca = source/'nca.c'
    text = nca.read_text()
    old = ('sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n'
           '            sb->subsection_header.offset + sb->subsection_header.size != ctx->size')
    new = ('sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n'
           '            sb->subsection_header.offset + sb->subsection_header.size > ctx->size ||\n'
           '            (ctx->size - (sb->subsection_header.offset + sb->subsection_header.size)) % 0x200 != 0')
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool BKTR layout check changed')
        nca.write_text(text.replace(old, new))
    text = nca.read_text()
    old = 'if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {'
    new = ('if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset &&\n'
           '            (sb->relocation_header.size > sb->subsection_header.offset ||\n'
           '             ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset - sb->relocation_header.size)) {')
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool BKTR total-size check changed')
        nca.write_text(text.replace(old, new))
    config = source/'config.mk'
    if not config.exists():
        shutil.copy2(source/'config.mk.template', config)
    run(['make', '-j2', 'CC=clang'], logs/'hactool-build.log', cwd=source)
    return source/'hactool'

def step_update_data(tree, game, update, keys, logs, *, version='v112'):
    expected = VERSIONS[version]
    folder = tree/f'extracted/{version}/romfs'
    index = folder/'index.json'
    valid = False
    if index.is_file() and (folder/'romfs.bin').is_file():
        try:
            report = json.loads(index.read_text())
            valid = (report.get('status') == 'pass' and report['romfs_sha256'] == expected['romfs']['sha256']
                     and (folder/'romfs.bin').stat().st_size == expected['romfs']['size'])
        except (ValueError, KeyError):
            pass
    if not valid:
        hactool = build_hactool(tree, logs)
        if folder.exists():
            shutil.rmtree(folder)
        say(f'2/4 update: merging base + {expected["display_version"]} and verifying all IVFC levels')
        run([tree/'.venv/bin/python', 'scripts/extract_update_romfs.py', game, update,
             '--keys', keys, '--hactool', hactool, '--output', folder], logs/'update-romfs.log', cwd=tree)
        report = json.loads(index.read_text())
        check(f'{expected["display_version"]} RomFS', report['romfs_sha256'], expected['romfs']['sha256'])
        check(f'{expected["display_version"]} RomFS size', report['romfs_size'], expected['romfs']['size'])
    steps = [('make_romfs_patches.py', []), ('make_output_res_overlay.py', ['2560x1440'])]
    if version == 'v143':   # a TOTK_RENDER_RES scene above 1600x900 grows the same heaps (runtime/game_patches_v143.c)
        steps += [('make_output_res_overlay.py', [canvas, '--scene', scene])
                  for canvas in ('1920x1080', '2560x1440') for scene in ('1920x1080', '2560x1440')]
    for i, (script, arguments) in enumerate(steps):
        run([tree/'.venv/bin/python', 'scripts/'+script, *arguments, '--version', version],
            logs/(version+'-'+script+(f'-{i}' if i > 1 else '')+'.log'), cwd=tree)

def last_json(log):
    """The tool's report: its last line that is a JSON object."""
    for line in reversed(log.read_text(errors='replace').splitlines()):
        if line.startswith('{'):
            try: return json.loads(line)
            except ValueError: pass
    return None

def identify_game(tree, game, keys, logs):
    """'totk' from the title IDs of the NCAs in the game file (scripts/identify_game.py); the name is not used."""
    log = logs / 'identify-game.log'
    run([tree / '.venv/bin/python', 'scripts/identify_game.py', game, '--keys', keys], log, cwd=tree, quiet=False)
    report = last_json(log)
    if not report:
        raise BuildError(f'could not read the game file (see {log})')
    found = sorted({TITLES[t] for t in report['titles'] if t in TITLES})
    if len(found) != 1:
        raise BuildError(f'the game file holds {report["titles"]}: expected Tears of the Kingdom (0100F2C0115B6000)')
    return found[0]
