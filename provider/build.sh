#!/bin/sh
# Build the Vela FSR 4 provider (amd_fidelityfx_upscaler_vk.dll, x64) from this folder. Needs Docker and network access
# (ubuntu:24.04 image, apt packages, Vulkan-Headers v1.4.367 from GitHub). Run from this folder: sh build.sh
# Output: out/amd_fidelityfx_upscaler_vk.dll (+ debug DLL, probes, build-provenance.json). See README.md for the expected md5.
set -e
H=$(pwd); mkdir -p out
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$H/fsr4vk:/src:ro" -v "$H/overlay/ov:/ov:ro" -v "$H/overlay/ov2:/ov2:ro" -v "$H/overlay/gen:/gen:ro" \
  -v "$H/vela_assets:/vela_assets:ro" -v "$H/build:/prv:ro" -v "$H/out:/out" \
  ubuntu:24.04 bash /prv/dll_build_prv.sh 2>&1 | tee out/build.log
md5sum out/amd_fidelityfx_upscaler_vk.dll
