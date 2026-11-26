#!/bin/bash
# Inside the container: build one graph $G (a row of /src/graphs.txt) for HTP v73, v79 and v81.
# Mounts: /src = this folder (read-only), /qairt = QAIRT 2.47.0.260601, /fsr4vk = fsr4vk checkout, /v = work, /out = output.
# The generators read their inputs from fixed absolute paths (/vt /ht /ht3 /n /gn /nf /k1h /nsz /b); this script copies
# them there first.
set -u
row=$(awk -v g="$G" '$1 == g' /src/graphs.txt)
[ -n "$row" ] || { echo "$G: not in graphs.txt"; exit 1; }
read -r _ KIND PRESET m NSZ_W NSZ_HN NSZ_HR NSZ_WR <<< "$row"
FLG=$(cat /src/flags/$PRESET.txt); [ $KIND = ht ] && FLG=$(cat /src/flags/exact.txt)
INIT=/fsr4vk/assets/fsr411/general/1080/$PRESET/initializers.bin
case $PRESET in
  performance) WANT=a54e552ff69a3f7199861417f7ae461d1f67f636ab532c6ec9d8ea5d2963735a;;
  ultraperf)   WANT=3c42fc6eabfa9b5b1e489df0e9b2f58e3f9fa22048c584f6839bb27d32a0640e;;
esac
[ "$(sha256sum < "$INIT" | cut -c1-64)" = "$WANT" ] || { echo "$G: $INIT is not the fsr4vk 391f62bd file"; exit 1; }

# generator layout
S=/src/gen; P=/src/params/$PRESET
mkdir -p /vt /ht /ht3 /n/n1 /gn /nf /k1h/build /nsz/tools /b
cp $S/gen_bp.py /vt/; cp $S/gen_nsz.py /nsz/tools/; cp $S/gen_ht3.py /ht3/; cp $S/gen_ht.py /ht/; cp $S/gen_n1.py /n/n1/
cp $S/gen_n7.py $S/gen_n9.py /gn/; cp $S/gen_k1h.py /k1h/; cp $P/k1_w.npz /k1h/build/
cp $S/gen_nf2.py $S/$PRESET/gen_nf.py $P/params_n1.pkl $P/p13_w.npz /nf/
[ -f $P/p07_sh.npy ] && cp $P/p07_sh.npy /nf/
cp "$INIT" /b/initializers.bin
GEN=/vt/gen_bp.py; [ $KIND = n9 ] && GEN=/nsz/tools/gen_nsz.py

R=/qairt
mkdir -p /tmp/cb; ln -sf /usr/bin/clang++-14 /tmp/cb/clang++; ln -sf /usr/bin/clang-14 /tmp/cb/clang
export QNN_SDK_ROOT=$R PYTHONPATH=$R/lib/python LD_LIBRARY_PATH=$R/lib/x86_64-linux-clang PATH=/tmp/cb:$R/bin/x86_64-linux-clang:$PATH
W=/v/$G; mkdir -p $W/libs; cd $W
# 1. int8 QDQ ONNX graph $m.onnx + custom I/O file $m.io.yaml
KIND=$KIND VARIANTS="$m:$FLG:0" INIT_BIN=/b/initializers.bin NSZ_W=${NSZ_W:-} NSZ_HN=${NSZ_HN:-} NSZ_HR=${NSZ_HR:-} NSZ_WR=${NSZ_WR:-} \
  python3 $GEN $W > gen.log 2>&1
[ -f $m.onnx ] || { echo "$G GEN FAIL"; tail -15 gen.log; exit 1; }
# 2. QNN C++ model, 3. x86 model library (input of both chips)
python3 $R/bin/x86_64-linux-clang/qnn-onnx-converter -i $m.onnx --custom_io $m.io.yaml --float_fallback --float_bitwidth 16 -o $m.cpp > convert.log 2>&1 \
  || { echo "$G CONVERT FAIL"; tail -8 convert.log; exit 1; }
qnn-model-lib-generator -c $m.cpp -b $m.bin -l $m -t x86_64-linux-clang -o libs > libgen.log 2>&1 || { echo "$G LIBGEN FAIL"; tail -8 libgen.log; exit 1; }
echo "ONNX $G md5=$(md5sum < $m.onnx | cut -c1-32) lib=$(md5sum < libs/x86_64-linux-clang/lib$m.so | cut -c1-32)"
# 4. HTP context binaries, dsp_arch:soc_model (43 = SM8550, 69 = SM8750, 87 = SM8850), vtcm_mb 8, O 3
rc=0
for h in v73:43 v79:69 v81:87; do IFS=: read -r a soc <<< "$h"; mkdir -p /out/$a
  echo "{\"graphs\": [{\"graph_names\": [\"$m\"], \"vtcm_mb\": 8, \"O\": 3}], \"devices\": [{\"soc_model\": $soc, \"dsp_arch\": \"$a\"}]}" > htp_$a.json
  echo "{\"backend_extensions\": {\"shared_library_path\": \"$R/lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so\", \"config_file_path\": \"$W/htp_$a.json\"}}" > ctxcfg_$a.json
  qnn-context-binary-generator --backend $R/lib/x86_64-linux-clang/libQnnHtp.so --model libs/x86_64-linux-clang/lib$m.so \
    --binary_file $G --output_dir /out/$a --config_file ctxcfg_$a.json > ctx_$a.log 2>&1 \
    || { echo "$G CTX $a FAIL"; grep -iE error ctx_$a.log | head -3; rc=1; continue; }
  echo "CTX $a/$G.bin sha256=$(sha256sum < /out/$a/$G.bin | cut -c1-64) size=$(stat -c %s /out/$a/$G.bin) $(grep -E '^(spill|fill)_bytes' ctx_$a.log | tr '\n' ' ')"
done
exit $rc
