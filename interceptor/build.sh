#!/usr/bin/env bash
# Build the RE4 FSR 2 interceptor as an ARM64EC dxgi.dll. Everything the build needs is either in this folder or
# downloaded here at a pinned version and checked (llvm-mingw: sha256 of the archive; Detours, Vulkan-Headers:
# git commit id).
#
# With Docker (any host), from the root of the repository:
#
#   docker build -t vela-interceptor interceptor
#   docker run --rm -v "$PWD/interceptor:/interceptor:ro" -v "$PWD/interceptor-out:/out" \
#       -v vela-interceptor-deps:/deps vela-interceptor
#   # -> interceptor-out/arm64ec/dxgi.dll
#
# (on Windows with Git Bash prefix the docker run with MSYS_NO_PATHCONV=1)
#
# Without Docker: an x86-64 Linux with bash, curl, git, xz, tar, sha256sum (Ubuntu 22.04 is what is tested):
#
#   DEPS=$HOME/vela-deps OUT=$PWD/out bash interceptor/build.sh
#
# DEPS (default ./vela-interceptor-deps) keeps the downloads between runs; OUT (default ./out) gets arm64ec/dxgi.dll.
# Pre-downloaded inputs are used when they are in DEPS under the names below (the checks still run).
set -euo pipefail

# ---- pinned inputs
LLVM_MINGW_VER=20260908
LLVM_MINGW_NAME=llvm-mingw-${LLVM_MINGW_VER}-ucrt-ubuntu-22.04-x86_64
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VER}/${LLVM_MINGW_NAME}.tar.xz
LLVM_MINGW_SHA256=2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092
# Microsoft Detours (MIT), https://github.com/microsoft/Detours
DETOURS_URL=https://github.com/microsoft/Detours.git
DETOURS_COMMIT=1dbd4ea281d73d478aa8d09a8b83fbd265f715f7
# Khronos Vulkan-Headers (Apache-2.0), tag v1.4.310
VULKAN_URL=https://github.com/KhronosGroup/Vulkan-Headers.git
VULKAN_TAG=v1.4.310
VULKAN_COMMIT=cacef3039d277c448c89336290ec3937270b0996

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$HERE/src
DEPS=${DEPS:-$PWD/vela-interceptor-deps}
OUT=${OUT:-$PWD/out}/arm64ec
mkdir -p "$DEPS" "$OUT/obj" "$OUT/detours-obj"

for h in SGSR3_V1f_spv.h SGSR3_Student_spv.h misc_spv.h sgsr2_spv.h; do
  [ -f "$SRC/$h" ] || { echo "ERROR: src/$h missing" >&2; exit 1; }
done
# optional: an fp16 Vela AI recipe for devices without what the main one needs (INT8 dot products, q8 formats)
[ -f "$SRC/SGSR3_Student_spv_fallback.h" ] && echo "Vela AI fallback header: yes" || echo "Vela AI fallback header: no"

# ---- download and check
git_pin() {  # dir url commit: a checkout of exactly that commit
  local dir=$1 url=$2 commit=$3
  if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
  fi
  if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$commit" ]; then
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q --force FETCH_HEAD
  fi
  [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || { echo "ERROR: $dir is not at $commit" >&2; exit 1; }
}

TC_DIR=$DEPS/$LLVM_MINGW_NAME
if [ ! -d "$TC_DIR/bin" ]; then
  if [ ! -f "$DEPS/$LLVM_MINGW_NAME.tar.xz" ]; then
    echo ">> downloading llvm-mingw $LLVM_MINGW_VER"
    curl -fsSL -o "$DEPS/$LLVM_MINGW_NAME.tar.xz.part" "$LLVM_MINGW_URL"
    mv "$DEPS/$LLVM_MINGW_NAME.tar.xz.part" "$DEPS/$LLVM_MINGW_NAME.tar.xz"
  fi
  echo "$LLVM_MINGW_SHA256  $DEPS/$LLVM_MINGW_NAME.tar.xz" | sha256sum -c - \
    || { echo "ERROR: llvm-mingw archive does not match the pinned sha256" >&2; exit 1; }
  tar xf "$DEPS/$LLVM_MINGW_NAME.tar.xz" -C "$DEPS"
fi
echo ">> Detours $DETOURS_COMMIT"
git_pin "$DEPS/Detours" "$DETOURS_URL" "$DETOURS_COMMIT"
echo ">> Vulkan-Headers $VULKAN_TAG ($VULKAN_COMMIT)"
git_pin "$DEPS/Vulkan-Headers" "$VULKAN_URL" "$VULKAN_COMMIT"

TC=$TC_DIR/bin
CXX=$TC/arm64ec-w64-mingw32-clang++
DET=$OUT/include            # <detours/detours.h>
VK=$DEPS/Vulkan-Headers/include

# ---- Detours: static library for arm64ec (headers into $DET/detours, sources built from a patched copy)
mkdir -p "$DET/detours" "$OUT/detours-src"
cp "$DEPS/Detours/src/detours.h" "$DEPS/Detours/src/detver.h" "$DET/detours/"
cp "$DEPS/Detours/src/"*.cpp "$DEPS/Detours/src/"*.h "$OUT/detours-src/"
# MSVC's 'ui64' literal suffix is not standard C++.
sed -i 's/~0ui64/~0ULL/g' "$OUT/detours-src/detours.cpp"
( cd "$OUT/detours-src"
  for f in detours modules disasm image creatwth disolarm64; do
    "$CXX" -c "$f.cpp" -o "$OUT/detours-obj/detours_$f.o" -I. -O2 -DWIN32_LEAN_AND_MEAN -Wno-everything
  done )
"$TC/llvm-ar" rcs "$OUT/libdetours.a" "$OUT"/detours-obj/detours_*.o

# ---- the interceptor
FLAGS="-std=c++23 -O2 -DWIN32 -DNDEBUG -D_WINDOWS -D_USRDLL -DUNICODE -D_UNICODE
  -Wno-c++11-narrowing -Wno-everything -I$SRC -I$DET -I$VK"

for f in dllmain dxgi_proxy d3d12_recon vela_fsr2 sgsr3_fast_vk student_vk exposure_fix hdr_wrap ext_upscalers sgsr2_vk; do
  echo ">> $f"
  $CXX $FLAGS -c "$SRC/$f.cpp" -o "$OUT/obj/$f.o"
done

# mingw ships no vulkan-1 import library: make one from the vk* functions the objects call
{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"
  "$TC/llvm-nm" --undefined-only "$OUT"/obj/*.o \
    | awk '{print $NF}' | sed 's/^#//' | grep -E '^vk[A-Za-z0-9]+$' | sort -u
} > "$OUT/vulkan-1.def"
"$TC/arm64ec-w64-mingw32-dlltool" -d "$OUT/vulkan-1.def" -l "$OUT/libvulkan-1.a" -m arm64ec

$CXX -shared -o "$OUT/dxgi.dll" "$OUT"/obj/*.o \
  -L"$OUT" -ldetours -lvulkan-1 \
  -Wl,--out-implib,"$OUT/dxgi.lib" -ldxguid -lole32 -luuid \
  -static-libgcc -static-libstdc++

$TC/llvm-readobj --file-headers "$OUT/dxgi.dll" | grep -iE "Format|Machine" | head
md5sum "$OUT/dxgi.dll"
echo "built $OUT/dxgi.dll"
