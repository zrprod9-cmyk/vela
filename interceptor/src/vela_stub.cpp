// PC build (x64, real D3D12): no vkd3d interop, so no Vela -- the recon still runs, Vela stays off.
#include "vela_fsr2.h"
void VelaInit(ID3D12Device*, BufLookup) {}
bool VelaEnabled() { return false; }
bool VelaOnFsrPass(ID3D12GraphicsCommandList*, const std::string&, bool, const std::vector<VelaTex>&, UINT64,
                   const uint8_t*, uint32_t)
{
    return true;
}
void VelaAfterFsrPass(ID3D12GraphicsCommandList*, bool) {}
bool VelaOnPostPass(ID3D12GraphicsCommandList*, const std::vector<VelaTex>&, UINT64) { return true; }
void VelaAfterPostPass(ID3D12GraphicsCommandList*) {}
void VelaOnPresent() {}
void VelaOnResourceFreed(ID3D12Resource*) {}
void VelaRequestCapture(int) {}
