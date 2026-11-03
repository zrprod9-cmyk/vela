#include "ext_upscalers.h"
#ifndef VELA_NO_VULKAN
#include "interop_vkd3d.h"
#include "fsr4_warm.h"
#endif
#include "log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Ext
{
namespace
{
std::atomic<int> g_own{0};
struct OwnScope
{
    OwnScope() { g_own++; }
    ~OwnScope() { g_own--; }
};

// ---- FFX API subset (FidelityFX SDK headers, MIT): the providers' ABI (as the fork's Fsr4VkBridge.h) ----------------
using ffxStructType_t = uint64_t;
using ffxReturnCode_t = uint32_t;
using ffxContext = void*;
struct ffxApiHeader
{
    ffxStructType_t type;
    ffxApiHeader* pNext;
};
struct Dim2
{
    uint32_t width, height;
};
struct Flt2
{
    float x, y;
};
struct ResDesc
{
    uint32_t type, format, width, height, depth, mipCount, flags, usage;
};
struct FfxRes
{
    void* resource;
    ResDesc description;
    uint32_t state;
};
struct CreateUpscale
{
    ffxApiHeader header;
    uint32_t flags;
    Dim2 maxRenderSize, maxUpscaleSize;
    void (*fpMessage)(uint32_t type, const wchar_t* message);
};
struct DispatchUpscale
{
    ffxApiHeader header;
    void* commandList;
    FfxRes color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    Flt2 jitterOffset, motionVectorScale;
    Dim2 renderSize, upscaleSize;
    bool enableSharpening;
    float sharpness, frameTimeDelta, preExposure;
    bool reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    uint32_t flags;
};
#ifndef VELA_NO_VULKAN
struct BackendVk
{
    ffxApiHeader header;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
};
#endif
struct BackendDx12
{
    ffxApiHeader header;
    ID3D12Device* device;
};
struct OverrideVersion
{
    ffxApiHeader header;
    uint64_t versionId;
};
struct DeviceFeatures // fsr4vk's own create descriptor
{
    ffxApiHeader header;
    uint32_t enabledFlags, apiVersion, queueFamilyIndex;
};
struct VelaLossy
{
    ffxApiHeader header;
    uint32_t networkEvery;
};
struct VelaNpu
{
    ffxApiHeader header;
    uint32_t mode;
};
using PfnCreate = ffxReturnCode_t (*)(ffxContext*, ffxApiHeader*, const void*);
using PfnDestroy = ffxReturnCode_t (*)(ffxContext*, const void*);
using PfnDispatch = ffxReturnCode_t (*)(ffxContext*, const ffxApiHeader*);
constexpr ffxStructType_t kCreateUpscale = 0x00010000u, kDispatchUpscale = 0x00010001u, kBackendDx12 = 2u,
                          kBackendVk = 3u, kOverrideVersion = 5u, kDeviceFeatures = 0x4653523444455643ull,
                          kVelaLossy = 0x56454C414C535931ull, kVelaNpu = 0x56454C414E505531ull;
constexpr uint32_t kDepthInverted = 1u << 3, kAutoExposure = 1u << 5;
constexpr uint32_t kTexture2D = 2u, kUsageUav = 1u << 1;
constexpr uint32_t kStateUav = 1u << 1, kStateComputeRead = 1u << 2;
constexpr uint64_t kFsr411 = (0xF5A5CA1Eull << 32) | ((4ull << 22) | (1ull << 12) | 1ull);
constexpr uint64_t kFsr412Lite = (0xF5A5CA1Eull << 32) | ((4ull << 22) | (1ull << 12) | 2ull);
// FfxApiSurfaceFormat (ffx_api_types.h order)
constexpr uint32_t kRGBA16F = 4, kR11G11B10F = 16, kR10G10B10A2 = 17, kRG16F = 18, kR32F = 28, kR9G9B9E5 = 29,
                   kRGBA8 = 10, kRGBA32F = 3;

std::wstring DllDir()
{
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &DllDir, &h);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(h, p, MAX_PATH);
    std::wstring s(p);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : s.substr(0, k + 1);
}

HMODULE LoadNextToUs(const wchar_t* name)
{
    HMODULE m = GetModuleHandleW(name);
    if (m) return m;
    const std::wstring f = DllDir() + name;
    m = LoadLibraryExW(f.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) LOGI("vela: cannot load %ls (error %lu)", f.c_str(), GetLastError());
    return m;
}

void Message(uint32_t type, const wchar_t* message)
{
    if (!message) return;
    std::string s;
    for (const wchar_t* p = message; *p && s.size() < 400; p++) s += (*p < 128) ? (char) *p : '?';
    if (s.rfind("Vela NPU: ", 0) == 0) LOGI("%s", s.c_str()); // the plugin parses these lines exactly
    else LOGI("vela: FFX provider (%u): %s", type, s.c_str());
}

#ifndef VELA_NO_VULKAN
// ---- FSR 4.1.1 / 4.1.2 Lite: fsr4vk on vkd3d-proton's VkDevice (the fork's Fsr4VkBridge.h, OptiScaler taken out) ---
class Fsr4 : public Upscaler
{
  public:
    bool Init(ID3D12Device* device, bool lite, uint32_t every, uint32_t npuMode)
    {
        _lite = lite;
        _every = lite ? (std::max)(1u, every) : 1u;
        _npu = !lite || _every == 1 ? (std::min)(npuMode, 2u) : 0u;
        if (FAILED(device->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), (void**) &_interop)) || !_interop)
            return Fail("no vkd3d-proton interop");
        VkInstance inst = VK_NULL_HANDLE;
        if (FAILED(_interop->GetVulkanHandles(&inst, &_physical, &_vk)) || _vk == VK_NULL_HANDLE)
            return Fail("GetVulkanHandles failed");
        bool descBuf = false;
        UINT n = 0;
        if (SUCCEEDED(_interop->GetDeviceExtensions(&n, nullptr)) && n > 0)
        {
            std::vector<const char*> ext(n);
            if (SUCCEEDED(_interop->GetDeviceExtensions(&n, ext.data())))
                for (auto* e : ext) descBuf |= e && !strcmp(e, "VK_EXT_descriptor_buffer");
        }
        if (!descBuf) return Fail("vkd3d-proton's device has no VK_EXT_descriptor_buffer, the provider cannot run");
        HMODULE dll = LoadNextToUs(L"amd_fidelityfx_upscaler_vk.dll");
        if (!dll) return Fail("amd_fidelityfx_upscaler_vk.dll not found next to the DLL");
        _create = (PfnCreate) GetProcAddress(dll, "ffxCreateContext");
        _destroy = (PfnDestroy) GetProcAddress(dll, "ffxDestroyContext");
        _dispatch = (PfnDispatch) GetProcAddress(dll, "ffxDispatch");
        if (!_create || !_destroy || !_dispatch) return Fail("the provider has no FFX exports");
        CreateUpscale c{};
        c.header.type = kCreateUpscale;
        c.flags = kDepthInverted | kAutoExposure; // LDR input: the interceptor's reversible tonemap
        c.maxRenderSize = {rw, rh};
        c.maxUpscaleSize = {dw, dh};
        c.fpMessage = &Message;
        BackendVk b{};
        b.header.type = kBackendVk;
        b.device = _vk;
        b.physicalDevice = _physical;
        b.getDeviceProcAddr = vkGetDeviceProcAddr;
        OverrideVersion ov{};
        ov.header.type = kOverrideVersion;
        ov.versionId = _lite ? kFsr412Lite : kFsr411;
        DeviceFeatures df{};
        df.header.type = kDeviceFeatures;
        df.queueFamilyIndex = 0; // Turnip: one queue family
        VelaLossy lo{};
        lo.header.type = kVelaLossy;
        lo.networkEvery = _every;
        VelaNpu np{};
        np.header.type = kVelaNpu;
        np.mode = _npu;
        c.header.pNext = &b.header;
        b.header.pNext = &ov.header;
        ov.header.pNext = &df.header;
        ffxApiHeader* tail = &df.header;
        if (_lite && _every > 1) { tail->pNext = &lo.header; tail = &lo.header; }
        if (_npu) tail->pNext = &np.header;
        // context + first frame (the provider's pipeline build: 6-9 s with a cold Mesa cache, which froze
        // the game) on a worker thread (fsr4_warm.h); the game's FSR 2 runs until Pending() is false.
        // VELA_FSR4_SYNC=1: the previous behaviour
        const char* syncEnv = getenv("VELA_FSR4_SYNC");
        if (syncEnv && syncEnv[0] == '1')
        {
            const auto r = _create(&_ctx, &c.header, nullptr);
            if (r != 0 || !_ctx)
            {
                _ctx = nullptr;
                char why[160];
                snprintf(why, sizeof(why), "ffxCreateContext failed (%u) for %ux%u -> %ux%u", r, rw, rh, dw, dh);
                return Fail(why);
            }
        }
        else
        {
            Fsr4Warm::Params wp;
            wp.device = device;
            wp.interop = _interop;
            wp.vk = _vk;
            wp.physical = _physical;
            wp.create = (Fsr4Warm::PfnCreate) _create;
            wp.destroy = (Fsr4Warm::PfnDestroy) _destroy;
            wp.dispatch = (Fsr4Warm::PfnDispatch) _dispatch;
            wp.message = &Message;
            wp.flags = c.flags;
            wp.rw = rw;
            wp.rh = rh;
            wp.dw = dw;
            wp.dh = dh;
            wp.version = ov.versionId;
            wp.lossyEvery = _every;
            wp.npu = _npu;
            wp.lite = _lite;
            wp.log = [](const std::string& line) { LOGI("%s", line.c_str()); };
            _job = Fsr4Warm::Job::Start(wp);
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto*& f : _flush)
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&f))))
                return Fail("flush buffers failed");
        // one-descriptor shader-visible heaps, set between the provider's work and the game's own heaps
        // so vkd3d-proton binds its descriptor buffers again (see RebindGameHeaps)
        for (int k = 0; k < 2; k++)
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = k ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 1;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_dummyHeap[k]))))
                return Fail("dummy descriptor heaps failed");
        }
        {
            const char* e = getenv("VELA_FSR4_NOREBIND"); // negative control only: the previous behaviour
            _rebind = !(e && e[0] == '1');
        }
        // the plugin parses this line (py_modules/sgsr3.py _RX_FSR4), as the fork's
        LOGI("Vela FSR 4 VK: FSR %s VK INT8 context %ux%u -> %ux%u (hdr 0, network every %u frames)%s",
             _lite ? "4.1.2 Lite" : "4.1.1", rw, rh, dw, dh, _every,
             _job ? "; pipelines build on a worker thread, the game's FSR 2 runs until they are ready" : "");
        if (!_npu && npuMode) LOGI("Vela NPU: off");
        // two timestamps around the provider's work per frame, a ring of 8 (read 7 frames later, no wait)
        {
            VkPhysicalDeviceProperties pr{};
            vkGetPhysicalDeviceProperties(_physical, &pr);
            _tsNs = pr.limits.timestampPeriod;
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 * kTsRing;
            if (vkCreateQueryPool(_vk, &qi, nullptr, &_qp) != VK_SUCCESS) _qp = VK_NULL_HANDLE;
        }
        return true;
    }

    bool Pending() override
    {
        if (!_job) return false;
        const auto st = _job->Poll();
        if (st == Fsr4Warm::Job::Running) return true;
        if (st == Fsr4Warm::Job::Failed)
            LOGI("vela: FSR 4 VK: the worker's context failed (%s) after %.0f ms", _job->Why().c_str(), _job->Ms());
        else
        {
            _ctx = _job->Take();
            LOGI("Vela FSR 4 VK: context ready after %.0f ms on the worker thread (pipelines built, one warm-up frame on a "
                 "private queue); the game's frames go to FSR 4 now", _job->Ms());
        }
        _job.reset();
        return false; // a failed job leaves _ctx null: Evaluate fails, RunExt falls back as before
    }
    // the blue frames seen in Control with HDR: the first frames of a new context stay hidden -- the first one carries the
    // warm-up's black frame through the NPU's one-frame delay, and NPU exact equals the GPU network from its 7th frame
    // stand). VELA_FSR4_HIDE=<n> (default 8, 0 = show at once)
    bool Hiding() const override
    {
        static const uint64_t hide = []
        {
            const char* e = getenv("VELA_FSR4_HIDE");
            return (uint64_t) (e && *e ? (std::max)(0, atoi(e)) : 8);
        }();
        return _frames <= hide;
    }

    bool Evaluate(ID3D12GraphicsCommandList* cl, const Tex& color, const Tex& depth, const Tex& mv, const Tex& output,
                  const Frame& f) override
    {
        if (!_ctx) return false;
        LARGE_INTEGER t0{}, t1{}, qf{};
        if (_frames == 0) QueryPerformanceCounter(&t0);
        Img in[3], out;
        if (!Get(color, in[0], false) || !Get(depth, in[1], true) || !Get(mv, in[2], false) || !Get(output, out, false))
            return false;
        DispatchUpscale d{};
        d.header.type = kDispatchUpscale;
        d.color = R(in[0], rw, rh, kStateComputeRead, 0);
        d.depth = R(in[1], rw, rh, kStateComputeRead, 0);
        d.motionVectors = R(in[2], rw, rh, kStateComputeRead, 0);
        d.output = R(out, dw, dh, kStateUav, kUsageUav);
        if (!d.color.description.format || !d.depth.description.format || !d.motionVectors.description.format ||
            !d.output.description.format)
        {
            if (_failLogged++ < 3)
                LOGI("vela: FSR 4 VK: a format the provider does not take (colour %d depth %d mv %d out %d)", (int) in[0].fmt,
                     (int) in[1].fmt, (int) in[2].fmt, (int) out.fmt);
            return false;
        }
        d.jitterOffset = {f.jitterX, f.jitterY};
        d.motionVectorScale = {f.mvScaleX, f.mvScaleY};
        d.renderSize = {rw, rh};
        d.upscaleSize = {dw, dh};
        d.frameTimeDelta = f.frameTimeMs;
        d.preExposure = f.preExposure > 0 ? f.preExposure : 1.0f;
        d.reset = f.reset || _frames == 0; // the first game frame after the warm-up frame starts a new history
        d.cameraNear = f.cameraNear;
        d.cameraFar = f.cameraFar;
        d.cameraFovAngleVertical = f.fovV;
        cl->CopyBufferRegion(_flush[1], 0, _flush[0], 0, 4); // vkd3d emits its batched barriers before the interop
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (FAILED(_interop->BeginVkCommandBufferInterop(cl, &cb)) || cb == VK_NULL_HANDLE) return false;
        const uint32_t slot = (uint32_t) (_frames % kTsRing);
        if (_qp)
        {
            vkCmdResetQueryPool(cb, _qp, 2 * slot, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _qp, 2 * slot);
        }
        Transition(cb, in, 3, out, true);
        d.commandList = cb;
        const auto r = _dispatch(&_ctx, &d.header);
        Transition(cb, in, 3, out, false);
        if (_qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _qp, 2 * slot + 1);
        _interop->EndVkCommandBufferInterop(cl);
        RebindGameHeaps(cl);
        if (r != 0)
        {
            if (_failLogged++ < 3) LOGI("vela: FSR 4 VK: ffxDispatch failed (%u)", r);
            return false;
        }
        if (_frames++ == 0)
        {
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&qf);
            LOGI("vela: FSR 4 VK: first frame dispatched (colour fmt %d, depth fmt %d, mv fmt %d, out fmt %d); render thread %.1f ms",
                 (int) in[0].fmt, (int) in[1].fmt, (int) in[2].fmt, (int) out.fmt,
                 1000.0 * (double) (t1.QuadPart - t0.QuadPart) / (double) qf.QuadPart);
        }
        return true;
    }

    void Release() override
    {
        if (_job) { _job->Abandon(); _job.reset(); } // the worker destroys its context when it ends
        if (_ctx && _destroy)
        {
            if (_vk) vkDeviceWaitIdle(_vk); // the provider keeps up to eight frames in flight
            _destroy(&_ctx, nullptr);
        }
        _ctx = nullptr;
        if (_qp) { vkDestroyQueryPool(_vk, _qp, nullptr); _qp = VK_NULL_HANDLE; }
        for (auto*& b : _flush)
            if (b) { b->Release(); b = nullptr; }
        for (auto*& h : _dummyHeap)
            if (h) { h->Release(); h = nullptr; }
        if (_interop) { _interop->Release(); _interop = nullptr; }
    }
    ~Fsr4() override { Release(); }
    bool LastGpuMs(double& ms) override
    {
        if (!_qp || _frames < kTsRing) return false;
        const uint32_t slot = (uint32_t) ((_frames + 1) % kTsRing); // the oldest slot: written 7 frames ago
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(_vk, _qp, 2 * slot, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) !=
                VK_SUCCESS || ts[1] <= ts[0])
            return false;
        ms = (double) (ts[1] - ts[0]) * _tsNs / 1e6;
        return true;
    }

    // The RE4 GPU hang: vkd3d-proton (descriptor-buffer path) binds the game's CBV/SRV/UAV + sampler heaps
    // with vkCmdBindDescriptorBuffersEXT only when its global_heap_dirty is set (SetDescriptorHeaps with a different
    // heap, a new command buffer), and per dispatch only sets offsets into them. BeginVkCommandBufferInterop invalidates
    // pipeline/root state only, EndVkCommandBufferInterop does nothing, and fsr4vk binds ITS OWN descriptor buffers
    // (quality-context.hpp / fsr411-recorder.hpp: bind_descriptor_buffers(cb, 3, ...)). RE4's statically linked FSR 2
    // never changes heaps, so every later game draw/dispatch of the list read its descriptors at the game's offsets
    // inside the provider's buffers: garbage / zero descriptors -> TRANSLATION faults (BR_SP, UCHE, flag cache) and the
    // hangcheck. Games that call FSR / DLSS through OptiScaler re-set their heaps after the call, so they never saw it.
    // Fix: SetDescriptorHeaps(dummy) then the game's own heaps: vkd3d skips a same-VA set, so the dummy step makes the
    // second call mark global_heap_dirty, and the next game dispatch re-binds the game's descriptor buffers.
    void RebindGameHeaps(ID3D12GraphicsCommandList* cl)
    {
        if (!_rebind) return;
        ID3D12DescriptorHeap* game[2] = {};
        const UINT n = ListHeaps(cl, game);
        if (n == 0 || !game[0] || (n == 2 && !game[1]))
        {
            if (_heapWarn++ == 0) LOGI("vela: FSR 4 VK: no descriptor heaps recorded on the list; heaps not re-bound");
            return;
        }
        ID3D12DescriptorHeap* dummy[2] = {};
        for (UINT k = 0; k < n; k++)
            dummy[k] = _dummyHeap[game[k]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0];
        cl->SetDescriptorHeaps(n, dummy);
        cl->SetDescriptorHeaps(n, game);
        if (_heapRebinds++ == 0) LOGI("vela: FSR 4 VK: game descriptor heaps re-bound after the provider (%u heaps)", n);
    }

  private:
    struct Img
    {
        VkImage image = VK_NULL_HANDLE;
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    };
    bool Fail(const char* why)
    {
        LOGI("vela: FSR 4 VK: %s", why);
        return false;
    }
    bool Get(const Tex& t, Img& img, bool depth)
    {
        UINT64 handle = 0, offset = 0;
        if (!t.res || FAILED(_interop->GetVulkanResourceInfo1(t.res, &handle, &offset, &img.fmt)) || !handle) return false;
        img.image = (VkImage) handle;
        _interop->GetVulkanImageLayout(t.res, t.state, &img.layout);
        if (depth && (img.fmt == VK_FORMAT_D32_SFLOAT || img.fmt == VK_FORMAT_D32_SFLOAT_S8_UINT))
            img.aspect = VK_IMAGE_ASPECT_DEPTH_BIT; // RE4 hands FSR its own R32F copy; a depth buffer would need usage bits
        return true;
    }
    static uint32_t Fmt(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_R16G16B16A16_SFLOAT: return kRGBA16F;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return kR11G11B10F;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return kR9G9B9E5;
        case VK_FORMAT_R16G16_SFLOAT: return kRG16F;
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT: return kR32F;
        default: return 0;
        }
    }
    static FfxRes R(const Img& img, uint32_t w, uint32_t h, uint32_t state, uint32_t usage)
    {
        FfxRes r{};
        r.resource = (void*) img.image;
        r.description = {kTexture2D, Fmt(img.fmt), w, h, 1, 1, 0, usage};
        r.state = state;
        return r;
    }
    void Transition(VkCommandBuffer cb, const Img* in, int n, const Img& out, bool toProvider)
    {
        VkImageMemoryBarrier b[4]{};
        uint32_t k = 0;
        auto add = [&](const Img& img, VkImageLayout pl)
        {
            if (img.layout == pl) return;
            auto& x = b[k++];
            x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            x.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            x.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            x.oldLayout = toProvider ? img.layout : pl;
            x.newLayout = toProvider ? pl : img.layout;
            x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            x.image = img.image;
            x.subresourceRange = {img.aspect, 0, 1, 0, 1};
        };
        for (int i = 0; i < n; i++) add(in[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        add(out, VK_IMAGE_LAYOUT_GENERAL);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, k, k ? b : nullptr);
    }
    ID3D12DXVKInteropDevice1* _interop = nullptr;
    VkDevice _vk = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    PfnCreate _create = nullptr;
    PfnDestroy _destroy = nullptr;
    PfnDispatch _dispatch = nullptr;
    ffxContext _ctx = nullptr;
    ID3D12Resource* _flush[2] = {};
    ID3D12DescriptorHeap* _dummyHeap[2] = {};
    static constexpr uint32_t kTsRing = 8;
    VkQueryPool _qp = VK_NULL_HANDLE;
    double _tsNs = 1.0;
    uint64_t _frames = 0, _heapRebinds = 0;
    int _failLogged = 0, _heapWarn = 0;
    bool _rebind = true;
    bool _lite = false;
    uint32_t _every = 1, _npu = 0;
    std::shared_ptr<Fsr4Warm::Job> _job;
};

