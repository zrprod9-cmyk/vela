#!/bin/bash
# Build FSR 4.0.2 ML Frame Generation (mlfi_4k) NPU graphs (HTP context binaries for v73, v79 and v81) in Docker.
#
#   QAIRT_DIR=<qairt>/2.47.0.260601 FFX_FG_DLL=<path/to/amd_fidelityfx_framegeneration_vk.dll> OUT_DIR=<output> ./build.sh [all | s1 | s3]
#
# QAIRT_DIR   unpacked Qualcomm AI Engine Direct SDK 2.47.0.260601 folder (contains bin/ and lib/), mounted read-only
# FFX_FG_DLL  user-supplied FidelityFX SDK v2.4.0 amd_fidelityfx_framegeneration_vk.dll (SHA-256 verified), mounted read-only
# OUT_DIR     receives v73/mlfi_{s1,s3}.bin, v79/mlfi_{s1,s3}.bin and v81/mlfi_{s1,s3}.bin
# WORK_DIR    build folders and logs (default: $OUT_DIR/work)
# IMAGE       Docker image built from ../Dockerfile (default: vela-qnn)
# CPUS, MEM   container limits (default: 4, 8g)
# MODE        quantization mode: w8a16 (default), fp16, or w8a8
# VTCM_MB     HTP VTCM budget in MB (default: 8)
# ARCHS       space-separated dsp_arch:soc_model pairs (default: "v73:43 v79:69 v81:87")
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
: "${QAIRT_DIR:?set QAIRT_DIR}" "${FFX_FG_DLL:?set FFX_FG_DLL}" "${OUT_DIR:?set OUT_DIR}"
[ -f "$FFX_FG_DLL" ] || { echo "FFX_FG_DLL not found: $FFX_FG_DLL" >&2; exit 1; }

WORK_DIR=${WORK_DIR:-$OUT_DIR/work}
IMAGE=${IMAGE:-vela-qnn}
CPUS=${CPUS:-4}
MEM=${MEM:-8g}
MODE=${MODE:-w8a16}
VTCM_MB=${VTCM_MB:-8}
ARCHS=${ARCHS:-"v73:43 v79:69 v81:87"}

mkdir -p "$OUT_DIR" "$WORK_DIR"
if [ $# -eq 0 ] || [ "$1" = all ]; then
  set -- s1 s3
fi

STAGES=""
for arg in "$@"; do
  case "$arg" in
    s1|mlfi_s1) STAGES="$STAGES s1" ;;
    s3|mlfi_s3) STAGES="$STAGES s3" ;;
    *) echo "Unknown stage: $arg (expected all, s1, s3, mlfi_s1, mlfi_s3)" >&2; exit 1 ;;
  esac
done

fail=0
for ST in $STAGES; do
  echo "== mlfi_$ST ($MODE)"
  MSYS_NO_PATHCONV=1 docker run --rm --cpus "$CPUS" -m "$MEM" \
    -v "$HERE:/src:ro" \
    -v "$QAIRT_DIR:/qairt:ro" \
    -v "$FFX_FG_DLL:/ffx/amd_fidelityfx_framegeneration_vk.dll:ro" \
    -v "$WORK_DIR:/v" \
    -v "$OUT_DIR:/out" \
    -e STAGE="$ST" \
    -e MODE="$MODE" \
    -e VTCM_MB="$VTCM_MB" \
    -e ARCHS="$ARCHS" \
    "$IMAGE" bash /src/build_mlfi_graph.sh 2>&1 | tee "$WORK_DIR/mlfi_$ST.log"
  for h in $ARCHS; do
    a=${h%%:*}
    grep -q "^CTX ${a}/mlfi_${ST}\.bin" "$WORK_DIR/mlfi_$ST.log" || {
      echo "mlfi_$ST ($a): FAILED"
      fail=1
    }
  done
done
exit "$fail"
