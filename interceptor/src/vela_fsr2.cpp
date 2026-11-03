#include "vela_fsr2.h"
#include "contract.h"
#include "ext_upscalers.h"
#include "log.h"
#ifndef VELA_NO_VULKAN
#include "hdr_wrap.h"
#include "sgsr3_fast_vk.h"
#include "student_vk.h"
#else
// PC build: no vkd3d interop -- calibration, roles, the series recorder and FSR's own results still work
struct VkUpscalerV1f { bool LastTiming(double*) { return false; } };
struct SGSR3StudentVk { bool LastTiming(double*) { return false; } float DefaultMk() const { return 8.0f; } };
struct HdrWrap
{
    enum Mode { Forward = 0, Inverse = 1, Copy = 2 };
    bool Init(ID3D12Device*) { return false; }
    bool Ready() const { return false; }
    bool Run(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, ID3D12Resource*, D3D12_RESOURCE_STATES,
             uint32_t, uint32_t, Mode) { return false; }
};
struct ID3D12DXVKInteropDevice1;
#endif
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <set>

extern const float st_default_mk_value; // student_vk.cpp (the recipe's MK from SGSR3_Student_spv.h)

namespace
{
ID3D12Device* g_dev = nullptr;
ID3D12DXVKInteropDevice1* g_interop = nullptr;
BufLookup g_buf = nullptr;
int g_mode = 0; // 0 off, 1 replace, 2 shadow
// RE4 GPU fault hunt ("gpu fault iova=0x6006880 READ TRANSLATION UCHE" in the game scene with replace, none on
// the title): VELA_FAULT_CYCLE=<s> cycles, in one game session, A hooks only (off) / B shadow (Vela runs, nothing
// skipped) / C replace with only the accumulate (and RCAS) skipped / D replace as shipped / E D without the GPU copy of
// jitter -- each <s> seconds, the phase changes logged with local time to set against `journalctl -k`. Every skipped
// game dispatch is counted per pipeline, and the first skip of each (pipeline, depth read, R32F write) is logged.
int g_cycleSec = 0, g_phase = -1;
ULONGLONG g_cycleT0 = 0;
bool g_skipSeq = true;  // replace: skip the learned FSR sequence's other passes (false: only the accumulate)
bool g_noPatch = false; // no GPU copy of jitter / preExposure from the game's cbuffer
std::map<std::string, uint64_t> g_skipCount;
int g_algo = 0; // 0 ai, 1 sr, 2 both
bool g_wrap = true, g_rcas = true, g_depthInv = true;
float g_mvOverride = 1.0f;
// 0.8.3-re: FSR 4.1 / 4.1 Lite / FSR 3.1 in place of FSR 2 too (ext_upscalers.h), chosen by the live file or
// VELA_FSR2_ALGO=fsr4|fsr4lite|fsr3; the frame's model is fixed at its first FSR pass
Ext::Model g_envExt = Ext::Model::Unset;
Ext::Model g_frameModel = Ext::Model::Fsr2, g_ranModel = Ext::Model::Unset;
Ext::Upscaler* g_ext = nullptr;
std::string g_extFailed;     // the model / size / lossy / npu that could not be created (not retried)
Ext::JitterTrack g_jit;
// A replaced context is released 16 presents later, not at once. vkd3d-proton submits the game's
// D3D12 lists from its own submission thread, so at a model switch lists of the last frames that still carry the old
// context's work (FSR 4 + NPU: the mid-command-buffer wait on host event E2, set by that context's worker) may not be
// in Vulkan's queue yet: vkDeviceWaitIdle returned, ffxDestroyContext stopped the worker, and the late list then waited
// on an E2 nobody sets -> "hangcheck detected gpu lockup". The old context (and its NPU worker) now outlives them.
std::vector<std::pair<Ext::Upscaler*, uint64_t>> g_retired;
uint64_t g_presentN = 0;
void Retire(Ext::Upscaler*& u)
{
    if (!u) return;
    g_retired.push_back({u, g_presentN});
    LOGI("vela: %s context retired (released after 16 presents)", Ext::ModelName(u->model));
    u = nullptr;
}
void ReleaseRetired()
{
    g_presentN++;
    for (auto it = g_retired.begin(); it != g_retired.end();)
        if (g_presentN - it->second >= 16) { it->first->Release(); delete it->first; it = g_retired.erase(it); }
        else ++it;
}
uint32_t g_accN = 0;         // accumulates seen (the jitter tracker's frame counter)
bool g_lastCpu = false;      // the last accumulate had its cbFSR2 as root constants (jitter on the CPU)
LARGE_INTEGER g_qpcLast{}, g_qpcFreq{};

// ---- cbFSR2 layouts ---------------------------------------------------------------------------------------------
// RE4: FSR 2.0.x + one RE float4 at +48 (the field at +104 is render/display, so its MV scale is measured: 1).
// DRG (UE4 plugin, root constants): stock 2.0/2.1. FSR 2.2: renderSize, maxRenderSize, displaySize, ...
struct Layout
{
    const char* name;
    uint32_t display, jitter, mvScale, preExp, tanHalfFovH, phases, unitPair; // unitPair: a float2 that is (0, 1]
    bool mvTrusted;
};
const Layout kLayouts[] = {
    {"fsr2.2", 16, 64, 72, 96, 104, 108, 80, true},
    {"fsr2.0/2.1", 8, 40, 88, 104, 108, 120, 64, true},
    {"fsr2.0+re16", 8, 40, 104, 112, 116, 120, 80, false},
};

struct Calib
{
    bool ok = false;
    const Layout* lay = nullptr;
    int32_t rw = 0, rh = 0, dw = 0, dh = 0;
    float mvScale[2] = {}, preExp = 1, tanH = 0;
    int phases = 0; // FSR 2 jitter phase count (8 * (display / render)^2)
} g_cal;
struct Readback
{
    ID3D12Resource* rb = nullptr;
    ULONGLONG at = 0;
} g_calRb;

float Fat(const uint8_t* p, uint32_t o)
{
    float f;
    memcpy(&f, p + o, 4);
    return f;
}

// Fill g_cal from cbFSR2 bytes (a GPU readback or the root constants); tex: the colour texture's size
bool Calibrate(const uint8_t* p, uint32_t n, uint32_t texW, uint32_t texH)
{
    if (n < 128) return false;
    const int32_t* I = (const int32_t*) p;
    const int32_t rw = I[0], rh = I[1];
    for (const Layout& l : kLayouts)
    {
        if (l.phases + 4 > n) continue;
        const int32_t dw = I[l.display / 4], dh = I[l.display / 4 + 1];
        const float ph = Fat(p, l.phases), pe = Fat(p, l.preExp), t = Fat(p, l.tanHalfFovH);
        const float u0 = Fat(p, l.unitPair), u1 = Fat(p, l.unitPair + 4);
        const bool sizes = rw > 0 && rh > 0 && dw >= rw && dh >= rh && dw <= 8192 && dh <= 8192 &&
                           (uint32_t) rw <= texW && (uint32_t) rw + 64 >= texW && (uint32_t) rh <= texH &&
                           (uint32_t) rh + 64 >= texH;
        if (!sizes || !(ph >= 4 && ph <= 256 && std::fabs(ph - std::round(ph)) < 1e-3f) || !(pe > 0 && pe < 1e4f) ||
            !(t > 0.05f && t < 10) || !(u0 > 0 && u0 <= 1.01f && u1 > 0 && u1 <= 1.01f))
            continue;
        g_cal.lay = &l;
        g_cal.rw = rw;
        g_cal.rh = rh;
        g_cal.dw = dw;
        g_cal.dh = dh;
        g_cal.mvScale[0] = l.mvTrusted ? Fat(p, l.mvScale) : g_mvOverride;
        g_cal.mvScale[1] = l.mvTrusted ? Fat(p, l.mvScale + 4) : g_mvOverride;
        g_cal.preExp = pe;
        g_cal.tanH = t;
        g_cal.phases = (int) std::lround(ph);
        g_cal.ok = true;
        LOGI("vela: calibration ok: layout %s render %dx%d (texture %ux%u) display %dx%d jitter (%.4f, %.4f) mvScale "
             "(%.4f, %.4f)%s preExposure %.3f tan(hfov/2) %.3f phases %.0f",
             l.name, rw, rh, texW, texH, dw, dh, Fat(p, l.jitter), Fat(p, l.jitter + 4), g_cal.mvScale[0],
             g_cal.mvScale[1], l.mvTrusted ? "" : " (measured)", pe, t, ph);
        return true;
    }
    static int fails = 0;
    if (fails++ < 3) LOGI("vela: calibration: no known cbFSR2 layout fits (render %dx%d, texture %ux%u)", rw, rh, texW, texH);
    return false;
}

// ---- per-frame state ----------------------------------------------------------------------------------------------
struct Frame
{
    ID3D12Resource *color = nullptr, *depth = nullptr, *mv = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} g_fr;
bool g_newFrame = true;         // the next FSR pass starts a frame
std::set<ID3D12Resource*> g_srvPrev, g_srvCur; // read (SRV) by FSR's passes in the previous / this frame
bool g_lastReplaceable = false; // the last accumulate could be replaced
bool g_skipping = false;        // this frame: FSR's own passes are skipped
bool g_replacedThisFrame = false;
uint64_t g_frames = 0;
bool g_reset = true;
uint32_t g_texW = 0, g_texH = 0, g_stableN = 0;

struct Ups
{
    VkUpscalerV1f* sr = nullptr;
    SGSR3StudentVk* ai = nullptr;
    bool srTried = false, aiTried = false;
};
std::map<uint64_t, Ups> g_ups; // per (render, display) size; never released (frames may be in flight)
Ups* CurUps()
{
    const uint64_t k = ((uint64_t) g_cal.rw << 48) | ((uint64_t) g_cal.rh << 32) | ((uint64_t) g_cal.dw << 16) | g_cal.dh;
    return &g_ups[k];
}
HdrWrap g_hdr;
bool g_hdrTried = false;
std::map<uint64_t, ID3D12Resource*> g_texCache; // our own textures by (w, h, slot)
ID3D12Resource* g_target = nullptr;  // where the frame goes this frame (FSR's output or history)
ID3D12Resource* g_history = nullptr; // FSR's history (what RCAS reads), for RCAS detection

// fps / timing
uint64_t g_presents = 0, g_replaced = 0;
ULONGLONG g_fpsT0 = 0;

D3D12_RESOURCE_DESC D(ID3D12Resource* r) { return r->GetDesc(); }
bool IsColorFmt(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R10G10B10A2_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

ID3D12Resource* MakeBuf(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

// our RGBA16F UAV texture (w x h), one per slot; kept for the session
ID3D12Resource* OwnTex(uint32_t w, uint32_t h, int slot)
{
    const uint64_t k = ((uint64_t) w << 40) | ((uint64_t) h << 16) | (uint64_t) slot;
    auto it = g_texCache.find(k);
    if (it != g_texCache.end()) return it->second;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                   IID_PPV_ARGS(&r));
    g_texCache[k] = r;
    return r;
}

// ---- calibration from the GPU (constants in a buffer) ----------------------------------------------------------
void StartGpuCalibration(ID3D12GraphicsCommandList* cl, UINT64 cbVA)
{
    if (g_calRb.rb || !cbVA || !g_buf) return;
    UINT64 off = 0;
    ID3D12Resource* owner = g_buf(cbVA, off);
    if (!owner) { static bool once = false; if (!once) { once = true; LOGI("vela: cbFSR2 owner buffer not found"); } return; }
    g_calRb.rb = MakeBuf(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_calRb.rb) return;
    UINT64 n = D(owner).Width - off;
    cl->CopyBufferRegion(g_calRb.rb, 0, owner, off, n < 256 ? n : 256);
    g_calRb.at = GetTickCount64() + 1500;
}
void FinishGpuCalibration()
{
    if (!g_calRb.rb || GetTickCount64() < g_calRb.at) return;
    uint8_t* p = nullptr;
    if (SUCCEEDED(g_calRb.rb->Map(0, nullptr, (void**) &p)) && p)
    {
        if (!g_cal.ok && g_stableN >= 60) Calibrate(p, 256, g_texW, g_texH);
        g_calRb.rb->Unmap(0, nullptr);
    }
    g_calRb.rb->Release();
    g_calRb.rb = nullptr;
}

// ---- upscalers ------------------------------------------------------------------------------------------------
struct PerFrame
{
    UINT64 cbVA = 0;
    const uint8_t* cpu = nullptr; // root constants of the accumulate (CPU path)
    float jitter[2] = {}, preExp = 1;
    uint32_t accN = 0; // this accumulate's number (JitterTrack)
};

#ifndef VELA_NO_VULKAN
GpuPatches PatchesFor(const PerFrame& f, uint32_t dstJitter, uint32_t dstPreExp)
{
    GpuPatches p;
    if (f.cpu || g_noPatch) return p; // CPU path: values are in the constants already
    UINT64 off = 0;
    ID3D12Resource* owner = g_buf ? g_buf(f.cbVA, off) : nullptr;
    UINT64 h = 0, base = 0;
    if (!owner || !g_interop || FAILED(g_interop->GetVulkanResourceInfo1(owner, &h, &base, nullptr)) || !h) return p;
    p.src = (VkBuffer) h;
    p.list.push_back({base + off + g_cal.lay->jitter, dstJitter, 8});
    if (!g_wrap) p.list.push_back({base + off + g_cal.lay->preExp, dstPreExp, 4});
    return p;
}

#endif
FrameInputs Inputs(const PerFrame& f)
{
    FrameInputs in{};
    in.renderW = g_cal.rw;
    in.renderH = g_cal.rh;
    in.displayW = g_cal.dw;
    in.displayH = g_cal.dh;
    in.jitterX = f.jitter[0];
    in.jitterY = f.jitter[1];
    in.mvScaleX = g_cal.mvScale[0] * g_cal.rw; // contract: mvToUv = mvScale / render (cbFSR2 holds it per UV)
    in.mvScaleY = g_cal.mvScale[1] * g_cal.rh;
    in.mvTexW = g_cal.rw;
    in.mvTexH = g_cal.rh;
    auto dd = D(g_fr.depth);
    in.depthW = (uint32_t) dd.Width;
    in.depthH = dd.Height;
    in.preExposure = g_wrap ? 1.0f : f.preExp; // wrapped input is already compressed; Vela gives back raw units
    in.verticalFovRad = 2.0f * std::atan(g_cal.tanH * (float) g_cal.rh / (float) g_cal.rw);
    in.reset = g_reset;
    in.lowResMV = true;
    in.depthInverted = g_depthInv;
    in.hasReactive = false;
    return in;
}

bool RunSr(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState, ID3D12Resource* out,
           const PerFrame& f)
{
#ifdef VELA_NO_VULKAN
    return false;
#else
    Ups* u = CurUps();
    if (!u->sr && !u->srTried)
    {
        u->srTried = true;
        const float s = (float) g_cal.dw / (float) g_cal.rw;
        u->sr = new VkUpscalerV1f();
        if (!u->sr->Init(g_dev, g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, s < 1.75f ? 0 : s < 2.5f ? 1 : 2,
                         VkUpscalerV1f::Variant::Lock))
        {
            LOGI("vela: Vela SR init failed: %s", u->sr->Info().c_str());
            delete u->sr;
            u->sr = nullptr;
        }
        else
            LOGI("vela: Vela SR ready %dx%d -> %dx%d (%s)", g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, u->sr->Info().c_str());
    }
    if (!u->sr || !u->sr->Ready()) return false;
    SGSR3Constants c{};
    FillConstants(c, Inputs(f));
    auto cd = D(color);
    c.colorSizeRcp[0] = 1.0f / (float) cd.Width; // the colour may be a little larger than the render size (DRG)
    c.colorSizeRcp[1] = 1.0f / (float) cd.Height;
    u->sr->SetPatches(PatchesFor(f, offsetof(SGSR3Constants, jitter), offsetof(SGSR3Constants, preExposure)));
    using I = VkUpscalerV1f::Input;
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return u->sr->Evaluate(cl, I{color, colorState}, I{g_fr.depth, g_fr.depthState}, I{g_fr.mv, SRV}, I{}, I{},
                           I{out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}, &c, sizeof(c), g_reset);
#endif
}

bool RunAi(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState, ID3D12Resource* out,
           const PerFrame& f)
{
#ifdef VELA_NO_VULKAN
    return false;
#else
    Ups* u = CurUps();
    if (!u->ai && !u->aiTried)
    {
        u->aiTried = true;
        u->ai = new SGSR3StudentVk();
        if (!u->ai->Init(g_dev, g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, false))
        {
            LOGI("vela: Vela AI init failed: %s", u->ai->Info().c_str());
            delete u->ai;
            u->ai = nullptr;
        }
        else
            LOGI("vela: Vela AI ready %dx%d -> %dx%d (%s)", g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, u->ai->Info().c_str());
    }
    if (!u->ai || !u->ai->Ready()) return false;
    SGSR3StudentVk::Params p{};
    p.renderSize[0] = g_cal.rw;
    p.renderSize[1] = g_cal.rh;
    p.displaySize[0] = g_cal.dw;
    p.displaySize[1] = g_cal.dh;
    p.jitter[0] = f.jitter[0];
    p.jitter[1] = f.jitter[1];
    p.mvToUv[0] = g_cal.mvScale[0];
    p.mvToUv[1] = g_cal.mvScale[1];
    p.renderRcp[0] = 1.0f / g_cal.rw;
    p.renderRcp[1] = 1.0f / g_cal.rh;
    p.displayRcp[0] = 1.0f / g_cal.dw;
    p.displayRcp[1] = 1.0f / g_cal.dh;
    p.preExposure = g_wrap ? 1.0f : f.preExp;
    p.invScale = (float) g_cal.rw / (float) g_cal.dw;
    p.flags = 1u | (g_depthInv ? 2u : 0u) | 4u; // depth, inverted, low-res MV
    p.frameIndex = (uint32_t) g_frames;
    static float alphaK = -1;
    if (alphaK < 0) { char e[16] = {}; alphaK = GetEnvironmentVariableA("VELA_ALPHAK", e, sizeof(e)) ? (float) atof(e) : 1.0f; }
    p.alphaK = alphaK < 0.25f ? 0.25f : alphaK > 1.0f ? 1.0f : alphaK; // damping of the nearest current sample (Control: 0.75)
    p.mk = u->ai->DefaultMk(); // the running recipe's MK (main, or the fallback on a device without INT8)
    static float amax = -1;
    if (amax < 0) { char e[16] = {}; amax = GetEnvironmentVariableA("VELA_AMAX", e, sizeof(e)) ? (float) atof(e) : 0.0f; if (amax < 0) amax = 0; }
    p.amax = amax; // the current sample's weight cap w <= amax (report 37), 0 = none: the recipe bit for bit
    u->ai->SetPatches(PatchesFor(f, offsetof(SGSR3StudentVk::Params, jitter),
                                 offsetof(SGSR3StudentVk::Params, preExposure)));
    using I = SGSR3StudentVk::Input;
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return u->ai->Evaluate(cl, I{color, colorState}, I{g_fr.depth, g_fr.depthState}, I{g_fr.mv, SRV}, I{}, I{},
                           I{out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}, p, g_reset);
#endif
}

// FSR 4.1 / Lite / FSR 3.1 (ext_upscalers.h) on the same inputs: the jitter from the root constants, else predicted
std::string ExtKey(Ext::Model m, int& lossy, int& npu)
{
    const Ext::Live& lv = Ext::PollLive();
    lossy = m == Ext::Model::Fsr4Lite ? lv.lossy : 1;
    npu = Ext::IsFsr4Full(m) || m == Ext::Model::Fsr4Lite ? lv.npu : 0;
    char key[96];
    snprintf(key, sizeof(key), "%s %dx%d %dx%d %d %d", Ext::ModelName(m), g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, lossy, npu);
    return key;
}

// (debug, VELA_DUMP_OUT=N or the file Z:\tmp\vela-dump-out): the model's output (our RGBA16F texture, before Deliver; the wrapped LDR image when the
// HDR wrap is on) of the N-th frame after each context creation, copied to a readback buffer and written 30 frames later
// as Z:	mpela_out_<model>_<w>x<h>.rgba16f (rows of 8-byte texels, no padding). Off by default.
uint32_t g_dumpSince = 0;
struct Dump
{
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t w = 0, h = 0, wait = 0;
    std::string name;
} g_dump;
void DumpOut(ID3D12GraphicsCommandList* cl, ID3D12Resource* out, Ext::Model m)
{
    static int at = -1;
    if (at < 0)
    {
        // the environment, else the file Z:\tmp\vela-dump-out (a locked test run writes N there before the launch)
        char v[16] = {};
        at = GetEnvironmentVariableA("VELA_DUMP_OUT", v, sizeof(v)) ? atoi(v) : 0;
        if (!at)
            if (FILE* fi = fopen("Z:\\tmp\\vela-dump-out", "rb")) { if (fgets(v, sizeof(v), fi)) at = atoi(v); fclose(fi); }
        if (at > 0) LOGI("vela: output dump at frame %d after each context creation", at);
    }
    g_dumpSince++;
    if (g_dump.rb && g_dump.wait && --g_dump.wait == 0)
    {
        uint8_t* p = nullptr;
        D3D12_RANGE r{0, (SIZE_T) (g_dump.fp.Footprint.RowPitch * g_dump.h)};
        if (SUCCEEDED(g_dump.rb->Map(0, &r, (void**) &p)) && p)
        {
            if (FILE* fo = fopen(g_dump.name.c_str(), "wb"))
            {
                for (uint32_t y = 0; y < g_dump.h; y++) fwrite(p + (size_t) y * g_dump.fp.Footprint.RowPitch, 8, g_dump.w, fo);
                fclose(fo);
                LOGI("vela: output dump written: %s", g_dump.name.c_str());
            }
            D3D12_RANGE none{0, 0};
            g_dump.rb->Unmap(0, &none);
        }
        g_dump.rb->Release();
        g_dump.rb = nullptr;
    }
    if (at <= 0 || g_dumpSince != (uint32_t) at || g_dump.rb || !out) return;
    D3D12_RESOURCE_DESC d = out->GetDesc();
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &g_dump.fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&g_dump.rb))))
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = out;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g_dump.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g_dump.fp;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
    g_dump.w = (uint32_t) d.Width;
    g_dump.h = d.Height;
    g_dump.wait = 30;
    char n[160];
    snprintf(n, sizeof(n), "Z:\\tmp\\vela_out_%s_%ux%u.rgba16f", Ext::ModelName(m), g_dump.w, g_dump.h);
    g_dump.name = n;
}

