# FSR 4.0.2 ML Frame Generation (`mlfi_4k`) NPU Graphs

Clean-room builder scripts for compiling **AMD FSR 4.0.2 ML Frame Generation** (`mlfi_4k`, 1080p) into Qualcomm QNN Hexagon HTP context binaries (`mlfi_s1.bin` and `mlfi_s3.bin`). No proprietary AMD binaries, weights, or Qualcomm SDK files are committed here; users supply their own copy of `amd_fidelityfx_framegeneration_vk.dll` and QAIRT SDK.

## 1. Input Provenance

| input | source | sha256 |
|---|---|---|
| `amd_fidelityfx_framegeneration_vk.dll` | AMD FidelityFX SDK v2.4.0 | `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4` |
| Qualcomm AI Engine Direct SDK (QAIRT) | `v2.47.0.260601` (`QNN_SDK_ROOT`) | `d3497e110eae82c35a9152a93c0a18bbede402aaf9faa7a97c8079eb0f522b01` (zip) |

`extract_mlfi_onnx.py` verifies the DLL SHA-256 by default and unpacks the `mlfi_4k` weights (`BlockedTensor6h_NbCbHWNC`, `BlockedTensor6h_CbNbHWCN`, `BlockedTensor6f8_NbCbHWNC`, `BlockedTensor6f8_CbNbHWCN`, `Tensor4f8_HWCN`, and `Tensor1f` biases) directly into:

- `mlfi_stage1_1080p.onnx` (`1,6,144,240` → `1,4,144,240`, sha256 `3db03cd766c542f3f0281ec37b05ea2509eff6e70ec6b033f7ffb1ac1f12062d`)
- `mlfi_stage3_1080p.onnx` (`1,24,272,480` → `1,5,272,480`, sha256 `cc4bc5cd200f9e3b644feb560fb6a876ec52c0542ba9e86dbc1485f4c165e7c1`)

## 2. QNN Graph & Tensor Contracts

Both QNN graphs use **NHWC `QNN_DATATYPE_FLOAT_16`** inputs and outputs:

| context file | graph name (`w8a16`) | input tensor (NHWC fp16) | output tensor (NHWC fp16) | notes |
|---|---|---|---|---|
| `mlfi_s1.bin` | `mlfi_s1_w8a16` | `rgb01` `[1, 144, 240, 6]` | `flow_px` `[1, 144, 240, 4]` | 1/8-res optical flow in 1/8-res px; active rows `0..134`, rows `135..143` zero-padded |
| `mlfi_s3.bin` | `mlfi_s3_w8a16` | `feat24` `[1, 272, 480, 24]` | `weights5` `[1, 272, 480, 5]` | 1/4-res 5-way Softmax blend weights; active rows `0..269`, rows `270..271` zero-padded |

- **`mlfi_s1.bin` (`rgb01` → `flow_px`)**: ch `0..2` = frame 0 linear/SDR RGB (`8×8` box downsampled from 1080p); ch `3..5` = frame 1 RGB. Output `flow_px`: ch `0..1` = frame 0 offset in 1/8-res px (`uv + flow / (240, 135)`), ch `2..3` = frame 1 offset in 1/8-res px.
- **`mlfi_s3.bin` (`feat24` → `weights5`)**: ch `0..2` = GameMV warp 0, ch `3..5` = GameMV warp 1, ch `6..8` = ML warp 0, ch `9..11` = ML warp 1, ch `12` = `val0`, ch `13` = `val1`, ch `14..16` = frame 0 RGB, ch `17..19` = frame 1 RGB, ch `20..23` = luma difference features (`|Y(rgb0)-Y(rgb1)|`, `|Y(gw0)-Y(gw1)|`, `|Y(gw0)-Y(ml0)|`, `|Y(ml0)-Y(ml1)|`). Output `weights5`: Softmax weights for `[cand0, cand1, ml0, ml1, 0.5*(i0+i1)]`.

## 3. HTP v81 Simulator Accuracy (`dx=+32, dy=-16`, `vtcm_mb=8`, `O=3`)

Measured with `qnn-net-run` on the HTP v81 simulator (synthetic natural test scene at `1920×1080`):

