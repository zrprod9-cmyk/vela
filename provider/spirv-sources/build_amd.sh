#!/bin/sh
# Rebuild the three AMD-derived headers (overlay/ov/vela-fast-spv.hpp, overlay/gen/fsr4fu-spv.hpp, overlay/ov2/fsr412lite-blobs.hpp)
# from the sources in this folder and AMD's FSR 4.1.2 Lite provider DLL, and compare them with the copies in ../overlay.
# Needs Docker and network access (Ubuntu packages; the AMD DLL if it is not in the input folder yet).
# usage (from this folder): sh build_amd.sh [input dir, default ./amd_in]
#   input dir/amd_fidelityfx_upscaler_vk.dll  is downloaded by amd/fetch.sh when missing (SHA-256 checked)
# Output: out/amd/*.hpp and a line "IDENTICAL" or "DIFFERENT" per header (out/build_amd.log).
set -e
IN=${1:-./amd_in}
sh amd/fetch.sh "$IN"
H=$(pwd); mkdir -p out/amd; D=$(cd "$IN" && pwd)
MSYS_NO_PATHCONV=1 docker run --rm -v "$H:/w" -v "$H/../overlay:/overlay:ro" -v "$D:/amd_in:ro" ubuntu:24.04 bash -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq glslang-tools=15.1.0-2~ubuntu0.24.04.2 spirv-tools=2025.1~rc1-1~ubuntu0.24.04.2 python3 >/dev/null
W=/w; T=/w/tools; S=/w/out/amd; A=/tmp/amd
mkdir -p $S/fast $S/fu $S/lite $A /tmp/fu
python3 -I $W/amd/extract_lite.py /amd_in/amd_fidelityfx_upscaler_vk.dll $W/amd/lite_modules.tsv $A
export TOOLS=$T WEIGHTS=$A/lite_weights.bin
ENT=fsr4_model_v07_fp8_no_scale_

# FSR 4 Fast kernels: GLSL (Vulkan 1.1) and SPIR-V assembly
for n in q3kb_p q3kb_u q3 q1mkb_p q1mkb_u q1m rha_p rha_u rha15_p rha15s_p u15c u15b; do
  glslangValidator -V --target-env vulkan1.1 $W/fast/$n.comp -o $S/fast/$n.spv >/dev/null
done
for n in plk_p plk_u; do spirv-as --target-env vulkan1.1 $W/fast/$n.spvasm -o $S/fast/$n.spv; done

# Lite modules
for m in 0001 0003 0004 0006 0008 0010; do bash $T/xf.sh $A/$m.spv $S/lite/$m.spv $T; done
COMP=$W/lite/pre_lds.comp NAME=0000 ENTRY=${ENT}prepass OUT=$S/lite bash $T/p0.sh
COMP=$W/lite/pre_r2c.comp NAME=pre_r2c ENTRY=${ENT}prepass OUT=$S/lite bash $T/p0.sh
mkdir -p /tmp/o9 /tmp/o13
COMP=$W/lite/b1_0009.comp MOD=0009 ENTRY=${ENT}postpass OUT=/tmp/o9 bash $T/p.sh; cp /tmp/o9/0009.spv $S/lite/
COMP=$W/lite/k2cl.comp MOD=0013 ENTRY=${ENT}postpass OUT=/tmp/o13 bash $T/p.sh; cp /tmp/o13/0013.spv $S/lite/
TB=1 python3 -I $T/gen_conv_p1.py $W/lite/converted/0002.comp $A/lite_weights.bin 8 /tmp/c0002.comp
bash $T/conv.sh /tmp/c0002.comp ${ENT}pass1 $S/lite/0002.spv
python3 -I $T/gen_conv_px.py 2 1 8 /tmp/c0005.comp $W/lite/converted $A/lite_weights.bin
bash $T/conv.sh /tmp/c0005.comp ${ENT}pass2 $S/lite/0005.spv
python3 -I $T/gen_conv_px.py 3 1 8 /tmp/c0007.comp $W/lite/converted $A/lite_weights.bin
bash $T/conv.sh /tmp/c0007.comp ${ENT}pass3 $S/lite/0007.spv

# fused kernels of the NPU path
python3 -I $T/gen_fu.py $W/lite/pre_lds.comp $W/lite/b1_0009.comp $W/fused/npu_select.comp /tmp/fu
COMP=/tmp/fu/fu_pre.comp NAME=fu_pre ENTRY=${ENT}prepass OUT=$S/fu bash $T/p0.sh
for v in fu_k1 fu_k1s fu_k1d; do
  mkdir -p /tmp/o_$v; COMP=/tmp/fu/$v.comp MOD=0009 ENTRY=${ENT}postpass OUT=/tmp/o_$v bash $T/p.sh; cp /tmp/o_$v/0009.spv $S/fu/$v.spv
done
glslangValidator -V /tmp/fu/npu_select_fu.comp -o $S/fu/npu_select_fu.spv >/dev/null

# headers
python3 -I $T/gen_amd_hpp.py fast $S/vela-fast-spv.hpp $S/fast
python3 -I $T/gen_amd_hpp.py fu $S/fsr4fu-spv.hpp $S/fu
python3 -I $T/gen_amd_hpp.py lite $S/fsr412lite-blobs.hpp $S/lite $A/lite_weights.bin $W/lite/pass_table.inc
cmp $S/vela-fast-spv.hpp /overlay/ov/vela-fast-spv.hpp && echo "vela-fast-spv.hpp IDENTICAL" || echo "vela-fast-spv.hpp DIFFERENT"
cmp $S/fsr4fu-spv.hpp /overlay/gen/fsr4fu-spv.hpp && echo "fsr4fu-spv.hpp IDENTICAL" || echo "fsr4fu-spv.hpp DIFFERENT"
cmp $S/fsr412lite-blobs.hpp /overlay/ov2/fsr412lite-blobs.hpp && echo "fsr412lite-blobs.hpp IDENTICAL" || echo "fsr412lite-blobs.hpp DIFFERENT"
md5sum $S/*.hpp
' 2>&1 | tee out/build_amd.log