bool RunExt(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState, ID3D12Resource* out,
            const PerFrame& f)
{
#ifdef VELA_NO_VULKAN
    return false;
#else
    const Ext::Model m = g_frameModel;
    int lossy = 1, npu = 0;
    const std::string key = ExtKey(m, lossy, npu);
    if (key == g_extFailed) return false;
    if (!g_ext || g_ext->model != m || g_ext->rw != (uint32_t) g_cal.rw || g_ext->rh != (uint32_t) g_cal.rh ||
        g_ext->dw != (uint32_t) g_cal.dw || g_ext->dh != (uint32_t) g_cal.dh || g_ext->lossy != lossy || g_ext->npu != npu)
    {
        Retire(g_ext); // released 16 presents later (frames still in vkd3d's submission queue)
        g_ext = Ext::Create(g_dev, m, g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, lossy, npu);
        if (!g_ext) { g_extFailed = key; return false; }
        g_reset = true;
        g_dumpSince = 0;
    }
    if (g_ext->Pending()) return false; // still building off the render thread: the game's FSR 2 this frame
    Ext::Frame e;
    if (f.cpu) { e.jitterX = f.jitter[0]; e.jitterY = f.jitter[1]; }
    else if (!g_jit.Predict(f.accN, e.jitterX, e.jitterY)) return false;
    e.mvScaleX = g_cal.mvScale[0] * g_cal.rw; // FFX: MV scale to render pixels (cbFSR2 holds it per UV)
    e.mvScaleY = g_cal.mvScale[1] * g_cal.rh;
    e.preExposure = g_wrap ? 1.0f : f.preExp;
    e.fovV = 2.0f * std::atan(g_cal.tanH * (float) g_cal.rh / (float) g_cal.rw);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpcFreq.QuadPart) QueryPerformanceFrequency(&g_qpcFreq);
    const double ms = g_qpcLast.QuadPart ? 1000.0 * (double) (now.QuadPart - g_qpcLast.QuadPart) / (double) g_qpcFreq.QuadPart : 16.6;
    g_qpcLast = now;
    e.frameTimeMs = ms >= 1.0 && ms <= 200.0 ? (float) ms : 16.6f;
    if (g_depthInv) std::swap(e.cameraNear, e.cameraFar); // as the fork does for inverted depth
    e.depthInverted = g_depthInv;
    e.reset = g_reset;
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (g_ext->Evaluate(cl, Ext::Tex{color, colorState}, Ext::Tex{g_fr.depth, g_fr.depthState}, Ext::Tex{g_fr.mv, SRV},
                        Ext::Tex{out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}, e))
    {
        DumpOut(cl, out, m);
        // its first frames ran (history, NPU pipeline) but the game's FSR 2 picture stays for them
        if (g_ext->Hiding()) return false;
        static bool shownLogged = false;
        if (!shownLogged && (Ext::IsFsr4Full(m) || m == Ext::Model::Fsr4Lite)) { shownLogged = true; LOGI("Vela FSR 4 VK: %s picture shown (the game's FSR 2 covered its first frames)", Ext::ModelName(m)); }
        return true;
    }
    LOGI("vela: %s frame failed -- the game's FSR 2 from now on for this size", Ext::ModelName(m));
    Retire(g_ext);
    g_extFailed = key;
    return false;
