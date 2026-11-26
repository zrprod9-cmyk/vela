# How the Vela NPU graphs are built

This page describes how the 34 graph files in `vela-npu-sysext-14.raw` (the same files are in `vela-npu-sysext-15.raw`; package 17 has new v73 files, see [`npu/graphs/pkg17/README.md`](../npu/graphs/pkg17/README.md); package 18 adds the five ANF graph families, see [`npu/graphs/pkg18/README.md`](../npu/graphs/pkg18/README.md)) were made: 17 for HTP v73 (Snapdragon 8 Gen 2,
SM8550) and the same 17 for HTP v79 (Snapdragon 8 Elite, SM8750). It lists the inputs, the tool versions, the commands,
and the hashes a correct build gives.

The scripts, parameter files and flag strings are in [`npu/graphs/`](../npu/graphs/). Section "Rebuild" below has the
commands. Anything we have not measured is marked **[unknown]**.

## 0. In short

- The graphs are **AMD's FSR 4.1.1 INT8 network** (`general/1080` weights, Performance and Ultra Performance presets),
  rewritten by our own Python scripts as a QDQ int8 ONNX graph with **integer math that matches the shader bit for bit**.
  We do not export a framework model. We rebuild each layer from the captured compute shaders and their
  `initializers.bin` weight buffer.
- The ONNX graph goes through the normal QAIRT 2.47 chain: `qnn-onnx-converter` → `qnn-model-lib-generator` (x86 model
  library) → `qnn-context-binary-generator` (HTP context binary), with `soc_model 43 / v73` or `soc_model 69 / v79`,
  `vtcm_mb 8`, `O 3`.
- The build is deterministic. The same x86 model library rebuilds the shipped v73 file byte for byte, and two full
  rebuilds gave the same md5 for every graph.
- Every graph is checked against a numpy model of the shader math on the x86 HTP simulator. The result must be 0 bytes
  different. On the device, the installer checks each graph with a fixed input and an expected output hash (section 6).

## Rebuild

You need: Docker on an x86_64 machine (Linux, or Windows with Docker Desktop and Git Bash), about 8 GB RAM per container,
the QAIRT SDK (section 2) and a checkout of fsr4vk at the commit in section 1.1. One graph takes about 1.5 minutes with
4 CPUs.

```sh
# 1. the weights: fsr4vk at the exact commit (only assets/fsr411/general/1080/{performance,ultraperf}/initializers.bin are read)
git clone https://github.com/dvj5411/fsr4vk && git -C fsr4vk checkout 391f62bd198ac9a289aba8ee0d4396db2104f3a2
# 2. the SDK: download v2.47.0.260601.zip from Qualcomm (link and sha256 in section 2) and unpack it
unzip v2.47.0.260601.zip -d qairt-sdk        # gives qairt-sdk/qairt/2.47.0.260601
# 3. the build image (the SDK is mounted, not copied in); a --no-cache build of it gives the files of section 5
docker build -t vela-qnn npu/graphs
# 4. build: all 17 graphs, or name some (first column of npu/graphs/graphs.txt)
QAIRT_DIR=$PWD/qairt-sdk/qairt/2.47.0.260601 FSR4VK_DIR=$PWD/fsr4vk OUT_DIR=$PWD/out npu/graphs/build.sh all
QAIRT_DIR=$PWD/qairt-sdk/qairt/2.47.0.260601 FSR4VK_DIR=$PWD/fsr4vk OUT_DIR=$PWD/out npu/graphs/build.sh nf4_k1h nf4up_k1h_452
# 5. check: sha256 of out/v73/*.bin and out/v79/*.bin against section 5
python3 npu/graphs/compare.py out
```

`build.sh` runs one container per graph (`--cpus 4 -m 8g`, change with `CPUS` / `MEM`). It writes `out/v73/<file>.bin`,
`out/v79/<file>.bin`, and one build folder per graph in `out/work/<file>/` (ONNX, C++ model, model library, logs). Each
graph prints an `ONNX ... md5=` line and two `CTX ... sha256=` lines. `compare.py` exits with 1 if a built file differs.

What is in `npu/graphs/`:

| path | what |
|---|---|
| `build.sh` | host side: checks the arguments and runs `build_graph.sh` in Docker with the SDK, fsr4vk, work and output folders mounted |
| `build_graph.sh` | in the container: lays out the generator inputs, then generator → `qnn-onnx-converter` → `qnn-model-lib-generator` → `qnn-context-binary-generator` for v73 and v79 (sections 3.1-3.3) |
| `graphs.txt` | the 17 graphs: file name, generator kind, preset, graph name, size parameters (section 3.4) |
| `flags/performance.txt`, `flags/ultraperf.txt`, `flags/exact.txt` | the flag strings of section 1.2 step 3 |
| `gen/` | the graph generators (section 1.2 step 2); `gen/performance/gen_nf.py` and `gen/ultraperf/gen_nf.py` differ in the Ultra Performance pass-07 and pass-23 constants |
| `params/performance/`, `params/ultraperf/` | layer parameters read from the shaders: `params_n1.pkl` (passes 03-25), `p13_w.npz` (pass 13), `k1_w.npz` (pass 27), `p07_sh.npy` (Ultra Performance pass-07 store shift); sha256 in `params/SHA256SUMS` |
| `regen/` | rebuilds `params/` from the fsr4vk shaders (section "Regenerating the parameter files"): `Dockerfile` (SPIR-V tools image), `regen.sh` (host side), `decompile.sh` (spirv-cross and the SPIR-V transform), `shader_params.py` (`params_n1.pkl`, `p07_sh.npy`), `parse_p13.py` (`p13_w.npz`), `parse_k1.py` (`k1_w.npz`), `check.py` (compares with `params/`) |
| `Dockerfile` | the build image |
| `compare.py`, `expected.sha256` | the check against section 5 |

The generators read their inputs from fixed absolute paths inside the container (`/vt`, `/ht`, `/ht3`, `/n`, `/gn`, `/nf`,
`/k1h`, `/nsz`, `/b`); `build_graph.sh` copies the files there. The work folder of a graph must be `/v/<file>`, as
`build.sh` mounts it.

### Regenerating the parameter files

`npu/graphs/params/` is made from the fsr4vk shaders. To rebuild it and compare with the committed files:

```sh
docker build -t vela-spv npu/graphs/regen
FSR4VK_DIR=$PWD/fsr4vk OUT_DIR=$PWD/params-new npu/graphs/regen/regen.sh      # both presets, about 2 minutes each
python3 npu/graphs/regen/check.py params-new                                  # exit 1 if a file differs
```

