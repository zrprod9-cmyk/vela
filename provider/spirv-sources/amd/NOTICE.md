# AMD material in these sources

The GLSL, SPIR-V assembly and converted modules in `../fast`, `../lite` and the kernels the scripts here build derive from compute shaders and network
weights of AMD software:

- FSR 4.1.1 INT8: AMD FidelityFX SDK 2.3.0, `amd_fidelityfx_upscaler_dx12.dll`, translated to SPIR-V by vkd3d-proton (the bundles in `../../vela_assets/fsr411`).
- FSR 4.1.2 Lite: AMD FidelityFX SDK release v2.4.0, `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll`
  (SHA-256 4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7), fetched by `fetch.sh`; it is not stored in this repository.

AMD licenses these under the MIT license; the notice and the permission text are in `../../../plugin/assets/licenses/fsr4vk/AMD-FidelityFX-SDK-MIT.md`
and must stay with every copy of the derived files. Changes made here are GPL-3.0.
