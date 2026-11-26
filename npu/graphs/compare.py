#!/usr/bin/env python3
"""Compare built graphs with the package-14 hashes in expected.sha256.
Usage: python3 compare.py OUT_DIR   (OUT_DIR holds v73/*.bin, v79/*.bin and v81/*.bin, as build.sh writes them).
Exit code 1 if any built file differs; files not built are listed and do not fail."""
import hashlib, os, sys
here = os.path.dirname(os.path.abspath(__file__)); out = sys.argv[1]; ok = bad = miss = 0
for line in open(os.path.join(here, 'expected.sha256')):
    want, rel = line.split()
    f = os.path.join(out, rel)
    if not os.path.exists(f):
        print('%-24s not built' % rel); miss += 1; continue
    got = hashlib.sha256(open(f, 'rb').read()).hexdigest(); same = got == want; ok += same; bad += not same
    print('%-24s %s %s' % (rel, got, 'OK' if same else 'DIFFERS (expected %s)' % want))
print('match %d, differ %d, not built %d (of %d)' % (ok, bad, miss, ok + bad + miss))
sys.exit(1 if bad else 0)
