#!/bin/sh
# Build the SR4 skipped-frame kernels (sr_r4.spv, sr_w.spv) from src/, regenerate ../overlay/gen/fsr4sr4-spv.hpp and compare it with the
# committed header. Needs Docker and network access (ubuntu:24.04 image, apt packages at the versions below). Run from this folder: sh build.sh
# Output: out/sr_r4.spv, out/sr_w.spv, out/fsr4sr4-spv.hpp, out/sr_r4.regen.comp, out/build.log; the last lines say OK or MISMATCH.
set -e
H=$(pwd); mkdir -p out
MSYS_NO_PATHCONV=1 docker run --rm -v "$H:/k:ro" -v "$H/../overlay/gen:/gen:ro" -v "$H/out:/out" \
  ubuntu:24.04 bash /k/tools/compile.sh 2>&1 | tee out/build.log
