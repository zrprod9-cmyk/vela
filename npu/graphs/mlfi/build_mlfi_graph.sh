#!/bin/bash
# Inside the vela-qnn container: build one MLFI stage ($STAGE = s1 | s3) for HTP targets in $ARCHS.
# Mounts: /src = npu/graphs/mlfi (ro), /qairt = QAIRT 2.47.0.260601 (ro),
#         /ffx/amd_fidelityfx_framegeneration_vk.dll (ro), /v = work (rw), /out = output (rw).
set -eu
STAGE=${STAGE:?set STAGE (s1 or s3)}
MODE=${MODE:-w8a16}
VTCM_MB=${VTCM_MB:-8}
ARCHS=${ARCHS:-"v73:43 v79:69 v81:87"}

case "$STAGE" in
  s1)
    BIN=mlfi_s1
    ONNX=mlfi_stage1_1080p.onnx
    IN=rgb01
    OUT=flow_px
    ;;
  s3)
    BIN=mlfi_s3
    ONNX=mlfi_stage3_1080p.onnx
    IN=feat24
    OUT=weights5
    ;;
  *)
    echo "Unknown STAGE: $STAGE (expected s1 or s3)" >&2
    exit 1
    ;;
esac

case "$MODE" in
  fp16|w8a16|w8a8) ;;
  *)
    echo "Unknown MODE: $MODE (expected fp16, w8a16, or w8a8)" >&2
    exit 1
    ;;
esac

R=/qairt
mkdir -p /tmp/cb
ln -sf /usr/bin/clang++-14 /tmp/cb/clang++
ln -sf /usr/bin/clang-14 /tmp/cb/clang
export QNN_SDK_ROOT=$R
export PYTHONPATH=$R/lib/python
export LD_LIBRARY_PATH=$R/lib/x86_64-linux-clang
export PATH=/tmp/cb:$R/bin/x86_64-linux-clang:$PATH

mkdir -p /v/onnx /v/calib
if [ ! -f "/v/onnx/$ONNX" ]; then
  python3 /src/extract_mlfi_onnx.py --dll /ffx/amd_fidelityfx_framegeneration_vk.dll --out /v/onnx --stage "$STAGE" > "/v/extract_${STAGE}.log" 2>&1 || {
    echo "$STAGE EXTRACT FAIL"
    tail -20 "/v/extract_${STAGE}.log"
    exit 1
  }
fi

if [ "$MODE" != fp16 ] && [ ! -f "/v/calib/${STAGE}_list.txt" ]; then
  python3 /src/gen_calib.py --out /v/calib --stage "$STAGE" > "/v/calib_${STAGE}.log" 2>&1 || {
    echo "$STAGE CALIB FAIL"
    tail -20 "/v/calib_${STAGE}.log"
    exit 1
  }
fi

m="${BIN}_${MODE}"
W="/v/$m"
mkdir -p "$W/libs"
cd "$W"

cat > "$W/io.yaml" <<EOF
- IOName: $IN
  Datatype: float16
- IOName: $OUT
  Datatype: float16
EOF

CONV_ARGS=(-i "/v/onnx/$ONNX" --custom_io "$W/io.yaml" -o "$W/$m.cpp")
case "$MODE" in
  fp16)
    CONV_ARGS+=(--float_bitwidth 16)
    ;;
  w8a16)
    CONV_ARGS+=(--input_list "/v/calib/${STAGE}_list.txt" --act_bitwidth 16 --weights_bitwidth 8 --use_per_channel_quantization)
    ;;
  w8a8)
    CONV_ARGS+=(--input_list "/v/calib/${STAGE}_list.txt" --act_bitwidth 8 --weights_bitwidth 8 --use_per_channel_quantization)
    ;;
esac

python3 "$R/bin/x86_64-linux-clang/qnn-onnx-converter" "${CONV_ARGS[@]}" > convert.log 2>&1 || {
  echo "$m CONVERT FAIL"
  tail -20 convert.log
  exit 1
}

qnn-model-lib-generator -c "$W/$m.cpp" -b "$W/$m.bin" -l "$m" -t x86_64-linux-clang -o "$W/libs" > libgen.log 2>&1 || {
  echo "$m LIBGEN FAIL"
  tail -20 libgen.log
  exit 1
}

echo "ONNX $m md5=$(md5sum < "/v/onnx/$ONNX" | cut -c1-32) lib=$(md5sum < "$W/libs/x86_64-linux-clang/lib$m.so" | cut -c1-32)"

rc=0
for h in $ARCHS; do
  IFS=: read -r a soc <<< "$h"
  mkdir -p "/out/$a"
  cat > "$W/htp_$a.json" <<EOF
{"graphs": [{"graph_names": ["$m"], "vtcm_mb": $VTCM_MB, "O": 3}], "devices": [{"soc_model": $soc, "dsp_arch": "$a"}]}
EOF
  cat > "$W/ctxcfg_$a.json" <<EOF
{"backend_extensions": {"shared_library_path": "$R/lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so", "config_file_path": "$W/htp_$a.json"}}
EOF
  qnn-context-binary-generator --backend "$R/lib/x86_64-linux-clang/libQnnHtp.so" \
    --model "$W/libs/x86_64-linux-clang/lib$m.so" --binary_file "$BIN" \
    --output_dir "/out/$a" --config_file "$W/ctxcfg_$a.json" > "ctx_$a.log" 2>&1 || {
    echo "$m CTX $a FAIL"
    grep -iE error "ctx_$a.log" | head -5 || tail -10 "ctx_$a.log"
    rc=1
    continue
  }
  echo "CTX $a/$BIN.bin sha256=$(sha256sum < "/out/$a/$BIN.bin" | cut -c1-64) size=$(stat -c %s "/out/$a/$BIN.bin") $(grep -E '^(spill|fill)_bytes' "ctx_$a.log" | tr '\n' ' ')"
done
exit "$rc"
