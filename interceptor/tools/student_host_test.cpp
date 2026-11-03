// PC stand for the interceptor's Vela AI host (src/student_vk.cpp, SGSR3StudentVk) against the reference executor of
// (src/exec.h, as vela_exec runs it): both on one Vulkan device, the same synthetic inputs and Params
// (vela_exec's pattern, pan and jitter), frame after frame from a reset; output (RGBA16F) and history compared byte
// for byte. vkd3d-proton's interop is replaced by a stub: the "game" images are plain Vulkan images behind fake
// ID3D12Resource objects, the "command list" is a Vulkan command buffer of ours.
//
//   student_host_test.exe <exec spv dir> <plan> [-r WxH] [-o WxH] [-n frames] [--lowmv] [--host-amax W]
//     --host-amax: the host's Params.amax only (a negative control: the comparison must see the difference)
//     <exec spv dir>: gen_<model>_<file>.spv of the recipe compiled as gen_student_spv.py compiles them (tools/
//     student_host_test.ps1 does it), the host runs the same recipe from the header it was built with.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include "../../19-adreno-codegen/src/exec.h"
#include "../src/student_vk.h"

#include <cstdio>
#include <map>

// ---- stub D3D12 objects (only the calls student_vk.cpp makes) ---------------------------------------------------
template <int I>
static HRESULT STDMETHODCALLTYPE Trap(void*)
{
    fprintf(stderr, "stub: unexpected vtable slot %d\n", I);
    exit(3);
}
template <int... I>
static void FillTraps(void** v, std::integer_sequence<int, I...>)
{
    ((v[I] = (void*) &Trap<I>), ...);
}

struct FakeRes
{
    void** vtbl;
    bool buffer = false;
    Buffer buf {};
    Image img {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    D3D12_RESOURCE_DESC desc {};
};
static ULONG STDMETHODCALLTYPE ResAddRef(FakeRes*) { return 1; }
static ULONG STDMETHODCALLTYPE ResRelease(FakeRes*) { return 0; }
static HRESULT STDMETHODCALLTYPE ResSetName(FakeRes*, LPCWSTR) { return S_OK; }
static HRESULT STDMETHODCALLTYPE ResMap(FakeRes* r, UINT, const D3D12_RANGE*, void** p)
{
    if (!r->buffer) return E_FAIL;
    *p = r->buf.map;
    return S_OK;
}
static void STDMETHODCALLTYPE ResUnmap(FakeRes*, UINT, const D3D12_RANGE*) {}
// MSVC x64: a member function returning a struct takes the return slot after `this`
static D3D12_RESOURCE_DESC* STDMETHODCALLTYPE ResGetDesc(FakeRes* r, D3D12_RESOURCE_DESC* ret)
{
    *ret = r->desc;
    return ret;
}
static void** ResVtbl()
{
    static void* v[32];
    static bool init = false;
    if (!init)
    {
        FillTraps(v, std::make_integer_sequence<int, 32>());
        v[1] = (void*) &ResAddRef;
        v[2] = (void*) &ResRelease;
        v[6] = (void*) &ResSetName;
        v[8] = (void*) &ResMap;
        v[9] = (void*) &ResUnmap;
        v[10] = (void*) &ResGetDesc;
        init = true;
    }
    return v;
}
static FakeRes* WrapImage(const Image& im, VkImageLayout layout)
{
    auto* r = new FakeRes;
    r->vtbl = ResVtbl();
    r->img = im;
    r->layout = layout;
    r->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    r->desc.Width = im.w;
    r->desc.Height = im.h;
    return r;
}

static VkCommandBuffer g_cur = VK_NULL_HANDLE;  // the "command list" being recorded

struct FakeList
{
    void** vtbl;
};
static void STDMETHODCALLTYPE ListCopyBufferRegion(FakeList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) {}
static void** ListVtbl()
{
    static void* v[96];
    static bool init = false;
    if (!init)
    {
        FillTraps(v, std::make_integer_sequence<int, 96>());
        v[15] = (void*) &ListCopyBufferRegion;
        init = true;
    }
    return v;
}

// the features create_device enabled, as vkd3d-proton reports its own
static VkPhysicalDeviceVulkan13Features g_f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
static VkPhysicalDeviceVulkan12Features g_f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &g_f13 };
static VkPhysicalDeviceFeatures2 g_f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &g_f12 };