Per preset, `regen.sh` runs one container (`--cpus 4 -m 8g`) that:

1. decompiles `pass-03` … `pass-27` of `general/1080/<preset>/` with `spirv-cross --vulkan-semantics` into
   `<preset>/dec/o/pNN.comp`. Passes 15, 17, 19 and 27 are also transformed and decompiled again into
   `<preset>/dec/t/qNN.comp`: `spirv-dis` → `provider/spirv-sources/tools/sudot.py` (each OpSDot becomes OpSUDot plus a
   correction term, exact modulo 2^32) → `provider/spirv-sources/tools/zconst.py` (reads of the z invocation id become 0;
   fsr4vk dispatches one workgroup in z) → `Unroll` on every loop → `spirv-as` → `spirv-val` → `spirv-cross`. The
   transform keeps the math and gives straight-line code the parsers can read (`regen/decompile.sh`).
2. `shader_params.py` writes `params_n1.pkl` (passes 03-25 except 13) and, for Ultra Performance, `p07_sh.npy`. It
   evaluates the address expressions of the GLSL at invocation 0 and reads the weight words from `initializers.bin`.
3. `parse_p13.py` writes `p13_w.npz`.
4. `parse_k1.py` writes `k1_w.npz`. It runs the K1 part of the q27 GLSL in numpy, finds the weights with impulse inputs,
   and checks on random inputs that a plain conv with those weights gives the same bytes as the GLSL code (0 differ).

The parsers match the exact text this SPIRV-Cross commit writes (variable names, expression form), so the image pins it:
Ubuntu 24.04.5 by digest, SPIRV-Cross `57e4efcc84112abcd426dc5ec248b5339bee04f2`, SPIRV-Tools
`2025.1~rc1-1~ubuntu0.24.04.2`, Python 3.12.3, numpy 2.5.3. Another SPIRV-Cross version can rename variables; the
parsers then stop at an assert instead of writing other numbers.

Result on 2026-11-22 (image built with `--no-cache`): `params_n1.pkl` byte for byte for both presets; `p13_w.npz`,
`k1_w.npz` and `p07_sh.npy` array for array (an `.npz` holds file times, so its bytes always differ).

## 1. Inputs

### 1.1 The network and its weights

| item | value |
|---|---|
| network | AMD FSR 4.1.1, INT8 path, model passes `pass-03` … `pass-25` (U-Net body), plus `pass-27` (the first post-pass block, "K1") on all graphs except `nf4_exact` |
| source of the passes | the `fsr4vk` project, `assets/fsr411/general/1080/<preset>/` (SPIR-V `pass-NN.spv` + `initializers.bin` + `manifest.json`), https://github.com/dvj5411/fsr4vk, tag `v0.4.3` + 1 commit (`391f62bd198ac9a289aba8ee0d4396db2104f3a2`) |
| how fsr4vk got them | per its `manifest.json`: captured from AMD's official FSR 4.1.1 DLL (`dll_sha256 d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46`) running under vkd3d-proton with a GFXReconstruct capture and a shader dump; `"version": "4.1.1"`, `"arithmetic": "int8"`, `"backend": "portable"`, capacity 1920x1080 |
| Performance weights | `general/1080/performance/initializers.bin`, 131072 bytes, sha256 `a54e552ff69a3f7199861417f7ae461d1f67f636ab532c6ec9d8ea5d2963735a` (manifest render size 960x540) |
| Ultra Performance weights | `general/1080/ultraperf/initializers.bin`, 131072 bytes, sha256 `3c42fc6eabfa9b5b1e489df0e9b2f58e3f9fa22048c584f6839bb27d32a0640e` (manifest render size 640x360) |
| which graphs use which | `nf4_*` = Performance weights; `nf4up_*` = Ultra Performance weights |

Only the `general/1080` weights are used. The same weights serve every output size. Only the tensor shapes change
(section 1.3).

### 1.2 How the ONNX graph is made (our scripts)

There is no ONNX export from AMD. Our chain builds the graph from scratch:

1. **Read the layer parameters from the shaders.** For each pass, a parser reads the decompiled shader and pulls out the
   packed int8 weights, the shift `sh` of each layer and the residual factors. It writes one parameter file per weight
   set. Biases are read from `initializers.bin`. Pass 13 (2x2 stride-2 conv, 32→64, `sh 8`) has its own parser, and so
   does the K1 block (pass 27). The outputs are in `npu/graphs/params/`; the parsers are in `npu/graphs/regen/`, and
   `regen/regen.sh` rebuilds every file from fsr4vk (section "Regenerating the parameter files").
2. **Write the int8 graph** (`gen_n1.py` → `gen_nf.py` → `gen_nf2.py` → `gen_ht.py` → `gen_k1h.py`, then the shipped
   builder `gen_bp.py`, which applies the patches in `gen_ht3.py`; `gen_n7.py` for 720p; `gen_nsz.py` + `gen_n9.py` for
   the other sizes). All are in `npu/graphs/gen/`. Rules the graph follows:
   - Activations have scale 1. Each uint8 byte is the shader's int8 byte XOR 0x80 (zero point 128). Weights are
     per-channel with scale `2^-sh`. Every Concat has its own QuantizeLinear/DequantizeLinear pair. ReLU outputs are uint8
     with zero point 0.
   - The shader rounds half to even after `>> sh`. One HTP rounding cannot do that, so each conv runs as two "views"
     (`rhe(a,s) = rhu(a+h-1, s+1) + rhu(a-h, s+1)`, `h = 2^(s-1)`) added at scale 1. Biases come in through constant
     input channels on the centre tap, with HTP bias 0. On most layers a per-layer "bias split", plus a one-channel bias
     move found by a search on the simulator, removes one of the two views and keeps the output exact. That is what the
     flag strings below select.
   - Residuals `R*y` are folded into the 1x1 conv as identity input channels, with R split into int8 pieces
     (256 = 127+127+2, 128 = 127+1). The two transposed-conv tails are built as SpaceToDepth → 1x1 conv → DepthToSpace
     (DCR), so the int32 accumulator is exact.
   - Pixel pairs are packed: tensors are declared as `W/2 x 32` channels, which holds the same bytes as `W x 16`.
   - ONNX opset 13, `ir_version 8`, checked with `onnx.checker`.
