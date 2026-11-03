# gen_hpp.py SR_R4.spv SR_W.spv OUT.hpp : embeds the skipped-frame kernels as C++ arrays (GPL-2.0-or-later, see ../LICENSE)
import sys, hashlib, struct
r, w, out = sys.argv[1:4]
def arr(name, p):
    b = open(p, 'rb').read(); assert len(b) % 4 == 0 and b[:4] == b'\x03\x02\x23\x07'
    words = struct.unpack(f'<{len(b) // 4}I', b)
    return f'// {p.replace(chr(92), "/").split("/")[-1]} md5 {hashlib.md5(b).hexdigest()}\ninline constexpr std::uint32_t {name}[] = {{' + ','.join(f'0x{x:08x}' for x in words) + '};\n'
with open(out, 'w', newline='\n') as f:
    f.write('#pragma once\n// SPDX-License-Identifier: GPL-2.0-or-later\n// Skipped-frame kernels sr_r4 (reproject + rest / uncovered / edge rules) and sr_w (Lite K2 on the warped feature plane); rules derived from\n'
            '// fsr4-shader-performance-overrides (FSR 4.1.2 lossy). Source and build: provider/sr4-kernels/ (src/*.comp, build.sh). Kept apart from the MIT recorder code.\n'
            '#include <cstdint>\nnamespace fsr4sr4_spv {\n' + arr('k_sr_r4', r) + arr('k_sr_w', w) + '}  // namespace fsr4sr4_spv\n')
