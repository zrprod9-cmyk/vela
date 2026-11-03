// PC stand-in for "a game with FSR 2 linked into its exe": a D3D12 app that statically links AMD's FSR 2.1.2
// DX12 backend (MIT; the symbol-renamed _212 build in the fork's library/fsr2_212 folder)
// and calls ffxFsr2ContextDispatch every frame. It exports nothing and loads no FFX DLL, so -- exactly like
// RE4 -- an export-hooking interceptor cannot see FSR 2 here; only the graphics-API recon (our dxgi.dll next
// to this exe) can. Used to validate the D3D12 recon/replacement on a PC without the console.
//
//   fsr2_harness.exe [frames=240] [ms_per_frame=16]
//   HARNESS_RELOAD=K: every K frames a "level load": FSR context and all textures destroyed, 5 frames without FSR,
//   everything recreated at the other render size (the RE4 GPU fault came after a level load)
//
// Inputs are synthetic but recognisable in a dump: colour = coloured rings, depth = horizontal ramp (inverted
// Z), motion vectors = constant (+2, -1) render pixels. 960x540 -> 1920x1080, like RE4's FSR 2 Performance.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "ffx_fsr2.h"
#include "dx12/ffx_fsr2_dx12.h"

using namespace Fsr212;

static UINT RW = 960, RH = 540; // render size; HARNESS_RELOAD alternates it with 1280x720
static const UINT DW = 1920, DH = 1080;

static ID3D12Device* dev;
static ID3D12CommandQueue* queue;
static ID3D12CommandAllocator* alloc;
static ID3D12GraphicsCommandList* list;
static ID3D12Fence* fence;
static UINT64 fenceVal;
static HANDLE fenceEvt;

static void waitGpu()
{
    queue->Signal(fence, ++fenceVal);
    if (fence->GetCompletedValue() < fenceVal)
    {
        fence->SetEventOnCompletion(fenceVal, fenceEvt);
        WaitForSingleObject(fenceEvt, INFINITE);
    }
}

static ID3D12Resource* tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r))))
        printf("tex create failed fmt %d\n", (int) f);
    return r;
}

static uint16_t f2h(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000, e = ((x >> 23) & 0xff), m = x & 0x7fffff;
    int ee = (int) e - 127 + 15;
    if (e == 0) return (uint16_t) s;
    if (ee <= 0) return (uint16_t) s;
    if (ee >= 31) return (uint16_t) (s | 0x7c00);
    return (uint16_t) (s | (ee << 10) | (m >> 13));
}

// upload `bytes` (tightly packed rows of rowBytes) into a texture in COPY_DEST, then leave it in `after`
static void upload(ID3D12Resource* dst, const void* bytes, UINT rowBytes, UINT h, D3D12_RESOURCE_STATES after,
                   D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COPY_DEST)
{
    auto d = dst->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows;
    UINT64 rb, total;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rb, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                 IID_PPV_ARGS(&up));
    uint8_t* p = nullptr;
    up->Map(0, nullptr, (void**) &p);
    for (UINT y = 0; y < h; y++) memcpy(p + (size_t) y * fp.Footprint.RowPitch, (const uint8_t*) bytes + (size_t) y * rowBytes, rowBytes);
    up->Unmap(0, nullptr);
    alloc->Reset();
    list->Reset(alloc, nullptr);
    if (before != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER b0{};
        b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0.Transition.pResource = dst;
        b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b0.Transition.StateBefore = before;
        b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b0);
    }
    D3D12_TEXTURE_COPY_LOCATION s{}, t{};
    s.pResource = up;
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint = fp;
    t.pResource = dst;
    t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dst;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* l[] = {list};
    queue->ExecuteCommandLists(1, l);
    waitGpu();
    up->Release();
}