3. **Flags of the shipped graphs** (environment variable `VARIANTS="k1h_a:<flags>:0"`; files `npu/graphs/flags/*.txt`):
   - Performance (`nf4_k1h*`):
     `T2+P07S+P13S+K1+Zp03c3+Zp27c3@26k-1+Zp05c3@10k-1+Zp25c3@1k-1+Zp09c3+Zp11c3@24k1+Zp15c3+Zp17c3+Zp19c3+Zp21c3@14k1+Zp23c3+Zp03f@5k1+Zp05f@12k1+Zp25f+Zp27f+Zp09f@48k1+Zp11f+Zp15f@30k1+Zp17f+Zp19f+Zp21f+Zp23f@46k1+Zp07@36k-1+Zp13+Zp23t`
   - Ultra Performance (`nf4up_k1h*`):
     `T2+P07S+P13S+K1+Zp03c3+Zp27c3+Zp05c3@9k-1+Zp25c3@18k1+Zp17c3@60k1+Zp15c3+Zp19c3+Zp03f@26k1+Zp05f@5k-1+Zp25f@19k-1+Zp27f+Zp07+Zp13+Zp19t+Zp23t+Zp09c3+Zp11c3+Zp21c3+Zp23c3+Zp09f+Zp11f+Zp15f+Zp21f+Zp17f@108k1+Zp19f@25k-1+Zp23f@54k1`
   - `nf4_exact` (graph `ht_t2s7p13`, `KIND=ht`, no K1):
     `T2+P07S+P13S+Zp03c3+Zp05c3@10k-1+Zp25c3@1k-1+Zp09c3+Zp11c3@24k1+Zp15c3+Zp17c3+Zp19c3+Zp21c3@14k1+Zp23c3+Zp03f@5k1+Zp05f@12k1+Zp25f+Zp09f@48k1+Zp11f+Zp15f@30k1+Zp17f+Zp19f+Zp21f+Zp23f@46k1+Zp07@36k-1+Zp13+Zp23t`
     (= the Performance string without `K1`, `Zp27c3@26k-1`, `Zp27f`). Rebuilt with it, the
     v73 and v79 files equal the shipped ones byte for byte.
   The builder writes `k1h_a.onnx` (or `ht_t2s7p13.onnx`) and a custom-I/O file `k1h_a.io.yaml`:
   ```yaml
   - IOName: x
     Datatype: uint8
     QuantParam:
       Type: QNN_DEFINITION_DEFINED
       Scale: 1.0
       Offset: -128
   - IOName: k1        # "out" for nf4_exact
     Datatype: uint8
     QuantParam:
       Type: QNN_DEFINITION_DEFINED
       Scale: 1.0
       Offset: -128
   ```
4. **No calibration and no encodings file.** Every quantisation parameter is fixed by the shader math and set in the
   ONNX graph itself. The converter runs without `--input_list`, and QNN's own quantiser is not used
   (`--float_fallback`, section 3.1).

### 1.3 The 17 graphs and their tensor shapes

Graph name inside every `nf4_k1h*` / `nf4up_k1h*` file: **`k1h_a`**. Inside `nf4_exact`: **`ht_t2s7p13`**. Every
tensor is `QNN_DATATYPE_UFIXED_POINT_8`, scale 1.0, offset -128, NHWC. These shapes are read from the context binaries
(`qnn-context-binary-utility`):

| file | output size | preset | input `x` | output |
|---|---|---|---|---|
| `nf4_k1h` | 1920x1080 | P | 1x540x480x32 | `k1` 1x540x480x32 |
| `nf4up_k1h` | 1920x1080 | UP | 1x540x480x32 | `k1` 1x540x480x32 |
| `nf4_k1h_452` / `nf4up_k1h_452` | 1600x904 | P / UP | 1x452x480x32 | `k1` 1x452x400x32 |
| `nf4_k1h_450` / `nf4up_k1h_450` | 1600x900 | P / UP | 1x450x480x32 | `k1` 1x450x400x32 |
| `nf4_k1h_405` / `nf4up_k1h_405` | 1440x810 | P / UP | 1x405x480x32 | `k1` 1x405x360x32 |
| `nf4_k1h_400` / `nf4up_k1h_400` | 1280x800 | P / UP | 1x400x480x32 | `k1` 1x400x320x32 |
| `nf4_k1h_360` / `nf4up_k1h_360` | 1280x720 | P / UP | 1x360x480x32 | `k1` 1x360x320x32 |
| `nf4_k1h_324` / `nf4up_k1h_324` | 1152x648 | P / UP | 1x324x480x32 | `k1` 1x324x288x32 |
| `nf4_k1h_270` / `nf4up_k1h_270` | 960x540 | P / UP | 1x270x480x32 | `k1` 1x270x240x32 |
| `nf4_exact` | 1920x1080 | P | 1x540x480x32 | `out` 1x540x480x32 |

Notes:
- The network runs at half the output size, 16 channels. Packed in pairs, that is `(W/2)/2 x 32`.
- `x` always has width 480 (32 channels). That is the 960-pixel row stride the Vela pre-pass writes. The graph slices out
  the columns it uses.
- The 1440x810 and 960x540 graphs compute on a grid of 408 and 272 rows. They zero the pad rows inside the graph.
- `k1` is the output of the K1 post-pass block, which the GPU post-pass reads. `out` (`nf4_exact` only) is the network
  output before K1.
- A 1366x768 graph (`_384w683`) was built but is **not** in package 14.

## 2. Tools

| tool | version | where it runs |
|---|---|---|
| Qualcomm AI Engine Direct SDK (QAIRT) | **2.47.0.260601**. Download from Qualcomm: `https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/2.47.0.260601/v2.47.0.260601.zip`. The file we used has sha256 `d3497e110eae82c35a9152a93c0a18bbede402aaf9faa7a97c8079eb0f522b01`. | mounted read-only into the container at `/qairt`; `QNN_SDK_ROOT=/qairt` |
| host | Windows 11 PC with Docker Desktop. All build steps run inside an x86_64 Linux container. | |
| container | `FROM ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956`; apt: `python3 python3-pip python3-venv python3-yaml unzip make clang-14 libc++-14-dev libc++abi-14-dev g++ file`; pip: `numpy==1.26.4 onnx==1.16.2 onnxruntime==1.18.1 protobuf==7.36.2 pandas==2.3.3 packaging==26.3 typing-extensions==4.16.0` (`npu/graphs/Dockerfile`, below) | |
| Python | 3.10.12 (Ubuntu 22.04.5) | |
| C++ compiler for the model library | `clang++-14`, linked as `clang++` on `PATH` (the SDK Makefile calls `clang++`) | |
| `onnxsim` | **not installed.** The converter logs "Onnx model simplification failed due to: No module named 'onnxsim'" and goes on. We have not tested whether a build with `onnxsim` installed gives the same files, so leave it out. | |

