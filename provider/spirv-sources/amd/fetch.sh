#!/bin/sh
# Download AMD's signed FSR 4.1.2 Lite provider from the FidelityFX SDK release v2.4.0 and check it.
# usage: sh fetch.sh <output dir>        (needs curl and sha256sum)
set -e
OUT=${1:?output dir}
URL=https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll
SHA256=4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
mkdir -p "$OUT"
[ -f "$OUT/amd_fidelityfx_upscaler_vk.dll" ] || curl -fL -o "$OUT/amd_fidelityfx_upscaler_vk.dll" "$URL"
echo "$SHA256  $OUT/amd_fidelityfx_upscaler_vk.dll" | sha256sum -c -
