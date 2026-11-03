"""cres.py in.spvasm out.spvasm : loads from Function arrays that have a constant initializer, at a constant index -> OpCopyObject of the element constant."""
import re, sys
L = open(sys.argv[1]).read().split('\n')
consts = {}
comps = {}
for l in L:
    m = re.match(r'^\s*(%\w+) = OpConstant %u?int (-?\d+)$', l)
    if m:
        consts[m.group(1)] = int(m.group(2))
    m = re.match(r'^\s*(%\w+) = OpConstantComposite %\w+ (.*)$', l)
    if m:
        comps[m.group(1)] = m.group(2).split()
vars_ = {}
for l in L:
    m = re.match(r'^\s*(%\w+) = OpVariable %\w+ (Function|Private) (%\w+)$', l)
    if m and m.group(3) in comps:
        vars_[m.group(1)] = comps[m.group(3)]
ac = {}
out = []
n = 0
left = 0
for l in L:
    m = re.match(r'^(\s*)(%\w+) = OpAccessChain %\w+ (%\w+) (%\w+)$', l)
    if m and m.group(3) in vars_ and m.group(4) in consts:
        ac[m.group(2)] = (m.group(3), consts[m.group(4)])
        out.append(l)
        continue
    m = re.match(r'^(\s*)(%\w+) = OpLoad (%\w+) (%\w+)$', l)
    if m and m.group(4) in ac:
        v, i = ac[m.group(4)]
        out.append('%s%s = OpCopyObject %s %s' % (m.group(1), m.group(2), m.group(3), vars_[v][i]))
        n += 1
        continue
    if re.match(r'^\s*(%\w+) = OpAccessChain %\w+ (%\w+) ', l) and any((' ' + v + ' ') in l for v in vars_):
        left += 1
    out.append(l)
print('cres: arrays', len(vars_), 'loads resolved', n, 'dynamic access chains left', left)
open(sys.argv[2], 'w').write('\n'.join(out))
