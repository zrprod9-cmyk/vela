#!/usr/bin/env bash
# aarch64 test bench (dispatch-smoke with the provider compiled in), Vulkan-Headers pinned to v1.4.367
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
dpkg --add-architecture arm64
sed -i 's|^URIs: http://archive.ubuntu.com/ubuntu/|URIs: http://archive.ubuntu.com/ubuntu/\nArchitectures: amd64|; s|^URIs: http://security.ubuntu.com/ubuntu/|URIs: http://security.ubuntu.com/ubuntu/\nArchitectures: amd64|' /etc/apt/sources.list.d/ubuntu.sources
cat >> /etc/apt/sources.list.d/ubuntu.sources <<'X'

Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: noble noble-updates
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
X
apt-get update -qq >/dev/null
apt-get install -y -qq g++-aarch64-linux-gnu libvulkan-dev:arm64 git python3 zstd >/dev/null
git -c advice.detachedHead=false clone -q --depth 1 --branch v1.4.367 https://github.com/KhronosGroup/Vulkan-Headers.git /tmp/vkh
cp -r /src /w && cd /w
cp /ov/* /w/native/
# the bench takes the DLL's provider (overlay/ov2 + the Vela Lite files), so the create-time NPU gate is the one the game runs
cp /ov2/fsr412lite-blobs.hpp /ov2/fsr412lite-context.hpp /ov2/vela_npu_abi.h /ov2/vela_npu_ext.h /gen/fsr412lite-npu-spv.hpp /gen/fsr4fu-spv.hpp /gen/fsr4sr4-spv.hpp /w/native/; cp /ov2/provider/ffx_vk_provider.cpp /w/provider/ffx_vk_provider.cpp

aarch64-linux-gnu-g++ -std=c++20 -O2 -static-libstdc++ -static-libgcc -I/tmp/vkh/include \
    -Iamd-fidelityfx-sdk/Kits/FidelityFX/api/include native/dispatch-smoke.cpp \
    -L/usr/lib/aarch64-linux-gnu -lvulkan -ldl -lpthread -o /out/$OUTNAME 2>&1 | grep -E "error|Error" -A3 | head -60
ls -la /out
