# PC build (x64, MSVC) for testing on a PC without the console:
#   out\pc\dxgi.dll          our recon/replacement DLL (same sources as the ARM64EC console build)
#   out\pc\fsr2_harness.exe  a D3D12 app with FSR 2.1.2 linked statically (stand-in for RE4's built-in FSR 2)
#
#   powershell -ExecutionPolicy Bypass -File build_pc.ps1
#   cd out\pc; .\fsr2_harness.exe 240 16
#
# Detours (MIT) and FSR 2.1.2 (MIT, symbol-renamed _212 build) come prebuilt from the OptiScaler fork tree: set
# OPTISCALER_DIR to its OptiScaler folder (include\ and library\ below it).
$ErrorActionPreference = "Stop"
$env:Path = $env:Path -replace '"', ''
$root = $PSScriptRoot
if (-not $env:OPTISCALER_DIR) { throw "set OPTISCALER_DIR to the fork's OptiScaler folder" }
$fork = $env:OPTISCALER_DIR
$out = "$root\out\pc"
New-Item -ItemType Directory -Force -Path $out | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue "$out\dxgi.dll", "$out\fsr2_harness.exe"

$vcvars = (Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $vcvars) { throw "vcvars64.bat not found" }

# vela_fsr2.cpp with VELA_NO_VULKAN: roles, calibration and the series recorder run on the PC (no vkd3d interop)
$dll = "cl /nologo /std:c++17 /EHsc /O2 /MD /DWIN32 /DNDEBUG /DUNICODE /D_UNICODE /DVELA_NO_VULKAN " +
       "/I`"$root\src`" /I`"$fork\include`" " +
       "`"$root\src\dllmain.cpp`" `"$root\src\dxgi_proxy.cpp`" `"$root\src\d3d12_recon.cpp`" `"$root\src\vela_fsr2.cpp`" `"$root\src\ext_upscalers.cpp`" " +
       "/LD /Fe:`"$out\dxgi.dll`" /link `"$fork\library\detours\detours.lib`" d3d12.lib dxguid.lib ole32.lib " +
       "/IMPLIB:`"$out\recon_implib.lib`" /DEF:`"$root\src\dxgi_exports.def`" /MAP:`"$out\dxgi.map`""

$fsr = "$fork\include\fsr2_212"
$flib = "$fork\library\fsr2_212"
$exe = "cl /nologo /std:c++17 /EHsc /O2 /MD /DNDEBUG /I`"$fsr`" `"$root\tools\fsr2_harness.cpp`" " +
       "/Fe:`"$out\fsr2_harness.exe`" /link `"$flib\ffx_fsr2_212_api_dx12_x64.lib`" `"$flib\ffx_fsr2_212_api_x64.lib`" " +
       "d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib"

$bat = "$out\_build.bat"
@"
call "$vcvars"
cd /d "$out"
$dll
$exe
"@ | Set-Content -Encoding ascii $bat
cmd /c "`"$bat`""
foreach ($f in "dxgi.dll", "fsr2_harness.exe") { if (-not (Test-Path "$out\$f")) { throw "build failed: $f" } }
Write-Host "built $out\dxgi.dll and $out\fsr2_harness.exe"