#endif // VELA_NO_VULKAN

// ---- FSR 3.1: the FidelityFX SDK DX12 DLL (the plugin's amd_fidelityfx_dx12.dll, SDK 1.1.4 -> FSR 3.1.4) ------------
class Fsr3 : public Upscaler
{
  public:
    bool Init(ID3D12Device* device)
    {
        _dev = device;
        HMODULE dll = LoadNextToUs(L"amd_fidelityfx_dx12.dll");
        if (!dll) return Fail("amd_fidelityfx_dx12.dll not found next to the DLL");
        _create = (PfnCreate) GetProcAddress(dll, "ffxCreateContext");
        _destroy = (PfnDestroy) GetProcAddress(dll, "ffxDestroyContext");
        _dispatch = (PfnDispatch) GetProcAddress(dll, "ffxDispatch");
        if (!_create || !_destroy || !_dispatch) return Fail("amd_fidelityfx_dx12.dll has no FFX exports");
        CreateUpscale c{};
        c.header.type = kCreateUpscale;
        c.flags = kDepthInverted | kAutoExposure;
        c.maxRenderSize = {rw, rh};
        c.maxUpscaleSize = {dw, dh};
        c.fpMessage = &Message;
        BackendDx12 b{};
        b.header.type = kBackendDx12;
        b.device = device;
        c.header.pNext = &b.header;
        ffxReturnCode_t r;
        {
            OwnScope own; // its pipelines and descriptor heaps go through the recon hooks
            r = _create(&_ctx, &c.header, nullptr);
        }
        if (r != 0 || !_ctx)
        {
            _ctx = nullptr;
            char why[128];
            snprintf(why, sizeof(why), "ffxCreateContext failed (%u) for %ux%u -> %ux%u", r, rw, rh, dw, dh);
            return Fail(why);
        }
        {
            const char* e = getenv("VELA_FSR3_NORESTORE"); // negative control only: the previous behaviour
            _restore = !(e && e[0] == '1');
        }
        LOGI("vela: FSR 3.1 context %ux%u -> %ux%u (amd_fidelityfx_dx12.dll)", rw, rh, dw, dh);
        return true;
    }
    bool Evaluate(ID3D12GraphicsCommandList* cl, const Tex& color, const Tex& depth, const Tex& mv, const Tex& output,
                  const Frame& f) override
    {
        if (!_ctx) return false;
        DispatchUpscale d{};
        d.header.type = kDispatchUpscale;
        d.commandList = cl;
        d.color = R(color, rw, rh, 0);
        d.depth = R(depth, rw, rh, 0);
        d.motionVectors = R(mv, rw, rh, 0);
        d.output = R(output, dw, dh, kUsageUav);
        if (!d.color.description.format || !d.depth.description.format || !d.motionVectors.description.format ||
            !d.output.description.format)
        {
            if (_failLogged++ < 3) LOGI("vela: FSR 3.1: a texture format it does not take");
            return false;
        }
        d.jitterOffset = {f.jitterX, f.jitterY};
        d.motionVectorScale = {f.mvScaleX, f.mvScaleY};
        d.renderSize = {rw, rh};
        d.upscaleSize = {dw, dh};
        d.frameTimeDelta = f.frameTimeMs;
        d.preExposure = f.preExposure > 0 ? f.preExposure : 1.0f;
        d.reset = f.reset;
        d.cameraNear = f.cameraNear;
        d.cameraFar = f.cameraFar;
        d.cameraFovAngleVertical = f.fovV;
        d.viewSpaceToMetersFactor = 1.0f;
        ffxReturnCode_t r;
        {
            OwnScope own;
            r = _dispatch(&_ctx, &d.header);
        }
        // FFX leaves its heaps / root signature / pipeline / root arguments bound on the game's list;
        // RE4 never sets them again in that list -> the crash right after the first FSR 3.1 frame (RestoreGameState)
        const UINT restored = _restore ? RestoreGameState(cl) : 0;
        if (_restoreLogs++ == 0)
            LOGI("vela: FSR 3.1: the game's compute state set again after FFX's passes (%u calls)%s", restored,
                 _restore ? "" : " -- VELA_FSR3_NORESTORE=1, not restored");
        if (r != 0)
        {
            if (_failLogged++ < 3) LOGI("vela: FSR 3.1: ffxDispatch failed (%u)", r);
            return false;
        }
        if (_frames++ == 0) LOGI("vela: FSR 3.1: first frame dispatched");
        return true;
    }
    void Release() override
    {
        if (_ctx && _destroy)
        {
            WaitIdle(_dev);
            OwnScope own;
            _destroy(&_ctx, nullptr);
        }
        _ctx = nullptr;
    }
    ~Fsr3() override { Release(); }

