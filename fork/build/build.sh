#!/bin/sh
# Builds Vela's OptiScaler fork (ARM64EC OptiScaler.dll) from a clean clone.
#
#   sh fork/build/build.sh <outdir>
#
# Run from the repository root. Needs Docker on an x86-64 host and network access.
# The build runs in the pinned image below, in a new scratch Docker volume that is removed afterwards.
# FORK_BUILD_DATE=YYYY-MM-DD fixes the build date that OptiScaler stores in its version resource
# (default: today). Output: <outdir>/OptiScaler.dll and <outdir>/build.log.
set -eu
IMAGE="ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956"
OUT=${1:-out}; mkdir -p "$OUT"
VOL="vela-fork-work-$$"
win() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }
docker volume create "$VOL" >/dev/null
status=0
MSYS_NO_PATHCONV=1 docker run --rm \
  -e FORK_BUILD_DATE="${FORK_BUILD_DATE:-}" \
  -v "$VOL":/work \
  -v "$(win "$(pwd)/fork")":/fork:ro \
  -v "$(win "$(cd "$OUT" && pwd)")":/out \
  "$IMAGE" bash /fork/build/build_in_container.sh || status=$?
docker volume rm "$VOL" >/dev/null || true
exit $status