class FakeInterop : public ID3D12DXVKInteropDevice1
{
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT*, const char**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT*, const char**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** f) override
    {
        *f = &g_f2;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance* i, VkPhysicalDevice* p, VkDevice* d) override
    {
        *i = g_inst;
        *p = g_phys;
        *d = g_dev;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue*, VkQueue*, UINT32*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource* r, D3D12_RESOURCE_STATES, VkImageLayout* l) override
    {
        *l = ((FakeRes*) r)->layout;
    }
    HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource* r, UINT64* h, UINT64* o) override
    {
        return GetVulkanResourceInfo1(r, h, o, nullptr);
    }
    HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo1(ID3D12Resource* res, UINT64* h, UINT64* o, VkFormat* f) override
    {
        auto* r = (FakeRes*) res;
        if (r->buffer)
        {
            *h = (UINT64) r->buf.buf;
            if (f) *f = VK_FORMAT_UNDEFINED;
        }
        else
        {
            *h = (UINT64) r->img.img;
            if (f) *f = r->img.fmt;
        }
        *o = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateInteropCommandQueue(const D3D12_COMMAND_QUEUE_DESC*, UINT32, ID3D12CommandQueue**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateInteropCommandAllocator(D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE BeginVkCommandBufferInterop(ID3D12CommandList*, VkCommandBuffer* cb) override
    {
        *cb = g_cur;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndVkCommandBufferInterop(ID3D12CommandList*) override { return S_OK; }
};
static FakeInterop g_interop;

struct FakeDevice
{
    void** vtbl;
};
static HRESULT STDMETHODCALLTYPE DevQI(FakeDevice*, REFIID, void** p)
{
    *p = &g_interop;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE DevRelease(FakeDevice*) { return 1; }
static HRESULT STDMETHODCALLTYPE DevCreateCommitted(FakeDevice*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                    const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE*, REFIID, void** pp)
{
    if (d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return E_FAIL;  // the dump mirrors: not used here
    auto* r = new FakeRes;
    r->vtbl = ResVtbl();
    r->buffer = true;
    r->desc = *d;
    r->buf = make_host_buffer(d->Width, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    *pp = r;
    return S_OK;
}
static void** DevVtbl()
{
    static void* v[64];
    static bool init = false;
    if (!init)
    {
        FillTraps(v, std::make_integer_sequence<int, 64>());
        v[0] = (void*) &DevQI;
        v[2] = (void*) &DevRelease;
        v[27] = (void*) &DevCreateCommitted;
        init = true;
    }
    return v;
}

static Image Wrap(VkImage img, VkFormat f, uint32_t w, uint32_t h)
{
    Image im;
    im.img = img;
    im.fmt = f;
    im.w = w;
    im.h = h;
    return im;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <exec spv dir> <plan> [-r WxH] [-o WxH] [-n frames] [--lowmv]\n", argv[0]);
        return 2;
    }
    g_spv = argv[1];
    const std::string plan = argv[2];
    uint32_t rw = 960, rh = 540, ow = 1920, oh = 1080;
    int frames = 8;
    bool lowmv = false;
    float hostAmax = 0.0f;
    for (int i = 3; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "-r" && i + 1 < argc) sscanf(argv[++i], "%ux%u", &rw, &rh);
        else if (a == "-o" && i + 1 < argc) sscanf(argv[++i], "%ux%u", &ow, &oh);
        else if (a == "-n" && i + 1 < argc) frames = atoi(argv[++i]);
        else if (a == "--lowmv") lowmv = true;
        else if (a == "--host-amax" && i + 1 < argc) hostAmax = (float) atof(argv[++i]);  // negative control
    }
    g_want_extfmt = true;
    g_want_intdot = true;
    create_device("student_host_test");
    g_f13.dynamicRendering = VK_TRUE;
    g_f13.shaderIntegerDotProduct = g_have_intdot;
    g_f12.shaderFloat16 = VK_TRUE;
    {
        VkPhysicalDeviceFeatures base {};
        vkGetPhysicalDeviceFeatures(g_phys, &base);
        g_f2.features.shaderStorageImageExtendedFormats = base.shaderStorageImageExtendedFormats;
        g_f2.features.fragmentStoresAndAtomics = base.fragmentStoresAndAtomics;
    }
    printf("shaderIntegerDotProduct %d\n", (int) g_have_intdot);

    // inputs as vela_exec makes them
    const VkImageUsageFlags SMP = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::vector<uint8_t> rgba((size_t) rw * rh * 4);
    std::vector<float> depth((size_t) rw * rh);
    for (uint32_t y = 0; y < rh; y++)
        for (uint32_t x = 0; x < rw; x++)
        {
            size_t i = (size_t) y * rw + x;
            rgba[i * 4 + 0] = (uint8_t) ((x ^ y) & 0xFF);
            rgba[i * 4 + 1] = (uint8_t) (x * 255 / rw);
            rgba[i * 4 + 2] = (uint8_t) (y * 255 / rh);
            rgba[i * 4 + 3] = 255;
            depth[i] = 0.5f + 0.4f * std::sin(x * 0.01f) * std::cos(y * 0.013f);
        }
    Image in_color = make_image(rw, rh, VK_FORMAT_R8G8B8A8_UNORM, SMP | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image in_depth = make_image(rw, rh, VK_FORMAT_R32_SFLOAT, SMP);
    upload(in_color, rgba.data(), rgba.size(), RO);
    upload(in_depth, depth.data(), depth.size() * 4, RO);
    uint32_t mw = lowmv ? rw : ow, mh = lowmv ? rh : oh;
    Image mv = make_image(mw, mh, VK_FORMAT_R16G16_SFLOAT, SMP);
    std::vector<uint16_t> mvf((size_t) mw * mh * 2);
    for (size_t k = 0; k < mvf.size(); k += 2)
    {
        mvf[k] = float_to_half(lowmv ? 1.5f / rw : 1.5f);
        mvf[k + 1] = float_to_half(lowmv ? -0.5f / rh : -0.5f);
    }
    upload(mv, mvf.data(), mvf.size() * 2, RO);
    Image react = make_image(rw, rh, VK_FORMAT_R8_UNORM, SMP), tc = make_image(rw, rh, VK_FORMAT_R8_UNORM, SMP);
    std::vector<uint8_t> mask((size_t) rw * rh);
    for (size_t k = 0; k < mask.size(); k++) mask[k] = (k % 97 == 0) ? 200 : 0;
    upload(react, mask.data(), mask.size(), RO);
    upload(tc, mask.data(), mask.size(), RO);

    // the reference: the reference executor, output RGBA16F (as the replay)
    Exec R;
    if (!R.load(plan))
    {
        fprintf(stderr, "cannot read %s\n", plan.c_str());
        return 1;
    }
    R.out_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    R.init(rw, rh, ow, oh, in_color, in_depth, mv, react, tc, false);
    printf("reference %s: %zu passes + resolve\n", R.model.c_str(), R.passes.size());

    // ours: the host behind the stub interop; the game's output RGBA16F with storage (imageStore from the resolve)
    Image ours_out = make_image(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    init_layout(ours_out, VK_IMAGE_LAYOUT_GENERAL);
    FakeDevice dev { DevVtbl() };
    FakeList list { ListVtbl() };
    SGSR3StudentVk st;
    if (!st.Init((ID3D12Device*) &dev, rw, rh, ow, oh, false))
    {
        fprintf(stderr, "host Init failed: %s\n", st.Info().c_str());
        return 1;
    }
    printf("host: %s\n", st.Info().c_str());
    if (st.ModelName() != R.model)
        printf("WARNING: host runs %s, the plan is %s\n", st.ModelName(), R.model.c_str());
    using In = SGSR3StudentVk::Input;
    In cColor { (ID3D12Resource*) WrapImage(in_color, RO) }, cDepth { (ID3D12Resource*) WrapImage(in_depth, RO) },
        cMv { (ID3D12Resource*) WrapImage(mv, RO) }, cReact { (ID3D12Resource*) WrapImage(react, RO) },
        cTc { (ID3D12Resource*) WrapImage(tc, RO) }, cOut { (ID3D12Resource*) WrapImage(ours_out, VK_IMAGE_LAYOUT_GENERAL) };

    auto halton = [](int i, int b) {
        float f = 1, r = 0;
        for (int k = i; k > 0; k /= b)
        {
            f /= b;
            r += f * (k % b);
        }
        return r;
    };
    int bad = 0;
    for (int f = 0; f < frames; f++)
    {
        const int ph = f % 8 + 1;
        const float jx = halton(ph, 2) - 0.5f, jy = halton(ph, 3) - 0.5f;
        const float mvx = lowmv ? (float) rw : 1.0f / ow, mvy = lowmv ? (float) rh : 1.0f / oh;
        R.set_frame(jx, jy, mvx, mvy, 1.0f, true, false, lowmv, f == 0, true, true);
        VkCommandBuffer cb = begin_cmd();
        R.record(cb, [](int) {});
        submit_wait(cb);

        SGSR3StudentVk::Params p {};
        p.renderSize[0] = (int32_t) rw;
        p.renderSize[1] = (int32_t) rh;
        p.displaySize[0] = (int32_t) ow;
        p.displaySize[1] = (int32_t) oh;
        p.jitter[0] = jx;
        p.jitter[1] = jy;
        p.mvToUv[0] = mvx;
        p.mvToUv[1] = mvy;
        p.renderRcp[0] = 1.0f / rw;
        p.renderRcp[1] = 1.0f / rh;
        p.displayRcp[0] = 1.0f / ow;
        p.displayRcp[1] = 1.0f / oh;
        p.preExposure = 1.0f;
        p.invScale = (float) (1.0 / ((double) ow / (double) rw));
        p.flags = 1u | (lowmv ? 4u : 0u) | 16u | 32u;
        p.alphaK = 1.0f;
        p.mk = 8.0f;
        p.amax = hostAmax;
        g_cur = begin_cmd();
        if (!st.Evaluate((ID3D12GraphicsCommandList*) &list, cColor, cDepth, cMv, cReact, cTc, cOut, p, f == 0))
        {
            fprintf(stderr, "host Evaluate failed at frame %d\n", f);
            return 1;
        }
        submit_wait(g_cur);

        auto a = readback(R.out, VK_IMAGE_LAYOUT_GENERAL, 8), b = readback(ours_out, VK_IMAGE_LAYOUT_GENERAL, 8);
        auto ha = readback(R.hrgb[R.parity], VK_IMAGE_LAYOUT_GENERAL, 4),
             hb = readback(Wrap(st.WrittenHistoryImage(false), VK_FORMAT_B10G11R11_UFLOAT_PACK32, ow, oh),
                           VK_IMAGE_LAYOUT_GENERAL, 4);
        const uint32_t sb = R.hstate[0].fmt == VK_FORMAT_R8G8B8A8_UNORM ? 4 : R.hstate[0].fmt == VK_FORMAT_R8G8_UNORM ? 2 : 1;
        auto sa = readback(R.hstate[R.parity], VK_IMAGE_LAYOUT_GENERAL, sb),
             sbb = readback(Wrap(st.WrittenHistoryImage(true), R.hstate[0].fmt, ow, oh), VK_IMAGE_LAYOUT_GENERAL, sb);
        size_t dOut = 0, dHist = 0, dState = 0;
        for (size_t i = 0; i < a.size(); i += 8)
            dOut += memcmp(&a[i], &b[i], 6) != 0;  // RGB (alpha: 1 in ours, the recipe's in the reference)
        for (size_t i = 0; i < ha.size(); i++) dHist += ha[i] != hb[i];
        for (size_t i = 0; i < sa.size(); i++) dState += sa[i] != sbb[i];
        double sum = 0;
        for (size_t i = 0; i < a.size(); i += 8) sum += half_to_float(*(const uint16_t*) &a[i]);
        printf("frame %d: output %zu of %zu px differ, history %zu bytes, state %zu bytes (output mean R %.4f)\n", f, dOut,
               a.size() / 8, dHist, dState, sum / (a.size() / 8));
        bad += dOut != 0 || dHist != 0 || dState != 0;
    }
    printf("%s: %d frames, %s\n", R.model.c_str(), frames, bad ? "DIFFERENT" : "host == reference, bit for bit");
    vkDeviceWaitIdle(g_dev);
    return bad ? 1 : 0;
}