  private:
    bool Fail(const char* why)
    {
        LOGI("vela: FSR 3.1: %s", why);
        return false;
    }
    static uint32_t Fmt(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return kRGBA16F;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return kRGBA32F;
        case DXGI_FORMAT_R11G11B10_FLOAT: return kR11G11B10F;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return kR10G10B10A2;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return kRGBA8;
        case DXGI_FORMAT_R16G16_FLOAT: return kRG16F;
        case DXGI_FORMAT_R32_FLOAT: return kR32F;
        default: return 0;
        }
    }
    // the D3D12 backend moves a resource from the given state to what it needs and back
    static FfxRes R(const Tex& t, uint32_t w, uint32_t h, uint32_t usage)
    {
        FfxRes r{};
        r.resource = t.res;
        r.description = {kTexture2D, t.res ? Fmt(t.res->GetDesc().Format) : 0u, w, h, 1, 1, 0, usage};
        r.state = t.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? kStateUav : kStateComputeRead;
        return r;
    }
    ID3D12Device* _dev = nullptr;
    PfnCreate _create = nullptr;
    PfnDestroy _destroy = nullptr;
    PfnDispatch _dispatch = nullptr;
    ffxContext _ctx = nullptr;
    uint64_t _frames = 0, _restoreLogs = 0;
    int _failLogged = 0;
    bool _restore = true;
};

