#!/bin/sh
# Builds the Vela NPU daemon fsr4npud and its checker fsr4npu-check for aarch64 Linux (glibc).
# Needs an aarch64 toolchain (see Dockerfile and README.md) and the headers of the Qualcomm AI Engine Direct SDK
# (QAIRT 2.47.0.260601); the SDK is not part of this repository.
#   QNN_SDK_ROOT  the unpacked SDK, the folder that contains include/QNN   (required)
#   OUT           output folder (default: bin/ next to this script)
#   CROSS         toolchain prefix (default aarch64-linux-gnu-; set it empty when building on an aarch64 machine)
# Outputs: fsr4npud (with debug info), fsr4npud.stripped (the file the system image ships, before its RPATH is set),
#          fsr4npu.o, fsr4npu-check (static, with debug info; the image ships it as is).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
QI=${QNN_SDK_ROOT:?set QNN_SDK_ROOT to the unpacked Qualcomm AI Engine Direct SDK}/include/QNN
[ -f "$QI/QnnInterface.h" ] || { echo "$QI/QnnInterface.h not found: QNN_SDK_ROOT is not the SDK root"; exit 1; }
O=${OUT:-$HERE/bin}
P=${CROSS-aarch64-linux-gnu-}
CC=${P}gcc; CXX=${P}g++
W="-O2 -g -Wall -Wno-unused-result -Wno-misleading-indentation"
mkdir -p "$O"
$CC $W -c "$HERE/fsr4npu.c" -o "$O/fsr4npu.o"
$CXX $W -std=c++17 -I"$QI" -I"$HERE" "$HERE/fsr4npud.cpp" -o "$O/fsr4npud" -ldl -pthread -Wl,--disable-new-dtags,-rpath,'$ORIGIN'
$CC $W -I"$HERE" "$HERE/fsr4npu_check.c" "$HERE/fsr4npu.c" -o "$O/fsr4npu-check" -static
${P}strip -o "$O/fsr4npud.stripped" "$O/fsr4npud"
${P}readelf -d "$O/fsr4npud" | grep -E "NEEDED|RPATH"
${P}readelf -V "$O/fsr4npud" | grep -oE "GLIBC(XX)?_[0-9.]+" | sort -Vu | tail -3
ls -la "$O"
