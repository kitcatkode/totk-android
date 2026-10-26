#!/usr/bin/env python3
"""Vulkan reflection for the Android shader seed (android/build_android.py place_shaders).

The seed (scripts/prepare_shader_seed.py --keep-spirv) keeps every variant's Vulkan SPIR-V (<variant>.spv), the
translator's resource list (<variant>.res.json) and the Mac reflection (<variant>_b.json, whose binding ids are Metal
argument-buffer ids). This reads the SPIR-V itself, classifies every descriptor the way the translation service's
Reflect() classifies the MSL (tools/totk_xlate/Service.cs; the names are Ryujinx's: <stage>_c<n> uniform
banks, <stage>_s<n> storage buffers, <stage>_t_tcb_<handle> textures, <stage>_s_tcb_<handle> separate samplers,
<stage>_i_..._<handle>_<format> images, support_buffer), and writes for the device:

  <variant>.vk    one text file per variant (format below; android/gfx/vk_shader.c reads it)
  <key>.vv        the shader's variants: one line per variant, "<variant>\\t<dependencies>"

.vk format, one record per line:
  vk 1
  stage <0|1|5>                      NVN stage: vertex, fragment, compute
  local <x> <y> <z>                  compute thread group size
  c1 <hex>                           the shader's own constant bank (c1), may be empty
  vin <loc>:<class> ...              vertex inputs (location = NVN attribute; class 0 float, 1 uint, 2 sint)
  fout <idx> ...                     fragment colour outputs
  depth <0|1>                        the fragment stage writes depth
  b <binding> <count> <vk descriptor type> <kind> <reg> <sreg> <off> <rw> <target> <int>

kinds (vk_int.h RB_*): 1 support, 2 cbuf, 3 c1, 4 drv0, 5 drv2, 6 sbuf, 7 sbuf_ubo, 8 ubo_ptr, 9 tex, 10 tbuf, 11 img,
12 samp, 13 texsamp (combined image + sampler; sreg -1: the sampler comes from the texture's combined handle).
target: NVN texture target of the declared image (for empty-slot dummies). Unknown descriptors get kind 0 and are
reported; the device binds a dummy there.

Dependencies (.vv): the answers the variant was translated for, flattened to "kind:slot=value" joined by ";"
(img values "type/fmt"), compared by android/gfx/vk_shader.c exactly as totk_shader_rt.m deps_match does.
"""
import json
import re
import struct
import sys
from pathlib import Path

TEX_BASE, SAMP_BASE, IMG_BASE, COMBINED_BASE, COMBINED_SLOTS = 0x168, 0x568, 0x120, 0x20, 32
RB = dict(support=1, cbuf=2, c1=3, drv0=4, drv2=5, sbuf=6, sbuf_ubo=7, ubo_ptr=8, tex=9, tbuf=10, img=11, samp=12, texsamp=13)
# VkDescriptorType
SAMPLER, COMBINED, SAMPLED_IMAGE, STORAGE_IMAGE, UNIFORM_TEXEL, STORAGE_TEXEL, UNIFORM_BUFFER, STORAGE_BUFFER = 0, 1, 2, 3, 4, 5, 6, 7


