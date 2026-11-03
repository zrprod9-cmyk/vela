// SGSR 2 (Vela 0.8.13): model sgsr2 in the RE4 interceptor.
// Snapdragon(TM) Game Super Resolution 2, 2-pass compute variant (convert, upscale), Copyright (c) 2023-2024 Qualcomm
// Innovation Center, Inc., BSD-3-Clause (shaders/SGSR2_LICENSE; shaders/sgsr2_{convert,upscale}.comp, snapdragon-gsr
// d926f07 sgsr/v2 glsl_2_pass_cs with the changes listed at the top of each shader).
// Runs on vkd3d-proton's own VkDevice through the interop on the game's command list, like hdr_wrap.cpp: classic
// descriptor sets (vkCmdBindDescriptorSets) and push constants; vkd3d invalidates its pipeline / root state at
// BeginVkCommandBufferInterop and sets them again at the game's next dispatch. As for FSR 4, the game's descriptor
// heaps are set again afterwards (dummy heap, then the game's) so vkd3d also binds its descriptor buffers again.
// Inputs as the other models get them (RunExt): colour (the interceptor's reversible tonemap, preExposure 1), depth
// (RE4's R32 copy, reverse-Z), render-size MVs (FFX: prev = cur + mv * mvScale / renderSize), jitter (FFX convention,
// render pixels), reset. Output: our RGBA16F display-size texture, which Deliver takes back to the game's FSR output.
#include "ext_upscalers.h"
#ifndef VELA_NO_VULKAN
#include "interop_vkd3d.h"
#include "log.h"
#include "sgsr2_spv.h"
#include <cmath>
#include <cstdlib>
#include <deque>
#include <utility>

