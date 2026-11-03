#!/bin/sh
# Builds the OptiScaler fork the plugin carries (ARM64EC, Docker Desktop on the PC) and puts it into
# assets/sgsr3/OptiScaler.dll:
#   sh decky-upscale/tools/build_fork.sh [EXTRA_DIR]
# EXTRA_DIR (optional): files laid over the patched tree by their repository paths, e.g. a colour fix of the v1
# DXIL path from another branch: EXTRA_DIR/OptiScaler/upscalers/sgsr3/SGSR3_Shaders.hlsl + SGSR3_Shaders_dxbc.h.
# Work dirs: Docker volume slopt-plugin-work (sources, toolchain; ~2.5 GB).
# A new DLL in assets needs no VERSION.json edit to be recognised (the plugin takes its md5 from the file); edit
# VERSION.json's "dll" / "source" for the label the panel shows.
set -eu
SLOPT_DIR=${SLOPT_DIR:-$HOME/slopt-plugin}   # work folder on the host (out/, extra/)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
export PATH="/c/Program Files/Docker/Docker/resources/bin:$PATH"
OUT=$SLOPT_DIR/out
mkdir -p "$OUT" $SLOPT_DIR/extra
EXTRA=${1:-$SLOPT_DIR/extra}
cp "$REPO/decky-upscale/tools/fork_in_container.sh" $SLOPT_DIR/fork_in_container.sh
rm -f "$OUT/OptiScaler.dll"
win() { cygpath -m "$1" 2>/dev/null || echo "$1"; }
MSYS_NO_PATHCONV=1 docker run --rm -e PATCHES="${PATCHES:-}" \
  -v slopt-plugin-work:/work \
  -v "$(win "${PATCHES_DIR:-$REPO/fork/patches}"):/patches:ro" \
  -v "$(win "$EXTRA"):/extra:ro" \
  -v "$(win $SLOPT_DIR):/host:ro" \
  -v "$(win "$OUT"):/out" \
  ubuntu:22.04 bash -c "cp /host/fork_in_container.sh /tmp/b.sh && bash /tmp/b.sh"
[ -f "$OUT/OptiScaler.dll" ] || { echo "no DLL built"; exit 1; }
cp "$OUT/OptiScaler.dll" "$REPO/decky-upscale/assets/sgsr3/OptiScaler.dll"
md5sum "$REPO/decky-upscale/assets/sgsr3/OptiScaler.dll"
