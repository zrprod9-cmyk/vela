AMD FSR 4 model and shader material in amd_fidelityfx_upscaler_vk.dll

This provider embeds compute shaders and network weights derived from AMD software:
- FSR 4.1.1 INT8: translated (vkd3d-proton SPIR-V) from amd_fidelityfx_upscaler_dx12.dll,
  AMD FidelityFX SDK 2.3.0, Kits/FidelityFX/signedbin/ (commit 60f4ea81909200d8542eca14dccb2628b763a9a3),
  and modified by the Vela and fsr4vk projects.
- FSR 4.1.2 Lite: captured from amd_fidelityfx_upscaler_vk.dll, AMD FSR SDK 2.4.0,
  Kits/FidelityFX/signedbin/ (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7), and modified by the Vela project.
This build contains no FSR 4.0.2 material (Vela release provider, md5 9aa4f51d3fcfb6c5b2c963fa2ccba038).
AMD's SDK license (docs/license.md) lists these signed DLLs under the following license:

Copyright (C) Advanced Micro Devices, Inc.
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. This project is independent and is not
provided, sponsored or endorsed by AMD. The file name amd_fidelityfx_upscaler_vk.dll is required by the FFX loader.

The FFX API headers compiled into the provider (ffx_api.h, ffx_api_types.h, ffx_upscale.h) carry AMD's MIT notice of
AMD FidelityFX SDK 2.0.0: AMD-FidelityFX-SDK-2.0.0-MIT.md next to this file.