// ---- HARNESS_PSOTEST=1: compute PSOs on every creation path + ExecuteIndirect --------------------------------------
// One small CS whose reflection carries FSR 2 names (cbFSR2, fDeviceToViewDepth, jitterOffset) is created through
// CreateComputePipelineState, CreatePipelineState (stream), ID3D12PipelineLibrary::LoadComputePipeline and
// ID3D12PipelineLibrary1::LoadPipeline: the interceptor's log must show the same DXIL hash on all four paths (RE4
// loads 1439 of its 1441 compute PSOs from its library). Then the library PSO runs through ExecuteIndirect and writes
// 1 into a buffer: printed back, so a run with VELA_SKIP_EI=<its hash> must print 0.
#include <d3dcompiler.h>
static int psoTest()
{
    const char* hlsl =
        "cbuffer cbFSR2 : register(b0) { uint4 fDeviceToViewDepth; uint2 jitterOffset; };\n"
        "RWStructuredBuffer<uint> o : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() { o[0] = fDeviceToViewDepth.x + jitterOffset.y; }\n";
    ID3DBlob *cs = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), "t", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err)))
    {
        printf("compile failed: %s\n", err ? (const char*) err->GetBufferPointer() : "?");
        return 1;
    }
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* sig = nullptr;
    D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    ID3D12RootSignature* rs = nullptr;
    dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = rs;
    cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ID3D12PipelineState *p0 = nullptr, *p1 = nullptr, *p2 = nullptr, *p3 = nullptr;
    HRESULT h0 = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&p0));
    struct alignas(void*) Stream
    {
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t0 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        ID3D12RootSignature* rs;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t1 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
        D3D12_SHADER_BYTECODE cs;
    } st;
    st.rs = rs;
    st.cs = cd.CS;
    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(st), &st};
    ID3D12Device2* d2 = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&d2));
    HRESULT h1 = d2 ? d2->CreatePipelineState(&sd, IID_PPV_ARGS(&p1)) : E_FAIL;
    ID3D12Device1* d1 = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&d1));
    ID3D12PipelineLibrary1* lib = nullptr;
    HRESULT hl = d1 ? d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib)) : E_FAIL;
    HRESULT h2 = E_FAIL, h3 = E_FAIL;
    if (lib)
    {
        lib->StorePipeline(L"cs", p0);
        lib->StorePipeline(L"cs_stream", p1);
        h2 = lib->LoadComputePipeline(L"cs", &cd, IID_PPV_ARGS(&p2));
        h3 = lib->LoadPipeline(L"cs_stream", &sd, IID_PPV_ARGS(&p3));
    }
    printf("pso paths: CreateComputePipelineState 0x%08x, CreatePipelineState 0x%08x, library 0x%08x: LoadComputePipeline "
           "0x%08x, LoadPipeline 0x%08x\n", (unsigned) h0, (unsigned) h1, (unsigned) hl, (unsigned) h2, (unsigned) h3);
    if (!p2) return 1;

    auto buf = [](UINT64 n, D3D12_HEAP_TYPE t, D3D12_RESOURCE_STATES s, D3D12_RESOURCE_FLAGS f) {
        D3D12_HEAP_PROPERTIES hp{t};
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = n;
        b.Height = 1;
        b.DepthOrArraySize = 1;
        b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        b.Flags = f;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, s, nullptr, IID_PPV_ARGS(&r));
        return r;
    };
    ID3D12Resource* cb = buf(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* args = buf(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* out = buf(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* rb = buf(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    uint32_t* m = nullptr;
    cb->Map(0, nullptr, (void**) &m);
    memset(m, 0, 256);
    m[0] = 1; // fDeviceToViewDepth.x = 1 -> the shader writes 1
    cb->Unmap(0, nullptr);
    args->Map(0, nullptr, (void**) &m);
    m[0] = m[1] = m[2] = 1;
    args->Unmap(0, nullptr);
    D3D12_INDIRECT_ARGUMENT_DESC ad{};
    ad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sdesc{12, 1, &ad, 0};
    ID3D12CommandSignature* csig = nullptr;
    dev->CreateCommandSignature(&sdesc, nullptr, IID_PPV_ARGS(&csig));
    alloc->Reset();
    list->Reset(alloc, nullptr);
    list->SetComputeRootSignature(rs);
    list->SetPipelineState(p2); // the PSO loaded from the library
    list->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list->ExecuteIndirect(csig, 1, args, 0, nullptr, 0);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(rb, 0, out, 0, 4);
    list->Close();
    ID3D12CommandList* l[] = {list};
    queue->ExecuteCommandLists(1, l);
    waitGpu();
    rb->Map(0, nullptr, (void**) &m);
    printf("ExecuteIndirect wrote %u (1 = ran, 0 = skipped); device status 0x%08x\n", m[0],
           (unsigned) dev->GetDeviceRemovedReason());
    rb->Unmap(0, nullptr);
    return 0;
}

int main(int argc, char** argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 240;
    int msPer = argc > 2 ? atoi(argv[2]) : 16;

    if (getenv("HARNESS_DEBUG"))
    {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); printf("debug layer on\n"); }
    }
    IDXGIFactory1* fac = nullptr; // goes through our dxgi.dll (exe folder) -> recon hooks installed
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { printf("no dxgi\n"); return 1; }
    IDXGIAdapter1* ad = nullptr;
    fac->EnumAdapters1(0, &ad);
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no d3d12\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ID3D12Resource *color = nullptr, *depth = nullptr, *mv = nullptr, *out = nullptr;
    std::vector<uint8_t> scratch;
    FfxFsr2Context ctx{};
    auto build = [&]() -> bool {
    if (getenv("HARNESS_PSOTEST")) return psoTest();
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    color = tex(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_COPY_DEST);
    // a depth buffer as games have (depth-stencil flag): the interceptor finds FSR's depth input by that flag
    depth = tex(RW, RH, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_COPY_DEST);
    mv = tex(RW, RH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    out = tex(DW, DH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {
        std::vector<uint16_t> c(RW * RH * 4);
        std::vector<float> z(RW * RH);
        std::vector<uint16_t> m(RW * RH * 2);
        for (UINT y = 0; y < RH; y++)
            for (UINT x = 0; x < RW; x++)
            {
                float dx = (x - RW * 0.5f) / RH, dy = (y - RH * 0.5f) / RH, r = sqrtf(dx * dx + dy * dy);
                float ring = 0.5f + 0.5f * cosf(r * 40.0f);
                size_t i = (size_t) y * RW + x;
                c[i * 4 + 0] = f2h(ring * (0.3f + 0.7f * x / RW));
                c[i * 4 + 1] = f2h(ring * 0.6f);
                c[i * 4 + 2] = f2h((1 - ring) * (0.3f + 0.7f * y / RH));
                c[i * 4 + 3] = f2h(1.0f);
                z[i] = 1.0f - 0.9f * x / RW; // inverted Z: near = 1
                m[i * 2 + 0] = f2h(2.0f / RW);
                m[i * 2 + 1] = f2h(-1.0f / RH);
            }
        upload(color, c.data(), RW * 8, RH, SRV);
        upload(depth, z.data(), RW * 4, RH, SRV);
        upload(mv, m.data(), RW * 4, RH, SRV);
    }

    scratch.assign(ffxFsr2GetScratchMemorySizeDX12_212(), 0);
    FfxFsr2ContextDescription cd{};
    if (ffxFsr2GetInterfaceDX12_212(&cd.callbacks, dev, scratch.data(), scratch.size()) != FFX_OK)
    {
        printf("fsr2 interface failed\n");
        return false;
    }
    cd.device = ffxGetDeviceDX12_212(dev);
    cd.maxRenderSize = {RW, RH};
    cd.displaySize = {DW, DH};
    cd.flags = FFX_FSR2_ENABLE_DEPTH_INVERTED | FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    ctx = FfxFsr2Context{};
    if (ffxFsr2ContextCreate212(&ctx, &cd) != FFX_OK) { printf("fsr2 create failed\n"); return false; }
        return true;
    };
    auto teardown = [&]() {
        waitGpu();
        ffxFsr2ContextDestroy212(&ctx);
        for (ID3D12Resource* r : {color, depth, mv, out}) if (r) r->Release();
        color = depth = mv = out = nullptr;
    };
    if (!build()) return 1;
    const int reload = getenv("HARNESS_RELOAD") ? atoi(getenv("HARNESS_RELOAD")) : 0;
    const bool badva = getenv("HARNESS_BADVA") != nullptr; // HARNESS_BADVA=1: see RECON_VACHECK
    const bool scroll = getenv("HARNESS_SCROLL") != nullptr; // HARNESS_SCROLL=1: moving content with exact MVs
    int loads = 0, sinceLoad = 0;
    int phases = ffxFsr2GetJitterPhaseCount212(RW, DW);

    for (int f = 0; f < frames; f++)
    {
        if (reload > 0 && f > 0 && f % reload == 0)
        {
            teardown();
            for (int k = 0; k < 5; k++) // the loading screen: frames without FSR
            {
                alloc->Reset();
                list->Reset(alloc, nullptr);
                list->Close();
                ID3D12CommandList* l0[] = {list};
                queue->ExecuteCommandLists(1, l0);
                waitGpu();
                Sleep(msPer);
            }
            if (RW == 960) { RW = 1280; RH = 720; } else { RW = 960; RH = 540; }
            if (!build()) return 1;
            phases = ffxFsr2GetJitterPhaseCount212(RW, DW);
            loads++;
            sinceLoad = 0;
            printf("level load %d: %ux%u\n", loads, RW, RH);
        }
        float jx = 0, jy = 0;
        ffxFsr2GetJitterOffset212(&jx, &jy, f, phases);
        if (scroll) // known motion for tools/series_eval.py: +2 px x, -1 px y per frame, with this frame's jitter
        {
            const float sx = 2.0f, sy = -1.0f;
            std::vector<uint16_t> c((size_t) RW * RH * 4), m((size_t) RW * RH * 2);
            for (UINT y = 0; y < RH; y++)
                for (UINT x = 0; x < RW; x++)
                {
                    // pixel p shows the scene at p - jitter; the scene moves by (sx, sy) per frame on screen
                    const float X = x + 0.5f - jx - sx * f, Y = y + 0.5f - jy - sy * f;
                    const float r = 0.5f + 0.5f * sinf(X * 0.07f) * cosf(Y * 0.05f);
                    const float g = 0.5f + 0.5f * sinf((X + Y) * 0.031f);
                    const size_t i = (size_t) y * RW + x;
                    c[i * 4 + 0] = f2h(r);
                    c[i * 4 + 1] = f2h(g);
                    c[i * 4 + 2] = f2h(0.5f * (r + g));
                    c[i * 4 + 3] = f2h(1.0f);
                    m[i * 2 + 0] = f2h(-sx / RW); // FSR: previous position = p + mv * renderSize
                    m[i * 2 + 1] = f2h(-sy / RH);
                }
            const auto SRVs = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            upload(color, c.data(), RW * 8, RH, SRVs, SRVs);
            upload(mv, m.data(), RW * 4, RH, SRVs, SRVs);
        }
        alloc->Reset();
        list->Reset(alloc, nullptr);
        FfxFsr2DispatchDescription dd{};
        dd.commandList = ffxGetCommandListDX12_212(list);
        dd.color = ffxGetResourceDX12_212(&ctx, color, L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
        dd.depth = ffxGetResourceDX12_212(&ctx, depth, L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
        dd.motionVectors = ffxGetResourceDX12_212(&ctx, mv, L"mv", FFX_RESOURCE_STATE_COMPUTE_READ);
        dd.exposure = ffxGetResourceDX12_212(&ctx, nullptr, L"exposure");
        dd.output = ffxGetResourceDX12_212(&ctx, out, L"output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dd.jitterOffset = {jx, jy};
        dd.motionVectorScale = {(float) RW, (float) RH};
        dd.renderSize = {RW, RH};
        dd.enableSharpening = false;
        dd.frameTimeDelta = (float) msPer;
        dd.preExposure = 1.0f;
        dd.reset = sinceLoad++ == 0;
        dd.cameraNear = 0.1f;
        dd.cameraFar = 1000.0f;
        dd.cameraFovAngleVertical = 1.0f;
        if (badva && f % 10 == 0)
        {
            // positive control for RECON_VACHECK: a below-4-GB address with nothing drawn from it (the GPU never reads)
            D3D12_INDEX_BUFFER_VIEW ib{0x6006880ull, 64, DXGI_FORMAT_R16_UINT};
            list->IASetIndexBuffer(&ib);
        }
        FfxErrorCode e = ffxFsr2ContextDispatch212(&ctx, &dd);
        if (e != FFX_OK) printf("dispatch error %d at frame %d\n", (int) e, f);
        list->Close();
        ID3D12CommandList* l[] = {list};
        queue->ExecuteCommandLists(1, l);
        waitGpu();
        Sleep(msPer);
    }
    if (getenv("HARNESS_DEBUG"))
    {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&iq))))
        {
            UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n && i < 12; i++)
            {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                std::vector<char> buf(len);
                auto* m = (D3D12_MESSAGE*) buf.data();
                iq->GetMessage(i, m, &len);
                printf("D3D12 %d: %s\n", (int) m->Severity, m->pDescription);
            }
        }
    }
    teardown();
    printf("harness done: %d frames %ux%u -> %ux%u, device status 0x%08x\n", frames, RW, RH, DW, DH,
           (unsigned) dev->GetDeviceRemovedReason());
    return 0;
}