```dockerfile
# QAIRT host tools (x86_64 Linux): ONNX converter, model-library generator, context-binary generator.
# The SDK is not in the image: download QAIRT 2.47.0.260601 from Qualcomm; build.sh mounts it at /qairt.
#   docker build -t vela-qnn .
# Pinned: the Ubuntu 22.04.5 base image by digest and the pip packages; the Ubuntu packages (Python 3.10.12, clang 14.0.0,
# PyYAML 5.4.1) come from the 22.04 archive. Built with --no-cache on 2026-11-22, this image rebuilt nf4_exact and
# nf4up_k1h_452 for v73 and v79 with the sha256 of expected.sha256. Against the package-14 image only patch releases
# differ (glibc 2.35-0ubuntu3.18 vs 3.15, perl, ca-certificates, linux-libc-dev); the pip packages are the same.
FROM ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq >/dev/null && apt-get install -y -qq python3 python3-pip python3-venv python3-yaml unzip make clang-14 libc++-14-dev libc++abi-14-dev g++ file >/dev/null \
 && pip3 install -q "numpy==1.26.4" "onnx==1.16.2" "onnxruntime==1.18.1" "protobuf==7.36.2" "pandas==2.3.3" "packaging==26.3" "typing-extensions==4.16.0"
```

Environment inside the container (every step):

```sh
R=/qairt          # QAIRT_DIR, mounted by build.sh
mkdir -p /tmp/cb; ln -sf /usr/bin/clang++-14 /tmp/cb/clang++; ln -sf /usr/bin/clang-14 /tmp/cb/clang
export QNN_SDK_ROOT=$R PYTHONPATH=$R/lib/python LD_LIBRARY_PATH=$R/lib/x86_64-linux-clang \
       PATH=/tmp/cb:$R/bin/x86_64-linux-clang:$PATH
```

Resources we used: 4–5 CPUs and 5–8 GB RAM per container. One graph takes about 1.5 minutes.

## 3. Commands (one graph)

`m` = the graph name inside the file (`k1h_a`, or `ht_t2s7p13` for `nf4_exact`). `G` = the file name (`nf4_k1h_450` …).
**The graph name matters: the shipped files were built with these exact names. Another name gives a different file.**

### 3.1 ONNX → QNN C++ model

```sh
# step 0 (npu/graphs/build_graph.sh): the generator writes $m.onnx and $m.io.yaml into the build folder, e.g.
#   KIND=n9 VARIANTS="$m:$FLAGS:0" INIT_BIN=<fsr4vk>/assets/fsr411/general/1080/<performance|ultraperf>/initializers.bin \
#     python3 gen_nsz.py <build-dir>            (with NSZ_W / NSZ_HN / NSZ_HR / NSZ_WR for the size, section 3.4)
python3 $R/bin/x86_64-linux-clang/qnn-onnx-converter \
  -i $m.onnx --custom_io $m.io.yaml --float_fallback --float_bitwidth 16 -o $m.cpp
```

`--float_fallback` turns QNN's own quantisation off ("QNN Quantization is disabled as --float_fallback flag is
provided"). The int8 encodings come from the QDQ nodes in the ONNX graph. The converter writes `$m.cpp`, `$m.bin`
(weights) and `${m}_net.json`.

### 3.2 x86 model library

```sh
qnn-model-lib-generator -c $m.cpp -b $m.bin -l $m -t x86_64-linux-clang -o libs
# -> libs/x86_64-linux-clang/lib$m.so
```

The x86 library is the input for both chips. The v73 and v79 files of one graph are made from the same `lib$m.so`.

### 3.3 Context binary for HTP v73, v79 and v81

```sh
for a in "v73 43" "v79 69" "v81 87"; do set -- $a      # dsp_arch, soc_model (43 = SM8550, 69 = SM8750, 87 = SM8850)
  cat > htp_$1.json <<EOF
{"graphs": [{"graph_names": ["$m"], "vtcm_mb": 8, "O": 3}], "devices": [{"soc_model": $2, "dsp_arch": "$1"}]}
EOF
  cat > ctxcfg_$1.json <<EOF
{"backend_extensions": {"shared_library_path": "$R/lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so", "config_file_path": "$PWD/htp_$1.json"}}
EOF
  qnn-context-binary-generator --backend $R/lib/x86_64-linux-clang/libQnnHtp.so \
    --model libs/x86_64-linux-clang/lib$m.so --binary_file $G --output_dir ctx/$1 --config_file ctxcfg_$1.json
  qnn-context-binary-utility --context_binary ctx/$1/$G.bin --json_file ctx/$1/$G.json   # optional: shapes, names
done
# -> ctx/v73/$G.bin  (package path usr/lib64/vela-npu/$G.bin)
# -> ctx/v79/$G.bin  (package path usr/lib64/vela-npu/v79/$G.bin)
# -> ctx/v81/$G.bin  (package path usr/lib64/vela-npu/v81/$G.bin)
```

Backend settings used for every shipped graph:
- `vtcm_mb 8`, optimisation level `O 3`.
- Precision: the graph is int8/uint8 throughout. The converter's float fallback bit width is 16 (`--float_bitwidth 16`).
  The backend config sets no fp16 option. **[unknown: whether any op actually falls back to fp16. The converter prints
  "Following OPs fallback to float" with an empty list.]**
- Not used: no `dlbc` (weight compression) option, and no `--profiling_level` for the packaged files. Profiling builds
  went to separate folders.
- The compiler prints a "DDR bandwidth summary" (spill/fill/write/read bytes) in its log. Section 5 lists ours, so you can
  check that your build made the same plan.

### 3.4 The size parameters

`gen_nsz.py` takes the size from four variables. The 1080p and 720p graphs were built by the same chain with
`KIND=k1h` and `KIND=n7` (`npu/graphs/graphs.txt`).

| `G` suffix | `NSZ_W` (grid width, px/2) | `NSZ_HN` (grid rows) | `NSZ_HR` (real rows) | `NSZ_WR` (real width) |
|---|---|---|---|---|
| `_452` | 800 | 452 | 452 | 800 |
| `_450` | 800 | 452 | 450 | 800 |
| `_405` | 720 | 408 | 405 | 720 |
| `_400` | 640 | 400 | 400 | 640 |
| `_324` | 576 | 324 | 324 | 576 |
| `_270` | 480 | 272 | 270 | 480 |

## 4. Checking a build

### 4.1 Exactness on the x86 HTP simulator (before the device)

```sh
qnn-net-run --backend $R/lib/x86_64-linux-clang/libQnnHtp.so --retrieve_context ctx/v73/$G.bin \
  --input_list list.txt --use_native_input_files --use_native_output_files \
  --output_dir sim/$G/v73 --config_file ctxcfg_v73.json --log_level warn
```