float Halton(int i, int b)
{
    float f = 1, r = 0;
    while (i > 0)
    {
        f /= (float) b;
        r += f * (float) (i % b);
        i /= b;
    }
    return r;
}
} // namespace

bool OwnWork() { return g_own.load() > 0; }

const char* ModelName(Model m)
{
    return m == Model::Ai ? "ai" : m == Model::Sr ? "sr" : m == Model::Sgsr2 ? "sgsr2" : m == Model::Fsr4 ? "fsr4"
         : m == Model::Fsr4Lite ? "fsr4lite" : m == Model::Fsr3 ? "fsr3" : m == Model::Fsr2 ? "fsr2" : m == Model::Fsr4Fast ? "fsr4fast" : "-";
}
const char* ModelLabel(Model m)
{
    return m == Model::Fsr4 ? "FSR 4" : m == Model::Fsr4Fast ? "FSR 4 Fast" : m == Model::Fsr4Lite ? "FSR 4 Lite" : m == Model::Fsr3 ? "FSR 3.1"
         : m == Model::Sgsr2 ? "SGSR 2" : m == Model::Fsr2 ? "the game's FSR 2" : m == Model::Ai ? "Vela AI"
         : m == Model::Sr ? "Vela SR" : "-";
}

const Live& PollLive(bool force)
{
    static Live cur;
    static std::string path, text;
    static FILETIME time{};
    static DWORD size = 0xffffffffu;
    static uint32_t calls = 0;
    static bool init = false;
    if (!init)
    {
        init = true;
        force = true;
        char id[32] = {};
        for (const char* k : {"SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID"})
            if (GetEnvironmentVariableA(k, id, sizeof(id)) && id[0] >= '0' && id[0] <= '9') break;
            else id[0] = 0;
        if (id[0]) path = std::string("Z:\\tmp\\vela-live\\") + id + ".ini";
        LOGI("vela: live settings file %s", path.empty() ? "- (no SteamAppId)" : path.c_str());
    }
    if (path.empty() || (!force && (calls++ % 15) != 0)) return cur;
    std::string t;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa))
    {
        if (CompareFileTime(&fa.ftLastWriteTime, &time) == 0 && fa.nFileSizeLow == size) return cur;
        time = fa.ftLastWriteTime;
        size = fa.nFileSizeLow;
        if (FILE* f = fopen(path.c_str(), "rb"))
        {
            char buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && t.size() < 8192) t.append(buf, n);
            fclose(f);
        }
    }
    else
        size = 0xffffffffu;
    if (t == text) return cur;
    text = t;
    Live v;
    size_t pos = 0;
    while (pos < t.size())
    {
        size_t end = t.find('\n', pos);
        if (end == std::string::npos) end = t.size();
        std::string line = t.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        const std::string k = line.substr(0, eq), s = line.substr(eq + 1);
        if (k == "model")
        {
            // Vela AI / Vela SR are no longer offered; an old or unknown key runs the game's FSR 2
            v.model = s == "sgsr2" ? Model::Sgsr2 : s == "fsr4" ? Model::Fsr4 : s == "fsr4fast" ? Model::Fsr4Fast : s == "fsr4lite" ? Model::Fsr4Lite
                    : s == "fsr3" ? Model::Fsr3 : s == "fsr2" ? Model::Fsr2 : s.empty() ? Model::Unset : Model::Fsr2;
            if (v.model == Model::Fsr2 && s != "fsr2")
                LOGI("vela: model '%s' is not offered (models: fsr4, fsr4fast, fsr4lite, fsr3, sgsr2) -- the game's FSR 2 runs",
                     s.c_str());
        }
        else if (k == "lossy")
            v.lossy = (std::max)(1, (std::min)(atoi(s.c_str()), 8));
        else if (k == "npu")
            v.npu = (std::max)(0, (std::min)(atoi(s.c_str()), 2));
    }
    cur = v;
    LOGI("vela: live (%s): model %s lossy %d npu %d", t.empty() ? "no file, launch settings" : path.c_str(),
         ModelName(cur.model), cur.lossy, cur.npu);
    return cur;
}

