#!/bin/sh
# Rebuild overlay/ov/fv_spv.hpp and overlay/gen/fsr412lite-npu-spv.hpp from the GLSL in this folder and compare them with the
# copies the provider build uses. Needs Docker and network access. Run from this folder: sh build.sh
# Output: out/fv_spv.hpp, out/fsr412lite-npu-spv.hpp and a line "IDENTICAL" or "DIFFERENT" per header.
set -e
H=$(pwd); mkdir -p out
MSYS_NO_PATHCONV=1 docker run --rm -v "$H:/w" -v "$H/../overlay:/overlay:ro" ubuntu:24.04 bash -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq glslang-tools=15.1.0-2~ubuntu0.24.04.2 spirv-tools=2025.1~rc1-1~ubuntu0.24.04.2 python3 >/dev/null
glslangValidator --version | head -1
mkdir -p /tmp/s && cd /tmp/s
for s in fv_xfer fv_select; do
  glslangValidator -V --target-env vulkan1.2 /w/fv/$s.comp -o $s.spv >/dev/null; spirv-val --target-env vulkan1.2 $s.spv
done
python3 /w/tools/gen_hpp.py fv /w/out/fv_spv.hpp fv_xfer.spv fv_select.spv
for s in npu_pack npu_unpack npu_select npu_netref npu_pack_zc npu_unpack_zc; do
  glslangValidator -V --target-env vulkan1.1 /w/npu/$s.comp -o $s.spv >/dev/null
done
python3 /w/tools/gen_hpp.py npu /w/out/fsr412lite-npu-spv.hpp npu_pack.spv npu_unpack.spv npu_select.spv npu_netref.spv npu_pack_zc.spv npu_unpack_zc.spv
cmp /w/out/fv_spv.hpp /overlay/ov/fv_spv.hpp && echo "fv_spv.hpp IDENTICAL" || echo "fv_spv.hpp DIFFERENT"
cmp /w/out/fsr412lite-npu-spv.hpp /overlay/gen/fsr412lite-npu-spv.hpp && echo "fsr412lite-npu-spv.hpp IDENTICAL" || echo "fsr412lite-npu-spv.hpp DIFFERENT"
md5sum /w/out/*.hpp
' 2>&1 | tee out/build.log