We compare the output with a numpy model of the shader integer math (round half to even after `>> sh`, int8 clamp). The
inputs are 2 real frames per weight set, plus noise and flipped frames for the 1080p/900p/720p graphs. The bar is
**0 bytes different**. Every shipped graph met it on v73 and on v79. The 900p graphs (P and UP), run on real 900p frames,
also match the GPU output of the real FSR 4.1.1 shaders at 0 bytes. The input frames and the numpy reference are not
published (section 9).

### 4.2 Same file as ours

Rebuilding from the same model library gives the shipped file byte for byte. The v73 rebuilds of all 7
1080p/900p/720p/exact graphs matched, and rebuilding the 900p graphs through the size generator gave the shipped
`_450` files on both chips. A second full rebuild of every graph gave the same md5s. So if your md5 differs, an input or
a tool version is different.

## 5. Expected files (package 14)

Hashes of the files inside `vela-npu-sysext-14.raw` (image sha256
`10acba9196a7a55f712d14451ed64a20385cc5eab045744be6a63dd264392ac1`).
The graph files are the same in package 15 (`vela-npu-sysext-15.raw`, sha256
`5cdfc9c15180eb5c14ee4810f89d221a8d390d9459560c08346af6358c91ae73`). Package 17 (`vela-npu-sysext-17.raw`) ships new files for the same
graphs (a faster, bit-exact build form) and a second set `low4/`: see [`npu/graphs/pkg17/README.md`](../npu/graphs/pkg17/README.md). The hashes in this section are those of package 14/15. The image's own
`usr/lib64/vela-npu/MANIFEST.md5` has the same md5s.

### 5.1 HTP v73 (`usr/lib64/vela-npu/`)

| file | bytes | md5 | sha256 |
|---|---|---|---|
| nf4_exact.bin | 1622016 | 2b8e1499edade62543532b03602c27d3 | 01c62fda83a9f40001cd1cf6bc5a4b4cf8fe3a8aa5d736e21097fe2936a4bc57 |
| nf4_k1h.bin | 1757184 | a2c73bc9723bc99780ebd1ed4e052765 | 0cbe84ff752906863da34385ff026045add23c4cd55e4560a39857ca3980d953 |
| nf4_k1h_270.bin | 1130496 | 3b4c1703d7f462f8f73b28490c80e4fa | f8ee22d3394d60b77c1c8ad7bc6a7940c62f034fbc3dff3c093a503a2da82262 |
| nf4_k1h_324.bin | 1323008 | 8685a9b656398e8b3673653da83b3b9a | 88dcb05299619008cd915c1ef5ccd0ce4799ea35cad5535ca53555e8659c1ec8 |
| nf4_k1h_360.bin | 1253376 | be867d7d4ede06c4d5b83cd69b863102 | 3fea93d96d52627187700f90d83f5cfdfe68479e79b9e5fcf0c37716575441a7 |
| nf4_k1h_400.bin | 1282048 | f5a38f36957354f48fcedb6c8adda3f8 | 96ff1d19f8bbaf199c31751dc46e70715218e3f4de0ac84a20aeaef0f77d5fed |
| nf4_k1h_405.bin | 1409024 | 11742856ea4cf733bfd93349553af398 | ce350513503962d882c377efe851c16cc1dc88c565990360b9a7fa8d2dad23d4 |
| nf4_k1h_450.bin | 1556480 | d25d78529d2dc9bb93d019098233ce60 | f6d5178116dc7754d532ca76985bdad947f44628a06f58b1d3787dcf7b8584d1 |
| nf4_k1h_452.bin | 1527808 | 07aed14c54a538ff67f65ba2ed7b32c0 | 9621706eeab4b77523930adaf564f0bdfd8df3ec4c6742057944887e263b1ffe |
| nf4up_k1h.bin | 1789952 | b01ea30ab8ba300b0d5bc9ea89ab7098 | a9de543c0bc1f8a61b01f45cd457d7009ef147d1ca080600c57d6f91f1e98c27 |
| nf4up_k1h_270.bin | 1142784 | aba14a85257280b487f765323b37e6f1 | 61cb2d42a432d8c0fb68531d6a58238e50210974b9bef8f28828ae931bee1bd9 |
| nf4up_k1h_324.bin | 1339392 | 27e64c24c1961c010e7dbc5191b221d6 | 57f5f5e4ee72cd17f0e7dbcae466e4570e86be894764a29cc980f635d99fb6cb |
| nf4up_k1h_360.bin | 1241088 | 800b917c548e5349fb67fe8ad0b52cb8 | 7738d82b67fae8e6e89c56fdda83b729ce728f57d4223a4ede3133b7e480dd26 |
| nf4up_k1h_400.bin | 1314816 | 307727e8aaa1f76856e3b70ecb1642f1 | b779996323f2396b51c2bbc591e8180ea34b91bad34e5679a58059da229b5d2b |
| nf4up_k1h_405.bin | 1404928 | 7648bca41d820ca49a4a60af99d9c9cc | 3923a77274705492a46a2393439ea0c0d907780b460b038e119f1aa306315a21 |
| nf4up_k1h_450.bin | 1572864 | 39f3861c4c6e50ffbc55bb430fb1064c | 545798b705828b67490da4b9798a340d9085d7ebafae12700e7777b23d3cb580 |
| nf4up_k1h_452.bin | 1540096 | e0d3f26d1040a86bb1c02827c85d480b | 6d225002b42308f428b7d6ca06cf3db3177b4928fa4e77b9cebc1d9006d95efd |

### 5.2 HTP v79 (`usr/lib64/vela-npu/v79/`)