void WaitIdle(ID3D12Device* dev)
{
#ifdef VELA_NO_VULKAN
    (void) dev; // PC: native D3D12, the caller's own fence would be needed; contexts are not re-created there
#else
    ID3D12DXVKInteropDevice1* i = nullptr;
    if (!dev || FAILED(dev->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), (void**) &i)) || !i) return;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice ph = VK_NULL_HANDLE;
    VkDevice vk = VK_NULL_HANDLE;
    if (SUCCEEDED(i->GetVulkanHandles(&inst, &ph, &vk)) && vk) vkDeviceWaitIdle(vk);
    i->Release();
#endif
}

Upscaler* Create(ID3D12Device* dev, Model m, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, int lossy, int npu)
{
    if (m == Model::Fsr4Lite && (rw < 645 || rh < 360 || rw > 1920 || rh > 1080 || dw != 1920 || dh != 1080))
    {
        // the fork's line (py_modules/sgsr3.py: "FSR 4.1 Lite runs only ")
        LOGI("vela: FSR 4.1 Lite runs only from 645x360 to 1920x1080 render -> 1920x1080, not %ux%u -> %ux%u; the game's FSR 2 instead",
             rw, rh, dw, dh);
        if (npu) LOGI("Vela NPU: fallback size, GPU path");
        return nullptr;
    }
    Upscaler* u = nullptr;
    bool ok = false;
#ifndef VELA_NO_VULKAN
    if (IsFsr4Full(m) || m == Model::Fsr4Lite)
    {
        auto* f = new Fsr4();
        f->rw = rw; f->rh = rh; f->dw = dw; f->dh = dh;
        ok = f->Init(dev, m == Model::Fsr4Lite, (uint32_t) lossy, (uint32_t) npu);
        u = f;
    }
    else if (m == Model::Sgsr2)
    {
        u = CreateSgsr2(dev, rw, rh, dw, dh);
        ok = u != nullptr;
    }
    else
#endif
    if (m == Model::Fsr3)
    {
        auto* f = new Fsr3();
        f->rw = rw; f->rh = rh; f->dw = dw; f->dh = dh;
        ok = f->Init(dev);
        u = f;
    }
    if (!ok)
    {
        delete u;
        LOGI("vela: model %s is not available here; the game's FSR 2 instead", ModelName(m));
        return nullptr;
    }
    u->model = m;
    u->lossy = lossy;
    u->npu = npu;
    return u;
}

