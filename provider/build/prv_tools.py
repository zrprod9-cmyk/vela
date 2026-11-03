# Run INSIDE the build container on the fsr4vk copy (/work-fsr4vk/tools), after the CRLF strip: freeze and embed only the
# fsr411/ bundle (FSR 4.1.1); the FSR 4.0.2 bundles general/, portable/, colors/ are neither copied nor embedded.
import sys
from pathlib import Path
T = Path(sys.argv[1])
def sub(name, old, new):
    p = T / name; t = p.read_text(); n = t.count(old)
    if n != 1: sys.exit(f'{name}: expected 1 x {old[:80]!r}, found {n}')
    p.write_text(t.replace(old, new))
sub('freeze-provider-inputs.py', "for name in ('general', 'portable', 'colors', 'fsr411'):", "for name in ('fsr411',):   # 4.1.1 only")
sub('embed-general-assets.py',
    "subprocess.run([sys.executable, str(Path(__file__).with_name('verify-general-assets.py')), str(a.general)], check=True)\n"
    "portable_root = a.general.parent/'portable'\n"
    "print(f'Verified {portable_assets.verify(a.general, portable_root)} portable shaders.')\n",
    "# no FSR 4.0.2 bundles (general/portable/colors); only fsr411/ below\n")
p = T / 'embed-general-assets.py'; t = p.read_text()
a = t.index("for manifest in sorted(a.general.glob('*/*/manifest.json')):"); b = t.index("fsr411_spec = ")
p.write_text(t[:a] + t[b:])
sub('verify-provider-build.py', "models=['4.0.2', '4.1.1']", "models=['4.1.1', '4.1.2 Lite']")
print('tools patched', T)
