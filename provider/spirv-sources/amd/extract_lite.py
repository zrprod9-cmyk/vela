#!/usr/bin/env python3
"""Extract the 13 SPIR-V modules of the FSR 4.1.2 Lite passes and the Lite weights from AMD's amd_fidelityfx_upscaler_vk.dll
(FidelityFX SDK v2.4.0, signed binary, MIT).

The DLL keeps its compute shaders as 'FXZF' + u32 size + zlib streams, listed by an (size, pointer) table in .data that points into
.rdata. A module is picked by the SHA-256 of its decompressed bytes (lite_modules.tsv). The Lite weights are a 27392-byte block in .rdata
at the RVA given in lite_modules.tsv, checked against the SHA-256 there.

usage: extract_lite.py <amd_fidelityfx_upscaler_vk.dll> <modules.tsv> <out dir>
writes <out dir>/NNNN.spv (module index of the table) and <out dir>/lite_weights.bin"""
import hashlib, struct, sys, zlib
from pathlib import Path


def parse(v):
    pe = struct.unpack_from('<I', v, 0x3c)[0]
    nsec = struct.unpack_from('<H', v, pe + 6)[0]
    opt = struct.unpack_from('<H', v, pe + 20)[0]
    base = struct.unpack_from('<Q', v, pe + 24 + 24)[0]
    secs = {}
    for k in range(nsec):
        o = pe + 24 + opt + 40 * k
        name = v[o:o + 8].rstrip(b'\0').decode()
        vsize, va, rsize, raw = struct.unpack_from('<IIII', v, o + 8)
        secs[name] = (va, vsize, raw, rsize)
    return secs, base


def blobs(dll):
    v = dll.read_bytes()
    secs, base = parse(v)
    rd, da = secs['.rdata'], secs['.data']
    lo = base + rd[0]
    hi = lo + rd[1]
    for o in range(da[2], da[2] + da[3] - 16, 8):
        size, ptr = struct.unpack_from('<QQ', v, o)
        if lo <= ptr < hi and 500 < size < 3_000_000 and ptr + size <= hi:
            off = ptr - lo + rd[2]
            if v[off:off + 4] == b'FXZF':
                data = zlib.decompress(v[off + 8:off + size])
                assert len(data) == struct.unpack_from('<I', v, off + 4)[0]
                yield data


def main():
    dll, table, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    want = {}
    for line in table.read_text().splitlines():
        if line and not line.startswith('#'):
            idx, size, sha = line.split()[:3]
            want[sha] = (idx, int(size))
    out.mkdir(parents=True, exist_ok=True)
    wline = [l for l in table.read_text().splitlines() if l.startswith('# lite weights:')][0]
    rva, wsize, wsha = int(wline.split('RVA')[1].split(',')[0], 16), int(wline.split(',')[1].split()[0]), wline.split('sha256')[1].strip()
    v = dll.read_bytes()
    secs, _ = parse(v)
    rd = secs['.rdata']
    weights = v[rd[2] + rva - rd[0]: rd[2] + rva - rd[0] + wsize]
    if hashlib.sha256(weights).hexdigest() != wsha:
        sys.exit('Lite weights: SHA-256 mismatch')
    (out / 'lite_weights.bin').write_bytes(weights)
    found = set()
    for data in blobs(dll):
        sha = hashlib.sha256(data).hexdigest()
        if sha in want and sha not in found:
            idx, size = want[sha]
            assert len(data) == size, (idx, len(data), size)
            (out / (idx + '.spv')).write_bytes(data)
            found.add(sha)
    missing = [want[s][0] for s in want if s not in found]
    if missing:
        sys.exit('modules not found in the DLL: ' + ' '.join(sorted(missing)))
    print('extracted %d modules and the Lite weights' % len(found))


if __name__ == '__main__':
    main()