// ---- JitterTrack --------------------------------------------------------------------------------------------------
void JitterTrack::Record(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* owner, UINT64 off,
                         uint32_t frame, int phases)
{
    if (phases < 4 || phases > 256) return;
    if (phases != _phases)
    {
        _phases = phases;
        for (int k = 0; k < phases; k++) { _tab[k][0] = Halton(k + 1, 2) - 0.5f; _tab[k][1] = Halton(k + 1, 3) - 0.5f; }
        _locked = false;
        _hits = _misses = 0;
    }
    if (!_rb)
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kSlots * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&_rb))) || !_rb)
            return;
        if (FAILED(_rb->Map(0, nullptr, (void**) &_map)) || !_map) { _rb->Release(); _rb = nullptr; return; }
        LOGI("vela: jitter readback on (FSR 2 Halton sequence, %d phases)", phases);
    }
    // the slot written kLag frames ago: done by now unless the GPU is that far behind (then still the NaN mark)
    const int old = (int) ((frame - kLag) % kSlots);
    if (frame >= (uint32_t) kLag && _pending[old] && _slotFrame[old] == frame - kLag)
    {
        float v[2];
        memcpy(v, _map + old * 16, 8);
        _pending[old] = false;
        if (!std::isnan(v[0])) Observe(frame - kLag, v[0], v[1]);
    }
    const int s = (int) (frame % kSlots);
    const float nan = std::nanf("");
    memcpy(_map + s * 16, &nan, 4);
    memcpy(_map + s * 16 + 4, &nan, 4);
    _slotFrame[s] = frame;
    _pending[s] = true;
    cl->CopyBufferRegion(_rb, (UINT64) s * 16, owner, off, 8);
}

