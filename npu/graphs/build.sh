#!/bin/bash
# Build Vela NPU graphs (HTP context binaries for v73, v79 and v81) in Docker.
#
#   QAIRT_DIR=<qairt>/2.47.0.260601 FSR4VK_DIR=<fsr4vk checkout> OUT_DIR=<output> ./build.sh [all | <file> ...]
#
# QAIRT_DIR   the unpacked Qualcomm AI Engine Direct SDK version folder (contains bin/ and lib/), mounted read-only
# FSR4VK_DIR  a checkout of https://github.com/dvj5411/fsr4vk at 391f62bd198ac9a289aba8ee0d4396db2104f3a2, mounted read-only
# OUT_DIR     receives v73/<file>.bin, v79/<file>.bin and v81/<file>.bin
# WORK_DIR    build folders and logs, one per graph (default: $OUT_DIR/work)
# IMAGE       Docker image built from ./Dockerfile (default: vela-qnn)
# CPUS, MEM   container limits (default: 4, 8g)
# <file>      a first column of graphs.txt (nf4_exact, nf4_k1h, nf4up_k1h_452, ...); "all" (default) builds all 17.
# Graphs are built one after another. Then check them: python3 compare.py "$OUT_DIR"
# On Windows, run it from Git Bash and give the three folders as Windows paths with forward slashes.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
: "${QAIRT_DIR:?set QAIRT_DIR}" "${FSR4VK_DIR:?set FSR4VK_DIR}" "${OUT_DIR:?set OUT_DIR}"
WORK_DIR=${WORK_DIR:-$OUT_DIR/work}; IMAGE=${IMAGE:-vela-qnn}; CPUS=${CPUS:-4}; MEM=${MEM:-8g}
mkdir -p "$OUT_DIR" "$WORK_DIR"
if [ $# -eq 0 ] || [ "$1" = all ]; then set -- $(grep -v '^#' "$HERE/graphs.txt" | awk '{print $1}'); fi
fail=0
for G in "$@"; do
  awk -v g="$G" '$1 == g {f = 1} END {exit !f}' "$HERE/graphs.txt" || { echo "$G: not in graphs.txt"; fail=1; continue; }
  echo "== $G"
  # MSYS_NO_PATHCONV stops Git Bash on Windows from rewriting the container paths; elsewhere it does nothing.
  MSYS_NO_PATHCONV=1 docker run --rm --cpus "$CPUS" -m "$MEM" \
    -v "$HERE:/src:ro" -v "$QAIRT_DIR:/qairt:ro" -v "$FSR4VK_DIR:/fsr4vk:ro" -v "$WORK_DIR:/v" -v "$OUT_DIR:/out" \
    -e G="$G" "$IMAGE" bash /src/build_graph.sh 2>&1 | tee "$WORK_DIR/$G.log"
  grep -q "^CTX v73/" "$WORK_DIR/$G.log" && grep -q "^CTX v79/" "$WORK_DIR/$G.log" && grep -q "^CTX v81/" "$WORK_DIR/$G.log" || { echo "$G: FAILED"; fail=1; }
done
exit $fail