#endif
}

// Vela's frame into our RGBA16F texture (slot 0 Vela AI, 1 Vela SR, 2 FSR 4 / FSR 3.1); the input wrapped if HDR wrap is on
bool RunVela(ID3D12GraphicsCommandList* cl, int algo, const PerFrame& f, ID3D12Resource*& outTex)
{
    if (!f.cpu)
    {
        // the per-frame fields are copied from the game's constant buffer on the GPU: without a known, live owner
        // (freed / recreated on a level load) Vela must not run -- FSR 2 takes the frame
        UINT64 off = 0;
        if (!g_buf || !g_buf(f.cbVA, off)) return false;
    }
    if (!g_hdrTried) { g_hdrTried = true; g_hdr.Init(g_dev); if (!g_hdr.Ready()) { LOGI("vela: no HDR wrap -- Vela takes the input as is"); g_wrap = false; } }
    auto cd = D(g_fr.color);
    ID3D12Resource* color = g_fr.color;
    D3D12_RESOURCE_STATES cs = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (g_wrap)
    {
        ID3D12Resource* tmIn = OwnTex((uint32_t) cd.Width, cd.Height, 10);
        if (!tmIn || !g_hdr.Run(cl, g_fr.color, cs, tmIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, (uint32_t) cd.Width,
                                cd.Height, HdrWrap::Forward))
            return false;
        color = tmIn;
        cs = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    outTex = OwnTex(g_cal.dw, g_cal.dh, algo);
    if (!outTex) return false;
    return algo == 1 ? RunSr(cl, color, cs, outTex, f) : algo == 2 ? RunExt(cl, color, cs, outTex, f) : RunAi(cl, color, cs, outTex, f);
}

// our texture -> the game's target (inverse tonemap when wrapped, else a copy; any target format)
bool Deliver(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst)
{
    if (!g_hdr.Ready()) return false;
    auto d = D(dst);
    return g_hdr.Run(cl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     (uint32_t) d.Width, d.Height, g_wrap ? HdrWrap::Inverse : HdrWrap::Copy);
}

// ---- shadow captures: crops of several consecutive frames ----------------------------------------------------
int g_capReq = -1, g_capLeft = 0, g_capFrame = 0, g_capIndex = 0;
D3D12_BOX g_capBox{}; // display pixels
struct CapItem
{
    ID3D12Resource* rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    uint32_t w, h, rowBytes;
    DXGI_FORMAT fmt;
    std::string name;
};
struct Cap
{
    std::vector<CapItem> items;
    ULONGLONG at = 0;
    std::string base;
};
std::vector<Cap> g_caps;
Cap g_cur;
bool g_capThisFrame = false;
ID3D12Resource* g_vela[2] = {}; // this frame's Vela outputs (shadow)
ID3D12Resource* g_rcasOut = nullptr;
bool g_rcasLogged = false;

void Grab(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, const char* name, bool renderRes)
{
    if (!src) return;
    auto d = D(src);
    D3D12_BOX b = g_capBox;
    if (renderRes)
    {
        const float sx = (float) g_cal.rw / g_cal.dw, sy = (float) g_cal.rh / g_cal.dh;
        b.left = (UINT) (b.left * sx);
        b.top = (UINT) (b.top * sy);
        b.right = (UINT) (b.right * sx);
        b.bottom = (UINT) (b.bottom * sy);
    }
    if (d.Width == 1 && d.Height == 1) b = D3D12_BOX{0, 0, 0, 1, 1, 1};
    b.right = (std::min)(b.right, (UINT) d.Width);
    b.bottom = (std::min)(b.bottom, d.Height);
    if (b.right <= b.left || b.bottom <= b.top) return;
    D3D12_RESOURCE_DESC cd = d;
    cd.Width = b.right - b.left;
    cd.Height = b.bottom - b.top;
    cd.MipLevels = 1;
    cd.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    g_dev->GetCopyableFootprints(&cd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    CapItem it{MakeBuf(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST), fp, (uint32_t) cd.Width,
               cd.Height, (uint32_t) rowBytes, d.Format, name};
    if (!it.rb) return;
    D3D12_TEXTURE_COPY_LOCATION dst{}, s{};
    dst.pResource = it.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &s, &b);
    g_cur.items.push_back(it);
}

void GrabBytes(ID3D12GraphicsCommandList* cl, UINT64 va, const char* name)
{
    UINT64 off = 0;
    ID3D12Resource* owner = g_buf ? g_buf(va, off) : nullptr;
    if (!owner) return;
    CapItem it{MakeBuf(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST), {}, 64, 1, 64,
               DXGI_FORMAT_UNKNOWN, name};
    if (!it.rb) return;
    it.fp.Footprint.RowPitch = 64;
    UINT64 n = D(owner).Width - off;
    cl->CopyBufferRegion(it.rb, 0, owner, off, n < 64 ? n : 64);
    g_cur.items.push_back(it);
}

void FlushCaptures()
{
    for (auto it = g_caps.begin(); it != g_caps.end();)
    {
        if (GetTickCount64() < it->at) { ++it; continue; }
        CreateDirectoryA("C:\\sgsr3intercept", nullptr);
        CreateDirectoryA("C:\\sgsr3intercept\\vela", nullptr);
        char js[600];
        snprintf(js, sizeof(js),
                 "{\"render\": [%d, %d], \"display\": [%d, %d], \"crop\": [%u, %u, %u, %u], \"layout\": \"%s\", "
                 "\"wrap\": %d, \"rcas\": %d, \"images\": {",
                 g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, g_capBox.left, g_capBox.top, g_capBox.right - g_capBox.left,
                 g_capBox.bottom - g_capBox.top, g_cal.lay ? g_cal.lay->name : "?", g_wrap ? 1 : 0, g_rcas ? 1 : 0);
        std::string j = js;
        bool first = true;
        for (auto& c : it->items)
        {
            uint8_t* p = nullptr;
            if (SUCCEEDED(c.rb->Map(0, nullptr, (void**) &p)) && p)
            {
                if (FILE* f = fopen((it->base + "_" + c.name + ".bin").c_str(), "wb"))
                {
                    for (uint32_t y = 0; y < c.h; y++) fwrite(p + (size_t) y * c.fp.Footprint.RowPitch, 1, c.rowBytes, f);
                    fclose(f);
                }
                c.rb->Unmap(0, nullptr);
                char e[200];
                snprintf(e, sizeof(e), "%s\"%s\": {\"size\": [%u, %u], \"dxgi\": %d, \"texel_bytes\": %u}",
                         first ? "" : ", ", c.name.c_str(), c.w, c.h, (int) c.fmt, c.rowBytes / (c.w ? c.w : 1));
                j += e;
                first = false;
            }
            c.rb->Release();
        }
        j += "}}\n";
        if (FILE* f = fopen((it->base + ".json").c_str(), "w")) { fputs(j.c_str(), f); fclose(f); }
        LOGI("vela: capture written %s (%zu images)", it->base.c_str(), it->items.size());
        it = g_caps.erase(it);
    }
}
// ---- series recorder: the plugin's "record frames" (same protocol and files as the fork's dumper, patch 0007) ------
// The plugin writes /tmp/vela_capture: "N" and the folder as Wine sees it; we poll it every 30 presents, then record
// N frames: fNNNNNN.json + fNNNNNN_<name>.bin (written after its images, by a worker thread), and "done" at the end.
// Inputs at full size (color, depth -- RE4's R32 copy or the depth plane --, mv, exposure 1x1, fsr2_cb = the raw 256
// bytes of cbFSR2 for that frame); results as a crop (VELA_CAP_FULL=1: whole frame): fsr2 = FSR 2's own result
// (before RCAS when RCAS follows), fsr2_rcas, vela_ai / vela_sr. To have FSR 2's result as the reference, the frames
// of a series run as in shadow mode (FSR 2 is what the player sees for those ~2 s), after 12 warm-up frames so FSR's
// history is valid; Vela renders the same inputs into its own textures.
const char* kSerTrigger = "Z:\\tmp\\vela_capture";
const int kSerWarm = 12;
struct SerItem
{
    ID3D12Resource* rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    uint32_t w, h, rowBytes, texel, ox, oy;
    DXGI_FORMAT fmt;
    std::string name;
};
struct SerFrame
{
    std::vector<SerItem> items;
    long frame = 0;
    int presents = 0; // flushed 3 presents later (the GPU copies are done by then)
    bool last = false;
};
struct Ser
{
    bool active = false;
    int warm = 0, left = 0, total = 0, polls = 0;
    std::string dir;
    bool full = false;
    SerFrame cur;
    bool thisFrame = false, record = false; // this frame runs as shadow / is recorded
    std::vector<SerFrame> pending;
} g_ser;

// writer thread (the game only maps and packs rows)
std::mutex g_wm;
std::condition_variable g_wcv;
std::deque<std::pair<std::string, std::vector<uint8_t>>> g_wq;
size_t g_wqBytes = 0;
std::thread g_writer;
void WriterLoop()
{
    for (;;)
    {
        std::pair<std::string, std::vector<uint8_t>> job;
        {
            std::unique_lock<std::mutex> lk(g_wm);
            g_wcv.wait(lk, [] { return !g_wq.empty(); });
            job = std::move(g_wq.front());
            g_wq.pop_front();
        }
        if (FILE* f = fopen(job.first.c_str(), "wb"))
        {
            if (!job.second.empty()) fwrite(job.second.data(), 1, job.second.size(), f);
            fclose(f);
        }
        else
            LOGI("vela: capture: cannot write %s", job.first.c_str());
        {
            std::lock_guard<std::mutex> lk(g_wm);
            g_wqBytes -= job.second.size();
        }
        g_wcv.notify_all();
    }
}
void Push(std::string path, std::vector<uint8_t> data)
{
    if (!g_writer.joinable()) { g_writer = std::thread(WriterLoop); g_writer.detach(); }
    std::unique_lock<std::mutex> lk(g_wm);
    g_wcv.wait(lk, [] { return g_wqBytes < (512u << 20); }); // the game waits only if the disk falls far behind
    g_wqBytes += data.size();
    g_wq.emplace_back(std::move(path), std::move(data));
    g_wcv.notify_all();
}

uint32_t TexelOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: return 1;
    default: return 4;
    }
}

