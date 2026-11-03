#!/bin/bash
# GLSL compute kernel -> SPIR-V with literal weights: glslang (Vulkan 1.3), entry rename, OpSDot -> OpSUDot (sudot.py), Z-constants (zconst.py),
# then wl3.sh.  env: COMP (GLSL file), MOD (module name), ENTRY (entry point name), OUT, TOOLS, WEIGHTS (see wl3.sh)
set -e
cd /tmp
glslangValidator -V --target-env vulkan1.3 "$COMP" -o /tmp/$MOD.g.spv
spirv-dis /tmp/$MOD.g.spv -o /tmp/$MOD.g.asm
sed -i "s/\"main\"/\"$ENTRY\"/" /tmp/$MOD.g.asm
V=$(grep -m1 'Version:' /tmp/$MOD.g.asm | sed 's/.*Version: //')
python3 $TOOLS/sudot.py /tmp/$MOD.g.asm /tmp/$MOD.g1 | tail -1
python3 $TOOLS/zconst.py /tmp/$MOD.g1 /tmp/$MOD.g2 | tail -1
mkdir -p /tmp/a
spirv-as --target-env spv$V /tmp/$MOD.g2 -o /tmp/a/$MOD.spv
AIN=/tmp/a MODS=$MOD bash $TOOLS/wl3.sh
