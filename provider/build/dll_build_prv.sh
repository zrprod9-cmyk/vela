#!/usr/bin/env bash
# The provider DLL build (runs in ubuntu:24.04, started by ../build.sh). Fixed for a repeatable result: the mingw packages are installed
# at exact versions, Vulkan-Headers is checked out at the v1.4.367 tag and its commit is verified, SOURCE_DATE_EPOCH is set (the provider
# prints __DATE__/__TIME__), and the DLL image base is fixed (GNU ld 2.41 otherwise picks a random base, which changes every absolute
# address in the file). Only the fsr411/ assets are embedded (prv_tools.py).
# /src = fsr4vk @391f62b (read-only), /ov /ov2 = overlay/ov, overlay/ov2, /gen = overlay/gen,
# /vela_assets = vela_assets (only its fsr411/ is used), /prv = this folder.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive SOURCE_DATE_EPOCH=1793577600
apt-get update -qq >/dev/null
MV=13.2.0-6ubuntu1+26.1   # the mingw-w64 GCC 13 packages of Ubuntu 24.04 (noble)
apt-get install -y -qq g++-mingw-w64-x86-64-posix=$MV gcc-mingw-w64-x86-64-posix=$MV gcc-mingw-w64-x86-64-posix-runtime=$MV \
  gcc-mingw-w64-base=$MV binutils-mingw-w64-x86-64=2.41.90.20240122-1ubuntu1+11.4 mingw-w64-x86-64-dev=11.0.1-3build1 \
  mingw-w64-common=11.0.1-3build1 zstd=1.5.5+dfsg2-2build1.1 python3 libvulkan-dev git >/dev/null
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix >/dev/null 2>&1 || true
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix >/dev/null 2>&1 || true
x86_64-w64-mingw32-g++ --version | head -1; dpkg -l | grep -E "mingw-w64|zstd " | awk '{print $2, $3}'
cp -r /src /work-fsr4vk
# the public source package (provider/fsr4vk) is not a git checkout; fsr4vk's freeze step records git status / HEAD (provenance json only)
[ -d /work-fsr4vk/.git ] || (cd /work-fsr4vk && git init -q && git add -A && git -c user.name=build -c user.email=build@localhost commit -qm "fsr4vk 391f62b without assets")
cp /ov/quality-context.hpp /ov/vela-baked-pre.hpp /ov/fsr411-recorder.hpp /ov/fv_npu.hpp /ov/fv_spv.hpp /ov/vela-fast-spv.hpp /work-fsr4vk/native/
cp /ov/fsr411-assets.py /work-fsr4vk/tools/fsr411-assets.py
cp /ov2/fsr412lite-blobs.hpp /ov2/fsr412lite-context.hpp /ov2/vela_npu_abi.h /ov2/vela_npu_ext.h /gen/fsr412lite-npu-spv.hpp /gen/fsr4fu-spv.hpp /gen/fsr4sr4-spv.hpp /work-fsr4vk/native/
cp /ov2/provider/ffx_vk_provider.cpp /work-fsr4vk/provider/ffx_vk_provider.cpp
cd /work-fsr4vk
find . -path ./.git -prune -o -type f \( -name "*.sh" -o -name "*.py" \) -print0 | xargs -0 sed -i 's/\r$//'
python3 /prv/prv_tools.py /work-fsr4vk/tools
# fixed DLL image base (0x250460000) for the provider link line; the release and the debug DLL are linked by the same line
n=$(grep -c -F -e '-Wl,-Bstatic -o "$build_dir/$name"' tools/build-vulkan-provider-windows.sh)
[ "$n" = 1 ] || { echo "link line not found ($n)"; exit 1; }
sed -i 's|-Wl,-Bstatic -o "\$build_dir/\$name"|-Wl,-Bstatic -Wl,--image-base=0x250460000 -o "$build_dir/$name"|' tools/build-vulkan-provider-windows.sh
mkdir -p /sdk/include
git -c advice.detachedHead=false clone -q --depth 1 --branch v1.4.367 https://github.com/KhronosGroup/Vulkan-Headers.git /tmp/vkh
[ "$(git -C /tmp/vkh rev-parse HEAD)" = 9fbc2a782ee3f8a4db64e7d991e23da75e893f44 ] || { echo "Vulkan-Headers v1.4.367 is not commit 9fbc2a78"; exit 1; }
cp -r /tmp/vkh/include/vulkan /tmp/vkh/include/vk_video /sdk/include/
grep -m1 "VK_HEADER_VERSION " /sdk/include/vulkan/vulkan_core.h
x86_64-w64-mingw32-g++ -std=c++20 -O1 -c -I/sdk/include -Iamd-fidelityfx-sdk/Kits/FidelityFX/api/include provider/ffx_vk_provider.cpp -o /tmp/probe.o 2>/dev/null || true
{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"; grep -ohE "\bvk[A-Z][A-Za-z0-9]+\b" /sdk/include/vulkan/vulkan_core.h | sort -u; } > /tmp/vulkan-1.def
x86_64-w64-mingw32-dlltool -d /tmp/vulkan-1.def -l /tmp/libvulkan-1.a -m i386:x86-64 2>/dev/null
export VULKAN_SDK=/sdk FSR4_VULKAN_IMPORT_LIBRARY=/tmp/libvulkan-1.a
# the published fsr4vk/ has no assets/ folder; the only asset bundle is Vela's vela_assets/fsr411
mkdir -p /work-fsr4vk/assets && cp -r /vela_assets/fsr411 /work-fsr4vk/assets/fsr411
export FSR4_EMBED_ASSETS=/work-fsr4vk/assets/general FSR4_PROVIDER_OUTPUT_DIR=/out
bash tools/build-vulkan-provider-windows.sh 2>&1 | grep -v -e "misleading" -e "note:" -e "In member function" -e "^ *[0-9]* |" -e "^ *| " | tail -30
ls -la /out