// a copy of `src` (whole, or the series crop for display-size images) into a readback buffer, no barriers (the
// resource is in a shader-readable state here; see the recon notes)
void SerGrab(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, const char* name, bool crop)
{
    if (!src) return;
    auto d = D(src);
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D12_BOX b{0, 0, 0, (UINT) d.Width, d.Height, 1};
    if (crop && !g_ser.full && d.Width > 960)
    {
        const UINT w = 960, h = 540;
        b.left = ((UINT) d.Width - w) / 2;
        b.top = (d.Height - h) / 2;
        b.right = b.left + w;
        b.bottom = b.top + h;
    }
    D3D12_RESOURCE_DESC cd = d;
    cd.Width = b.right - b.left;
    cd.Height = b.bottom - b.top;
    cd.MipLevels = 1;
    cd.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    g_dev->GetCopyableFootprints(&cd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    SerItem it{MakeBuf(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST), fp, (uint32_t) cd.Width,
               cd.Height, 0, TexelOf(fp.Footprint.Format), b.left, b.top, d.Format, name};
    if (!it.rb) return;
    it.rowBytes = it.w * it.texel;
    D3D12_TEXTURE_COPY_LOCATION dst{}, s{};
    dst.pResource = it.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &s, &b);
    g_ser.cur.items.push_back(it);
}
void SerGrabCb(ID3D12GraphicsCommandList* cl, UINT64 va, const uint8_t* cpu, uint32_t cpuSize)
{
    SerItem it{MakeBuf(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST), {}, 256, 1, 256, 1, 0, 0,
               DXGI_FORMAT_UNKNOWN, "fsr2_cb"};
    if (!it.rb) return;
    it.fp.Footprint.RowPitch = 256;
    if (cpu) // root constants: the bytes are here already
    {
        uint8_t* m = nullptr;
        if (SUCCEEDED(it.rb->Map(0, nullptr, (void**) &m)) && m)
        {
            memset(m, 0, 256);
            memcpy(m, cpu, cpuSize < 256 ? cpuSize : 256);
            it.rb->Unmap(0, nullptr);
        }
    }
    else
    {
        UINT64 off = 0;
        ID3D12Resource* owner = g_buf ? g_buf(va, off) : nullptr;
        if (!owner) { it.rb->Release(); return; }
        UINT64 n = D(owner).Width - off;
        cl->CopyBufferRegion(it.rb, 0, owner, off, n < 256 ? n : 256);
    }
    g_ser.cur.items.push_back(it);
}