| file | bytes | md5 | sha256 |
|---|---|---|---|
| nf4_exact.bin | 1646592 | fa426ff679429d6f8f3b37026ff0dcd6 | 5429be58ae6abe3b5c53dd33ebcf7f9785f72cc166d625d373bdd3806aa1fac4 |
| nf4_k1h.bin | 1773568 | 4d013c9382fcc976143a50bd9c34c17b | 24661f480dffe373376b9e3b75ca068e783e799e0678539764f7554409ed734c |
| nf4_k1h_270.bin | 1134592 | 7344ae7c6bc3090b34dbbe62129f9282 | 3954614f35f7fec9128c2ab7d73500ed82502a6d2166f16b3b0df0f93649dda9 |
| nf4_k1h_324.bin | 1343488 | e8a162307da4a71a67249558cacfcf7b | a08da0fee8dbf3745f002a1bca87fdf4ec224fc70ed24497ec069bf36b4aca2b |
| nf4_k1h_360.bin | 1335296 | 174216b30a60e0c6c19ba85c4a2ee75a | 25752e7f17022d42c612ee4de6b5d458dcdf2f92d34bae8105c6fa15532efcdb |
| nf4_k1h_400.bin | 1388544 | 6fde07b09a2d7653405f832335dc61e0 | 1d428fddfc25522fdc97f3e5a65c1db28997e480ab337270cf46fabf511da774 |
| nf4_k1h_405.bin | 1404928 | 5ec8b81fff97d820c7008c5b732d35d1 | 383c1f4748111e3a251ae73f2149cc16ad000d7654d90e34f02c0b20ac511d3f |
| nf4_k1h_450.bin | 1548288 | e172a6ef518545381b6af87319bab25a | 3dd563721fb5aa10b8a96984958f68416140d05cb720010fcf4c50bdb48e54c4 |
| nf4_k1h_452.bin | 1552384 | 2fa209c6822d4bbeca1f1a9ec39bda87 | 263c72c54993c2f0f65fd20a76d671db7f9b298e650ba60fd9a29f0faad501fb |
| nf4up_k1h.bin | 1765376 | 962865cb0589631690ecb72f8d1c0843 | ace49f5c226cb1d1deadd5ac178224c91931c535638f584256cd6b31c717395c |
| nf4up_k1h_270.bin | 1146880 | 7479a317b6ac148f00350329c39b3915 | ba53e052e43d62c29bf7e4fab3aba1715b099f9c34680abdc144f87b102da9af |
| nf4up_k1h_324.bin | 1343488 | 215b15b467ef48755705feb73556464c | 1ebbaef032d237ed95ad4326df91e87f858ccd83949029fbdeca09af83793147 |
| nf4up_k1h_360.bin | 1343488 | 0b4d6dad062eb125d5bd1fba4d736d29 | 3ca66d1f3261480d8c76eabc7f35307cd3fb7751d97ab0a968fff94eb119dda7 |
| nf4up_k1h_400.bin | 1404928 | 6a5a0727991f85411ec9cb73db015a28 | 1c9134927e7ca7c68c336ae4b75d2cd5ed2cbecc88910f054861890d19376cc3 |
| nf4up_k1h_405.bin | 1449984 | abdaf8dc60182d3441b6e5ad240a7226 | fddefb1142dd44fb40c49370a5c76e55ea2c5eaf9b00c16e98e28e6f7bf5b387 |
| nf4up_k1h_450.bin | 1589248 | b35a945e7698a4e5a03c6005b6c0b108 | 3f1fb35d0c6cac6b5a522d1bf1c664dc61891f5a4c0b70a86faedcb5f8591bba |
| nf4up_k1h_452.bin | 1564672 | fca3f48796e54a13164975c5ebf4d91e | e66ecff0b0c9f3b8c608a2d18e9b09580e7c19a08df37d7862d33f4593c7a46b |

### 5.3 Compiler plan per graph (v73; DDR MB spill / fill / write / read)

| graph | P | UP |
|---|---|---|
| 1080 (`nf4_k1h`, `nf4up_k1h`) | 29.50 / 29.62 / 37.85 / 49.36 | 29.50 / 29.62 / 37.85 / 49.36 |
| `_452` | 14.95 / 15.05 / 20.79 / 21.75 | 14.95 / 15.05 / 20.79 / 21.74 |
| `_450` | 14.95 / 15.05 / 20.79 / 21.77 | same |
| `_405` | 7.28 / 7.28 / 11.98 / 12.92 | same |
| `_400` | 0 / 0 / 4.10 / 4.95 | same |
| `_360` | 0 / 0 / 3.69 / 4.54 | same |
| `_324` | 23.37 / 23.37 / 26.39 / 27.34 | 23.37 / 23.37 / 26.39 / 27.35 |
| `_270` | 3.19 / 3.19 / 5.28 / 6.24 | 3.19 / 3.19 / 5.28 / 6.25 |
| `nf4_exact` | 29.50 / 29.62 / 37.85 / 49.32 | – |

On v79 the 1080p graphs plan less traffic (spill/fill about 21.1–21.2 MB). The other sizes plan the same as on v73.

## 6. The device check (installer step 6)

`install-vela-npu.sh` (package 14 and 15) starts the daemon and then, for each of the 17 graphs, runs
`/usr/lib64/vela-npu/fsr4npu-check --name fsr4npud_<graph> --runs 20` as the user.

- **Fixed input** (`fsr4npu_check.c`): byte `i` of `x` = `(i*7 + (i >> 12)) % 59`. Outputs are filled with
  `0xA5 ^ run` before each run, so a run that writes nothing cannot pass. The checker prints `input hash` and
  `output hash` (FNV-1a 64 of the bytes) and whether all 20 runs gave the same output.
- **Pass on SM8550:** the daemon is active, the checker prints `RESULT OK`, and the output hash equals:

| graph | output hash | | graph | output hash |
|---|---|---|---|---|
| nf4up_k1h | 60a3d9ad466fca76 | | nf4_k1h | 8a735e657feee21f |
| nf4up_k1h_450 | 90a028a11df7fc3e | | nf4_k1h_450 | 2b9d68691c111dbe |
| nf4up_k1h_360 | f27ed0e09ef0d7b8 | | nf4_k1h_360 | 2606832826ab4c91 |
| nf4up_k1h_452 | 040ded774ade640a | | nf4_k1h_452 | 2742c79e72e89a76 |
| nf4up_k1h_405 | 1eb47d3c70a78111 | | nf4_k1h_405 | 80e8e720a693b5d5 |
| nf4up_k1h_400 | 98d9a4339a8d0d76 | | nf4_k1h_400 | b6a428542d1f38a3 |
| nf4up_k1h_324 | 0cfda9f2b7cabc25 | | nf4_k1h_324 | 8b967666e675804b |
| nf4up_k1h_270 | a71e9b4f04112039 | | nf4_k1h_270 | 501dbf44d6b0410d |
| nf4_exact | a83aff6f6a4633e8 | | | |

- The x86 HTP simulator gives the same hashes on the same input, for the v73 and for the v79 files. You can therefore
  check a new build on a PC: build `x` with the formula above at the graph's input size, run `qnn-net-run` (section 4.1),
  and FNV-1a-64 the output. **[unknown: we have not written this PC-side hash script as a standalone tool yet]**
- **Pass on SM8750:** on a real HTP v79 (measured on an Odin 3) the outputs are not bit-exact from run to run. They
  are within about 40 dB of the simulator output. So the installer accepts a v79 graph that is active, reports
  `backend qnn (NPU)` and prints no `FAIL` line, without comparing the hash.

## 7. Other chips

