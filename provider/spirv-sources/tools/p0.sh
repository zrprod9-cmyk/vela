#!/bin/bash
# GLSL prepass -> SPIR-V: glslang (Vulkan 1.3), entry rename, DerivativeGroupLinear execution mode, spirv-as, spirv-val.
# env: COMP (GLSL file), NAME (output stem), ENTRY (entry point name), OUT (output dir)
set -e
cd /tmp
glslangValidator -V --target-env vulkan1.3 "$COMP" -o /tmp/$NAME.g.spv
spirv-dis /tmp/$NAME.g.spv -o /tmp/$NAME.g.asm
sed -i "s/\"main\"/\"$ENTRY\"/" /tmp/$NAME.g.asm
python3 - <<PY
p = '/tmp/$NAME.g.asm'
s = open(p).read()
s = s.replace('OpCapability Shader\n', 'OpCapability Shader\n               OpCapability ComputeDerivativeGroupLinearKHR\n', 1)
i = s.index('OpExtInstImport')
j = s.rfind('\n', 0, i) + 1
s = s[:j] + '               OpExtension "SPV_KHR_compute_shader_derivatives"\n' + s[j:]
s = s.replace('LocalSize 256 1 1\n', 'LocalSize 256 1 1\n               OpExecutionMode %main DerivativeGroupLinearKHR\n', 1)
open(p, 'w').write(s)
PY
V=$(grep -m1 'Version:' /tmp/$NAME.g.asm | sed 's/.*Version: //')
spirv-as --target-env spv$V /tmp/$NAME.g.asm -o "$OUT/$NAME.spv"
spirv-val --target-env vulkan1.3 "$OUT/$NAME.spv" && echo VAL_OK $NAME
