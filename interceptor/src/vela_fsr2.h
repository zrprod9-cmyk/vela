// Vela in place of a game's built-in FSR 2. Driven by the D3D12 recon hooks (d3d12_recon.cpp), which
// find the FSR 2 passes and resolve their bindings; this module decides, per pass, whether FSR runs, and runs Vela.
//
//   VELA_FSR2=off       (default) recon only
//   VELA_FSR2=replace   skip FSR's passes (except exposure / the game's own depth preparation); Vela renders the
//                       frame, which goes where FSR's accumulate would have written it (its output, or the history
//                       that the game's RCAS then sharpens)
//   VELA_FSR2=shadow    FSR runs untouched; Vela renders the same inputs into its own textures; a capture saves
//                       crops of several frames of both (same-frame comparisons, noise / flicker metrics)
//   VELA_FSR2_ALGO      ai (Vela AI, default) | sr (Vela SR) | both (shadow)
//   VELA_HDRWRAP=0      no reversible tonemap around Vela (default on: tames HDR spikes the way FSR 2 does)
//   VELA_RCAS=0         skip the game's RCAS after Vela (default: RCAS runs as with FSR)
//   VELA_MVSCALE=<s>    MV scale for layouts whose MV scale is not trusted (RE4; default 1)
//   VELA_ALPHAK=<k>     Vela AI: damping of the nearest current sample, 0.25..1 (default 1; Control's profile 0.75)
//   VELA_AMAX=<w>       Vela AI: cap on the current sample's weight, w <= amax (default 0 = no cap, as the recipe)
//   VELA_SKIP_SDF=1     RE4: drop the SDF pass (ExecuteIndirect of DXIL 1b96d027..., ~40 ms on the console)
//   VELA_SKIP_EI=<hash prefix>[,...]   drop ExecuteIndirect calls of these compute pipelines (PERF_SKIP_EI: alias)
//   VELA_CAP_FRAMES / VELA_CAP_RECT=x,y,w,h   shadow capture: frames (8) and crop in display pixels
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

struct VelaTex
{
    int kind; // 1 SRV, 2 UAV
    UINT reg;
    ID3D12Resource* res;
};

using BufLookup = ID3D12Resource* (*)(UINT64 va, UINT64& off); // the buffer that owns a GPU VA

void VelaInit(ID3D12Device* dev, BufLookup lookup);
bool VelaEnabled();
// One FSR 2 pass is about to be dispatched. cbVA: its constant buffer (GPU), cpuCb/cpuCbSize: its root constants
// (when the game passes cbFSR2 that way). Returns false if the hook must skip the game's dispatch.
bool VelaOnFsrPass(ID3D12GraphicsCommandList* cl, const std::string& hash, bool accumulate,
                   const std::vector<VelaTex>& tex, UINT64 cbVA, const uint8_t* cpuCb, uint32_t cpuCbSize);
// After the game's own dispatch of an FSR pass ran (shadow captures).
void VelaAfterFsrPass(ID3D12GraphicsCommandList* cl, bool accumulate);
// The dispatch right after an accumulate on the same command list (RCAS, when the game sharpens).
bool VelaOnPostPass(ID3D12GraphicsCommandList* cl, const std::vector<VelaTex>& tex, UINT64 cbVA);
void VelaAfterPostPass(ID3D12GraphicsCommandList* cl);
// The game freed a resource (its public count reached 0): drop every pointer to it.
void VelaOnResourceFreed(ID3D12Resource* r);
// Once per present (fps / timing log, flush of pending readbacks).
void VelaOnPresent();
// Capture request from the recon trigger: in shadow mode, save FSR's and Vela's results of the next frames.
void VelaRequestCapture(int index);