- **HTP v79 (Snapdragon 8 Elite, SM8750):** same model library as v73, with `soc_model 69`, `dsp_arch v79` in
  `htp.json` (section 3.3). Nothing else changes. On the device, `vela-npu-setup.service` checks
  `/proc/device-tree/compatible` for `qcom,sm8750` and links `/run/vela-npu/graphs` to `/usr/lib64/vela-npu/v79`. A
  drop-in then mounts each v79 file over its v73 path in the daemon's mount namespace. Status: experimental. Proven on
  the simulator, run on an Odin 3, not bit-exact on the device.
- **HTP v75 (Snapdragon 8 Gen 3, SM8650):** the v73 graphs run unchanged with the QAIRT 2.47.0.260601 HTP v75 runtime
  (`libQnnHtpV75Stub.so`, `libQnnHtpV75Skel.so`, `libQairtHtpV75Skel.so`, `libQnnHtpV75.so`). On a Lenovo Legion Tab
  Gen 3 (TB321FU) all 17 graphs give the SM8550 output hashes of section 6, so the installer treats SM8650 as SM8550. A
  native v75 build (`soc_model 57`, `dsp_arch v75` in `htp.json`) of `nf4_k1h`, `nf4_k1h_400` and `nf4_k1h_324` gets the
  same compiler plan as v73 (same spill/fill bytes), the same output hashes and the same NPU time on the device
  (`fsr4npu-check` NPU execute p50, v73 / v75: 4.64 / 4.65 ms, 2.25 / 2.24 ms, 2.42 / 2.37 ms), so no v75 graph set is shipped.
- **HTP v81 (Snapdragon 8 Elite Gen 5, SM8850 / Kaanapali / Canoe):** same model library as v73/v79, with the QAIRT 2.47.0.260601
  HTP v81 runtime (`libQnnHtpV81Stub.so`, `libQnnHtpV81Skel.so`, `libQairtHtpV81Skel.so`, `libQnnHtpV81.so`). On a real SM8850P
  device (Lenovo Legion Y700 Gen 5 running HyperOS 4, `CANOEP`, `soc_id 661`, Android 17 / QAIRT 2.47.0.260601 unsigned PD,
  `turbo` DCVS+HMX, 20 runs each with `fsr4npu_check` fixed input & FNV-1a-64 check formula):

  | graph | build | binary bytes | spillFillBufferSize | rc | runs bit-exact | check hash | min / p50 / max ms |
  |---|---|---|---|---|---|---|---|
  | `nf4_k1h` | `v73` E-BS (`socModel 43`, `dspArch 73`) | 1,564,672 | 16,842,752 (16.84 MB) | 0x0 | 20 / 20 | `8a735e657feee21f` (ref match) | 3.21 / 3.23 / 4.44 |
  | `nf4_k1h` | `v79` E-BS (`socModel 69`, `dspArch 79`) | 1,564,672 | 14,548,992 (14.55 MB) | 0x0 | 20 / 20 | `8a735e657feee21f` (ref match) | 2.91 / 2.93 / 3.64 |
  | `nf4_k1h` | `v81` E-BS (`socModel 87`, `dspArch 81`) | 1,822,720 | 13,762,560 (13.76 MB) | 0x0 | 20 / 20 | `8a735e657feee21f` (ref match) | 2.76 / 2.77 / 3.56 (-14.2%) |
  | `nf4up_k1h` | `v73` E-BS | 1,593,344 | — | 0x0 | 20 / 20 | `60a3d9ad466fca76` (ref match) | 3.04 / 3.05 / 3.77 |
  | `nf4up_k1h` | `v79` E-BS | 1,581,056 | — | 0x0 | 20 / 20 | `60a3d9ad466fca76` (ref match) | 2.82 / 2.84 / 3.53 |
  | `nf4up_k1h` | `v81` E-BS | 1,839,104 | — | 0x0 | 20 / 20 | `60a3d9ad466fca76` (ref match) | 2.76 / 2.77 / 3.50 (-9.2%) |

  Because package 18's shipped `v73` graphs are E-BS (whereas shipped `v79/` are non-E-BS), SM8850 defaults to the existing
  `v73` graphs on the HTP v81 runtime (zero image size increase, preserving `low4/` start self-check fallback). When `v81/`
  graphs (`soc_model 87`, `dsp_arch v81` in `htp.json`) are provided via `GRAPHS_V81` in `mk_public_sysext.sh`,
  `vela-npu-setup.service` detects `qcom,kaanapali` / `qcom,sm8850` / `qcom,canoe` / `qcom,canoep` and links
  `/run/vela-npu/graphs` to `/usr/lib64/vela-npu/v81` for the ~9–14% NPU latency reduction. **[unknown: full 17-graph
  Linux Docker build and armadaOS sysext run not yet tested]**
- **Other chips:** our notes cover v73 (SM8550), v75 (SM8650), v79 (SM8750) and v81 (SM8850). For another Hexagon generation you would change
  `soc_model`/`dsp_arch`, check the result on the simulator, and add that chip's runtime libraries. **[unknown: never
  built or tested by us]**

## 8. Runtime (what loads the graphs)

### 8.1 The daemon
`fsr4npud` (source in `npu/src/`) is started as
`fsr4npud --graphs nf4_k1h,…,nf4up_k1h_270 --graph-dir /usr/lib64/vela-npu`, one socket per graph
(`fsr4npud_<graph>`). For each graph it:
1. reads `<graph-dir>/<graph>.bin`;
2. reads the graph name and the tensors with `QnnSystemContext_getBinaryInfo` (inputs[0] = `x`; outputs = `k1` or
   `out`; with two outputs, the one named `p3u_o` goes first);
3. creates the context with `QnnContext_createFromBinary` on the HTP backend (`libQnnHtp.so`) when the first client
   connects, keeps up to `--max-loaded` contexts, and unloads a graph after `--graph-idle-ms` with no client;
4. registers the client's dma-bufs with `QnnMem_register` and runs `QnnGraph_execute`.

All graphs share one cDSP session in one process.

### 8.2 Files in the image (from Qualcomm QAIRT 2.47.0.260601, unmodified; V75 since package 15, V81 added for SM8850)

