"""zforce.py in.spvasm out.spvasm : forces .z of GlobalInvocationID/WorkgroupId/LocalInvocationId to 0 (dispatch z=1, local z=1)
so spirv-opt can see the constant trip counts (4+WorkGroupID.z ...). Also optional UBO-less."""
import re, sys
L = open(sys.argv[1]).read().split('\n')
bi = {}
for l in L:
    m = re.match(r'^\s*OpDecorate (%\w+) BuiltIn (GlobalInvocationId|WorkgroupId|LocalInvocationId)$', l)
    if m: bi[m.group(1)] = m.group(2)
names = {m.group(1): m.group(2) for l in L for m in [re.match(r'^\s*OpName (%\w+) "(\w+)"$', l)] if m}
for k, v in names.items():
    if v in ('gl_GlobalInvocationID', 'gl_WorkGroupID', 'gl_LocalInvocationID'): bi[k] = v
u0 = next(m.group(1) for l in L for m in [re.match(r'^\s*(%\w+) = OpConstant %uint 0$', l)] if m)
v3 = next(m.group(1) for l in L for m in [re.match(r'^\s*(%\w+) = OpTypeVector %uint 3$', l)] if m)
i2 = next(m.group(1) for l in L for m in [re.match(r'^\s*(%\w+) = OpConstant %int 2$', l)] if m)
zch = set(); out = []; n = 0
for l in L:
    m = re.match(r'^(\s*)(%\w+) = OpAccessChain (%\w+) (%\w+) (%\w+)$', l)
    if m and m.group(4) in bi and m.group(5) == i2:
        zch.add(m.group(2)); out.append(l); continue
    m = re.match(r'^(\s*)(%\w+) = OpLoad (%\w+) (%\w+)$', l)
    if m and m.group(4) in zch:
        out.append('%s%s = OpCopyObject %s %s' % (m.group(1), m.group(2), m.group(3), u0)); n += 1; continue
    if m and m.group(4) in bi and m.group(3) == v3:
        out.append('%s%s_o = OpLoad %s %s' % (m.group(1), m.group(2), v3, m.group(4)))
        out.append('%s%s = OpCompositeInsert %s %s %s_o 2' % (m.group(1), m.group(2), v3, u0, m.group(2))); n += 1; continue
    out.append(l)
open(sys.argv[2], 'w').write('\n'.join(out))
print('zforce: %d loads patched, builtins %s' % (n, sorted(set(bi.values()))))
