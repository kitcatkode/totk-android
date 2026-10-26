#!/usr/bin/env python3
"""Synthetic Switch modules for testing the rewriter and the native loader without game files.

Builds main / subsdk0 / sdk from the .S files here (clang + ld.lld, aarch64), packs each into an uncompressed NSO,
rewrites it with third_party/d2r/patch_nso.py for the host selected by DM_HOST_TP (tpidr_el0 = Linux / Android,
default tpidrro_el0 = Darwin), runs the structural checker and the emulator equivalence check on it, and writes the
loader's runtime_tables.h for the result.

    make_fixture.py <out dir>          (DM_HOST_TP=tpidr_el0 for Linux / Android)
"""
import hashlib, json, os, struct, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOTK = HERE.parents[1]
D2R = TOTK / 'third_party/d2r'
sys.path.insert(0, str(D2R))
from relr import decode_relr

out = Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
CLANG = os.environ.get('CLANG', 'clang'); LLD = os.environ.get('LLD', 'ld.lld')
NAMES = ['main', 'subsdk0', 'sdk']
host = os.environ.get('DM_HOST_TP', 'tpidrro_el0')


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'command failed: {" ".join(map(str, cmd))}\n{r.stdout}{r.stderr}')
    return r.stdout


def elf_segments(path):
    d = path.read_bytes()
    assert d[:4] == b'\x7fELF' and d[4] == 2 and d[5] == 1
    phoff, = struct.unpack_from('<Q', d, 0x20); phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    loads = []
    for i in range(phnum):
        p_type, flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
        if p_type == 1: loads.append((flags, off, vaddr, filesz, memsz))
    return d, loads


def symbol(path, name):
    for line in run(['llvm-nm', path]).splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == name: return int(f[0], 16)
    raise KeyError(name)


def to_nso(elf, nso, name):
    d, loads = elf_segments(elf)
    assert [l[0] for l in loads] == [5, 4, 6], f'{name}: expected r-x, r--, rw- segments, got {loads}'
    assert loads[0][2] == 0
    img = bytearray(loads[2][2] + loads[2][4])
    for _, off, vaddr, filesz, _ in loads: img[vaddr:vaddr + filesz] = d[off:off + filesz]
    text = (0, loads[0][3]); ro = (loads[1][2], loads[1][3]); data = (loads[2][2], loads[2][3])
    bss = loads[2][4] - loads[2][3]
    # dynstr / dynsym extents (relative to ro), as a real NSO header records them
    dyn = symbol(elf, '_DYNAMIC'); tags = {}
    for o in range(dyn, dyn + 16 * 64, 16):
        t, v = struct.unpack_from('<qQ', img, o)
        if t == 0: break
        tags[t] = v
    nsym = struct.unpack_from('<I', img, tags[4] + 4)[0]
    hdr = bytearray(0x100); hdr[:4] = b'NSO0'
    blobs = [bytes(img[a:a + n]) for a, n in (text, ro, data)]
    fo = 0x100
    for i, ((mo, sz), blob) in enumerate(zip((text, ro, data), blobs)):
        struct.pack_into('<III', hdr, 0x10 + 0x10 * i, fo, mo, sz); fo += len(blob)
    struct.pack_into('<I', hdr, 0x3c, bss)
    hdr[0x40:0x60] = hashlib.sha256(b'totk-android-fixture:' + name.encode() + bytes(img)).digest()
    struct.pack_into('<III', hdr, 0x60, *(len(b) for b in blobs))
    struct.pack_into('<IIII', hdr, 0x90, tags[5] - ro[0], tags[10], tags[6] - ro[0], nsym * 24)
    nso.write_bytes(bytes(hdr) + b''.join(blobs))
    return bytes(img)


env = dict(os.environ, DM_HOST_TP=host)
modules = []
for name in NAMES:
    objs = []
    for src in ('crt0.S', f'{name}.S'):
        o = out / f'{name}.{src[:-2]}.o'
        run([CLANG, '--target=aarch64-linux-gnu', '-c', HERE / src, '-o', o]); objs.append(o)
    elf = out / f'{name}.elf'
    run([LLD, '-shared', '-T', HERE / 'module.ld', '--eh-frame-hdr', '--hash-style=sysv', '-z', 'max-page-size=0x4000',
         '-z', 'norelro', '-z', 'now', '-soname', name, *objs, '-o', elf])
    nso = out / f'{name}.nso'
    to_nso(elf, nso, name)
    log = run([sys.executable, D2R / 'patch_nso.py', nso, out, name], env=env)
    print(log.strip().splitlines()[-1] if name != 'main' else log.strip().splitlines()[0])
    modules.append(name)

# Structural check and emulator equivalence of every rewrite, as verify_rewrites.py runs them on the game's modules.
for name in NAMES:
    r = subprocess.run([sys.executable, D2R / 'structck.py', out, name, out / f'{name}.nso'], capture_output=True, text=True,
                       env=dict(env, LLVM_BIN=os.environ.get('LLVM_BIN', '/usr/bin')))
    print(f'structck {name}:', (r.stdout.strip().splitlines() or ['(no output)'])[-1])
    if r.returncode: sys.exit(r.stdout + r.stderr)
    r = subprocess.run([sys.executable, D2R / 'equiv.py', out, name, out / f'{name}.nso', '--jobs', '1', '--mutate', '20'],
                       capture_output=True, text=True, env=env)
    print(f'equiv {name}:', (r.stdout.strip().splitlines() or ['(no output)'])[-1])
    if r.returncode: sys.exit(r.stdout + r.stderr)