| path in image | from SDK | sha256 |
|---|---|---|
| usr/lib64/vela-npu/libQnnHtp.so | lib/aarch64-oe-linux-gcc11.2/libQnnHtp.so | 9e1a73ed4f3e7cf3ef3199a23dfb3885b12a64edea30e607b0687b25b28e94f7 |
| usr/lib64/vela-npu/libQnnSystem.so | lib/aarch64-oe-linux-gcc11.2/libQnnSystem.so | 1dcc4e535f94b7d921d5c294eafc06bc0c87968077e5ee36bb14538820ef6990 |
| usr/lib64/vela-npu/libQnnHtpV73Stub.so | lib/aarch64-oe-linux-gcc11.2/libQnnHtpV73Stub.so | 71253e59ef34bc1b41bd7a54c0a1bd780e65aa47f4e2d542305106eb02af6e32 |
| usr/lib64/vela-npu/libQnnHtpV79Stub.so | lib/aarch64-oe-linux-gcc11.2/libQnnHtpV79Stub.so | e8b7712f1df3e05e08b61f1d9ebde5b966cca0f598c55fd96c85d6bf37c41dcc |
| usr/lib64/vela-npu/libQnnHtpV75Stub.so | lib/aarch64-oe-linux-gcc11.2/libQnnHtpV75Stub.so | 6b83dc2f78f809f469aee92d4d12473dd69db6095602f37061906e9081cc915f |
| usr/lib/rfsa/adsp/libQnnHtpV73Skel.so | lib/hexagon-v73/unsigned/libQnnHtpV73Skel.so | b5084469a693b05e372eedf8861e29e92c6e012c1cf543144c894f6141a5348f |
| usr/lib/rfsa/adsp/libQairtHtpV73Skel.so | lib/hexagon-v73/unsigned/libQairtHtpV73Skel.so | 472a7e75763a83dca3463b14f543f54bb4cd5b392a615343375ea1738159cafc |
| usr/lib/rfsa/adsp/libQnnHtpV73.so | lib/hexagon-v73/unsigned/libQnnHtpV73.so | 9be81519a066a7beee544d1c1a5fe7947df04b53e087fd8e55160b846db0c614 |
| usr/lib/rfsa/adsp/libQnnHtpV79Skel.so | lib/hexagon-v79/unsigned/libQnnHtpV79Skel.so | dd8987c17928e53877e51ce5739c550d3e74393bd5b1b5ccf311326eddb08612 |
| usr/lib/rfsa/adsp/libQairtHtpV79Skel.so | lib/hexagon-v79/unsigned/libQairtHtpV79Skel.so | bea936b4ab0d5d8a3d172a634cbeaba38ab6b59c103c23b4580191f95717707c |
| usr/lib/rfsa/adsp/libQnnHtpV79.so | lib/hexagon-v79/unsigned/libQnnHtpV79.so | 998a3f868c906db88969b11a328ef9adefa4269017f5216b33780574f06d2e0c |
| usr/lib/rfsa/adsp/libQnnHtpV75Skel.so | lib/hexagon-v75/unsigned/libQnnHtpV75Skel.so | 607be3d7ec64df053f019438ad6eea59dd3eea5d5f985001709d52d5663479ba |
| usr/lib/rfsa/adsp/libQairtHtpV75Skel.so | lib/hexagon-v75/unsigned/libQairtHtpV75Skel.so | 1229177eb5bc53220ddb5c77bb25e4025209e4c8ea0a46712aedd92e4f4e705e |
| usr/lib/rfsa/adsp/libQnnHtpV75.so | lib/hexagon-v75/unsigned/libQnnHtpV75.so | 14efadf5fdbac6116767d797394cb60454d252748895b284e24d2540b0560449 |
| usr/lib64/vela-npu/libQnnHtpV81Stub.so | lib/aarch64-oe-linux-gcc11.2/libQnnHtpV81Stub.so | 8e8f8301ec2afaac62b9c607ac35cd92a62a82abf0c23aa3ccd519419ec60d7a |
| usr/lib/rfsa/adsp/libQnnHtpV81Skel.so | lib/hexagon-v81/unsigned/libQnnHtpV81Skel.so | 0b4fa7419e7265ae33d5e57f214d49cd11abbf7302149bb7ff78d9b602903899 |
| usr/lib/rfsa/adsp/libQairtHtpV81Skel.so | lib/hexagon-v81/unsigned/libQairtHtpV81Skel.so | 279a84ce80f1b9c3680f7f027b173978307ab2a54dc5f225735491829bd28a60 |
| usr/lib/rfsa/adsp/libQnnHtpV81.so | lib/hexagon-v81/unsigned/libQnnHtpV81.so | 0262f2b7309fdf358dcb2b823efe549c0cb68db1b88579011a12f5d7c4b779b9 |

The image also carries quic/fastrpc built from source (commit `a56e9d4`): `cdsprpcd`, `libcdsprpc.so.1.0.0` and
`libcdsp_default_listener.so.1.0.0`. The service uses `LD_LIBRARY_PATH=/usr/lib64/vela-npu` and
`ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp`.

### 8.3 DSP shell, taken from the device
The image does **not** carry the DSP-side shell. Installer step 2 copies 3 files read-only with
`debugfs -R "dump /cdsp/<name> …"` from the device's own `dsp<slot>` partition
(`/dev/disk/by-partlabel/dsp_a` or `_b`, slot from `androidboot.slot_suffix`). They go into a second system extension,
`armada-vela-npu-qnn`, under `usr/lib/rfsa/adsp/`. Hashes on the tested Retroid Pocket 6 (SM8550):

| file | sha256 |
|---|---|
| fastrpc_shell_unsigned_3 | cb2a0a8ed448bd6e0fea0a81bd0c3892b93373f3eff5c43ca577c7a0628d7251 |
| libc++.so.1 | 6050f67f1416a69a81d3b7f34fa4ccdca139d8c752a24698a7fd679bf418b90f |
| libc++abi.so.1 | 934f8039214f15b1a692a2278909a9468f1b5561affafbf60efb96e158583563 |

Another hash only prints a warning: an SM8750 device has its own copies, and those are used.

## 9. What is not published

- **The simulator input frames and the numpy reference** of section 4.1 are lost; the fidelity check needs new GPU dumps.
  The byte-for-byte check (section 5) and the fixed-input check (section 6) do not need them.
- **The ONNX files and model libraries** are not published; `build.sh` writes them into `out/work/<file>/`.
- **The Ubuntu packages of the build images are not pinned** (only the base images by digest and the pip packages):
  the Ubuntu archive drops old patch releases. An image built from `npu/graphs/Dockerfile` with `--no-cache` on
  2026-11-22 had newer glibc, perl, ca-certificates and linux-libc-dev patch releases than ours and rebuilt
  `nf4_exact` and `nf4up_k1h_452` for v73 and v79 with the sha256 of section 5. The other 15 graphs were not rebuilt
  with that image. **[unknown: whether a later Ubuntu update changes a file]**
- Everything else is reproducible from public inputs: the parameter files from fsr4vk (section "Regenerating the
  parameter files"), the graphs from those files and the QAIRT SDK (section "Rebuild").
