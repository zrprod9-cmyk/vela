#!/usr/bin/env bash
# Runs inside the container started by ../build.sh. /k = this folder (read-only), /gen = ../overlay/gen, /out = output folder.
# glslang-tools 15.1.0-2~ubuntu0.24.04.2 and spirv-tools 2025.1~rc1-1~ubuntu0.24.04.2 are the Ubuntu 24.04 (noble-updates) packages.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq glslang-tools=15.1.0-2~ubuntu0.24.04.2 spirv-tools=2025.1~rc1-1~ubuntu0.24.04.2 python3 >/dev/null
glslangValidator --version | head -1; spirv-opt --version | head -1
cd /tmp
T=/k/tools

# 1. sr_r4.comp is generated from sr_r.comp; the result must equal the committed source
python3 $T/gen_r4.py /k/src/sr_r.comp /out && mv /out/sr_r4.comp /out/sr_r4.regen.comp
cmp /out/sr_r4.regen.comp /k/src/sr_r4.comp && echo "gen_r4.py: sr_r4.comp regenerated, identical"

# 2. sr_r4.spv: glslang, then spirv-opt -O
glslangValidator -V --target-env vulkan1.3 /k/src/sr_r4.comp -o /tmp/r.spv > /tmp/r.log || { cat /tmp/r.log; exit 1; }
spirv-opt -O --target-env=vulkan1.3 /tmp/r.spv -o /out/sr_r4.spv
spirv-val --target-env vulkan1.3 /out/sr_r4.spv

# 3. sr_w.spv: glslang, then the weight-folding pipeline (the Lite K2 weights, tools/weights.bin, become literals; int8 dot products
#    are rewritten to mixed-signedness dot products; z of the dispatch ids is constant 0), then constant folding with spirv-opt
glslangValidator -V --target-env vulkan1.3 /k/src/sr_w.comp -o /tmp/k2s.g.spv > /tmp/w.log || { cat /tmp/w.log; exit 1; }
spirv-dis /tmp/k2s.g.spv -o /tmp/k2s.g.asm
V=$(grep -m1 'Version:' /tmp/k2s.g.asm | sed 's/.*Version: //')
python3 $T/sudot.py /tmp/k2s.g.asm /tmp/k2s.g1 | tail -1
python3 $T/zconst.py /tmp/k2s.g1 /tmp/k2s.g2 | tail -1
mkdir -p /tmp/a; spirv-as --target-env spv$V /tmp/k2s.g2 -o /tmp/a/k2s.spv
OPT0="--eliminate-local-single-block --eliminate-local-single-store --eliminate-local-multi-store --ccp --simplify-instructions --eliminate-dead-code-aggressive"
OPT1="--loop-unroll --scalar-replacement=600 --eliminate-local-single-block --eliminate-local-single-store --eliminate-local-multi-store --ccp --simplify-instructions --eliminate-dead-branches --eliminate-dead-code-aggressive --ccp --simplify-instructions --eliminate-dead-branches --eliminate-dead-code-aggressive"
OPT2="--ccp --simplify-instructions --eliminate-local-multi-store --eliminate-dead-code-aggressive --ccp --simplify-instructions --eliminate-dead-code-aggressive"
b=k2s
spirv-dis /tmp/a/$b.spv -o /tmp/$b.a
python3 $T/zforce.py /tmp/$b.a /tmp/$b.z
sed -i -E 's/OpLoopMerge (%[0-9A-Za-z_]+) (%[0-9A-Za-z_]+) None/OpLoopMerge \1 \2 Unroll/' /tmp/$b.z
spirv-as --target-env spv1.5 /tmp/$b.z -o /tmp/$b.b.spv
spirv-opt --target-env=vulkan1.3 $OPT0 /tmp/$b.b.spv -o /tmp/$b.b0.spv
spirv-opt --target-env=vulkan1.3 $OPT1 /tmp/$b.b0.spv -o /tmp/$b.c0.spv
spirv-opt --target-env=vulkan1.3 $OPT1 /tmp/$b.c0.spv -o /tmp/$b.c.spv
spirv-dis /tmp/$b.c.spv -o /tmp/$b.c
python3 $T/wlit.py /tmp/$b.c /tmp/$b.d $T/weights.bin
python3 $T/cres.py /tmp/$b.d /tmp/$b.e
spirv-as --target-env spv1.5 /tmp/$b.e -o /tmp/$b.e.spv
spirv-opt --target-env=vulkan1.3 $OPT1 /tmp/$b.e.spv -o /tmp/$b.f.spv
spirv-dis /tmp/$b.f.spv -o /tmp/$b.f
python3 $T/kfold.py /tmp/$b.f /tmp/$b.g
spirv-as --target-env spv1.5 /tmp/$b.g -o /tmp/$b.g.spv
spirv-opt --target-env=vulkan1.3 $OPT2 /tmp/$b.g.spv -o /out/sr_w.spv
spirv-val --target-env vulkan1.3 /out/sr_w.spv

# 4. the C++ header the provider includes
python3 $T/gen_hpp.py /out/sr_r4.spv /out/sr_w.spv /out/fsr4sr4-spv.hpp
md5sum /out/sr_r4.spv /out/sr_w.spv
if cmp -s /out/fsr4sr4-spv.hpp /gen/fsr4sr4-spv.hpp; then echo "OK: fsr4sr4-spv.hpp identical to overlay/gen/fsr4sr4-spv.hpp"; else echo "MISMATCH: fsr4sr4-spv.hpp differs from overlay/gen/fsr4sr4-spv.hpp"; fi
