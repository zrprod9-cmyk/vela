#!/usr/bin/env python3
"""Write the three AMD-derived SPIR-V headers of the provider from built SPIR-V modules.
usage: gen_amd_hpp.py fast  <out.hpp> <dir with the Fast modules (<name>.spv)>
       gen_amd_hpp.py fu    <out.hpp> <dir with fu_pre, fu_k1, fu_k1s, fu_k1d, npu_select_fu .spv>
       gen_amd_hpp.py lite  <out.hpp> <dir with 0000..0010, 0013, pre_r2c .spv> <lite_weights.bin> <pass_table.inc>"""
import struct, sys
from pathlib import Path


def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def words(b):
    assert len(b) % 4 == 0 and b[:4] == b'\x03\x02\x23\x07'
    return struct.unpack('<%dI' % (len(b) // 4), b)


def write(path, lines):
    with open(path, 'w', newline='\n') as f:
        f.write(lines)


def fast(out, d):
    mods = [('kFfQ3kbP', 'q3kb_p'), ('kFfQ3kbU', 'q3kb_u'), ('kFfQ3', 'q3'), ('kFfPlkP', 'plk_p'), ('kFfPlkU', 'plk_u'),
            ('kFfQ1mkbP', 'q1mkb_p'), ('kFfQ1mkbU', 'q1mkb_u'), ('kFfQ1m', 'q1m'), ('kFfRhaP', 'rha_p'), ('kFfRhaU', 'rha_u'),
            None, ('kU15P', 'rha15_p'), ('kU15sP', 'rha15s_p'), None, ('kU15C', 'u15c'), ('kU15B', 'u15b')]
    o = ['#pragma once', '#include <cstdint>',
         '// generated SPIR-V words of the FSR 4 Fast kernels, derived from the AMD FSR 4.1.1 INT8 shaders (MIT), FNV-1a64 checked at generation; '
         'see ../../spirv-sources/README.md']
    for i, m in enumerate(mods):
        if m is None:
            o.append('')
            o.append('// Fused P720 + 1.5x K2' if i == 10 else '// Separate 1.5x pass')
            continue
        b = (d / (m[1] + '.spv')).read_bytes()
        o.append('// %s.spv %d B FNV %016x' % (m[1], len(b), fnv(b)))
        o.append('inline constexpr std::uint32_t %s[] = {%s};' % (m[0], ','.join('0x%08xu' % w for w in words(b))))
    write(out, '\n'.join(o) + '\n')


def fu(out, d):
    o = ['#pragma once', '// generated SPIR-V words of the fused prepass / K1 kernels, derived from the AMD FSR 4.1.2 Lite compute shaders (MIT); '
         'see ../../spirv-sources/README.md', '#include <cstdint>', 'namespace fsr4fu_spv {']
    for n in ('fu_pre', 'fu_k1', 'fu_k1s', 'fu_k1d', 'npu_select_fu'):
        o.append('inline constexpr std::uint32_t k_%s[] = {%s};' % (n, ','.join('0x%08x' % w for w in words((d / (n + '.spv')).read_bytes()))))
    o.append('}')
    write(out, '\n'.join(o) + '\n')


def lite(out, d, weights, table):
    o = ['// generated file (do not edit)', '#pragma once', '#include <cstdint>', 'namespace fsr4lite {']

    def arr(name, b):
        b = b + b'\0' * (-len(b) % 4)
        w = [int.from_bytes(b[i:i + 4], 'little') for i in range(0, len(b), 4)]
        o.append('inline constexpr std::uint32_t %s[%d] = {' % (name, len(w)))
        for i in range(0, len(w), 16):
            o.append(','.join('0x%x' % x for x in w[i:i + 16]) + ',')
        o.append('};')
    for i in list(range(11)) + [13]:
        arr('kSpv%02d' % i, (d / ('%04d.spv' % i)).read_bytes())
    arr('kSpvPreR2c', (d / 'pre_r2c.spv').read_bytes())
    arr('kWeights', weights.read_bytes())
    write(out, '\n'.join(o) + '\n' + table.read_text())


if __name__ == '__main__':
    mode, out = sys.argv[1], Path(sys.argv[2])
    if mode == 'fast':
        fast(out, Path(sys.argv[3]))
    elif mode == 'fu':
        fu(out, Path(sys.argv[3]))
    else:
        lite(out, Path(sys.argv[3]), Path(sys.argv[4]), Path(sys.argv[5]))