# ---------- SPIR-V ----------
class Spirv:
    def __init__(self, data):
        if len(data) < 20 or len(data) % 4:
            raise ValueError('not SPIR-V')
        w = struct.unpack(f'<{len(data) // 4}I', data)
        if w[0] != 0x07230203:
            raise ValueError('bad SPIR-V magic')
        self.names, self.decos, self.types, self.consts, self.vars = {}, {}, {}, {}, []
        self.exec_model, self.local, self.member_decos = None, None, {}
        i = 5
        while i < len(w):
            op, n = w[i] & 0xffff, w[i] >> 16
            if n == 0:
                raise ValueError('bad SPIR-V instruction length')
            a = w[i + 1:i + n]
            if op == 5:   # OpName
                self.names[a[0]] = self.string(a[1:])
            elif op == 71:   # OpDecorate
                self.decos.setdefault(a[0], {})[a[1]] = list(a[2:])
            elif op == 72:   # OpMemberDecorate
                self.member_decos.setdefault(a[0], {}).setdefault(a[1], {})[a[2]] = list(a[3:])
            elif op == 15:   # OpEntryPoint
                self.exec_model = a[0]
            elif op == 16 and a[1] == 17:   # OpExecutionMode LocalSize
                self.local = tuple(a[2:5])
            elif op == 43:   # OpConstant
                self.consts[a[1]] = a[2]
            elif op == 59:   # OpVariable
                self.vars.append((a[1], a[0], a[2]))   # id, pointer type, storage class
            elif op in (19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 32):
                self.types[a[0]] = (op, list(a[1:]))
            i += n

    @staticmethod
    def string(words):
        b = b''.join(struct.pack('<I', x) for x in words)
        return b.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def deco(self, id_, d):
        v = self.decos.get(id_, {}).get(d)
        return v[0] if v else None

    def has_deco(self, id_, d):
        return d in self.decos.get(id_, {})

    def pointee(self, ptr):
        op, a = self.types[ptr]
        assert op == 32   # OpTypePointer
        return a[1]

    def strip_array(self, t):
        count = 1
        op, a = self.types[t]
        if op == 28:   # OpTypeArray
            count = self.consts.get(a[1], 1); t = a[0]
        elif op == 29:   # OpTypeRuntimeArray
            count = 0; t = a[0]
        return t, count

    def scalar_class(self, t):
        """0 float, 1 uint, 2 sint for a scalar / vector type"""
        op, a = self.types[t]
        if op == 23:   # vector
            return self.scalar_class(a[0])
        if op == 21:   # OpTypeInt width signedness
            return 2 if a[1] else 1
        return 0


# NVN texture target of an image type (dim, arrayed, ms), as nvn_mtl.m target_name in reverse
def image_target(dim, arrayed, ms):
    if dim == 5:   # Buffer
        return 10
    if dim == 0:
        return 3 if arrayed else 0
    if dim == 2:
        return 2
    if dim == 3:
        return 9 if arrayed else 8
    if ms:
        return 6 if arrayed else 5
    return 4 if arrayed else 1


# ---------- the translator's name decoding (Service.cs DecodeTex / Slot) ----------
def slot(word, base, cb):
    return (word * 4 - base) // 8 if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None


