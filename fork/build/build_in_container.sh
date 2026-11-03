#!/usr/bin/env bash
# Runs inside the pinned ubuntu:22.04 image (started by build.sh), on an empty /work volume.
# Mounts: /fork (the fork/ directory, read-only), /work (scratch volume), /out (result).
#
# Pinned inputs (the compiler is llvm-mingw, so the result does not depend on the host compiler):
#   Sloptiscaler          f80e4da339cd4624212fd0546922571fe01205c1 (submodules as recorded there)
#   llvm-mingw            20260908-ucrt-ubuntu-22.04-x86_64, sha256 below (the version arm64ec/build.sh asks for)
#   DirectXMath, Detours  commits below; freetype tag VER-2-13-3 (set by arm64ec/build.sh)
# The source path matters: the compiler stores it in the objects, so the tree must live at /work/src.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
BASE=f80e4da339cd4624212fd0546922571fe01205c1
LLVM_MINGW_VER=20260908
LLVM_MINGW_SHA256=2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092
DIRECTXMATH_COMMIT=f5d82574b03417d905fdd2f85bfa9bf7a2f1deac
DETOURS_COMMIT=1dbd4ea281d73d478aa8d09a8b83fbd265f715f7
PATCHES="0001 0002 0003 0004 0005 0006 0007 0008 0010 0011 0012 0013 0014 0015 0016 0017 0018 0019"

apt-get update -qq
apt-get install -y -qq git curl xz-utils cmake ninja-build python3 build-essential ca-certificates >/dev/null
echo "host tools: $(cmake --version | head -1), ninja $(ninja --version), $(git --version), $(python3 --version)"

( cd /fork/patches && md5sum -c ../patches.md5 )

mkdir -p /work/deps && cd /work

git clone -q https://github.com/justradical/Sloptiscaler.git src
git -C src checkout -q "$BASE"
git -C src submodule update --init --recursive -q

pin() {  # pin <name> <url> <commit>: shallow checkout of one commit into /work/deps/<name>
  git init -q "/work/deps/$1" && git -C "/work/deps/$1" remote add origin "$2"
  git -C "/work/deps/$1" fetch -q --depth 1 origin "$3" && git -C "/work/deps/$1" checkout -q FETCH_HEAD
}
pin DirectXMath https://github.com/microsoft/DirectXMath.git "$DIRECTXMATH_COMMIT"
pin Detours     https://github.com/microsoft/Detours        "$DETOURS_COMMIT"

LLVM_DIR=llvm-mingw-${LLVM_MINGW_VER}-ucrt-ubuntu-22.04-x86_64
curl -sL -o deps/llvm-mingw.tar.xz \
  "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VER}/${LLVM_DIR}.tar.xz"
echo "$LLVM_MINGW_SHA256  deps/llvm-mingw.tar.xz" | sha256sum -c -
tar xf deps/llvm-mingw.tar.xz -C deps

cd src
for n in $PATCHES; do
  p=$(ls /fork/patches/${n}-*.patch)
  git apply --ignore-whitespace "$p"; echo "patch: $(basename "$p")"
done

# OptiScaler stores the build date in its version resource; FORK_BUILD_DATE overrides today's date.
if [ -n "${FORK_BUILD_DATE:-}" ]; then
  date() { if [ "${1:-}" = "+%Y-%m-%d" ]; then echo "$FORK_BUILD_DATE"; else command date "$@"; fi; }
  export -f date
fi

./arm64ec/build.sh /work/deps > /out/build.log 2>&1 || { tail -40 /out/build.log; exit 1; }
cp /work/deps/build/OptiScaler.dll /out/OptiScaler.dll
md5sum /out/OptiScaler.dll