# runtime_tables.h, the same format scripts/prepare_runtime.py writes for the game.
tables = []
for name in NAMES:
    image = (out / f'{name}.patched.bin').read_bytes()
    layout = {}
    for line in (out / f'{name}.fixups.tsv').read_text().splitlines():
        _, key, value = line.split(); layout[key] = int(value, 0)
    mod0 = struct.unpack_from('<I', image, 4)[0]; assert image[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]; tags = {}
    for o in range(dyn, dyn + 16 * 64, 16):
        t, v = struct.unpack_from('<qQ', image, o)
        if t == 0: break
        tags[t] = v
    nsym = struct.unpack_from('<I', image, tags[4] + 4)[0]
    symbols = []
    for i in range(nsym):
        no, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
        end = image.index(0, tags[5] + no)
        symbols.append(dict(name=image[tags[5] + no:end].decode(), value=value, size=size, defined=bool(section), binding=info >> 4, type=info & 15))
    relocs = [dict(where=w, kind=1027, symbol=0, addend=a) for w, a in decode_relr(image, tags, layout['DATA_OFF'], layout['KEY_PAGE_OFF'])]
    for address, size in [(tags.get(7, 0), tags.get(8, 0)), (tags.get(23, 0), tags.get(2, 0))]:
        for o in range(address, address + size, 24):
            where, info, addend = struct.unpack_from('<QQq', image, o)
            relocs.append(dict(where=where, kind=info & 0xffffffff, symbol=info >> 32, addend=addend))
    assert all(r['kind'] in (257, 1025, 1026, 1027) for r in relocs), relocs
    gates = []
    for line in (out / f'{name}.gates.tsv').read_text().splitlines():
        ret, kind, arg, original = line.split()
        gates.append(dict(ret=int(ret, 0), timer=kind != 'svc', arg=int(arg), original=int(original, 0)))
    tables.append(dict(name=name, sha256=hashlib.sha256(image).hexdigest(), size=len(image), layout=layout, tags=tags,
                       symbols=symbols, relocs=relocs, gates=gates))

main = tables[0]
target = '_ZN2nn2os13GetSystemTickEv'
slot = next(r['where'] for r in main['relocs'] if main['symbols'][r['symbol']]['name'] == target and r['kind'] == 1026)
image = (out / 'main.patched.bin').read_bytes(); plt = []
for a in range(0, main['layout']['TEXT_END'] - 12, 4):
    w0, w1, _, w3 = struct.unpack_from('<IIII', image, a)
    if w0 & 0x9f00001f != 0x90000010 or w1 & 0xffc003ff != 0xf9400211 or w3 != 0xd61f0220: continue
    imm = ((w0 >> 29) & 3) | (((w0 >> 5) & 0x7ffff) << 2); imm = (imm ^ (1 << 20)) - (1 << 20)
    if (a & ~4095) + (imm << 12) + ((w1 >> 10) & 0xfff) * 8 == slot: plt.append(a)
assert len(plt) == 1, plt

lines = ['/* Generated by android/fixture/make_fixture.py from synthetic modules (no game data). */']
for n, m in enumerate(tables):
    lines.append(f'static const Reloc reloc_{n}[] = {{'); lines += [f'{{{r["where"]}ULL,{r["kind"]},{r["symbol"]},{r["addend"]}LL}},' for r in m['relocs']]; lines.append('};')
    lines.append(f'static const Symbol symbol_{n}[] = {{')
    lines += ['{' + f'{json.dumps(s["name"])},{s["value"]}ULL,{s["size"]}ULL,{int(s["defined"])},{s["binding"]},{s["type"]}' + '},' for s in m['symbols']]
    lines.append('};')
    lines.append(f'static const Gate gate_{n}[] = {{'); lines += [f'{{{g["ret"]}ULL,{g["original"]}ULL,{int(g["timer"])},{g["arg"]}}},' for g in m['gates']] or ['{0,0,0,0},']; lines.append('};')
lines.append('static Module modules[] = {')
for n, m in enumerate(tables):
    l = m['layout']
    data_end = l['KEY_PAGE_OFF']
    lines.append('{' + f'.name={json.dumps(m["name"])},.digest={json.dumps(m["sha256"])},.file_size={m["size"]}ULL,.text_end={l["TEXT_END"]}ULL,.ro_off={l["RO_OFF"]}ULL,.data_off={l["DATA_OFF"]}ULL,.patch_off={l["PATCH_OFF"]}ULL,.key_off={l["KEY_PAGE_OFF"]}ULL,.symbols=symbol_{n},.nsym={len(m["symbols"])},.relocs=reloc_{n},.nreloc={len(m["relocs"])},.gates=gate_{n},.ngate={len(m["gates"])},.init=0ULL,.ctor_flag={l["DATA_OFF"]}ULL,.init_array=0ULL,.init_size=0ULL' + '},')
lines.append('};')
lines.append(f'#define PROBE_PLT {plt[0]}ULL\n#define PROBE_SLOT {slot}ULL')
(out / 'runtime_tables.h').write_text('\n'.join(lines) + '\n')
print(json.dumps(dict(host=host, modules=[dict(name=m['name'], relocs=len(m['relocs']), gates=len(m['gates']), symbols=len(m['symbols'])) for m in tables], probe_plt=hex(plt[0]))))