void SerPoll()
{
    if (g_ser.active || ++g_ser.polls < 30) return;
    g_ser.polls = 0;
    static char trig[MAX_PATH] = {};
    if (!trig[0] && !GetEnvironmentVariableA("VELA_CAP_TRIGGER", trig, MAX_PATH)) strcpy(trig, kSerTrigger); // PC tests
    if (GetFileAttributesA(trig) == INVALID_FILE_ATTRIBUTES) return;
    int n = 32;
    std::string dir;
    if (FILE* f = fopen(trig, "r"))
    {
        int k = 0;
        if (fscanf(f, "%d", &k) == 1 && k > 0) n = k < 64 ? k : 64;
        char line[512] = {};
        if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f))
        {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
            dir = line;
        }
        fclose(f);
    }
    DeleteFileA(trig);
    if (dir.empty() || GetFileAttributesA(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LOGI("vela: capture: no folder '%s', nothing recorded", dir.c_str());
        return;
    }
    char e[4] = {};
    g_ser.full = GetEnvironmentVariableA("VELA_CAP_FULL", e, 4) > 0 && e[0] == '1';
    g_ser.active = true;
    g_ser.warm = kSerWarm;
    g_ser.left = g_ser.total = n;
    g_ser.dir = dir;
    LOGI("vela: capture: %d frames into %s (after %d warm-up frames with FSR 2 as reference; results %s)", n, dir.c_str(),
         kSerWarm, g_ser.full ? "full" : "cropped 960x540 at the centre");
}

// a frame of the series starts (first FSR pass of the frame)
void SerFrameStart()
{
    g_ser.thisFrame = g_ser.active && g_cal.ok;
    g_ser.record = g_ser.thisFrame && g_ser.warm <= 0 && g_ser.left > 0;
    if (g_ser.thisFrame && g_ser.warm > 0) g_ser.warm--;
    g_ser.cur = SerFrame{};
}

void SerFlush(bool all)
{
    for (auto it = g_ser.pending.begin(); it != g_ser.pending.end();)
    {
        if (!all && ++it->presents < 3) { ++it; continue; }
        float jx = 0, jy = 0, pe = 1;
        std::string js, imgs;
        for (auto& c : it->items)
        {
            uint8_t* p = nullptr;
            if (FAILED(c.rb->Map(0, nullptr, (void**) &p)) || !p) { c.rb->Release(); continue; }
            std::vector<uint8_t> data((size_t) c.rowBytes * c.h);
            for (uint32_t y = 0; y < c.h; y++) memcpy(data.data() + (size_t) y * c.rowBytes, p + (size_t) y * c.fp.Footprint.RowPitch, c.rowBytes);
            if (c.name == "fsr2_cb" && g_cal.lay)
            {
                jx = Fat(data.data(), g_cal.lay->jitter);
                jy = Fat(data.data(), g_cal.lay->jitter + 4);
                pe = Fat(data.data(), g_cal.lay->preExp);
            }
            c.rb->Unmap(0, nullptr);
            c.rb->Release();
            char e[300];
            snprintf(e, sizeof(e), "%s\n    \"%s\": {\"size\": [%u, %u], \"dxgi\": %d, \"plane_dxgi\": %d, \"texel_bytes\": %u, "
                     "\"origin\": [%u, %u]}", imgs.empty() ? "" : ",", c.name.c_str(), c.w, c.h, (int) c.fmt,
                     (int) c.fp.Footprint.Format, c.texel, c.ox, c.oy);
            imgs += e;
            char path[600];
            snprintf(path, sizeof(path), "%s\\f%06ld_%s.bin", g_ser.dir.c_str(), it->frame, c.name.c_str());
            Push(path, std::move(data));
        }
        char head[1500];
        snprintf(head, sizeof(head),
                 "{\n  \"frame\": %ld,\n  \"render\": [%d, %d],\n  \"display\": [%d, %d],\n  \"jitter\": [%.9g, %.9g],\n"
                 "  \"mv_scale\": [%.9g, %.9g],\n  \"pre_exposure\": %.9g,\n  \"depth_inverted\": %d,\n  \"jittered_mv\": 0,\n"
                 "  \"low_res_mv\": 1,\n  \"auto_exposure\": 1,\n  \"hdr\": 1,\n  \"reset\": 0,\n  \"has_depth\": 1,\n"
                 "  \"mv_base\": [0, 0],\n  \"color_base\": [0, 0],\n  \"source\": \"interceptor static FSR 2 recorder\",\n"
                 "  \"fsr2_layout\": \"%s\",\n  \"mv_units\": \"screen fractions x mv_scale / render (measured: RE4 1.0)\",\n"
                 "  \"vela\": {\"algo\": \"%s\", \"wrap\": %d, \"rcas\": %d},\n  \"images\": {",
                 it->frame, g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh, jx, jy, g_cal.mvScale[0] * g_cal.rw,
                 g_cal.mvScale[1] * g_cal.rh, pe, g_depthInv ? 1 : 0, g_cal.lay ? g_cal.lay->name : "?",
                 g_algo == 1 ? "sr" : g_algo == 2 ? "both" : "ai", g_wrap ? 1 : 0, g_rcas ? 1 : 0);
        js = std::string(head) + imgs + "\n  }\n}\n";
        char path[600];
        snprintf(path, sizeof(path), "%s\\f%06ld.json", g_ser.dir.c_str(), it->frame); // after its images
        Push(path, std::vector<uint8_t>(js.begin(), js.end()));
        if (it->last)
        {
            Push(g_ser.dir + "\\done", {});
            LOGI("vela: capture: %d frames written to %s", g_ser.total, g_ser.dir.c_str());
        }
        it = g_ser.pending.erase(it);
    }
}
} // namespace

