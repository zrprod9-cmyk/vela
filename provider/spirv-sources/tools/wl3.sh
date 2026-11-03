#!/bin/bash
# Weight-literal pipeline on a SPIR-V module: Z-forcing, loop unroll, constant folding, weights SSBO loads -> literals, constant-array resolve,
# SUDot correction fold.  env: MODS (module names), AIN (dir with <name>.spv), OUT (output dir), TOOLS (this folder), WEIGHTS (Lite weights file)
set -e
OPT0="--eliminate-local-single-block --eliminate-local-single-store --eliminate-local-multi-store --ccp --simplify-instructions --eliminate-dead-code-aggressive"
OPT1="--loop-unroll --scalar-replacement=600 --eliminate-local-single-block --eliminate-local-single-store --eliminate-local-multi-store --ccp --simplify-instructions --eliminate-dead-branches --eliminate-dead-code-aggressive --ccp --simplify-instructions --eliminate-dead-branches --eliminate-dead-code-aggressive"
OPT2="--ccp --simplify-instructions --eliminate-local-multi-store --eliminate-dead-code-aggressive --ccp --simplify-instructions --eliminate-dead-code-aggressive"
for b in $MODS; do
  spirv-dis $AIN/$b.spv -o /tmp/$b.a
  python3 $TOOLS/zforce.py /tmp/$b.a /tmp/$b.z
  sed -i -E 's/OpLoopMerge (%[0-9A-Za-z_]+) (%[0-9A-Za-z_]+) None/OpLoopMerge \1 \2 Unroll/' /tmp/$b.z
  spirv-as --target-env spv1.5 /tmp/$b.z -o /tmp/$b.b.spv
  spirv-opt --target-env=vulkan1.3 $OPT0 /tmp/$b.b.spv -o /tmp/$b.b0.spv
  spirv-opt --target-env=vulkan1.3 $OPT1 /tmp/$b.b0.spv -o /tmp/$b.c0.spv
  spirv-opt --target-env=vulkan1.3 $OPT1 /tmp/$b.c0.spv -o /tmp/$b.c.spv
  spirv-dis /tmp/$b.c.spv -o /tmp/$b.c
  python3 $TOOLS/wlit.py /tmp/$b.c /tmp/$b.d "$WEIGHTS"
  python3 $TOOLS/cres.py /tmp/$b.d /tmp/$b.e
  spirv-as --target-env spv1.5 /tmp/$b.e -o /tmp/$b.e.spv
  spirv-opt --target-env=vulkan1.3 $OPT1 /tmp/$b.e.spv -o /tmp/$b.f.spv
  spirv-dis /tmp/$b.f.spv -o /tmp/$b.f
  python3 $TOOLS/kfold.py /tmp/$b.f /tmp/$b.g
  spirv-as --target-env spv1.5 /tmp/$b.g -o /tmp/$b.g.spv
  spirv-opt --target-env=vulkan1.3 $OPT2 /tmp/$b.g.spv -o $OUT/$b.spv
  spirv-val --target-env vulkan1.3 $OUT/$b.spv
  echo "$b done"
done
