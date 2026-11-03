"""Rewrites every OpSDot (signed x signed packed int8, emulated on Adreno 740) into OpSUDot (mixed signedness, the
dp4acc A740 accelerates): sdot(x, y) = sudot(y, x ^ 0x80808080) - 128 * sudot(y, 0x01010101), exact modulo 2^32.
Usage: sudot.py in.spvasm out.spvasm"""
import re
import sys

src = open(sys.argv[1]).read().split('\n')
out, n, consts_done = [], 0, False
pat = re.compile(r'^(\s*)(%[\w]+) = OpSDot (%[\w]+) (%[\w]+) (%[\w]+) PackedVectorFormat4x8Bit$')
for line in src:
    m = pat.match(line)
    if m:
        ind, r, ty, x, y = m.groups()
        n += 1
        out += [f'{ind}%sx{n}a = OpBitwiseXor {ty} {x} %vela_c80',
                f'{ind}%sx{n}b = OpSUDot {ty} {y} %sx{n}a PackedVectorFormat4x8Bit',
                f'{ind}%sx{n}c = OpSUDot {ty} {y} %vela_c01 PackedVectorFormat4x8Bit',
                f'{ind}%sx{n}d = OpShiftLeftLogical {ty} %sx{n}c %vela_c7',
                f'{ind}{r} = OpISub {ty} %sx{n}b %sx{n}d']
        continue
    out.append(line)
    if not consts_done and re.match(r'^\s*%uint = OpTypeInt 32 0$', line):
        out += ['%vela_c80 = OpConstant %uint 2155905152', '%vela_c01 = OpConstant %uint 16843009',
                '%vela_c7 = OpConstant %uint 7']
        consts_done = True
assert consts_done or n == 0, 'no %uint type'
open(sys.argv[2], 'w').write('\n'.join(out))
print(f'{sys.argv[1]}: {n} OpSDot rewritten')