void VelaInit(ID3D12Device* dev, BufLookup lookup)
{
    g_dev = dev;
    g_buf = lookup;
    char v[32] = {};
    if (GetEnvironmentVariableA("VELA_FSR2", v, sizeof(v)))
        g_mode = !strcmp(v, "replace") ? 1 : !strcmp(v, "shadow") ? 2 : 0;
    if (GetEnvironmentVariableA("VELA_FSR2_ALGO", v, sizeof(v)))
    {
        g_algo = !strcmp(v, "sr") ? 1 : !strcmp(v, "both") ? 2 : 0;
        g_envExt = !strcmp(v, "fsr4") ? Ext::Model::Fsr4 : !strcmp(v, "fsr4fast") ? Ext::Model::Fsr4Fast : !strcmp(v, "fsr4lite") ? Ext::Model::Fsr4Lite
                 : !strcmp(v, "fsr3") ? Ext::Model::Fsr3 : Ext::Model::Unset;
    }
    if (GetEnvironmentVariableA("VELA_HDRWRAP", v, sizeof(v))) g_wrap = v[0] != '0';
    if (GetEnvironmentVariableA("VELA_RCAS", v, sizeof(v))) g_rcas = v[0] != '0';
    if (GetEnvironmentVariableA("VELA_DEPTH_INV", v, sizeof(v))) g_depthInv = v[0] != '0';
    if (GetEnvironmentVariableA("VELA_MVSCALE", v, sizeof(v))) g_mvOverride = (float) atof(v);
#ifndef VELA_NO_VULKAN
    if (g_mode && FAILED(dev->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), (void**) &g_interop)))
    {
        LOGI("vela: no vkd3d interop -- FSR stays");
        g_mode = 0;
    }
#else
    LOGI("vela: PC build (no vkd3d interop): roles, calibration and the series recorder only");
#endif
    if (g_mode == 1) Ext::PollLive(true); // logs the live file and what it asks for
    LOGI("vela: mode %s, algo %s, hdr wrap %s, game RCAS %s", g_mode == 1 ? "replace" : g_mode == 2 ? "shadow" : "off",
         g_algo == 1 ? "Vela SR" : g_algo == 2 ? "both" : "Vela AI", g_wrap ? "on" : "off", g_rcas ? "kept" : "skipped");
}

bool VelaEnabled() { return g_mode != 0; }

void VelaRequestCapture(int index) { g_capReq = index; }

