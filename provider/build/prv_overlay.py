# Release provider WITHOUT the FSR 4.0.2 model (FSR 4.0.2 is not among the AMD files under the MIT licence). Input = the
# previous overlay sources (<in>/ov, <in>/ov2), copied to <out>/ov, <out>/ov2 and edited there. Changes: the provider offers and accepts only 4.1.1 and 4.1.2 Lite (an unversioned request or the
# FSR 3.1.4 alias gets 4.1.1; an explicit 4.0.2 request = FFX_API_RETURN_NO_PROVIDER); every model loads from <assets>/fsr411;
# QualityContext refuses the 4.0.2 layout. The 4.1.1 / Fast / 1.5x / NPU / Lite code paths are unchanged.
# Run: python -I prv_overlay.py <in> <out>   (a record of the edit; its result is in overlay/)
import shutil, sys
from pathlib import Path
SRC = Path(sys.argv[1]); OUT = Path(sys.argv[2])
for d in ('ov', 'ov2'):
    if (OUT / d).exists(): sys.exit(f'{OUT / d} exists: use a fresh output folder')
    shutil.copytree(SRC / d, OUT / d)
def sub(path, old, new, count=1):
    p = OUT / path; t = p.read_text(encoding='utf-8'); n = t.count(old)
    if n != count: sys.exit(f'{path}: expected {count} x {old[:80]!r}, found {n}')
    p.write_text(t.replace(old, new), encoding='utf-8', newline='')
P = 'ov2/provider/ffx_vk_provider.cpp'
sub(P, 'constexpr std::uint64_t kVersionId =\n    (0xF5A5CA1Eull << 32) | ((4ull << 22) | (0ull << 12) | 2ull);\n',
       '// The FSR 4.0.2 model is not in this build (no 4.0.2 shaders or weights embedded or accepted).\n')
sub(P, 'constexpr const char* kVersionName = " 4.0.2 VK INT8";\n', '')
sub(P, '    std::uint64_t version = kVersionId;\n', '    std::uint64_t version = kVersion411Id;\n')
sub(P, 'query->versionName = query->versionId==kVersion411Id ? kVersion411Name : query->versionId==kVersionLiteId ? kVersionLiteName : kVersionName;',
       'query->versionName = query->versionId==kVersionLiteId ? kVersionLiteName : kVersion411Name;')
sub(P, '    // Well-behaved callers query this DLL and request kVersionId. Some games\n',
       '    // An explicit FSR 4.0.2 request is refused (not in this build); unversioned callers get 4.1.1. Some games\n')
sub(P, 'if (version != 0 && version != kVersionId && version != kVersion411Id && version != kVersionLiteId &&',
       'if (version != 0 && version != kVersion411Id && version != kVersionLiteId &&')
sub(P, 'provider->version = version==kVersion411Id ? kVersion411Id : version==kVersionLiteId ? kVersionLiteId : kVersionId;',
       'provider->version = version==kVersionLiteId ? kVersionLiteId : kVersion411Id;')
sub(P, 'const auto assets=version==kVersion411Id ? asset_root()/"fsr411" : asset_root();',
       'const auto assets=asset_root()/"fsr411";   // the only asset bundle (4.1.1); Lite reads none')
sub(P, '(provider->version==kVersion411Id ? kVersion411Name : kVersionName));',
       '(provider->version==kVersionLiteId ? kVersionLiteName : kVersion411Name));')
OLD_V = '''        if (capacity > 0) {
            if (query->versionIds) query->versionIds[0] = kVersionId;
            if (query->versionNames) query->versionNames[0] = kVersionName;
        }
        if (capacity > 1) {
            if (query->versionIds) query->versionIds[1] = kVersion411Id;
            if (query->versionNames) query->versionNames[1] = kVersion411Name;
        }
        if (capacity > 2) {
            if (query->versionIds) query->versionIds[2] = kVersionLiteId;
            if (query->versionNames) query->versionNames[2] = kVersionLiteName;
        }
        *query->outputCount = 3;'''
NEW_V = '''        if (capacity > 0) {   // 4.1.1 first (no 4.0.2 in this build)
            if (query->versionIds) query->versionIds[0] = kVersion411Id;
            if (query->versionNames) query->versionNames[0] = kVersion411Name;
        }
        if (capacity > 1) {
            if (query->versionIds) query->versionIds[1] = kVersionLiteId;
            if (query->versionNames) query->versionNames[1] = kVersionLiteName;
        }
        *query->outputCount = 2;'''
sub(P, OLD_V, NEW_V)
t = (OUT / P).read_text(encoding='utf-8')
for w in ('kVersionId', 'kVersionName', '4.0.2'):
    print('remaining', w, t.count(w))
sub('ov/quality-context.hpp', '        if(fsr411 && !nms) throw std::invalid_argument("4.1.1 requires the general layout");\n',
    '        if(!fsr411) throw std::invalid_argument("FSR 4.0.2 is not in this build (Vela release provider): 4.1.1 only");\n'
    '        if(fsr411 && !nms) throw std::invalid_argument("4.1.1 requires the general layout");\n')
print('overlay applied', OUT)
