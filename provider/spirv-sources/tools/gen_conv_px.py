#!/usr/bin/env python3
"""Generator of the FSR 4.1.2 Lite pass2 (module 0005) and pass3 (module 0007) kernels from the converted AMD modules: 3x3 conv 16->16, concat
plane1 -> 1x1 32->64 relu -> 1x1 64->32 + residual; pass3 adds a 2x2 subpixel 1x1 32->16 tail to 960x540. Weights and biases are literals;
SDot(a,w) == SUDot(w, a^0x80808080) - 128*sum(w) with the correction folded into the bias (exact mod 2^32).
usage: gen_conv_px.py PASS N BAR OUT.comp <converted dir with 0005.comp and 0007.comp> <lite weights.bin>
  N   pixels per thread (1, or 2 = vertical pair)
  BAR outputs per block (multiple of 4; 0 = straight line); each block sits in its own uniform branch and is requantised and stored right after"""
import re, sys, struct
PASS, N, BAR, OUT = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
CONV, WB = sys.argv[5], sys.argv[6]
SRC = '%s/%s.comp' % (CONV, '0005' if PASS == 2 else '0007')
T = open(SRC).read()
_wb = open(WB, 'rb').read()
IB = struct.unpack('<%dI' % (len(_wb) // 4), _wb)


def arr(name):
    m = re.search(r'const uint %s\[(\d+)\] = uint\[\]\(([^)]*)\);' % name, T)
    v = [int(x.strip().rstrip('u')) for x in m.group(2).split(',')]
    assert len(v) == int(m.group(1))
    return v


if PASS == 2:
    for s in ['(_1784 + 9344u) >> 2u', '(_1784 + 11520u) >> 2u', '(_1784 + 13824u) >> 2u',
              '((((256u * _1880) + (768u * _1866)) + _1887) + (16u * ((_1859 * 4u) + uint(_1944)))) >> 2u',
              '(((2048u * (_2485 + _2471)) + _2492) + (32u * ((_2464 * 4u) + uint(_2528)))) >> 2u',
              '(((2048u * (_3296 + _3282)) + _3303) + (64u * ((_3275 * 4u) + uint(_3339)))) >> 2u',
              '+ 16684928u) >> 2u', '(7712u * _1913.y)', '_1759[0u][0u][0u] = max(', 'uvec2(480u, 270u)',
              '(_3392 ? 0.015625 : 0.03125)) + (vec4(_1762[0u][0u][0u]) * 0.00048828125)) * (_3392 ? 32.0 : 16.0)',
              'CsTensorSizes.tensorSizes[4].xy', '(!_3425) ? 134217728 :']:
        assert s in T, s
    a1, a2, a3 = arr('_1706'), arr('_1707'), arr('_1708')
    W1 = lambda t, o, i: a1[64 * t + 4 * o + i]
    W2 = lambda o, i: a2[8 * o + i]
    W3 = lambda o, i: a3[16 * o + i]
    BI1, BI2, BI3, INB = 2336, 2880, 3456, 1042808       # bias words, input base (uvec4 index)
else:
    for s in ['+ 16256u) >> 2u', '+ 13952u) >> 2u', '+ 18432u) >> 2u', '+ 16384u) >> 2u', '+ 20736u) >> 2u',
              '(((2048u * (_710 + _696)) + _717) + (64u * ((_665 * 4u) + uint(_747)))) + 18688u) >> 2u',
              '+ 22912u) >> 2u', '(((((512u * _895) + (1024u * _886)) + (32u * ((_1069 * 4u) + uint(_1113)))) + _1090) + 20864u) >> 2u',
              '(((uint(_674) + _817) < 16u) ? 0.03125 : 0.0625)) + (vec4(_137[_796][_803][_810]) * 0.00048828125)) * 32.0)',
              '* 0.03125) + (vec4(_140[0u][0u][3u][0u][0u]) * 0.0001220703125)) * 32.0)), bvec4(_1409))',
              'uvec2(960u, 540u)', '(15392u * _986.y)) + _986.z) + (8342464u * _986.w)) + 8342464u) >> 2u',
              '(15392u * _1439.y)) + _1439.z) + (8342464u * _1439.w)) + 16684928u)) >> 2u', '(!_1409) ? 134217728 :',
              'if (any(greaterThanEqual(_149, CsTensorSizes.tensorSizes[4].xy)))', '_134[_555][_562][_569] = max(', '(_134[_555][_562][_569] + ivec4(63)) + ((_134[_555][_562][_569] >> ivec4(7)) & ivec4(1))) >> ivec4(7)']:
        assert s in T, s
    assert T.count('max(') == 1
    W1 = lambda t, o, i: IB[3488 + 64 * (t % 3) + 192 * (t // 3) + 4 * o + i]
    W2 = lambda o, i: IB[4096 + 8 * o + i]
    W3 = lambda o, i: IB[4672 + 16 * o + i]
    W4 = lambda sx, sy, o, i: IB[5216 + 128 * sx + 256 * sy + 8 * o + i]
    BI1, BI2, BI3, INB = 4064, 4608, 5184, 0


def sb(w):
    return sum(((w >> (8 * k)) & 255) - (256 if (w >> (8 * k)) & 128 else 0) for k in range(4))


B1 = [(IB[BI1 + o] - 128 * sum(sb(W1(t, o, i)) for t in range(9) for i in range(4))) & 0xffffffff for o in range(16)]
B2 = [(IB[BI2 + o] - 128 * sum(sb(W2(o, i)) for i in range(8))) & 0xffffffff for o in range(64)]
B3 = [(IB[BI3 + o] - 128 * sum(sb(W3(o, i)) for i in range(16))) & 0xffffffff for o in range(32)]
if PASS == 3:
    B4 = {(sx, sy): [(IB[5728 + o] - 128 * sum(sb(W4(sx, sy, o, i)) for i in range(8))) & 0xffffffff for o in range(16)]
          for sx in range(2) for sy in range(2)}
P = range(N)
L = []
e = L.append
nb = [0]


def blocks(nout):
    gs = BAR or nout
    return [range(o0, o0 + gs) for o0 in range(0, nout, gs)]


def bopen():
    if BAR:
        nb[0] += 1
        e('    if (G != %du) {' % (0x40000000 + nb[0]))


def bclose():
    if BAR:
        e('    }')


def acc4(v, p, g):
    return 'ivec4(int({0}{1}_{2}), int({0}{1}_{3}), int({0}{1}_{4}), int({0}{1}_{5}))'.format(v, p, 4 * g, 4 * g + 1, 4 * g + 2, 4 * g + 3)


SBY = 'vec4(ivec4(int(s << 24u) >> 24, int(s << 16u) >> 24, int(s << 8u) >> 24, int(s) >> 24))'


def stage(v, bias, nout, terms, epi):
    """v: acc name; terms(o) -> [(weight, act-name-format with %d for p)]; epi(p, g) emits the pack of word g."""
    for blk in blocks(nout):
        for p in P:
            for o in blk:
                e('    uint %s%d_%d = %du;' % (v, p, o, bias[o]))
        bopen()
        for o in blk:
            for w, a in terms(o):
                for p in P:
                    e('    %s%d_%d += sud(%du, %s, 0);' % (v, p, o, w, a % p if '%' in a else a))
        if BAR:
            for p in P:
                for g in range(blk[0] // 4, blk[-1] // 4 + 1):
                    epi(p, g)
        bclose()
    if not BAR:
        for p in P:
            for g in range(nout // 4):
                epi(p, g)


e('''#version 450
#extension GL_EXT_spirv_intrinsics : require
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 141, std430) buffer SB4 { uvec4 q[]; } S4;
layout(set = 0, binding = 141, std430) buffer SB1 { uint d[]; } S1;
layout(set = 0, binding = 256, std140) uniform TS { uvec4 tensorSizes[9]; } TSZ;
spirv_instruction (extensions = ["SPV_KHR_integer_dot_product"], capabilities = [6019, 6018], id = 4452)
uint sud(uint a, uint b, spirv_literal uint f);
uint pk(ivec4 v) { return ((uint(clamp(v.x, -128, 127)) & 255u) | ((uint(clamp(v.y, -128, 127)) & 255u) << 8u)) | ((uint(clamp(v.z, -128, 127)) & 255u) << 16u) | ((uint(clamp(v.w, -128, 127)) & 255u) << 24u); }
ivec4 rq(ivec4 a) { return ((a + ivec4(127)) + ((a >> ivec4(8)) & ivec4(1))) >> ivec4(8); }
ivec4 rq7(ivec4 a) { return ((a + ivec4(63)) + ((a >> ivec4(7)) & ivec4(1))) >> ivec4(7); }
void main()
{
    uint X = gl_GlobalInvocationID.x;
    uint Y = gl_GlobalInvocationID.y;''')
if N == 2:
    e('    if ((Y & 1u) != 0u) return;')
e('    if (X >= 480u || Y >= 270u) return;')
e('    uint G = TSZ.tensorSizes[4].x;')
rows = sorted(set(p + dy for p in P for dy in range(3)))
for r in rows:
    for dx in range(3):
        e('    uvec4 t%d%d = S4.q[(X + %du) + 482u * (Y + %du) + %du];' % (r, dx, dx, r, INB))
        for i, c in enumerate('xyzw'):
            e('    uint a%d%d_%d = t%d%d.%s ^ 0x80808080u;' % (r, dx, i, r, dx, c))
for p in P:
    e('    uvec4 k%d = S4.q[(X + 1u) + 482u * (Y + %du) + %du];' % (p, p + 1, INB))
    e('    uvec4 j%d = S4.q[(X + 1u) + 482u * (Y + %du) + %du];' % (p, p + 1, INB + 131104))
    if PASS == 2:
        e('    bool inb%d = all(lessThan(uvec2(X, Y + %du), TSZ.tensorSizes[4].xy));' % (p, p))
    for g in range(4):
        e('    uint h%d_%d;' % (p, g))
    for g, c in enumerate('xyzw'):
        e('    uint h%d_%d = j%d.%s ^ 0x80808080u;' % (p, 4 + g, p, c))
    for g in range(16):
        e('    uint m%d_%d;' % (p, g))
    if PASS == 3:
        for g in range(8):
            e('    uint q%d_%d;' % (p, g))


def ep1(p, g):
    e('    h%d_%d = pk(rq(%s)) ^ 0x80808080u;' % (p, g, acc4('c', p, g)))


def ep2(p, g):
    e('    m%d_%d = pk(%s(max(%s, ivec4(0)))) ^ 0x80808080u;' % (p, g, 'rq7' if PASS == 3 else 'rq', acc4('e', p, g)))


def ep3(p, g):
    s = ('k%d' % p if g < 4 else 'j%d' % p) + '.' + 'xyzw'[g % 4]
    if PASS == 2:
        k1, k2 = ('0.015625', '32.0') if g < 4 else ('0.03125', '16.0')
        e('    { uint s = %s; ivec4 r = mix(ivec4(0), ivec4(roundEven(((%s * %s) + (vec4(%s) * 0.00048828125)) * %s)), bvec4(inb%d));'
          % (s, SBY, k1, acc4('f', p, g), k2, p))
        e('      S1.d[uint((!inb%d) ? 134217728 : int((16u * (X + 1u)) + (7712u * (Y + %du)) + %du)) >> 2u] = pk(r); }'
          % (p, p + 1, 2097664 * (g // 4) + 4 * (g % 4)))
    else:
        k1 = '0.03125' if g < 4 else '0.0625'
        e('    { uint s = %s; q%d_%d = pk(ivec4(roundEven(((%s * %s) + (vec4(%s) * 0.00048828125)) * 32.0))) ^ 0x80808080u; }'
          % (s, p, g, SBY, k1, acc4('f', p, g)))


stage('c', B1, 16, lambda o: [(W1(t, o, i), 'a%d%d_%d' % ('%d', t % 3, i) if False else None) for t in range(0)] or
      [(W1(t, o, i), None) for t in range(0)], ep1) if False else None
# stage 1 needs per-pixel row offsets: expand manually
for blk in blocks(16):
    for p in P:
        for o in blk:
            e('    uint c%d_%d = %du;' % (p, o, B1[o]))
    bopen()
    for o in blk:
        for t in range(9):
            dy, dx = divmod(t, 3)
            for i in range(4):
                for p in P:
                    e('    c%d_%d += sud(%du, a%d%d_%d, 0);' % (p, o, W1(t, o, i), p + dy, dx, i))
    if BAR:
        for p in P:
            for g in range(blk[0] // 4, blk[-1] // 4 + 1):
                ep1(p, g)
    bclose()
if not BAR:
    for p in P:
        for g in range(4):
            ep1(p, g)
stage('e', B2, 64, lambda o: [(W2(o, i), 'h%%d_%d' % i) for i in range(8)], ep2)
stage('f', B3, 32, lambda o: [(W3(o, i), 'm%%d_%d' % i) for i in range(16)], ep3)
if PASS == 3:
    for p in P:
        e('    bool ok%d = all(lessThan(uvec2(X, Y + %du), TSZ.tensorSizes[4].xy));' % (p, p))
    e('    if (!ok0) return;' if N == 1 else '    if (!(ok0 || ok1)) return;')
    for sy in range(2):
        for sx in range(2):
            e('    {')
            for p in P:
                e('    uint XX%d = 2u * X + %du; uint YY%d = 2u * (Y + %du) + %du; bool ib%d = all(lessThan(uvec2(XX%d, YY%d), uvec2(960u, 540u)));'
                  % (p, sx, p, p, sy, p, p, p))
                e('    uvec4 sk%d = S4.q[(XX%d + 1u) + 962u * (YY%d + 1u) + 521404u];' % (p, p, p))

            def ep4(p, g):
                c = ('      if (ok%d) ' % p) if N == 2 else '      '
                e('%s{ uint s = sk%d.%s; ivec4 r = mix(ivec4(0), ivec4(roundEven(((%s * 0.03125) + (vec4(%s) * 0.0001220703125)) * 32.0)), bvec4(ib%d));'
                  % (c, p, 'xyzw'[g], SBY, acc4('u', p, g), p))
                e('        S1.d[uint((!ib%d) ? 134217728 : int((16u * (XX%d + 1u)) + (15392u * (YY%d + 1u)) + %du + 16684928u)) >> 2u] = pk(r); }'
                  % (p, p, p, 4 * g))
            stage('u', B4[(sx, sy)], 16, lambda o, sx=sx, sy=sy: [(W4(sx, sy, o, i), 'q%%d_%d' % i) for i in range(8)], ep4)
            e('    }')
e('}')
open(OUT, 'w').write(chr(10).join(L) + chr(10))
print('wrote', OUT, len(L), 'lines, blocks', nb[0])
