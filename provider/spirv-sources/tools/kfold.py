"""kfold.py in.spvasm out.spvasm : fold the sudot weight-sum corrections (weights are constants) into per-chain constants.
 sdot(x,w) = SUDot(w, x^0x80808080) - (SUDot(w, 0x01010101) << 7). With w constant the second term is a constant K.
 Linear IAdd chains carry the sum of the K of their terms; the correction is subtracted once where a chain value is used by a non-IAdd."""
import re, sys
M = 0xffffffff
L = open(sys.argv[1]).read().split('\n')
cu = {}
for l in L:
    m = re.match(r'^\s*(%\w+) = OpConstant %uint (\d+)$', l)
    if m:
        cu[m.group(1)] = int(m.group(2))


def sbytes(w):
    return sum(((w >> (8 * i)) & 0xff) - (256 if (w >> (8 * i)) & 0x80 else 0) for i in range(4))


one = [k for k, v in cu.items() if v == 16843009]
sh7 = [k for k, v in cu.items() if v == 7]
assert one and sh7
cmap = {}
dmap = {}
rmap = {}
kmap = {}
out = []
ntrm = 0
for l in L:
    m = re.match(r'^\s*(%\w+) = OpSUDot %int (%\w+) (%\w+) PackedVectorFormat4x8Bit$', l)
    if m and m.group(3) in one and m.group(2) in cu:
        cmap[m.group(1)] = sbytes(cu[m.group(2)])
        continue
    m = re.match(r'^\s*(%\w+) = OpShiftLeftLogical %int (%\w+) (%\w+)$', l)
    if m and m.group(2) in cmap and m.group(3) in sh7:
        dmap[m.group(1)] = (cmap[m.group(2)] << 7)
        continue
    m = re.match(r'^(\s*)(%\w+) = OpISub %int (%\w+) (%\w+)$', l)
    if m and m.group(4) in dmap:
        rmap[m.group(2)] = dmap[m.group(4)]
        kmap[m.group(2)] = dmap[m.group(4)]
        out.append('%s%s = OpCopyObject %%int %s' % (m.group(1), m.group(2), m.group(3)))
        ntrm += 1
        continue
    m = re.match(r'^(\s*)(%\w+) = OpIAdd %int (%\w+) (%\w+)$', l)
    if m and (m.group(3) in kmap or m.group(4) in kmap):
        k = kmap.get(m.group(3), 0) + kmap.get(m.group(4), 0)
        kmap[m.group(2)] = k
    out.append(l)
# materialize: any use of a marked id by something other than OpIAdd/OpCopyObject-of-term definitions
newc = {}
res = []
fixc = 0
marked = set(kmap)
defline = {}
for idx, l in enumerate(out):
    m = re.match(r'^\s*(%\w+) = ', l)
    if m:
        defline[m.group(1)] = idx
need = set()
for l in out:
    m = re.match(r'^\s*(%\w+) = OpIAdd %int (%\w+) (%\w+)$', l)
    if m and (m.group(2) in marked or m.group(3) in marked):
        continue
    m2 = re.match(r'^\s*(%\w+) = OpCopyObject %int (%\w+)$', l)
    if m2 and m2.group(1) in rmap:
        continue
    for t in re.findall(r'%\w+', l):
        if t in marked:
            if re.match(r'^\s*' + re.escape(t) + r' = ', l):
                continue
            need.add(t)
for t in sorted(need):
    if kmap[t] & M == 0:
        need.discard(t)
cid = 0
fixname = {}
inserts = {}
for t in sorted(need):
    kv = kmap[t] & M
    sv = kv - (1 << 32) if kv >= (1 << 31) else kv
    cid += 1
    cn = '%%kf_c%d' % (cid, )
    fn = '%%kf_fix%d' % (cid, )
    newc[cn] = sv
    fixname[t] = fn
    inserts[defline[t]] = '       %s = OpISub %%int %s %s' % (fn, t, cn)
fin = []
for idx, l in enumerate(out):
    s = l
    sk = s.strip()
    skip_sub = re.match(r'^(%\w+) = OpIAdd %int (%\w+) (%\w+)$', sk) and ((re.match(r'^(%\w+) = OpIAdd %int (%\w+) (%\w+)$', sk).group(2) in marked) or (re.match(r'^(%\w+) = OpIAdd %int (%\w+) (%\w+)$', sk).group(3) in marked))
    if not skip_sub and not (re.match(r'^(%\w+) = OpCopyObject %int (%\w+)$', sk) and sk.split()[0] in rmap):
        mm = re.match(r'^(%\w+) = ', sk)
        defid = mm.group(1) if mm else None
        for t, fn in fixname.items():
            if t != defid and re.search(re.escape(t) + r'(?![\w])', s):
                s = re.sub(re.escape(t) + r'(?![\w])', fn, s)
                fixc += 1
    fin.append(s)
    if idx in inserts:
        fin.append(inserts[idx])
final = []
done = False
for l in fin:
    if not done and l.strip().startswith('%') and ' = OpFunction ' in l:
        for cn, sv in newc.items():
            final.append('%s = OpConstant %%int %d' % (cn, sv))
        done = True
    final.append(l)
print('kfold: terms folded', ntrm, 'chain values', len(kmap), 'materialized', len(need), 'operand rewrites', fixc)
open(sys.argv[2], 'w').write('\n'.join(final))