bool VelaOnFsrPass(ID3D12GraphicsCommandList* cl, const std::string& hash, bool accumulate,
                   const std::vector<VelaTex>& tex, UINT64 cbVA, const uint8_t* cpuCb, uint32_t cpuCbSize)
{
    if (!g_mode) return true;
    FinishGpuCalibration();
    if (!accumulate)
    {
        if (g_newFrame)
        {
            g_newFrame = false;
            g_srvPrev.swap(g_srvCur); // what the last frame's FSR passes read
            g_srvCur.clear();
            g_fr = Frame{};
            SerFrameStart();
            {
                // the frame's model: the panel's live file, else the launch options (VELA_FSR2_ALGO)
                const Ext::Live& lv = Ext::PollLive();
                // fsr4 / fsr4lite / fsr3 / sgsr2; no model given: fsr4; Vela AI / SR are not offered
                Ext::Model m = lv.model != Ext::Model::Unset ? lv.model : g_envExt != Ext::Model::Unset ? g_envExt
                             : Ext::Model::Fsr4;
                if (m == Ext::Model::Ai || m == Ext::Model::Sr) m = Ext::Model::Fsr2;
                // a model that could not be created at these sizes, or a jitter that is not FSR 2's: the game's FSR 2
                int lossy = 1, npu = 0;
                if (Ext::IsExt(m) && g_cal.ok && (ExtKey(m, lossy, npu) == g_extFailed || g_jit.Hopeless()))
                {
                    static bool o = false;
                    if (!o && g_jit.Hopeless()) { o = true; LOGI("vela: the game's jitter is not FSR 2's Halton sequence -- %s cannot run, the game's FSR 2 runs", Ext::ModelName(m)); }
                    m = Ext::Model::Fsr2;
                }
                if (m != g_frameModel) g_reset = true;
                g_frameModel = m;
            }
            // FSR 4 / FSR 3.1 take the jitter on the CPU: with a GPU-made cbFSR2, FSR 2 draws until its phase is locked
            const bool extReady = !Ext::IsExt(g_frameModel) || g_lastCpu || g_jit.Locked();
            g_skipping = g_mode == 1 && g_cal.ok && g_lastReplaceable && !g_ser.thisFrame && extReady &&
                         Ext::IsExt(g_frameModel);
            g_replacedThisFrame = false;
            if (g_capReq >= 0 && g_mode == 2 && g_cal.ok && !g_capLeft)
            {
                char n[16] = {};
                g_capLeft = GetEnvironmentVariableA("VELA_CAP_FRAMES", n, sizeof(n)) ? atoi(n) : 8;
                g_capFrame = 0;
                g_capIndex = g_capReq;
                char r[64] = {};
                UINT x = g_cal.dw / 2 - 320, y = g_cal.dh / 2 - 180, w = 640, h = 360;
                if (GetEnvironmentVariableA("VELA_CAP_RECT", r, sizeof(r))) sscanf(r, "%u,%u,%u,%u", &x, &y, &w, &h);
                g_capBox = D3D12_BOX{x, y, 0, x + w, y + h, 1};
                g_capReq = -1;
                LOGI("vela: shadow capture s%02d: %d frames, crop %u,%u %ux%u", g_capIndex, g_capLeft, x, y, w, h);
            }
            g_capThisFrame = g_capLeft > 0;
        }
        for (auto& t : tex)
            if (t.res && t.kind == 1) g_srvCur.insert(t.res);
        bool writesExposure = false, writesR32F = false;
        ID3D12Resource *ds = nullptr, *r32 = nullptr;
        for (auto& t : tex)
        {
            if (!t.res) continue;
            auto d = D(t.res);
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;
            if (t.kind == 1 && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ds = t.res;
            if (t.kind == 1 && !g_fr.color && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && IsColorFmt(d.Format) &&
                d.Width > 1)
                g_fr.color = t.res;
            if (t.kind == 1 && !g_fr.mv && d.Format == DXGI_FORMAT_R16G16_FLOAT) g_fr.mv = t.res;
            if (t.kind == 2 && d.Width == 1 && d.Height == 1 && d.Format == DXGI_FORMAT_R32G32_FLOAT) writesExposure = true;
            if (t.kind == 2 && d.Format == DXGI_FORMAT_R32_FLOAT && d.Width > 1) { writesR32F = true; r32 = t.res; }
        }
        bool depthPrep = false;
        if (ds)
        {
            if (writesR32F) { g_fr.depth = r32; g_fr.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; depthPrep = true; }
            else if (!g_fr.depth)
            {
                g_fr.depth = ds;
                g_fr.depthState = (D3D12_RESOURCE_STATES) (D3D12_RESOURCE_STATE_DEPTH_READ |
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
        // exposure (RCAS reads it) and the game's own depth preparation always run
        const bool run = writesExposure || depthPrep || !g_skipping || !g_skipSeq;
        if (!run)
        {
            const uint64_t n = ++g_skipCount[hash];
            static std::set<std::string> seen;
            const std::string key = hash + (ds ? " ds" : " -") + (writesR32F ? " r32f" : " -");
            if (seen.insert(key).second)
            {
                std::string b;
                for (auto& t : tex)
                    if (t.res)
                    {
                        auto d = D(t.res);
                        char x[64];
                        snprintf(x, sizeof(x), " %c%u %llux%u f%d", t.kind == 2 ? 'u' : 't', t.reg,
                                 (unsigned long long) d.Width, d.Height, (int) d.Format);
                        b += x;
                    }
                LOGI("vela: SKIP pso %s (reads depth-stencil %d, writes R32F %d; skip #%llu):%s", hash.c_str(), ds ? 1 : 0,
                     writesR32F ? 1 : 0, (unsigned long long) n, b.c_str());
            }
        }
        return run;
    }

    // ---- accumulate --------------------------------------------------------------------------------------------
    g_newFrame = true;
    const uint32_t accN = g_accN++;
    ID3D12Resource *finalOut = nullptr, *history = nullptr, *exposure = nullptr;
    for (auto& t : tex)
        if (t.res && t.kind == 1)
        {
            auto d = D(t.res);
            if (d.Width == 1 && d.Height == 1 && d.Format == DXGI_FORMAT_R32G32_FLOAT) exposure = t.res;
        }
    // Which UAV of the accumulate is what: lock status is read by an FSR pass of this same frame, the history is
    // read back by the next frame (FSR ping-pongs it), the output is read by neither. Flags do not tell: in DRG / the
    // AMD backend the lock status is a render target too. No output among them = the game sharpens with RCAS, which
    // reads the history: then the history is the target.
    for (auto& t : tex)
        if (t.res && t.kind == 1) g_srvCur.insert(t.res);
    for (auto& t : tex)
    {
        if (!t.res || t.kind != 2) continue;
        auto d = D(t.res);
        if (d.Width <= 1 || !IsColorFmt(d.Format) || g_srvCur.count(t.res)) continue; // lock status & co.
        if (g_srvPrev.count(t.res)) { if (!history) history = t.res; }               // FSR's history
        else if (!finalOut || ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
                               !(D(finalOut).Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)))
            finalOut = t.res;                                                          // FSR's output
    }
    if (!history && !finalOut) // the first frames, before a previous frame was seen: the RGBA16F one as history
        for (auto& t : tex)
            if (t.res && t.kind == 2 && !g_srvCur.count(t.res) && D(t.res).Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            { history = t.res; break; }
    if (finalOut && history && finalOut != history) history = nullptr; // an output of its own: no RCAS after it
    g_history = finalOut ? nullptr : history;
    g_target = finalOut ? finalOut : history;
    const bool replaceable = g_target && g_fr.color && g_fr.depth && g_fr.mv;
    {
        static ID3D12Resource *lc = nullptr, *ld = nullptr, *lm = nullptr;
        if (replaceable && (g_fr.color != lc || g_fr.depth != ld || g_fr.mv != lm))
        {
            if (lc) LOGI("vela: the game's input textures changed (level load?) -- Vela history restarts");
            lc = g_fr.color; ld = g_fr.depth; lm = g_fr.mv;
            g_reset = true;
        }
    }

    // render size = the colour texture's size: calibrate once it has been the same for 60 frames
    if (g_fr.color)
    {
        auto cd = D(g_fr.color);
        if ((uint32_t) cd.Width == g_texW && cd.Height == g_texH) g_stableN++;
        else
        {
            g_texW = (uint32_t) cd.Width;
            g_texH = cd.Height;
            g_stableN = 0;
            if (g_cal.ok)
            {
                LOGI("vela: colour texture now %ux%u -- FSR until recalibrated", g_texW, g_texH);
                g_cal = Calib{};
                g_reset = true;
            }
        }
    }
    PerFrame pf;
    pf.cbVA = cbVA;
    pf.accN = accN;
    if (!g_cal.ok)
    {
        if (g_stableN >= 60)
        {
            if (cpuCb) Calibrate(cpuCb, cpuCbSize, g_texW, g_texH);
            else StartGpuCalibration(cl, cbVA);
        }
        g_lastReplaceable = false;
        return true;
    }
    if (cpuCb && cpuCbSize >= g_cal.lay->phases + 4)
    {
        pf.cpu = cpuCb;
        pf.jitter[0] = Fat(cpuCb, g_cal.lay->jitter);
        pf.jitter[1] = Fat(cpuCb, g_cal.lay->jitter + 4);
        pf.preExp = Fat(cpuCb, g_cal.lay->preExp);
    }
    else
        pf.preExp = g_cal.preExp; // patched on the GPU (jitter; preExposure when not wrapped)
    g_lastCpu = pf.cpu != nullptr;
    if (g_mode == 1 && Ext::IsExt(g_frameModel) && !pf.cpu && cbVA && g_buf && g_cal.phases > 0)
    {
        UINT64 off = 0;
        if (ID3D12Resource* owner = g_buf(cbVA, off)) g_jit.Record(g_dev, cl, owner, off + g_cal.lay->jitter, accN, g_cal.phases);
    }
    {
        static bool logged = false;
        if (!logged && replaceable)
        {
            logged = true;
            LOGI("vela: inputs colour %p depth %p (%s) mv %p -> %s %p (accumulate %s; constants %s)", (void*) g_fr.color,
                 (void*) g_fr.depth, g_fr.depthState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ? "game's R32 copy" : "depth buffer",
                 (void*) g_fr.mv, finalOut ? "FSR output" : "FSR history (RCAS follows)", (void*) g_target, hash.c_str(),
                 cpuCb ? "root constants" : "GPU buffer");
        }
    }

    bool skipFsr = false;
    if (g_mode == 1 && !g_ser.thisFrame)
    {
        if (g_skipping && replaceable)
        {
            ID3D12Resource* out = nullptr;
            // only the chosen model; when it cannot run this frame the game's FSR 2 does (no Vela AI)
            bool ok = RunVela(cl, 2, pf, out);
            const Ext::Model ran = g_frameModel;
            if (ok && ran != g_ranModel)
            {
                g_ranModel = ran; // the plugin's status: the last "model ... runs" line (py_modules/sgsr3.py)
                LOGI("vela: model %s runs %dx%d -> %dx%d", Ext::ModelName(ran), g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh);
            }
            ok = ok && Deliver(cl, out, g_target);
            if (ok)
            {
                skipFsr = true;
                g_replacedThisFrame = true;
                g_replaced++;
                g_reset = false;
            }
            else
            {
                static uint32_t o = 0;
                if (o++ < 3) LOGI("vela: %s did not run this frame -- the game's FSR 2 runs", Ext::ModelName(g_frameModel));
                g_lastReplaceable = false;
                g_reset = true;
                return true;
            }
        }
        else if (!g_skipping)
        {
            g_reset = true;
            if (g_frameModel == Ext::Model::Fsr2 && g_ranModel != Ext::Model::Fsr2 && g_cal.ok)
            {
                g_ranModel = Ext::Model::Fsr2; // the plugin's status line (py_modules/sgsr3.py _RX_SRUNS)
                LOGI("vela: model fsr2 runs %dx%d -> %dx%d (the game's own FSR 2)", g_cal.rw, g_cal.rh, g_cal.dw, g_cal.dh);
            }
        }
    }
    else if ((g_mode == 2 || g_ser.thisFrame) && replaceable)
    {
        g_vela[0] = g_vela[1] = nullptr;
        // keep only what Vela actually produced this frame (a failed run leaves its texture stale)
        if (g_algo != 1 && !RunVela(cl, 0, pf, g_vela[0])) g_vela[0] = nullptr;
        if (g_algo != 0 && !RunVela(cl, 1, pf, g_vela[1])) g_vela[1] = nullptr;
        if (g_wrap) // our textures hold the wrapped frame: bring them back to linear for the comparison
            for (int k = 0; k < 2; k++)
                if (g_vela[k])
                {
                    ID3D12Resource* lin = OwnTex(g_cal.dw, g_cal.dh, 20 + k);
                    if (lin && Deliver(cl, g_vela[k], lin)) g_vela[k] = lin;
                }
        g_reset = false;
        if (g_capThisFrame)
        {
            char b[96];
            snprintf(b, sizeof(b), "C:\\sgsr3intercept\\vela\\s%02d_f%02d", g_capIndex, g_capFrame);
            g_cur = Cap{};
            g_cur.base = b;
            Grab(cl, g_fr.color, "color", true);
            Grab(cl, g_fr.mv, "mv", true);
            if (exposure) Grab(cl, exposure, "exposure", false);
            if (g_vela[0]) Grab(cl, g_vela[0], "vela_ai", false);
            if (g_vela[1]) Grab(cl, g_vela[1], "vela_sr", false);
            if (g_capFrame == 0 && cbVA) GrabBytes(cl, cbVA, "cbfsr2");
        }
        if (g_ser.record)
        {
            g_ser.cur.frame = (long) g_frames;
            SerGrab(cl, g_fr.color, "color", false);
            SerGrab(cl, g_fr.depth, "depth", false);
            SerGrab(cl, g_fr.mv, "mv", false);
            if (exposure) SerGrab(cl, exposure, "exposure", false);
            SerGrabCb(cl, cbVA, pf.cpu, cpuCbSize);
            if (g_vela[0]) SerGrab(cl, g_vela[0], "vela_ai", true);
            if (g_vela[1]) SerGrab(cl, g_vela[1], "vela_sr", true);
        }
    }
    g_lastReplaceable = replaceable;
    g_frames++;
    return !skipFsr;
}

void VelaAfterFsrPass(ID3D12GraphicsCommandList* cl, bool accumulate)
{
    if (accumulate && g_ser.record) SerGrab(cl, g_target, "fsr2", true); // FSR 2's own result (before RCAS if any)
    if (g_mode != 2 || !accumulate || !g_capThisFrame) return;
    Grab(cl, g_target, "fsr2", false); // FSR's own result of this frame (before RCAS when RCAS follows)
}

bool VelaOnPostPass(ID3D12GraphicsCommandList* cl, const std::vector<VelaTex>& tex, UINT64 cbVA)
{
    if (!g_mode || !g_history || g_target != g_history) return true;
    bool readsHistory = false;
    ID3D12Resource* out = nullptr;
    for (auto& t : tex)
    {
        if (!t.res) continue;
        if (t.kind == 1 && t.res == g_history) readsHistory = true;
        if (t.kind == 2 && !out && D(t.res).Width > 1) out = t.res;
    }
    if (!readsHistory || !out) return true;
    g_rcasOut = out;
    if (!g_rcasLogged)
    {
        g_rcasLogged = true;
        auto d = D(out);
        LOGI("vela: RCAS after the accumulate: output %llux%u fmt %d", (unsigned long long) d.Width, d.Height, (int) d.Format);
    }
    if (g_mode == 2 && g_capThisFrame && g_capFrame == 0 && cbVA) GrabBytes(cl, cbVA, "cbrcas");
    if (g_mode == 1 && !g_rcas && g_replacedThisFrame)
    {
        if (g_hdr.Run(cl, g_history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      (uint32_t) D(out).Width, D(out).Height, HdrWrap::Copy))
            return false; // our frame straight into the game's output, without RCAS
    }
    return true;
}

void VelaAfterPostPass(ID3D12GraphicsCommandList* cl)
{
    if (g_ser.record && g_rcasOut) SerGrab(cl, g_rcasOut, "fsr2_rcas", true);
    if (g_mode != 2 || !g_capThisFrame) return;
    if (g_rcasOut) Grab(cl, g_rcasOut, "fsr2_rcas", false);
}

static void FaultCycle()
{
    if (g_cycleSec < 0) return;
    if (g_cycleSec == 0)
    {
        char e[16] = {};
        g_cycleSec = GetEnvironmentVariableA("VELA_FAULT_CYCLE", e, sizeof(e)) ? atoi(e) : -1;
        if (g_cycleSec <= 0 || !g_mode)
        {
            g_cycleSec = -1;
            return;
        }
        g_cycleT0 = GetTickCount64();
    }
    static const char* names[5] = {"A hooks only (off)", "B shadow (Vela runs, nothing skipped)",
                                    "C replace, only the accumulate skipped", "D replace as shipped",
                                    "E replace without the GPU jitter copy"};
    const int ph = (int) (((GetTickCount64() - g_cycleT0) / (1000ull * g_cycleSec)) % 5);
    if (ph == g_phase) return;
    if (g_phase >= 0)
    {
        std::string l;
        for (auto& kv : g_skipCount)
            l += " " + kv.first.substr(0, 8) + " x" + std::to_string(kv.second);
        LOGI("vela: FAULTCYCLE end of phase %c: skipped dispatches:%s", 'A' + g_phase, l.empty() ? " none" : l.c_str());
    }
    g_skipCount.clear();
    g_phase = ph;
    g_mode = ph == 0 ? 0 : ph == 1 ? 2 : 1;
    g_skipSeq = ph != 2;
    g_noPatch = ph == 4;
    g_reset = true;
    SYSTEMTIME t;
    GetLocalTime(&t);
    LOGI("vela: FAULTCYCLE phase %s from %02d:%02d:%02d (local), %d s", names[ph], t.wHour, t.wMinute, t.wSecond,
         g_cycleSec);
}

void VelaOnPresent()
{
    g_presents++;
    ReleaseRetired();
    FaultCycle();
    // series recorder: the frame's copies are queued at its present, written 3 presents later
    if (g_ser.record && !g_ser.cur.items.empty())
    {
        g_ser.cur.last = --g_ser.left <= 0;
        g_ser.pending.push_back(std::move(g_ser.cur));
        g_ser.cur = SerFrame{};
        if (g_ser.left <= 0) g_ser.active = false;
    }
    g_ser.record = false;
    SerFlush(false);
    SerPoll();
    // a capture frame ends at the present: queue it
    if (g_capThisFrame && !g_cur.items.empty())
    {
        g_cur.at = GetTickCount64() + 2000;
        g_caps.push_back(g_cur);
        g_cur = Cap{};
        g_capFrame++;
        if (--g_capLeft <= 0) LOGI("vela: shadow capture s%02d queued (%d frames)", g_capIndex, g_capFrame);
        g_capThisFrame = false;
    }
    FlushCaptures();
    FinishGpuCalibration();
    ULONGLONG now = GetTickCount64();
    if (!g_fpsT0) g_fpsT0 = now;
    if (now - g_fpsT0 >= 10000)
    {
        const double s = (now - g_fpsT0) / 1000.0;
        std::string tim;
        double t[4];
        // the model that ran in this window, with its GPU time when measured (FSR 4 / Lite: timestamps)
        if (g_mode == 1 && g_ranModel != Ext::Model::Unset)
        {
            char b[96];
            double ms = 0;
            if (g_ext && g_ext->model == g_ranModel && g_ext->LastGpuMs(ms))
                snprintf(b, sizeof(b), ", %s GPU %.2f ms", Ext::ModelLabel(g_ranModel), ms);
            else
                snprintf(b, sizeof(b), ", %s", Ext::ModelLabel(g_ranModel));
            tim += b;
        }
        for (auto& kv : g_ups)
        {
            if (g_mode == 1) break; // Vela AI / SR objects run only in shadow mode / the series recorder now
            char b[80];
            if (kv.second.ai && kv.second.ai->LastTiming(t))
            {
                snprintf(b, sizeof(b), ", Vela AI GPU %.2f ms", t[0] + t[1] + t[2] + t[3]);
                tim += b;
            }
            if (kv.second.sr && kv.second.sr->LastTiming(t))
            {
                snprintf(b, sizeof(b), ", Vela SR GPU %.2f ms", t[0] + t[1]);
                tim += b;
            }
        }
        LOGI("fps %.1f (presents %llu in %.1f s; frames by Vela %llu)%s", g_presents / s,
             (unsigned long long) g_presents, s, (unsigned long long) g_replaced, tim.c_str());
        g_presents = 0;
        g_replaced = 0;
        g_fpsT0 = now;
    }
}

void VelaOnResourceFreed(ID3D12Resource* r)
{
    g_srvPrev.erase(r);
    g_srvCur.erase(r);
    bool hit = false;
    for (ID3D12Resource** p : {&g_fr.color, &g_fr.depth, &g_fr.mv, &g_target, &g_history, &g_rcasOut})
        if (*p == r) { *p = nullptr; hit = true; }
    if (hit)
    {
        g_reset = true;
        static int n = 0;
        if (n++ < 8) LOGI("vela: a texture Vela used was freed by the game -- dropped, history restarts");
    }
}
