// Other models in place of a game's built-in FSR 2 (Vela 0.8.3-re): the same choice OptiScaler games have, driven by
// the same live file the Decky panel writes (/tmp/vela-live/<appid>.ini, Z:\tmp\... in Wine; the fork's VelaLive.h):
// The live models are fsr4, fsr4lite, fsr3, sgsr2 only; Vela AI / Vela SR (ai, p2b, sr) left
// the choice. An unknown or old key, a model that cannot run (sizes, init) or a jitter that is not FSR 2's: the game's
// own FSR 2 runs (model fsr2 = pass-through), never Vela AI. No model in the live file: fsr4.
//   model=fsr4         FSR 4.1.1 VK INT8 (fsr4vk provider amd_fidelityfx_upscaler_vk.dll next to this DLL; npu=1|2:
//                      the network on the Hexagon NPU through VK_LAYER_VELA_npu, 960x540 -> 1920x1080 only)
//   model=fsr4lite     FSR 4.1.2 Lite on the same provider (render 645x360..1920x1080 -> 1920x1080; lossy=<N>, npu)
//   model=fsr3         FSR 3.1 from the FidelityFX SDK's amd_fidelityfx_dx12.dll next to this DLL (D3D12 backend)
//   model=sgsr2        Qualcomm Snapdragon GSR 2 (BSD-3), 2-pass compute on vkd3d's VkDevice (sgsr2_vk.cpp)
// FSR 4 runs on vkd3d-proton's own VkDevice through the interop the student uses -- the fork's Fsr4VkBridge.h without
// OptiScaler. The providers take the jitter as CPU numbers; a game whose cbFSR2 is made on the GPU (RE4) gets it from
// JitterTrack: FSR 2's Halton(2, 3) sequence, its phase locked from per-frame readbacks of the cbuffer's jitter.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace Ext
{
// Fsr4Fast = model=fsr4fast ("FSR 4 Fast"): FSR 4 on the same provider, which reads the live file's model=fsr4fast
// at context create (VELA_FAST=1 for a run without a live file); its own value, so fsr4 <-> fsr4fast re-creates the context
enum class Model { Unset, Ai, Sr, Sgsr2, Fsr4, Fsr4Lite, Fsr3, Fsr2, Fsr4Fast }; // Fsr2: the game's own FSR 2 (pass-through)
const char* ModelName(Model m);
// the fps line's name of a model ("FSR 4", "FSR 4 Lite", "FSR 3.1", "SGSR 2", "the game's FSR 2")
const char* ModelLabel(Model m);
inline bool IsFsr4Full(Model m) { return m == Model::Fsr4 || m == Model::Fsr4Fast; }
inline bool IsExt(Model m) { return IsFsr4Full(m) || m == Model::Fsr4Lite || m == Model::Fsr3 || m == Model::Sgsr2; }

struct Live
{
    Model model = Model::Unset;
    int lossy = 1; // FSR 4.1 Lite: the network every N-th frame
    int npu = 0;   // FSR 4.1 / Lite network on the NPU: 0 off, 1 exact, 2 async
};
// Reads the file when it changed (at most every 15 calls, or now with force); a change is logged once
const Live& PollLive(bool force = false);

struct Tex
{
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
struct Frame
{
    float jitterX = 0, jitterY = 0, mvScaleX = 1, mvScaleY = 1, preExposure = 1, frameTimeMs = 16.6f, fovV = 1.0f;
    float cameraNear = 0.1f, cameraFar = 1000.0f;
    bool reset = false;
    bool depthInverted = true; // reverse-Z (RE4); cameraNear / cameraFar are then swapped by RunExt
};

class Upscaler
{
  public:
    virtual ~Upscaler() = default;
    // colour: LDR (the interceptor's reversible tonemap), depth inverted, render-size non-jittered MVs
    virtual bool Evaluate(ID3D12GraphicsCommandList* cl, const Tex& color, const Tex& depth, const Tex& mv, const Tex& out,
                          const Frame& f) = 0;
    virtual void Release() = 0;
    // the GPU time of a recent frame of this model (timestamps around its work), false when not measured
    virtual bool LastGpuMs(double& ms) { (void) ms; return false; }
    // true while the model is still being built off the render thread, or while its first frames are kept
    // hidden; RunExt then lets the game's FSR 2 picture stand this frame (no failure, no retry key)
    virtual bool Pending() { return false; }
    virtual bool Hiding() const { return false; }
    Model model = Model::Unset;
    uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
    int lossy = 1, npu = 0;
};
// nullptr (and a log line) when the model cannot run here
Upscaler* Create(ID3D12Device* dev, Model m, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, int lossy, int npu);
// SGSR 2 (sgsr2_vk.cpp); nullptr when it cannot run
Upscaler* CreateSgsr2(ID3D12Device* dev, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);
// before a context is destroyed: vkd3d-proton's device idle (frames in flight)
void WaitIdle(ID3D12Device* dev);

// Our own D3D12 work (FSR 3.1's passes) runs through the recon hooks untouched while this is > 0
bool OwnWork();

// the descriptor heaps the game last bound on a command list (d3d12_recon's SetDescriptorHeaps hook);
// 0 when none was seen since its Reset
UINT ListHeaps(ID3D12GraphicsCommandList* cl, ID3D12DescriptorHeap* out[2]);
// the game's compute state on the list (heaps, root signature, pipeline, root arguments) set again
// through the original functions after FSR 3.1's D3D12 passes; the number of calls made
UINT RestoreGameState(ID3D12GraphicsCommandList* cl);

// FSR 2's jitter for a cbFSR2 the CPU never sees: ffxFsr2GetJitterOffset = (Halton(i + 1, 2) - 0.5, Halton(i + 1, 3) -
// 0.5), i = frame mod phases. Each frame's jitter is copied to a readback slot; slots 12 frames old are read, matched to
// the table and the phase offset (frame counter -> i) locked after 2 agreeing frames; 2 misses in a row unlock it.
class JitterTrack
{
  public:
    void Record(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* owner, UINT64 off, uint32_t frame,
                int phases);
    bool Predict(uint32_t frame, float& x, float& y) const;
    bool Locked() const { return _locked; }
    bool Hopeless() const { return _alien > 60 && _hits == 0; } // the game's jitter is not FSR 2's Halton sequence

  private:
    void Observe(uint32_t frame, float x, float y);
    static constexpr int kSlots = 64, kLag = 12;
    ID3D12Resource* _rb = nullptr;
    uint8_t* _map = nullptr;
    uint32_t _slotFrame[kSlots] = {};
    bool _pending[kSlots] = {};
    int _phases = 0;
    float _tab[256][2] = {};
    bool _locked = false;
    int64_t _offset = 0;
    int _hits = 0, _misses = 0, _alien = 0;
    float _sign = 1.0f;
};
} // namespace Ext
