#!/usr/bin/env python3
"""Generator of the FSR 4.1.2 Lite pass1 kernel (module 0002, 960x540 -> local 64x2, 32x4 px per group) from the converted AMD module.
Stage 1 3x3 16->16 (zero padding), >>7; stage 2 1x1 16->32 relu >>9; stage 3 1x1 32->16 + centre skip (float); store the full-res plane and
the shared cnb; barrier; tail 2x2/s2 64->32. Stages 1-3 use literal weights; SDot(a,w) == SUDot(w, a^0x80808080) - 128*sum(w) with the
correction folded into the bias. BAR = outputs per block, each block in its own uniform branch.
env TB=1: the tail sub-blocks each get their own uniform branch
usage: gen_conv_p1.py <converted 0002.comp> <lite weights.bin> BAR OUT.comp"""
import re, sys, struct
SRC, WB, BAR, OUT = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
import os
TB = os.environ.get('TB') == '1'   # tail: each 2x2 sub-block in its own uniform branch (stops ir3 hoisting all 128 IB loads)
T = open(SRC).read()
_wb = open(WB, 'rb').read()
IB = struct.unpack('<%dI' % (len(_wb) // 4), _wb)
for s in ['uint _1364 = ((((256u * _1335) + (768u * _1328)) + _1354) + (16u * _1344)) >> 2u;',
          'ivec4 _1577 = ((_1572 + ivec4(63)) + ((_1572 >> ivec4(7)) & ivec4(1))) >> ivec4(7);',
          '_1052 = (ivec2(gl_WorkGroupID.xy * uvec2(32u, 4u)) + (ivec2(_1035, _1034) * ivec2(32, 2))) + ivec2((_1045 * 2) + _1046, _1048);',
          'cnbOutputs[_1035][_1034][_1045][_1046][_1048] = _2744;', 'int _2748 = 8 * (_1046 + (2 * _1048));',
          'uint _2752 = ((4u * _2749) + 6912u) >> 2u;', 'uint _2796 = (_2794 + 4864u) >> 2u;', 'uint _3073 = (_2794 + 5376u) >> 2u;',
          'uint _3331 = (_2794 + 5888u) >> 2u;', 'uint _3589 = (_2794 + 6400u) >> 2u;', 'float _3873 = _3868 ? 0.015625 : 0.0078125;',
          '(_3930.z % 16u)) + 16684928u) >> 2u;', '(8342464u * _3849.w)) + 8342464u);', 'bool _3868 = _2748 < 16;',
          'ivec4 _1066 = ivec4(uvec4(InitializerBuffer._m0[864u]']:
    assert s in T, s
m = re.search(r'const uint _935\[576\] = uint\[\]\(([^)]*)\);', T)
A935 = [int(x.strip().rstrip('u')) for x in m.group(1).split(',')]
W1 = lambda t, o, i: A935[64 * (t % 3) + 192 * (t // 3) + 4 * o + i]
bvec = {}
for n, a in re.findall(r'ivec4 (_\d+) = ivec4\(uvec4\(InitializerBuffer\._m0\[(\d+)u\]', T):
    bvec[n] = int(a)
TOK = re.compile(r'spvSDot_int_uint_uint\((_\d+(?:\[\d\])?), (\d+)u, 0\)|(_\d+)\.([xyzw])\)')


def comps(rhs):
    out, cur = [], []
    for act, w, bn, bc in TOK.findall(rhs):
        if act:
            cur.append((act, int(w)))
        else:
            out.append((cur, bvec[bn] + 'xyzw'.index(bc)))
            cur = []
    assert len(out) == 4, rhs[:80]
    return out


mid = re.findall(r'ivec4 (_\d+) = max\(ivec4\((.*)\), ivec4\(0\)\);\n\s*ivec4 (_\d+) = \(\(\1 \+ ivec4\(255\)\) \+ \(\(\1 >> ivec4\(9\)\) & ivec4\(1\)\)\) >> ivec4\(9\);\n\s*uint (_\d+) = ', T)
assert len(mid) == 8
MW, MB, MN = {}, {}, {}
for g, (a, rhs, b, c) in enumerate(mid):
    MN[c] = g
    for j, (terms, bi) in enumerate(comps(rhs)):
        o = 4 * g + j
        MB[o] = bi
        assert [t[0] for t in terms] == ['_1030[3]', '_1030[2]', '_1030[1]', '_1030[0]']
        for act, w in terms:
            MW[(o, int(act[6]))] = w
st3 = re.findall(r'vec4 (_\d+) = vec4\(ivec4\((.*)\)\);\n\s*ivec4 (_\d+) = ivec4\(roundEven\(\(\(vec4\(ivec4\(int\((_\d+) << 24u\) >> 24, int\(\4 << 16u\) >> 24, int\(\4 << 8u\) >> 24, _2563\.([xyzw]) >> 24\)\) \* 0\.015625\) \+ \(\1 \* 0\.00048828125\)\) \* 32\.0\)\);', T)
assert len(st3) == 4 and [s[4] for s in st3] == list('xyzw')
FW, FB = {}, {}
for g, (a, rhs, b, sk, c) in enumerate(st3):
    for j, (terms, bi) in enumerate(comps(rhs)):
        o = 4 * g + j
        FB[o] = bi
        assert sorted(MN[t[0]] for t in terms) == list(range(8))
        for act, w in terms:
            FW[(o, MN[act])] = w
assert len(MW) == 128 and len(FW) == 128


def sb(w):
    return sum(((w >> (8 * k)) & 255) - (256 if (w >> (8 * k)) & 128 else 0) for k in range(4))


B1 = [(IB[864 + o] - 128 * sum(sb(W1(t, o, i)) for t in range(9) for i in range(4))) & 0xffffffff for o in range(16)]
B2 = [(IB[MB[o]] - 128 * sum(sb(MW[(o, i)]) for i in range(4))) & 0xffffffff for o in range(32)]
B3 = [(IB[FB[o]] - 128 * sum(sb(FW[(o, i)]) for i in range(8))) & 0xffffffff for o in range(16)]
L = []
e = L.append
nb = [0]


def blocks(n):
    gs = BAR or n
    return [range(o0, o0 + gs) for o0 in range(0, n, gs)]


def bopen():
    if BAR:
        nb[0] += 1
        e('    if (gl_WorkGroupID.z != %du) {' % nb[0])


def bclose():
    if BAR:
        e('    }')


def acc4(v, g):
    return 'ivec4(int({0}{1}), int({0}{2}), int({0}{3}), int({0}{4}))'.format(v, 4 * g, 4 * g + 1, 4 * g + 2, 4 * g + 3)


def stage(v, bias, nout, terms, epi):
    for blk in blocks(nout):
        for o in blk:
            e('    uint %s%d = %du;' % (v, o, bias[o]))
        bopen()
        for o in blk:
            for w, a in terms(o):
                e('    %s%d += sud(%du, %s, 0);' % (v, o, w, a))
        if BAR:
            for g in range(blk[0] // 4, blk[-1] // 4 + 1):
                epi(g)
        bclose()
    if not BAR:
        for g in range(nout // 4):
            epi(g)


e('''#version 450
#extension GL_EXT_spirv_intrinsics : require
layout(local_size_x = 64, local_size_y = 2, local_size_z = 1) in;
layout(set = 0, binding = 141, std430) buffer SB4 { uvec4 q[]; } S4;
layout(set = 0, binding = 141, std430) buffer SB1 { uint d[]; } S1;
layout(set = 0, binding = 21, std430) readonly buffer IBB { uint w[]; } IBW;
shared uvec4 cnb[2][16][2][2];
spirv_instruction (extensions = ["SPV_KHR_integer_dot_product"], capabilities = [6019, 6018], id = 4450)
int sdot(uint a, uint b, spirv_literal uint f);
spirv_instruction (extensions = ["SPV_KHR_integer_dot_product"], capabilities = [6019, 6018], id = 4452)
uint sud(uint a, uint b, spirv_literal uint f);
uint pk(ivec4 v) { return ((uint(clamp(v.x, -128, 127)) & 255u) | ((uint(clamp(v.y, -128, 127)) & 255u) << 8u)) | ((uint(clamp(v.z, -128, 127)) & 255u) << 16u) | ((uint(clamp(v.w, -128, 127)) & 255u) << 24u); }
ivec4 rq7(ivec4 a) { return ((a + ivec4(63)) + ((a >> ivec4(7)) & ivec4(1))) >> ivec4(7); }
ivec4 rq9(ivec4 a) { return ((a + ivec4(255)) + ((a >> ivec4(9)) & ivec4(1))) >> ivec4(9); }
void main()
{
    uint LX = gl_LocalInvocationID.x; uint LY = gl_LocalInvocationID.y;
    uint BX = LX >> 2u; uint DX = LX & 1u; uint DY = (LX >> 1u) & 1u;
    int PX = int(gl_WorkGroupID.x * 32u + BX * 2u + DX); int PY = int(gl_WorkGroupID.y * 4u + LY * 2u + DY);''')
for t in range(9):
    dy, dx = divmod(t, 3)
    e('    bool ok%d = all(greaterThanEqual(ivec2(PX + %d, PY + %d), ivec2(0))) && all(lessThan(ivec2(PX + %d, PY + %d), ivec2(960, 540)));'
      % (t, dx - 1, dy - 1, dx - 1, dy - 1))
    e('    uvec4 t%d = ok%d ? S4.q[uint(PX + %d) + 962u * uint(PY + %d)] : uvec4(0u);' % (t, t, dx, dy))
    for i, c in enumerate('xyzw'):
        e('    uint a%d_%d = t%d.%s ^ 0x80808080u;' % (t, i, t, c))
e('    uvec4 ctr = S4.q[uint(PX + 1) + 962u * uint(PY + 1)];')
for g in range(4):
    e('    uint h%d;' % g)
for g in range(8):
    e('    uint m%d;' % g)
for g in range(4):
    e('    uint r%d;' % g)


def ep1(g):
    e('    h%d = pk(rq7(%s)) ^ 0x80808080u;' % (g, acc4('c', g)))


def ep2(g):
    e('    m%d = pk(rq9(max(%s, ivec4(0)))) ^ 0x80808080u;' % (g, acc4('e', g)))


def ep3(g):
    e('    { uint s = ctr.%s; r%d = pk(ivec4(roundEven(((vec4(ivec4(int(s << 24u) >> 24, int(s << 16u) >> 24, int(s << 8u) >> 24, int(s) >> 24)) * 0.015625) + (vec4(%s) * 0.00048828125)) * 32.0))); }'
      % ('xyzw'[g], g, acc4('f', g)))


stage('c', B1, 16, lambda o: [(W1(t, o, i), 'a%d_%d' % (t, i)) for t in range(9) for i in range(4)], ep1)
stage('e', B2, 32, lambda o: [(MW[(o, i)], 'h%d' % i) for i in range(4)], ep2)
stage('f', B3, 16, lambda o: [(FW[(o, i)], 'm%d' % i) for i in range(8)], ep3)
e('''    uvec4 R = uvec4(r0, r1, r2, r3);
    S4.q[uint(PX + 1) + 962u * uint(PY + 1) + 521404u] = R;
    cnb[LY][BX][DX][DY] = R;
    barrier();
    uint OG = 8u * (DX + 2u * DY);
    int acc[8];
    for (uint j = 0u; j < 8u; j++) acc[j] = int(IBW.w[1728u + OG + j]);''')
for s, (sx, sy) in enumerate([(0, 0), (1, 0), (0, 1), (1, 1)]):
    e(('    if (gl_WorkGroupID.z != %du) ' % (100 + s) if TB else '    ') + '{ uvec4 v = cnb[LY][BX][%du][%du];' % (sx, sy))
    e('      for (uint j = 0u; j < 8u; j++) { uint b = %du + 4u * (OG + j); acc[j] += sdot(v.x, IBW.w[b], 0) + sdot(v.y, IBW.w[b + 1u], 0) + sdot(v.z, IBW.w[b + 2u], 0) + sdot(v.w, IBW.w[b + 3u], 0); } }'
      % (1216 + 128 * s))
e('''    float sc = (OG < 16u) ? 0.015625 : 0.0078125;
    ivec4 o0 = ivec4(roundEven(vec4(ivec4(acc[0], acc[1], acc[2], acc[3])) * sc));
    ivec4 o1 = ivec4(roundEven(vec4(ivec4(acc[4], acc[5], acc[6], acc[7])) * sc));
    uint ob = ((16u * (uint(PX / 2) + 1u) + 7712u * (uint(PY / 2) + 1u) + 2097664u * (OG / 16u) + (OG % 16u)) + 16684928u) >> 2u;
    S1.d[ob] = pk(o0);
    S1.d[ob + 1u] = pk(o1);
}''')
open(OUT, 'w').write(chr(10).join(L) + chr(10))
print('wrote', OUT, len(L), 'lines, blocks', nb[0])
