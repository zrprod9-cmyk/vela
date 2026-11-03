"""Replaces every load from the weights SSBO (%InitializerBuffer) whose index can be evaluated at compile time with a literal.
The index is evaluated symbolically: gl_GlobalInvocationID / gl_WorkGroupID / gl_LocalInvocationID have z = 0 (dispatch z = 1,
local z = 1), x and y unknown. Usage: wlit.py in.spvasm out.spvasm weights.bin"""
import re
import sys

M = 0xffffffff
lines = open(sys.argv[1]).read().split('\n')
w = open(sys.argv[3], 'rb').read()
names = {m.group(1): m.group(2) for l in lines for m in [re.match(r'^\s*OpName (%\w+) "(\w+)"$', l)] if m}
init = [k for k, v in names.items() if v == 'InitializerBuffer']
if not init:
    open(sys.argv[2], 'w').write('\n'.join(lines))
    print('no InitializerBuffer'); sys.exit(0)
INIT = init[0]
BUILTIN = {m.group(1) for l in lines for m in [re.match(r'^\s*OpDecorate (%\w+) BuiltIn (GlobalInvocationId|WorkgroupId|LocalInvocationId)$', l)] if m}
BUILTIN |= {k for k, v in names.items() if v in ('gl_GlobalInvocationID', 'gl_WorkGroupID', 'gl_LocalInvocationID')}
uint_t = next(m.group(1) for l in lines for m in [re.match(r'^\s*(%\w+) = OpTypeInt 32 0$', l)] if m)
defs = {}
for l in lines:
    m = re.match(r'^\s*(%\w+) = (Op\w+)(?: (.*))?$', l)
    if m:
        defs[m.group(1)] = (m.group(2), (m.group(3) or '').split())
memo = {}


def ev(i):
    if i in memo:
        return memo[i]
    memo[i] = None
    r = ev1(i)
    memo[i] = r
    return r


def ew(f, a, b):
    if isinstance(a, tuple) or isinstance(b, tuple):
        n = len(a) if isinstance(a, tuple) else len(b)
        aa = a if isinstance(a, tuple) else (a,) * n
        bb = b if isinstance(b, tuple) else (b,) * n
        return tuple(None if (x is None or y is None) else f(x, y) & M for x, y in zip(aa, bb))
    if a is None or b is None:
        return None
    return f(a, b) & M


def ev1(i):
    if i not in defs:
        return None
    op, a = defs[i]
    if op == 'OpConstant':
        try:
            return int(a[1]) & M
        except ValueError:
            return None
    if op in ('OpConstantComposite', 'OpCompositeConstruct'):
        return tuple(ev(x) for x in a[1:])
    if op in ('OpBitcast', 'OpCopyObject'):
        return ev(a[1])
    if op == 'OpCompositeExtract':
        v = ev(a[1])
        for k in a[2:]:
            if not isinstance(v, tuple):
                return None
            v = v[int(k)]
        return v
    if op == 'OpCompositeInsert':
        v = ev(a[2]); o = ev(a[1])
        if not isinstance(v, tuple):
            return None
        v = list(v); v[int(a[3])] = o
        return tuple(v)
    if op == 'OpLoad':
        p = a[1]
        if p in BUILTIN:
            return (None, None, 0)
        if p in defs and defs[p][0] == 'OpAccessChain' and defs[p][1][1] in BUILTIN:
            k = ev(defs[p][1][2])
            return (None, None, 0)[k] if k is not None and k < 3 else None
        return None
    ops = {'OpIAdd': lambda x, y: x + y, 'OpISub': lambda x, y: x - y, 'OpIMul': lambda x, y: x * y,
           'OpShiftRightLogical': lambda x, y: (x & M) >> (y & 31), 'OpShiftLeftLogical': lambda x, y: x << (y & 31),
           'OpBitwiseAnd': lambda x, y: x & y, 'OpBitwiseOr': lambda x, y: x | y, 'OpBitwiseXor': lambda x, y: x ^ y}
    if op in ops:
        return ew(ops[op], ev(a[1]), ev(a[2]))
    if op == 'OpUDiv':
        return ew(lambda x, y: x // y if y else 0, ev(a[1]), ev(a[2]))
    return None


chains = {}
for l in lines:
    m = re.match(r'^\s*(%\w+) = OpAccessChain %\w+ ' + re.escape(INIT) + r' (%\w+) (%\w+)$', l)
    if m:
        chains[m.group(1)] = m.group(3)
lit, nonconst, out, newc = 0, 0, [], {}
for l in lines:
    m = re.match(r'^(\s*)(%\w+) = OpLoad ' + re.escape(uint_t) + r' (%\w+)$', l)
    if m and m.group(3) in chains:
        idx = ev(chains[m.group(3)])
        if not isinstance(idx, int):
            nonconst += 1
        else:
            v = int.from_bytes(w[idx * 4: idx * 4 + 4], 'little')
            cn = f'%wl_{v}'
            newc[cn] = v
            out.append(f'{m.group(1)}{m.group(2)} = OpCopyObject {uint_t} {cn}')
            lit += 1
            continue
    out.append(l)
res, done = [], False
for l in out:
    if not done and re.match(r'^\s*%\w+ = OpFunction ', l):
        res += [f'{k} = OpConstant {uint_t} {v}' for k, v in newc.items()]
        done = True
    res.append(l)
open(sys.argv[2], 'w').write('\n'.join(res))
print(f'{sys.argv[1]}: {lit} loads -> literals ({len(newc)} distinct), {nonconst} non-constant index loads remain, {len(chains)} chains')