def decode_tex(name, kind_letter='[ti]'):
    m = re.match(r'^[a-z]+_' + kind_letter + r'\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m:
        return None
    slots = int(m.group(1)) if m.group(1) else -1
    h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2
    sb = (slots >> 16) - 1 if slots >= 0 and (slots >> 16) != 0 else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    words = [o0, o1] if ty else [o0]
    if kind_letter == 'i' or name.split('_', 2)[1].startswith('i'):
        t = next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < TEX_BASE and slot(x, IMG_BASE, tb) is not None), None)
        return t, None, ty
    t = next((slot(x, TEX_BASE, tb) for x in words if slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    if t is None:
        t = next((slot(x, COMBINED_BASE, tb) for x in words
                  if COMBINED_BASE <= x * 4 < COMBINED_BASE + 8 * COMBINED_SLOTS and slot(x, COMBINED_BASE, tb) is not None), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty


STAGE_BASE = {0: 0x110, 5: 0x310, 1: 0x510}


def reflect(spv_bytes, res, stage):
    """The descriptor records and stage facts of one variant."""
    sp = Spirv(spv_bytes)
    binds, unknown = [], []
    sbufs = sorted(res.get('sbufs', []), key=lambda b: b['binding'])
    vin, fout, depth = [], [], 0
    for vid, ptr, sc in sp.vars:
        name = sp.names.get(vid, '')
        t = sp.pointee(ptr)
        if sc == 1:   # Input
            if stage == 0 and sp.deco(vid, 30) is not None and not sp.has_deco(vid, 11):
                vin.append((sp.deco(vid, 30), sp.scalar_class(sp.strip_array(t)[0])))
            continue
        if sc == 3:   # Output
            if stage == 1 and sp.deco(vid, 30) is not None and not sp.has_deco(vid, 11):
                fout.append(sp.deco(vid, 30))
            if stage == 1 and sp.deco(vid, 11) == 22:   # BuiltIn FragDepth
                depth = 1
            continue
        if sc not in (0, 2, 12):   # UniformConstant, Uniform, StorageBuffer
            continue
        binding = sp.deco(vid, 33)
        if binding is None:
            continue
        inner, count = sp.strip_array(t)
        op, a = sp.types[inner]
        target, is_int = 1, 0
        if sc in (2, 12):
            block = sp.has_deco(inner, 2)
            dtype = UNIFORM_BUFFER if sc == 2 and block else STORAGE_BUFFER
        elif op == 26:   # OpTypeSampler
            dtype = SAMPLER
        elif op == 27:   # OpTypeSampledImage
            iop, ia = sp.types[a[0]]
            target = image_target(ia[1], ia[3], ia[4]); is_int = sp.scalar_class(ia[0]) != 0
            dtype = UNIFORM_TEXEL if ia[1] == 5 else COMBINED
        elif op == 25:   # OpTypeImage: sampled type, dim, depth, arrayed, ms, sampled, format
            target = image_target(a[1], a[3], a[4]); is_int = sp.scalar_class(a[0]) != 0
            if a[1] == 5:
                dtype = UNIFORM_TEXEL if a[5] == 1 else STORAGE_TEXEL
            else:
                dtype = SAMPLED_IMAGE if a[5] == 1 else STORAGE_IMAGE
        else:
            continue
        rec = dict(binding=binding, count=count if count else 32, dtype=dtype, kind=0, reg=-1, sreg=-1, off=0, rw=0,
                   target=target, int=int(bool(is_int)), name=name)
        m = re.match(r'^[a-z]+_c(\d+)$', name)
        ms = re.match(r'^[a-z]+_s(\d+)$', name)
        if name == 'support_buffer':
            rec['kind'] = RB['support']
        elif m and dtype == UNIFORM_BUFFER:
            n = int(m.group(1))
            if 3 <= n <= 16:
                rec.update(kind=RB['cbuf'], reg=n - 3)
            elif n == 1:
                rec['kind'] = RB['c1']
            elif n == 0:
                rec['kind'] = RB['drv0']
            elif n == 2:
                rec['kind'] = RB['drv2']
        elif ms and dtype == STORAGE_BUFFER:
            index = int(ms.group(1))
            b = sbufs[index] if index < len(sbufs) else None
            cb_slot, cb_off = (b['cb_slot'], b['cb_offset']) if b else (0, 0)
            off = cb_off * 4 if b and cb_slot == 0 else None
            rec['rw'] = int(bool(b and b.get('flags', 0) & 1))
            if stage == 5 and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0:
                rec.update(kind=RB['ubo_ptr'], reg=(off - 0x210) // 16)
            elif b and 3 <= cb_slot <= 16:
                rec.update(kind=RB['sbuf_ubo'], reg=cb_slot - 3, off=cb_off * 4)
            else:
                base = STAGE_BASE.get(stage)
                if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0:
                    rec.update(kind=RB['sbuf'], reg=(off - base) // 16)
        elif dtype == SAMPLER:
            d = decode_tex(name, 's')
            if d:
                t_, s_, ty = d
                if ty and s_ is not None:
                    rec.update(kind=RB['samp'], reg=s_)
                elif t_ is not None:
                    rec.update(kind=RB['samp'], reg=t_, sreg=-2)   # sampler of a combined handle in texture slot reg
        elif dtype in (COMBINED, SAMPLED_IMAGE, UNIFORM_TEXEL):
            d = decode_tex(name)
            if d and d[0] is not None:
                t_, s_, ty = d
                if dtype == COMBINED:
                    rec.update(kind=RB['texsamp'], reg=t_, sreg=s_ if ty and s_ is not None else -1)
                elif dtype == UNIFORM_TEXEL:
                    rec.update(kind=RB['tbuf'], reg=t_)
                else:
                    rec.update(kind=RB['tex'], reg=t_)
        elif dtype in (STORAGE_IMAGE, STORAGE_TEXEL):
            d = decode_tex(name, 'i')
            if d and d[0] is not None:
                rec.update(kind=RB['img'], reg=d[0], rw=1)
        if not rec['kind']:
            unknown.append(f'{name} (binding {binding}, type {dtype})')
        binds.append(rec)
    local = sp.local or (1, 1, 1)
    return dict(binds=binds, vin=sorted(vin), fout=sorted(set(fout)), depth=depth, local=local, unknown=unknown)


def vk_text(r, stage, c1):
    lines = ['vk 1', f'stage {stage}', 'local %d %d %d' % tuple(r['local']), f'c1 {c1}',
             'vin ' + ' '.join(f'{l}:{c}' for l, c in r['vin']), 'fout ' + ' '.join(str(f) for f in r['fout']), f'depth {r["depth"]}']
    for b in sorted(r['binds'], key=lambda b: b['binding']):
        lines.append('b %d %d %d %d %d %d %d %d %d %d' % (b['binding'], b['count'], b['dtype'], b['kind'], b['reg'], b['sreg'],
                                                           b['off'], b['rw'], b['target'], b['int']))
    return '\n'.join(lines) + '\n'


def flat_deps(deps):
    parts = []
    for kind in ('attr', 'fout', 'img', 'tex'):
        for slot_, v in sorted((deps.get(kind) or {}).items(), key=lambda kv: int(kv[0])):
            if isinstance(v, dict):
                v = v.get('type', '') + ('/' + v['fmt'] if v.get('fmt') else '')
            parts.append(f'{kind}:{slot_}={v}')
    return ';'.join(parts)


STAGES = {'vs': 0, 'ps': 1, 'cs': 5}


def convert(seed, out):
    """Every variant of the seed: <variant>.vk next to its .spv in out, and a <key>.vv per shader."""
    out.mkdir(parents=True, exist_ok=True)
    n = bad = 0
    unknown_total = {}
    for vfile in sorted(seed.glob('*.variants')):
        key = vfile.name[:-len('.variants')]
        rows = []
        for v in json.loads(vfile.read_text()):
            variant = v['variant']
            spv, res, refl = seed / f'{variant}.spv', seed / f'{variant}.res.json', seed / f'{variant}_b.json'
            if not (spv.is_file() and res.is_file() and refl.is_file()):
                bad += 1
                continue
            b = json.loads(refl.read_text())
            stage = STAGES[b['stage']]
            r = reflect(spv.read_bytes(), json.loads(res.read_text()), stage)
            if b.get('threadgroup_size'):
                r['local'] = tuple(b['threadgroup_size'])
            for u in r['unknown']:
                unknown_total[u.split(' (')[0].split('_', 1)[-1][:24]] = unknown_total.get(u.split(' (')[0].split('_', 1)[-1][:24], 0) + 1
            (out / f'{variant}.vk').write_text(vk_text(r, stage, b.get('c1', '')))
            rows.append(f'{variant}\t{flat_deps(v.get("deps") or {})}')
            n += 1
        (out / f'{key}.vv').write_text('\n'.join(rows) + '\n')
    return n, bad, unknown_total


def main(argv):
    if len(argv) != 3:
        print('usage: vk_reflect.py <seed folder> <output folder>'); return 2
    n, bad, unknown = convert(Path(argv[1]), Path(argv[2]))
    print(f'{n} variants reflected for Vulkan, {bad} incomplete')
    for k, c in sorted(unknown.items(), key=lambda kv: -kv[1])[:20]:
        print(f'  unclassified descriptor {k}: {c} variants')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
