"""fsr4vk 4.1.1 dispatches every pass with one workgroup in Z (fsr411-recorder.hpp: vkCmdDispatch(x, y, 1)) and
local size z = 1, so gl_WorkGroupID.z and gl_GlobalInvocationID.z are always 0. The shaders use them as loop
bounds (gl_WorkGroupID.z + 7u ...), which keeps ir3 from unrolling: replace every read of the z component with 0.
Usage: zconst.py in.spvasm out.spvasm"""
import re
import sys

lines = open(sys.argv[1]).read().split('\n')
VARS = ('%gl_WorkGroupID', '%gl_GlobalInvocationID')
consts = {m.group(1): m.group(2) for l in lines for m in [re.match(r'^\s*(%\w+) = OpConstant %uint (\d+)$', l)] if m}
two = {k for k, v in consts.items() if v == '2'}
zchains, vecs, out, n = set(), set(), [], 0
for l in lines:
    m = re.match(r'^(\s*)(%\w+) = OpAccessChain %\w+ (%\w+) (%\w+)$', l)
    if m and m.group(3) in VARS and m.group(4) in two:
        zchains.add(m.group(2))
    m = re.match(r'^(\s*)(%\w+) = OpLoad %uint (%\w+)$', l)
    if m and m.group(3) in zchains:
        out.append(f'{m.group(1)}{m.group(2)} = OpCopyObject %uint %vela_z0'); n += 1; continue
    m = re.match(r'^(\s*)(%\w+) = OpLoad %v3uint (%\w+)$', l)
    if m and m.group(3) in VARS:
        vecs.add(m.group(2))
    m = re.match(r'^(\s*)(%\w+) = OpCompositeExtract %uint (%\w+) 2$', l)
    if m and m.group(3) in vecs:
        out.append(f'{m.group(1)}{m.group(2)} = OpCopyObject %uint %vela_z0'); n += 1; continue
    out.append(l)
    if re.match(r'^\s*%uint = OpTypeInt 32 0$', l):
        out.append('%vela_z0 = OpConstant %uint 0')
open(sys.argv[2], 'w').write('\n'.join(out))
print(f'{sys.argv[1]}: {n} z reads -> 0')
