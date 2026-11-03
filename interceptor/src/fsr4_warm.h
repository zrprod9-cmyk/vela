#pragma once
// Vela 0.8.15: FSR 4 without the first-use freeze. The fsr4vk provider builds a model's 29+ compute pipelines inside
// the first ffxDispatch of a context (ffx_vk_provider.cpp: QualityContext created lazily per preset); with a cold Mesa
// cache that froze the game's render thread for 6.4 s (Control 960x540 -> 1080p HDR) / 8.8 s (640x360 -> 1080p).
// Here the context is created and its first frame is dispatched on a worker thread, on a private D3D12 queue with
// private cleared textures, and waited for by a fence; only then does the host take the context (Job::Take) and send it
// the game's frames. Until then the host keeps its previous upscaler running (the student / the game's FSR 2).
// First job of a process (and not Lite): the other 1080p networks (render 1/2 = P, 1/3 = UP of the same output) are
// built first on throw-away contexts without the NPU, so the Mesa disk cache holds their pipelines when the game's
// quality preset changes later. They run before the real context exists on purpose: the provider's NPU state is
// process-wide (fv_npu.hpp g_request / g_bridge); a context without the NPU created or dispatched while an NPU context
// lives would turn the NPU off for it.
// Shared by the fork (OptiScaler/upscalers/sgsr3) and the RE4 interceptor): include after
// the D3D12 headers, vulkan.h and the vkd3d-proton interop interface (ID3D12DXVKInteropDevice1).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Fsr4Warm
{
// FFX API ABI (FidelityFX SDK headers, MIT) as the hosts use it
struct Hdr
{
    uint64_t type;
    Hdr* pNext;
};
struct Res
{
    void* resource;
    uint32_t type, format, width, height, depth, mipCount, flags, usage;
    uint32_t state;
};
struct Create
{
    Hdr header;
    uint32_t flags;
    uint32_t maxRW, maxRH, maxDW, maxDH;
    void (*fpMessage)(uint32_t type, const wchar_t* message);
};
struct Dispatch
{
    Hdr header;
    void* commandList;
    Res color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    float jx, jy, mvx, mvy;
    uint32_t rw, rh, dw, dh;
    bool enableSharpening;
    float sharpness, frameTimeDelta, preExposure;
    bool reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    uint32_t flags;
};
struct Backend
{
    Hdr header;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    PFN_vkGetDeviceProcAddr gdpa;
};
struct Version
{
    Hdr header;
    uint64_t id;
};
struct Features
{
    Hdr header;
    uint32_t enabledFlags, apiVersion, queueFamilyIndex;
};
struct U32
{
    Hdr header;
    uint32_t v;
};
using PfnCreate = uint32_t (*)(void**, Hdr*, const void*);
using PfnDestroy = uint32_t (*)(void**, const void*);
using PfnDispatch = uint32_t (*)(void**, const Hdr*);

struct Params
{
    ID3D12Device* device = nullptr;
    ID3D12DXVKInteropDevice1* interop = nullptr;
    VkDevice vk = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PfnCreate create = nullptr;
    PfnDestroy destroy = nullptr;
    PfnDispatch dispatch = nullptr;
    void (*message)(uint32_t, const wchar_t*) = nullptr;
    uint32_t flags = 0, rw = 0, rh = 0, dw = 0, dh = 0;
    uint64_t version = 0;
    uint32_t lossyEvery = 1; // > 1: the VELALSY1 node (Lite)
    uint32_t npu = 0;        // != 0: the VELANPU1 node
    bool lite = false;
    std::function<void(const std::string&)> log;
};

inline std::atomic<bool>& FirstJobDone()
{
    static std::atomic<bool> done { false }; // the other networks were built once in this process
    return done;
}

class Job
{
  public:
    enum State : int { Running = 0, Ok = 1, Failed = 2 };

    // Starts the worker; the job keeps the device, interop and provider DLL referenced until it ends
    static std::shared_ptr<Job> Start(const Params& p)
    {
        auto j = std::shared_ptr<Job>(new Job(p));
        p.device->AddRef();
        p.interop->AddRef();
        j->_started = std::chrono::steady_clock::now();
        try
        {
            std::thread([j] { j->Run(); }).detach();
        }
        catch (...)
        {
            j->Finish(Failed, "no worker thread");
        }
        return j;
    }

    State Poll() const { return (State) _state.load(std::memory_order_acquire); }
    const std::string& Why() const { return _why; }
    double Ms() const { return _ms; }

    // After Poll() == Ok: the warmed context (once); the job forgets it
    void* Take()
    {
        std::lock_guard<std::mutex> l(_m);
        void* c = _ctx;
        _ctx = nullptr;
        return c;
    }

    // The host no longer wants the context: the worker destroys it when it ends (or now, if it already ended)
    void Abandon()
    {
        std::lock_guard<std::mutex> l(_m);
        _abandoned = true;
        if (_state.load() != Running && _ctx)
            DestroyCtx(_ctx);
    }

  private:
    explicit Job(const Params& p) : _p(p) {}

    void Log(const std::string& s)
    {
        if (_p.log)
            _p.log(s);
    }

    void DestroyCtx(void*& c)
    {
        if (!c || _leak)
            return;
        // no vkDeviceWaitIdle: the job's contexts only ran on the private queue, and every frame there was fenced
        // (and a worker may not wait on vkd3d-proton's queues, which its submit thread uses)
        _p.destroy(&c, nullptr);
        c = nullptr;
    }

    void Finish(State s, const std::string& why)
    {
        _ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _started).count();
        std::lock_guard<std::mutex> l(_m);
        _why = why;
        if (s != Ok || _abandoned)
            DestroyCtx(_ctx);
        _state.store(s, std::memory_order_release);
        _p.interop->Release();
        _p.device->Release();
    }

    void* CreateCtx(uint32_t rw, uint32_t rh, bool withNpu)
    {
        Create c {};
        c.header.type = 0x00010000u; // FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE
        c.flags = _p.flags;
        c.maxRW = rw;
        c.maxRH = rh;
        c.maxDW = _p.dw;
        c.maxDH = _p.dh;
        c.fpMessage = _p.message;
        Backend b {};
        b.header.type = 3u;
        b.device = _p.vk;
        b.physicalDevice = _p.physical;
        b.gdpa = vkGetDeviceProcAddr;
        Version v {};
        v.header.type = 5u;
        v.id = _p.version;
        Features f {};
        f.header.type = 0x4653523444455643ull;
        f.queueFamilyIndex = 0; // Turnip: one queue family
        U32 lossy {};
        lossy.header.type = 0x56454C414C535931ull;
        lossy.v = _p.lossyEvery;
        U32 npu {};
        npu.header.type = 0x56454C414E505531ull;
        npu.v = _p.npu;
        c.header.pNext = &b.header;
        b.header.pNext = &v.header;
        v.header.pNext = &f.header;
        Hdr* tail = &f.header;
        if (_p.lite && _p.lossyEvery > 1)
        {
            tail->pNext = &lossy.header;
            tail = &lossy.header;
        }
        if (withNpu && _p.npu)
            tail->pNext = &npu.header;
        void* ctx = nullptr;
        if (_p.create(&ctx, &c.header, nullptr) != 0)
            return nullptr;
        return ctx;
    }

    struct D3D
    {
        ID3D12CommandQueue* q = nullptr;
        ID3D12CommandAllocator* a = nullptr;
        ID3D12GraphicsCommandList* l = nullptr;
        ID3D12Fence* f = nullptr;
        ID3D12DescriptorHeap* rtv = nullptr;
        ID3D12Resource* t[4] = {}; // colour, depth (R32F, inverted), MV, output
        HANDLE ev = nullptr;
        uint64_t fv = 0;
        ~D3D()
        {
            for (auto* r : t)
                if (r)
                    r->Release();
            if (rtv)
                rtv->Release();
            if (l)
                l->Release();
            if (a)
                a->Release();
            if (f)
                f->Release();
            if (q)
                q->Release();
            if (ev)
                CloseHandle(ev);
        }
    };

    bool MakeD3D(D3D& d)
    {
        auto* dev = _p.device;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.q))) ||
            FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.a))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.f))))
            return false;
        d.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;
        if (!d.ev || FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&d.rtv))))
            return false;
        const DXGI_FORMAT fmt[4] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT };
        const uint32_t w[4] = { _p.rw, _p.rw, _p.rw, _p.dw }, h[4] = { _p.rh, _p.rh, _p.rh, _p.dh };
        for (int i = 0; i < 4; i++)
        {
            D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w[i];
            rd.Height = h[i];
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = fmt[i];
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    nullptr, IID_PPV_ARGS(&d.t[i]))))
                return false;
        }
        return true;
    }

    static uint32_t FfxFmt(VkFormat f)
    {
        return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 4u : f == VK_FORMAT_R16G16_SFLOAT ? 18u : f == VK_FORMAT_R32_SFLOAT ? 28u : 0u;
    }

    // One frame of ctx (render rw x rh -> the job's output) on the private queue, reset on, waited for
    bool WarmFrame(D3D& d, void* ctx, uint32_t rw, uint32_t rh)
    {
        auto* l = d.l;
        if (!l)
        {
            if (FAILED(_p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.a, nullptr, IID_PPV_ARGS(&d.l))))
                return false;
            l = d.l;
        }
        else if (FAILED(d.a->Reset()) || FAILED(l->Reset(d.a, nullptr)))
            return false;
        const UINT inc = _p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const float zero[4] = { 0, 0, 0, 1 };
        D3D12_RESOURCE_BARRIER b[4] {};
        for (int i = 0; i < 4; i++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE hnd = d.rtv->GetCPUDescriptorHandleForHeapStart();
            hnd.ptr += (SIZE_T) i * inc;
            _p.device->CreateRenderTargetView(d.t[i], nullptr, hnd);
            l->ClearRenderTargetView(hnd, zero, 0, nullptr);
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.pResource = d.t[i];
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b[i].Transition.StateAfter = i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        l->ResourceBarrier(4, b);
        VkImage img[4] {};
        VkFormat fmt[4] {};
        VkImageLayout lay[4] {};
        for (int i = 0; i < 4; i++)
        {
            UINT64 hnd = 0, off = 0;
            if (FAILED(_p.interop->GetVulkanResourceInfo1(d.t[i], &hnd, &off, &fmt[i])) || !hnd)
                return false;
            img[i] = (VkImage) hnd;
            _p.interop->GetVulkanImageLayout(d.t[i], b[i].Transition.StateAfter, &lay[i]);
        }
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (FAILED(_p.interop->BeginVkCommandBufferInterop(l, &cb)) || !cb)
            return false;
        auto barrier = [&](bool to)
        {
            VkImageMemoryBarrier ib[4] {};
            uint32_t k = 0;
            for (int i = 0; i < 4; i++)
            {
                const VkImageLayout pl = i == 3 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (lay[i] == pl)
                    continue;
                auto& x = ib[k++];
                x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                x.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                x.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                x.oldLayout = to ? lay[i] : pl;
                x.newLayout = to ? pl : lay[i];
                x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                x.image = img[i];
                x.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            }
            VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
                                 0, nullptr, k, k ? ib : nullptr);
        };
        Dispatch ds {};
        ds.header.type = 0x00010001u;
        auto res = [&](int i, uint32_t w, uint32_t h, uint32_t state, uint32_t usage)
        {
            Res r {};
            r.resource = (void*) img[i];
            r.type = 2u;
            r.format = FfxFmt(fmt[i]);
            r.width = w;
            r.height = h;
            r.depth = 1;
            r.mipCount = 1;
            r.usage = usage;
            r.state = state;
            return r;
        };
        ds.color = res(0, rw, rh, 1u << 2, 0);
        ds.depth = res(1, rw, rh, 1u << 2, 0);
        ds.motionVectors = res(2, rw, rh, 1u << 2, 0);
        ds.output = res(3, _p.dw, _p.dh, 1u << 1, 1u << 1);
        ds.mvx = (float) rw;
        ds.mvy = (float) rh;
        ds.rw = rw;
        ds.rh = rh;
        ds.dw = _p.dw;
        ds.dh = _p.dh;
        ds.frameTimeDelta = 16.6f;
        ds.preExposure = 1.0f;
        ds.reset = true;
        ds.cameraNear = 1000.0f; // inverted depth: near / far swapped as the hosts pass them
        ds.cameraFar = 0.1f;
        ds.cameraFovAngleVertical = 1.0472f;
        barrier(true);
        ds.commandList = cb;
        const uint32_t r = _p.dispatch(&ctx, &ds.header);
        barrier(false);
        _p.interop->EndVkCommandBufferInterop(l);
        for (auto& x : b)
            std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        l->ResourceBarrier(4, b);
        if (FAILED(l->Close()))
            return false;
        ID3D12CommandList* lists[1] = { l };
        d.q->ExecuteCommandLists(1, lists);
        d.q->Signal(d.f, ++d.fv);
        if (d.f->GetCompletedValue() < d.fv)
        {
            d.f->SetEventOnCompletion(d.fv, d.ev);
            WaitForSingleObject(d.ev, 30000);
        }
        if (d.f->GetCompletedValue() < d.fv)
            _leak = true; // still on the GPU after 30 s: never destroyed
        return r == 0 && d.f->GetCompletedValue() >= d.fv;
    }

    void Run()
    {
        D3D d;
        if (!MakeD3D(d))
            return Finish(Failed, "private queue / textures failed");
        char line[240];
        // the other networks of this output (P = render 1/2, UP = render 1/3), once per process, before any NPU context
        bool expected = false;
        if (!_p.lite && FirstJobDone().compare_exchange_strong(expected, true))
        {
            for (uint32_t div : { 2u, 3u })
            {
                const uint32_t w = _p.dw / div, h = _p.dh / div;
                if ((w == _p.rw && h == _p.rh) || w > _p.rw || h > _p.rh || std::getenv("VELA_FSR4_NOPREWARM"))
                    continue; // the real context builds it; the private textures hold at most the real render size
                const auto t0 = std::chrono::steady_clock::now();
                void* c = CreateCtx(w, h, false);
                const bool ok = c && WarmFrame(d, c, w, h);
                DestroyCtx(c);
                snprintf(line, sizeof(line), "Vela FSR 4 VK: network for %ux%u -> %ux%u built ahead (%s, %.0f ms, worker thread)",
                         w, h, _p.dw, _p.dh, ok ? "ok" : "failed",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                Log(line);
            }
        }
        {
            std::lock_guard<std::mutex> l(_m);
            if (_abandoned)
                return Finish(Failed, "abandoned");
        }
        void* c = CreateCtx(_p.rw, _p.rh, true);
        if (!c)
            return Finish(Failed, "ffxCreateContext failed");
        {
            std::lock_guard<std::mutex> l(_m);
            _ctx = c;
        }
        if (!WarmFrame(d, c, _p.rw, _p.rh))
            return Finish(Failed, "the warm-up frame failed");
        Finish(Ok, "");
    }

    Params _p;
    std::atomic<int> _state { Running };
    std::mutex _m;
    void* _ctx = nullptr;
    bool _abandoned = false;
    bool _leak = false;
    std::string _why;
    double _ms = 0;
    std::chrono::steady_clock::time_point _started;
};
} // namespace Fsr4Warm