void JitterTrack::Observe(uint32_t frame, float x, float y)
{
    int k = -1;
    float sign = 1.0f;
    // the 32-phase table is symmetric: -t[3] == t[6] and -t[4] == t[5] (Halton(2,3) - 0.5). The old
    // (entry outer, sign inner) search took phases 5 and 6 as the NEGATED phases 4 and 3: two misses in a row once
    // per 32 frames -> "phase lost" -> Vela AI frames + a model switch every ~1.2 s in RE4. Search the locked (else
    // the positive) sign over the whole table first, the other sign only when nothing matches.
    const float first = _locked ? _sign : 1.0f;
    for (float sg : {first, -first})
        for (int i = 0; i < _phases && k < 0; i++)
            if (std::fabs(x - sg * _tab[i][0]) < 2e-4f && std::fabs(y - sg * _tab[i][1]) < 2e-4f) { k = i; sign = sg; }
    if (k < 0)
    {
        if (_alien++ == 0) LOGI("vela: jitter (%.4f, %.4f) is not in FSR 2's Halton table of %d", x, y, _phases);
        return;
    }
    const int64_t off = (((int64_t) k - (int64_t) frame) % _phases + _phases) % _phases;
    if (_locked && off == _offset && sign == _sign) { _misses = 0; return; }
    if (_locked)
    {
        if (++_misses < 2) return;
        _locked = false;
        _hits = 0;
        LOGI("vela: jitter phase lost -- FSR 2 frames until it locks again");
    }
    if (_hits > 0 && off == _offset && sign == _sign) _hits++;
    else { _hits = 1; _offset = off; _sign = sign; }
    if (_hits >= 2)
    {
        _locked = true;
        _misses = 0;
        LOGI("vela: jitter locked (Halton phase offset %lld of %d%s)", (long long) _offset, _phases,
             _sign < 0 ? ", negated" : "");
    }
}

bool JitterTrack::Predict(uint32_t frame, float& x, float& y) const
{
    if (!_locked || _phases <= 0) return false;
    const int k = (int) (((int64_t) frame + _offset) % _phases);
    x = _sign * _tab[k][0];
    y = _sign * _tab[k][1];
    return true;
}
} // namespace Ext