namespace Ext
{
namespace
{
struct PushC // std430 push constants of both shaders (80 bytes)
{
    uint32_t renderSize[2], displaySize[2];
    float renderSizeRcp[2], displaySizeRcp[2], jitter[2], mvScale[2];
    float preExposure, cameraFovAngleHor, cameraNear, minLerp;
    uint32_t sameCamera, reset, depthInverted, pad;
};
static_assert(sizeof(PushC) == 80, "push constant layout");

class Sgsr2 : public Upscaler
{
  public:
    bool Init(ID3D12Device* dev)
    {
        _dev = dev;
        if (FAILED(dev->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), (void**) &_interop)) || !_interop)
            return Fail("no vkd3d-proton interop");
        VkInstance inst = VK_NULL_HANDLE;
        if (FAILED(_interop->GetVulkanHandles(&inst, &_phys, &_vk)) || !_vk) return Fail("GetVulkanHandles failed");
        // convert: 1 colour, 2 depth, 3 mv (sampled), 4 motion/depthclip/alpha, 5 YCoCg (storage)
        // upscale: 7 history in, 8 motion/depthclip/alpha, 9 YCoCg (sampled), 10 scene out, 11 history out (storage)
        const uint32_t nb[2] = {5, 5};
        const uint32_t first[2] = {1, 7};
        for (int p = 0; p < 2; p++)
        {
            VkDescriptorSetLayoutBinding b[5] = {};
            for (uint32_t i = 0; i < nb[p]; i++)
                b[i] = {first[p] + i, i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dl.bindingCount = nb[p];
            dl.pBindings = b;
            if (vkCreateDescriptorSetLayout(_vk, &dl, nullptr, &_dsl[p]) != VK_SUCCESS) return Fail("set layout");
            VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushC)};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &_dsl[p];
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pr;
            if (vkCreatePipelineLayout(_vk, &pli, nullptr, &_pl[p]) != VK_SUCCESS) return Fail("pipeline layout");
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = p ? sizeof(sgsr2_upscale_spv) : sizeof(sgsr2_convert_spv);
            mi.pCode = p ? sgsr2_upscale_spv : sgsr2_convert_spv;
            VkShaderModule sm = VK_NULL_HANDLE;
            if (vkCreateShaderModule(_vk, &mi, nullptr, &sm) != VK_SUCCESS) return Fail("shader module");
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"};
            ci.layout = _pl[p];
            const VkResult vr = vkCreateComputePipelines(_vk, VK_NULL_HANDLE, 1, &ci, nullptr, &_pipe[p]);
            vkDestroyShaderModule(_vk, sm, nullptr);
            if (vr != VK_SUCCESS) return Fail("compute pipeline");
        }
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * Ring}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * Ring}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 2 * Ring;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(_vk, &dpi, nullptr, &_pool) != VK_SUCCESS) return Fail("descriptor pool");
        for (int p = 0; p < 2; p++)
        {
            VkDescriptorSetLayout ls[Ring];
            for (auto& l : ls) l = _dsl[p];
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = _pool;
            ai.descriptorSetCount = Ring;
            ai.pSetLayouts = ls;
            if (vkAllocateDescriptorSets(_vk, &ai, _set[p]) != VK_SUCCESS) return Fail("descriptor sets");
        }
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(_vk, &si, nullptr, &_point) != VK_SUCCESS) return Fail("sampler");
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(_vk, &si, nullptr, &_linear) != VK_SUCCESS) return Fail("sampler");
        // own images as D3D12 UAV textures (vkd3d: VK_IMAGE_LAYOUT_GENERAL, storage + sampled)
        if (!OwnImage(rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, _mda) || !OwnImage(rw, rh, DXGI_FORMAT_R32_UINT, _ycc) ||
            !OwnImage(dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, _hist[0]) ||
            !OwnImage(dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, _hist[1]))
            return Fail("own textures");
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (int k = 0; k < 2; k++)
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    k ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                    nullptr, IID_PPV_ARGS(&_flush[k]))))
                return Fail("flush buffers");
        for (int k = 0; k < 2; k++)
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = k ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 1;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_dummyHeap[k])))) return Fail("dummy heaps");
        }
        {
            VkPhysicalDeviceProperties pr{};
            vkGetPhysicalDeviceProperties(_phys, &pr);
            _tsNs = pr.limits.timestampPeriod;
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 * kTsRing;
            if (vkCreateQueryPool(_vk, &qi, nullptr, &_qp) != VK_SUCCESS) _qp = VK_NULL_HANDLE;
        }
        const char* e = getenv("VELA_SGSR2_MINLERP"); // SGSR 2's MinLerpContribution (OptiScaler: 0.3)
        _minLerp = e && *e ? (float) atof(e) : 0.3f;
        LOGI("vela: SGSR 2 context %ux%u -> %ux%u (Qualcomm SGSR 2, 2-pass compute, Vulkan interop)", rw, rh, dw, dh);
        return true;
    }

    bool Evaluate(ID3D12GraphicsCommandList* cl, const Tex& color, const Tex& depth, const Tex& mv, const Tex& out,
                  const Frame& f) override
    {
        if (!_pipe[1]) return false;
        while (!_retiring.empty() && _retiring.front().first + 2 * Ring <= _frames)
        {
            vkDestroyImageView(_vk, _retiring.front().second, nullptr);
            _retiring.pop_front();
        }
        VkImageLayout lc, ld, lm, lo;
        const VkImageView vc = View(color, lc), vd = View(depth, ld), vm = View(mv, lm), vo = View(out, lo);
        if (!vc || !vd || !vm || !vo)
        {
            if (_failLogged++ < 3) LOGI("vela: SGSR 2: no Vulkan view of an input (colour %p depth %p mv %p out %p)", (void*) vc, (void*) vd, (void*) vm, (void*) vo);
            return false;
        }
        const bool reset = f.reset || _frames == 0;
        const uint32_t slot = (uint32_t) (_frames % Ring), p = (uint32_t) (_frames & 1); // read hist[p], write hist[1-p]
        const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo ci[5] = {{_point, vc, lc}, {_point, vd, ld}, {_point, vm, lm}, {VK_NULL_HANDLE, _mda.view, G},
                                       {VK_NULL_HANDLE, _ycc.view, G}};
        VkDescriptorImageInfo ui[5] = {{_linear, _hist[p].view, G}, {_linear, _mda.view, G}, {_point, _ycc.view, G},
                                       {VK_NULL_HANDLE, vo, lo}, {VK_NULL_HANDLE, _hist[1 - p].view, G}};
        VkWriteDescriptorSet wr[10] = {};
        for (int i = 0; i < 10; i++)
        {
            const int pass = i / 5, k = i % 5;
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = _set[pass][slot];
            wr[i].dstBinding = (pass ? 7u : 1u) + (uint32_t) k;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr[i].pImageInfo = pass ? &ui[k] : &ci[k];
        }
        vkUpdateDescriptorSets(_vk, 10, wr, 0, nullptr);
        PushC pc{};
        pc.renderSize[0] = rw; pc.renderSize[1] = rh; pc.displaySize[0] = dw; pc.displaySize[1] = dh;
        pc.renderSizeRcp[0] = 1.0f / (float) rw; pc.renderSizeRcp[1] = 1.0f / (float) rh;
        pc.displaySizeRcp[0] = 1.0f / (float) dw; pc.displaySizeRcp[1] = 1.0f / (float) dh;
        pc.jitter[0] = f.jitterX; pc.jitter[1] = f.jitterY;
        // FFX: prev UV = uv + mv * mvScale / renderSize; SGSR 2 (Y down): prev UV = uv - 0.5 * Motion
        pc.mvScale[0] = -2.0f * f.mvScaleX / (float) rw;
        pc.mvScale[1] = -2.0f * f.mvScaleY / (float) rh;
        pc.preExposure = f.preExposure > 0 ? f.preExposure : 1.0f;
        pc.cameraFovAngleHor = std::tan(0.5f * f.fovV) * (float) rw / (float) rh;
        pc.cameraNear = f.depthInverted ? f.cameraFar : f.cameraNear; // RunExt swaps them for reverse-Z
        pc.minLerp = _minLerp;
        pc.sameCamera = 0;
        pc.reset = reset ? 1u : 0u;
        pc.depthInverted = f.depthInverted ? 1u : 0u;
        cl->CopyBufferRegion(_flush[1], 0, _flush[0], 0, 4); // vkd3d emits its batched barriers before the interop
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (FAILED(_interop->BeginVkCommandBufferInterop(cl, &cb)) || !cb) return false;
        const uint32_t ts = (uint32_t) (_frames % kTsRing);
        if (_qp)
        {
            vkCmdResetQueryPool(cb, _qp, 2 * ts, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _qp, 2 * ts);
        }
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        const auto barrier = [&] {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                                 nullptr, 0, nullptr);
        };
        barrier();
        for (uint32_t pass = 0; pass < 2; pass++)
        {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _pipe[pass]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _pl[pass], 0, 1, &_set[pass][slot], 0, nullptr);
            vkCmdPushConstants(cb, _pl[pass], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushC), &pc);
            const uint32_t w = pass ? dw : rw, h = pass ? dh : rh;
            vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
            barrier();
        }
        if (_qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _qp, 2 * ts + 1);
        _interop->EndVkCommandBufferInterop(cl);
        RebindGameHeaps(cl);
        if (_frames++ == 0)
            LOGI("vela: SGSR 2: first frame dispatched (colour fmt %d depth fmt %d mv fmt %d, depth %s, MinLerp %.2f)",
                 (int) _lastFmt[0], (int) _lastFmt[1], (int) _lastFmt[2], f.depthInverted ? "reverse-Z" : "forward", _minLerp);
        return true;
    }

    bool LastGpuMs(double& ms) override
    {
        if (!_qp || _frames < kTsRing) return false;
        const uint32_t slot = (uint32_t) ((_frames + 1) % kTsRing);
        uint64_t t[2] = {};
        if (vkGetQueryPoolResults(_vk, _qp, 2 * slot, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
            t[1] <= t[0])
            return false;
        ms = (double) (t[1] - t[0]) * _tsNs / 1e6;
        return true;
    }

    void Release() override
    {
        if (_vk) vkDeviceWaitIdle(_vk); // our command buffers in flight
        for (auto& r : _retiring) vkDestroyImageView(_vk, r.second, nullptr);
        _retiring.clear();
        for (Own* o : {&_mda, &_ycc, &_hist[0], &_hist[1]})
        {
            if (o->view) vkDestroyImageView(_vk, o->view, nullptr);
            if (o->res) o->res->Release();
            *o = Own{};
        }
        for (int p = 0; p < 2; p++)
        {
            if (_pipe[p]) vkDestroyPipeline(_vk, _pipe[p], nullptr);
            if (_pl[p]) vkDestroyPipelineLayout(_vk, _pl[p], nullptr);
            if (_dsl[p]) vkDestroyDescriptorSetLayout(_vk, _dsl[p], nullptr);
            _pipe[p] = VK_NULL_HANDLE; _pl[p] = VK_NULL_HANDLE; _dsl[p] = VK_NULL_HANDLE;
        }
        if (_pool) vkDestroyDescriptorPool(_vk, _pool, nullptr);
        if (_point) vkDestroySampler(_vk, _point, nullptr);
        if (_linear) vkDestroySampler(_vk, _linear, nullptr);
        if (_qp) vkDestroyQueryPool(_vk, _qp, nullptr);
        _pool = VK_NULL_HANDLE; _point = _linear = VK_NULL_HANDLE; _qp = VK_NULL_HANDLE;
        for (auto*& b : _flush) if (b) { b->Release(); b = nullptr; }
        for (auto*& h : _dummyHeap) if (h) { h->Release(); h = nullptr; }
        if (_interop) { _interop->Release(); _interop = nullptr; }
    }
    ~Sgsr2() override { Release(); }

  private:
    static constexpr uint32_t Ring = 16, kTsRing = 8;
    struct Own
    {
        ID3D12Resource* res = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    bool Fail(const char* why)
    {
        LOGI("vela: SGSR 2: %s", why);
        return false;
    }
    bool OwnImage(uint32_t w, uint32_t h, DXGI_FORMAT fmt, Own& o)
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 nullptr, IID_PPV_ARGS(&o.res))))
            return false;
        UINT64 handle = 0, off = 0;
        VkFormat vf = VK_FORMAT_UNDEFINED;
        if (FAILED(_interop->GetVulkanResourceInfo1(o.res, &handle, &off, &vf)) || !handle) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = (VkImage) handle;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vf;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(_vk, &vi, nullptr, &o.view) == VK_SUCCESS;
    }
    // a view of a game image for this frame only (an image may be freed and its handle reused on a level load)
    VkImageView View(const Tex& t, VkImageLayout& layout)
    {
        UINT64 h = 0, off = 0;
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        if (!t.res || FAILED(_interop->GetVulkanResourceInfo1(t.res, &h, &off, &fmt)) || !h) return VK_NULL_HANDLE;
        _interop->GetVulkanImageLayout(t.res, t.state, &layout);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = (VkImage) h;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        const bool dep = fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                         fmt == VK_FORMAT_D16_UNORM;
        vi.subresourceRange = {dep ? (VkImageAspectFlags) VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags) VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(_vk, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
        _retiring.push_back({_frames, v});
        if (_nView < 3) _lastFmt[_nView] = fmt;
        _nView = (_nView + 1) % 4;
        return v;
    }
    // as FSR 4: the game's heaps set again through a dummy heap so vkd3d binds its descriptor buffers again
    void RebindGameHeaps(ID3D12GraphicsCommandList* cl)
    {
        ID3D12DescriptorHeap* game[2] = {};
        const UINT n = ListHeaps(cl, game);
        if (n == 0 || !game[0] || (n == 2 && !game[1]))
        {
            if (_heapWarn++ == 0) LOGI("vela: SGSR 2: no descriptor heaps recorded on the list; heaps not re-bound");
            return;
        }
        ID3D12DescriptorHeap* dummy[2] = {};
        for (UINT k = 0; k < n; k++)
            dummy[k] = _dummyHeap[game[k]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0];
        cl->SetDescriptorHeaps(n, dummy);
        cl->SetDescriptorHeaps(n, game);
        if (_heapRebinds++ == 0) LOGI("vela: SGSR 2: game descriptor heaps re-bound after its passes (%u heaps)", n);
    }

    ID3D12Device* _dev = nullptr;
    ID3D12DXVKInteropDevice1* _interop = nullptr;
    VkDevice _vk = VK_NULL_HANDLE;
    VkPhysicalDevice _phys = VK_NULL_HANDLE;
    VkDescriptorSetLayout _dsl[2] = {};
    VkPipelineLayout _pl[2] = {};
    VkPipeline _pipe[2] = {};
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set[2][Ring] = {};
    VkSampler _point = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    Own _mda, _ycc, _hist[2];
    ID3D12Resource* _flush[2] = {};
    ID3D12DescriptorHeap* _dummyHeap[2] = {};
    std::deque<std::pair<uint64_t, VkImageView>> _retiring;
    VkQueryPool _qp = VK_NULL_HANDLE;
    double _tsNs = 1.0;
    uint64_t _frames = 0, _heapRebinds = 0;
    int _failLogged = 0, _heapWarn = 0, _nView = 0;
    VkFormat _lastFmt[3] = {};
    float _minLerp = 0.3f;
};
} // namespace

Upscaler* CreateSgsr2(ID3D12Device* dev, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh)
{
    auto* s = new Sgsr2();
    s->rw = rw; s->rh = rh; s->dw = dw; s->dh = dh;
    if (!s->Init(dev)) { delete s; return nullptr; }
    return s;
}
} // namespace Ext
#endif // VELA_NO_VULKAN
