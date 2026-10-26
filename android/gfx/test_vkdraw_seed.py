#!/usr/bin/env python3
"""Shader seed for android/gfx/test_vkdraw.c: two shaders written in GLSL with the Ryujinx translator's resource names,
compiled to SPIR-V with glslangValidator and reflected by android/vk_reflect.py, filed under the keys the test's
synthetic NVN shader binaries hash to (SHA-256 of the 0x880-byte control section and the code area, as the runtime).
usage: test_vkdraw_seed.py <output folder>"""
import hashlib, json, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vk_reflect

VS = '''#version 450
layout(std140, set=0, binding=0) uniform support_buffer_t { vec4 alpha; } support_buffer;
layout(std140, set=0, binding=1) uniform vp_c3_t { vec4 off; } vp_c3;
layout(location=0) in vec2 pos;
void main() { gl_Position = vec4(pos + vp_c3.off.xy, 0.0, 1.0); }
'''
FS = '''#version 450
layout(std140, set=0, binding=0) uniform support_buffer_t { vec4 x; } support_buffer;
layout(std140, set=0, binding=1) uniform fp_c3_t { vec4 color; } fp_c3;
layout(set=0, binding=2) uniform sampler2D fp_t_tcb_5A;
layout(location=0) out vec4 o0;
void main() { o0 = texture(fp_t_tcb_5A, gl_FragCoord.xy / 64.0) * fp_c3.color; }
'''


def shader_binary(stage, salt):
    """The test's NVN shader: control section (stage at 0x714, code length at 0x704, c1 offset at 0x700) and code."""
    code = bytes([salt]) * 0x30 + bytes((i * 7 + salt) & 255 for i in range(0x200))
    control = bytearray(0x880)
    control[0x20:0x24] = (0x880).to_bytes(4, 'little')
    control[0x700:0x704] = len(code).to_bytes(4, 'little')
    control[0x704:0x708] = len(code).to_bytes(4, 'little')
    control[0x714:0x718] = stage.to_bytes(4, 'little')
    return bytes(control), code


def key_of(control, code):
    return hashlib.sha256(control + code).hexdigest()


def main(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for stage, src, salt, ext in ((0, VS, 1, 'vert'), (1, FS, 2, 'frag')):
            control, code = shader_binary(stage, salt)
            key = key_of(control, code)
            variant = f'{key}-000000000000'
            g = Path(tmp) / f's.{ext}'; g.write_text(src)
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', str(g), '-o', str(out / f'{variant}.spv')], check=True, stdout=subprocess.DEVNULL)
            r = vk_reflect.reflect((out / f'{variant}.spv').read_bytes(), {'sbufs': []}, stage)
            (out / f'{variant}.vk').write_text(vk_reflect.vk_text(r, stage, ''))
            (out / f'{key}.vv').write_text(f'{variant}\t\n')
            print(stage, key)


if __name__ == '__main__':
    main(sys.argv[1])
