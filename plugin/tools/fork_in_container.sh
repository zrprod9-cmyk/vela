#!/usr/bin/env bash
# Runs inside ubuntu:22.04 (decky-upscale/tools/build_fork.sh starts it): Sloptiscaler at the pinned commit in the
# volume (/work/src, cloned on the first run), the fork's patches from the repository (/patches, 0001-0008 by
# default, PATCHES="0001 0002 ..." to choose), then /extra copied over the tree (e.g. a fixed SGSR3_Shaders.hlsl +
# SGSR3_Shaders_dxbc.h), then arm64ec/build.sh. The DLL goes to /out.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
if ! command -v cmake >/dev/null; then
  apt-get update -qq && apt-get install -y -qq git curl xz-utils cmake ninja-build python3 build-essential >/dev/null
fi
cd /work
if [ ! -d src ]; then
  git clone -q https://github.com/justradical/Sloptiscaler.git src
  git -C src checkout -q f80e4da339cd4624212fd0546922571fe01205c1
  git -C src submodule update --init --depth 1 --jobs 8 -q
fi
cd src
git checkout -q f80e4da339cd4624212fd0546922571fe01205c1 2>/dev/null || true
git checkout -q -- . && git clean -fdq OptiScaler
for n in ${PATCHES:-0001 0002 0003 0004 0005 0006 0007 0008}; do
  p=$(ls /patches/${n}-*.patch)
  git apply --ignore-whitespace "$p" || { echo "PATCH FAILED: $(basename "$p")"; exit 1; }
  echo "patch: $(basename "$p")"
done
if [ -d /extra ] && [ -n "$(ls -A /extra 2>/dev/null)" ]; then
  cp -rv /extra/. . | sed 's/^/extra: /'
fi
./arm64ec/build.sh /work/deps 2>&1 | tail -25
find /work/deps/build -maxdepth 2 -iname 'OptiScaler*.dll' -newer /work/src/arm64ec/build.sh -exec cp -v {} /out/ \;
