#!/bin/sh
# Builds the Vela NPU Vulkan layer libVkLayer_VELA_npu.so (and the on-device test app wl_app) for aarch64 Linux (glibc).
# Toolchain: the image of ../npu/src/Dockerfile (Ubuntu 24.04, aarch64 GNU cross compiler g++ 13.3, binutils 2.42, Vulkan
# headers 1.3.275 and the arm64 libvulkan). From the repository root:
#   docker build -t vela-npu-build npu/src
#   docker run --rm -v "$PWD":/r vela-npu-build sh /r/npu-layer/build.sh
# The layer talks to fsr4npud through the client library of the NPU part (../npu/src: fsr4npu.c, fsr4npu.h, fsr4npu_proto.h);
# there is no second copy of it. Nothing from Qualcomm is needed.
#   OUT    output folder (default: bin/ next to this script)
#   CROSS  toolchain prefix (default aarch64-linux-gnu-; set it empty when building on an aarch64 machine)
# Outputs: libVkLayer_VELA_npu.so (with debug info), VkLayer_VELA_npu.json, fsr4npu_pic.o, wl_app.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CL=$HERE/../npu/src
O=${OUT:-$HERE/bin}
P=${CROSS-aarch64-linux-gnu-}
CC=${P}gcc; CXX=${P}g++
W="-O2 -g -Wall -Wno-unused-result -Wno-misleading-indentation"
mkdir -p "$O"
$CC $W -fPIC -fvisibility=hidden -I"$CL" -c "$CL/fsr4npu.c" -o "$O/fsr4npu_pic.o"
$CXX $W -std=c++17 -fPIC -fvisibility=hidden -shared -I"$HERE" -I"$CL" "$HERE/vela_npu_layer.cpp" "$O/fsr4npu_pic.o" -o "$O/libVkLayer_VELA_npu.so" \
  -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,--version-script="$HERE/vela_npu.map" -Wl,-z,now -lpthread
$CC $W -I"$HERE" "$HERE/wl_app.c" -o "$O/wl_app" -lvulkan
cp "$HERE/VkLayer_VELA_npu.json" "$O/"
${P}readelf -d "$O/libVkLayer_VELA_npu.so" | grep NEEDED
${P}readelf -d "$O/wl_app" | grep NEEDED
${P}nm -D --defined-only "$O/libVkLayer_VELA_npu.so" | grep -E " T "
${P}objdump -T "$O/libVkLayer_VELA_npu.so" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
md5sum "$O/libVkLayer_VELA_npu.so"