| mode | weights / acts | S1 bytes (spill / fill MB) | S3 bytes (spill / fill MB) | ML warp 0/1 PSNR | S3 ML-only PSNR | S3 GameMV+ML PSNR |
|---|---|---|---|---|---|---|
| `fp16` | FP16 / FP16 | 11,620,352 (18.59 / 26.47) | 1,048,576 (0 / 0) | 55.96 / 51.63 dB | 54.17 dB | 64.02 dB |
| **`w8a16` (default)** | **INT8 / INT16** | **6,967,296 (5.63 / 5.63)** | **925,696 (0 / 0)** | **56.64 / 53.86 dB** | **56.17 dB** | **87.18 dB** |
| `w8a8` | INT8 / INT8 | 6,144,000 (0 / 0) | 708,608 (0 / 0) | 36.92 / 36.64 dB | 41.58 dB | 58.58 dB |

`w8a16` is the default because 8-bit activations degrade Stage 1 sub-pixel flow regression by ~20 dB, whereas INT8 per-channel weights + 16-bit activations match/exceed FP16 while cutting S1 spill/fill by ~4×.

### Verified Windows QAIRT 2.47 `w8a16` HTP v81 (`soc_model=87`) Hashes

| file | bytes | md5 | sha256 |
|---|---|---|---|
| `v81/mlfi_s1.bin` | 6,967,296 | `6759d3d3bb9164b13ed004a860abbe3d` | `7c58401c87e2ca501f3965fed0074ed403085955487f5c1d731c6721e040d561` |
| `v81/mlfi_s3.bin` | 925,696 | `b24698401bc38ec0cb54ba5ba4047af2` | `7e4fa607d2c0c08af3939d76f4fd56ae14845f8a30bf9d65d90268a3b5de501c` |

## 4. Rebuild Commands

### Docker (Linux / Windows Git Bash, reuses `vela-qnn` image from `../Dockerfile`)

```sh
docker build -t vela-qnn npu/graphs
QAIRT_DIR=$PWD/qairt-sdk/qairt/2.47.0.260601 \
FFX_FG_DLL=$PWD/amd_fidelityfx_framegeneration_vk.dll \
OUT_DIR=$PWD/out \
  npu/graphs/mlfi/build.sh all
```

Optional environment variables: `MODE=w8a16|fp16|w8a8`, `VTCM_MB=8`, `ARCHS="v73:43 v79:69 v81:87"`, `CPUS=4`, `MEM=8g`.

### Native Windows (Python 3.10 + QAIRT 2.47 Windows SDK + MinGW `g++`)

```powershell
python npu/graphs/mlfi/compile_mlfi_qnn.py `
  --qairt C:\path\to\qairt\2.47.0.260601 `
  --dll C:\path\to\amd_fidelityfx_framegeneration_vk.dll `
  --out C:\path\to\out `
  --mode w8a16 --archs v73,v79,v81 --stages s1,s3 --eval
```

## 5. Verification Status

- **Verified (Simulator & Build):** HTP v81 (`soc_model=87`) simulator end-to-end accuracy (`fp16`, `w8a16`, `w8a8`), native Windows QAIRT 2.47 `w8a16` compilation, and ONNX extraction (`mlfi_stage1_1080p.onnx`, `mlfi_stage3_1080p.onnx`) against FidelityFX SDK v2.4.0 `amd_fidelityfx_framegeneration_vk.dll`.
- **Verified (Real SM8850 Hardware):** Live execution of `v81/mlfi_s1.bin` + `v81/mlfi_s3.bin` (`w8a16`) on Qualcomm SM8850 Hexagon HTP v81 (`soc_model=87`, Android 17, Adreno 840 Turnip v27.0.0-R3) concurrently with FSR 4.1.1 NPU Super Resolution (`v81/nf4_k1h.bin`, `960×540 → 1920×1080`, `3.3–3.4 ms` HTP):
  - Stage 1 (`mlfi_s1_w8a16`): `~4.2–4.6 ms` HTP mean (`300`-frame rolling average)
  - Stage 3 (`mlfi_s3_w8a16`): `~1.96–2.0 ms` HTP mean
  - Full FG pipeline wall time (S1 + GPU GameMV/depth splat & dilation + S3 + full-res blend, `gamemv=1, ui=1, cut=1`): `~10.1–10.9 ms`
  - Sustained `30.0 → 60.0 fps` presentation (`1920×1080`) with zero HTP/Vulkan errors.
- **Not yet tested:** Linux Docker builds and simulator/hardware runs for `v73` (`soc_model=43`) and `v79` (`soc_model=69`).
