#include <windows.h>
#include <d3d12.h>
#include "student_vk.h"
#include "log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>

// The generated headers, each in its own namespace (they define the same names). Their #include <cstdint> /
// <cstddef> are no-ops here (included above).
namespace st_main
{
#include "SGSR3_Student_spv.h"
}
#if __has_include("SGSR3_Student_spv_fallback.h")
namespace st_fb
{
#include "SGSR3_Student_spv_fallback.h"
}
#define ST_HAVE_FALLBACK 1
#endif

namespace
{
// format 2 members (ppx / ppy / fragment) where the header has them
template <class D>
auto PassExtra(const D& d, StModelPass& p, int) -> decltype((void) d.fragment, void())
{
    p.ppx = d.ppx > 0 ? d.ppx : 1;
    p.ppy = d.ppy > 0 ? d.ppy : 1;
    p.fragment = d.fragment;
}
template <class D>
void PassExtra(const D&, StModelPass&, long)
{
}
template <class T>
auto TensorPp(const T& t, int) -> decltype((bool) t.pp)
{
    return t.pp;
}
template <class T>
bool TensorPp(const T&, long)
{
    return false;
}
template <class D>
StModelPass ToPass(const D& d)
{
    StModelPass p;
    p.file = d.file;
    p.outLevel = d.outLevel;
    p.inLevel = d.inLevel;
    for (int i = 0; i < d.nIn; i++)
        p.in.push_back({ d.in[i].name, d.in[i].linear });
    for (int i = 0; i < d.nOut; i++)
        p.out.push_back(d.out[i]);
    p.spv = d.spv;
    p.spvBytes = d.spvBytes;
    p.wgx = d.wgx > 0 ? d.wgx : 8;
    p.wgy = d.wgy > 0 ? d.wgy : 8;
    PassExtra(d, p, 0);
    return p;
}
// OpCapability DotProduct (6018): the INT8 passes (SPV_KHR_integer_dot_product)
bool UsesIntDot(const uint32_t* spv, size_t bytes)
{
    for (size_t i = 5; i + 1 < bytes / 4;)
    {
        const uint32_t op = spv[i] & 0xFFFF, n = spv[i] >> 16;
        if (op == 17 && n == 2 && (spv[i + 1] == 6018 || spv[i + 1] == 6019))
            return true;
        if (op == 54 || n == 0)  // OpFunction: past the capabilities
            break;
        i += n;
    }
    return false;
}
template <class T, size_t NT, class P, size_t NP, class R>
StModel MakeModel(const char* name, float mk, int stateCh, const T (&tensors)[NT], const P (&passes)[NP],
                  const R& resolve, const uint32_t* vs, size_t vsBytes, const uint32_t* mrt, size_t mrtBytes,
                  const uint32_t* blit, size_t blitBytes)
{
    StModel m;
    m.name = name;
    m.defaultMk = mk;
    m.stateChannels = stateCh;
    for (const auto& t : tensors)
    {
        m.tensors.push_back({ t.name, t.level, t.fmt, t.channels, TensorPp(t, 0) });
        const std::string f = t.fmt;
        m.format2 = m.format2 || f == "q8" || f == "u8n" || f == "s8n" || f == "r32f" || f == "rg16f" ||
                    m.tensors.back().pp;
        m.extFormats = m.extFormats || (f == "q8" && t.channels % 16 > 4 && t.channels % 16 <= 8);
    }
    for (const auto& d : passes)
    {
        m.passes.push_back(ToPass(d));
        m.format2 = m.format2 || m.passes.back().fragment;
        m.intDot = m.intDot || UsesIntDot(d.spv, d.spvBytes);
    }
    m.resolve = ToPass(resolve);
    m.vs = vs;
    m.vsBytes = vsBytes;
    m.mrt = mrt;
    m.mrtBytes = mrtBytes;
    m.blit = blit;
    m.blitBytes = blitBytes;
    return m;
}
#define ST_MAKE_MODEL(NS)                                                                                            \
    MakeModel(NS::st_model_name, NS::st_default_mk, NS::st_state_channels, NS::st_tensors, NS::st_passes,            \
              NS::st_resolve, NS::st_spv_fullscreen_vert, sizeof(NS::st_spv_fullscreen_vert), NS::st_spv_resolve_mrt, \
              sizeof(NS::st_spv_resolve_mrt), NS::st_spv_blit_comp, sizeof(NS::st_spv_blit_comp))

// images of a tensor: (NAME.i, format). f16 / r11 / r16f / u8n: 4 channels per image; q8 (packed uint8 for the INT8
// passes): 16 channels per RGBA32UI, the remainder > 8 RGBA32UI, > 4 RG32UI, else R32UI (gen_mix.py q8_images)
std::vector<std::pair<std::string, VkFormat>> TensorImages(const StModelTensor& t)
{
    std::vector<std::pair<std::string, VkFormat>> v;
    if (t.fmt == "q8")
    {
        int i = 0;
        for (int c = t.channels; c > 0; c -= 16, i++)
            v.push_back({ t.name + "." + std::to_string(i), c > 8   ? VK_FORMAT_R32G32B32A32_UINT
                                                            : c > 4 ? VK_FORMAT_R32G32_UINT
                                                                    : VK_FORMAT_R32_UINT });
        return v;
    }
    const VkFormat f = t.fmt == "r11"    ? VK_FORMAT_B10G11R11_UFLOAT_PACK32
                       : t.fmt == "r16f"  ? VK_FORMAT_R16_SFLOAT
                       : t.fmt == "r32f"  ? VK_FORMAT_R32_SFLOAT
                       : t.fmt == "rg16f" ? VK_FORMAT_R16G16_SFLOAT
                       : t.fmt == "u8n"   ? VK_FORMAT_R8G8B8A8_UNORM
                       : t.fmt == "s8n"   ? VK_FORMAT_R8G8B8A8_SNORM
                                          : VK_FORMAT_R16G16B16A16_SFLOAT;
    for (int i = 0; i < t.channels / 4; i++)
        v.push_back({ t.name + "." + std::to_string(i), f });
    return v;
}
bool IsUintFormat(VkFormat f)
{
    return f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32G32_UINT || f == VK_FORMAT_R32G32B32A32_UINT;
}
} // namespace

const StModel& StMainModel()
{
    static const StModel m = ST_MAKE_MODEL(st_main);
    return m;
}
const StModel* StFallbackModel()
{
#ifdef ST_HAVE_FALLBACK
    static const StModel m = ST_MAKE_MODEL(st_fb);
    return &m;
#else
    return nullptr;
#endif
}

// (OptiScaler Util.h) only used for debug names of our images
static std::wstring string_to_wstring(const std::string& s) { return std::wstring(s.begin(), s.end()); }

static ID3D12Resource* CreateTex(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool rt,
                                 const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | (rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                                                : D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(name);
    return r;
}

static ID3D12Resource* CreateBuf(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

void SGSR3StudentVk::LevelSize(const char* level, uint32_t& w, uint32_t& h) const
{
    const std::string l = level;
    if (l == "R")
    {
        w = _rw;
        h = _rh;
    }
    else if (l == "L1")
    {
        w = _L1[0];
        h = _L1[1];
    }
    else if (l == "L2")
    {
        w = _L2[0];
        h = _L2[1];
    }
    else if (l == "L3")
    {
        w = _L3[0];
        h = _L3[1];
    }
    else
    {
        w = _dw;
        h = _dh;
    }
}

// history state of the recipe: R8 (state), RG8 (+ counter, "acc 1"), RGBA8 (+ temporal box x_prev, A, "hp 1")
static DXGI_FORMAT StateFormat(const StModel& m)
{
    return m.stateChannels == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM
           : m.stateChannels == 2 ? DXGI_FORMAT_R8G8_UNORM
                                  : DXGI_FORMAT_R8_UNORM;
}

static VkFormat VkFormatOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R16_FLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case DXGI_FORMAT_R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
}

bool SGSR3StudentVk::MakeImage(Img& img, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool rt, const wchar_t* name)
{
    // as the bench: activations and maps are storage + sampled; the history render target + sampled, no storage
    (void) name;
    return MakeImage(img, w, h, VkFormatOf(fmt),
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         (rt ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT));
}

bool SGSR3StudentVk::MakeImage(Img& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage)
{
    img.format = format;
    img.attachment = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    img.w = w;
    img.h = h;
    img.layout = VK_IMAGE_LAYOUT_GENERAL;
    VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = img.format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_vk, &ci, nullptr, &img.image) != VK_SUCCESS)
    {
        LOG_ERROR("SGSR3 student: vkCreateImage {0}x{1} fmt {2} failed", w, h, (int) img.format);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_vk, img.image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(_physical, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(_vk, &ai, nullptr, &img.mem) != VK_SUCCESS ||
        vkBindImageMemory(_vk, img.image, img.mem, 0) != VK_SUCCESS)
    {
        LOG_ERROR("SGSR3 student: image memory ({0} bytes) failed", req.size);
        return false;
    }
    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = img.format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(_vk, &vi, nullptr, &img.view) != VK_SUCCESS)
        return false;
    _ownImages.push_back(img.image);
    return true;
}

bool SGSR3StudentVk::MakePass(Pass& p, const uint32_t* spv, size_t bytes, uint32_t nSampled, uint32_t nStorage,
                              bool fragment, const VkFormat* rtFormats, uint32_t nRt, bool pcFragment)
{
    const VkShaderStageFlags stage = fragment ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    std::vector<VkDescriptorSetLayoutBinding> lb;
    lb.push_back({ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, nullptr });
    uint32_t b = 1;
    for (uint32_t i = 0; i < nSampled; i++)
        lb.push_back({ b++, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr });
    for (uint32_t i = 0; i < nStorage; i++)
        lb.push_back({ b++, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, nullptr });
    VkDescriptorSetLayoutCreateInfo dci { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = (uint32_t) lb.size();
    dci.pBindings = lb.data();
    if (vkCreateDescriptorSetLayout(_vk, &dci, nullptr, &p.dsl) != VK_SUCCESS)
        return false;
    VkPushConstantRange pr { pcFragment ? (VkShaderStageFlags) VK_SHADER_STAGE_FRAGMENT_BIT
                                        : (VkShaderStageFlags) VK_SHADER_STAGE_COMPUTE_BIT,
                             0, 16 };
    VkPipelineLayoutCreateInfo li { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1;
    li.pSetLayouts = &p.dsl;
    li.pushConstantRangeCount = (fragment && !pcFragment) ? 0 : 1;
    li.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(_vk, &li, nullptr, &p.pl) != VK_SUCCESS)
        return false;

    VkShaderModuleCreateInfo mi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = bytes;
    mi.pCode = spv;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(_vk, &mi, nullptr, &mod) != VK_SUCCESS)
        return false;

    VkResult vr;
    if (!fragment)
    {
        VkComputePipelineCreateInfo ci { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = p.pl;
        vr = vkCreateComputePipelines(_vk, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipe);
    }
    else
    {
        VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = _vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = mod;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        std::vector<VkPipelineColorBlendAttachmentState> cba(nRt);
        for (auto& c : cba)
            c.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = nRt;
        cb.pAttachments = cba.data();
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        ri.colorAttachmentCount = nRt;
        ri.pColorAttachmentFormats = rtFormats;
        VkGraphicsPipelineCreateInfo gci { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gci.pNext = &ri;
        gci.stageCount = 2;
        gci.pStages = st;
        gci.pVertexInputState = &vin;
        gci.pInputAssemblyState = &ia;
        gci.pViewportState = &vp;
        gci.pRasterizationState = &rs;
        gci.pMultisampleState = &ms;
        gci.pColorBlendState = &cb;
        gci.pDynamicState = &ds;
        gci.layout = p.pl;
        vr = vkCreateGraphicsPipelines(_vk, VK_NULL_HANDLE, 1, &gci, nullptr, &p.pipe);
    }
    vkDestroyShaderModule(_vk, mod, nullptr);
    if (vr != VK_SUCCESS)
    {
        LOG_ERROR("SGSR3 student: pipeline creation failed: {0}", (int) vr);
        return false;
    }

    for (uint32_t k = 0; k < Ring; k++)
    {
        VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = _pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p.dsl;
        if (vkAllocateDescriptorSets(_vk, &ai, &p.set[k]) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool SGSR3StudentVk::Init(ID3D12Device* device, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, bool blitOnly)
{
    _device = device;
    _rw = rw;
    _rh = rh;
    _dw = dw;
    _dh = dh;
    _blitOnly = blitOnly;
    const uint32_t wp = (rw + 7) / 8 * 8, hp = (rh + 7) / 8 * 8;
    _L1[0] = wp / 2;
    _L1[1] = hp / 2;
    _L2[0] = wp / 4;
    _L2[1] = hp / 4;
    _L3[0] = wp / 8;
    _L3[1] = hp / 8;

    if (FAILED(device->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), (void**) &_interop)) || _interop == nullptr)
    {
        LOG_ERROR("SGSR3 student: no ID3D12DXVKInteropDevice1 (not vkd3d-proton, or too old)");
        return false;
    }
    if (FAILED(_interop->GetVulkanHandles(&_instance, &_physical, &_vk)) || _vk == VK_NULL_HANDLE)
    {
        LOG_ERROR("SGSR3 student: GetVulkanHandles failed");
        return false;
    }
    GpuPatchInit(_vk);
    _beginRendering = (PFN_vkCmdBeginRendering) vkGetDeviceProcAddr(_vk, "vkCmdBeginRendering");
    _endRendering = (PFN_vkCmdEndRendering) vkGetDeviceProcAddr(_vk, "vkCmdEndRendering");
    if (_beginRendering == nullptr)
    {
        _beginRendering = (PFN_vkCmdBeginRendering) vkGetDeviceProcAddr(_vk, "vkCmdBeginRenderingKHR");
        _endRendering = (PFN_vkCmdEndRendering) vkGetDeviceProcAddr(_vk, "vkCmdEndRenderingKHR");
    }
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_physical, &props);
    _tsPeriodNs = props.limits.timestampPeriod;

    // what vkd3d-proton enabled on its VkDevice (the student's SPIR-V declares Float16)
    bool f16 = false, dynRendering = false, haveV12 = false;
    const VkPhysicalDeviceFeatures2* feats = nullptr;
    // the recipe: the main one, or the fallback when the device lacks what the main one needs (INT8 dot products,
    // RG32UI storage, the q8 formats as storage + colour attachment); VELA_STUDENT=fallback forces the fallback
    {
        _m = &StMainModel();
        char e[16] = {};
        const bool force = GetEnvironmentVariableA("VELA_STUDENT", e, sizeof(e)) > 0 && std::string(e) == "fallback";
        std::string why;
        const VkPhysicalDeviceFeatures2* f0 = nullptr;
        _interop->GetDeviceFeatures(&f0);
        if (force || !DeviceCan(*_m, f0, why))
        {
            const StModel* fb = StFallbackModel();
            LOG_INFO("SGSR3 student: {0} {1}{2}", _m->name, force ? "skipped (VELA_STUDENT=fallback)" : "not possible: ",
                     force ? "" : why);
            if (fb == nullptr || !DeviceCan(*fb, f0, why))
            {
                LOG_ERROR("SGSR3 student: no fallback recipe{0}{1}", fb ? ": " : "", fb ? why : "");
                return false;
            }
            _m = fb;
        }
    }
    if (SUCCEEDED(_interop->GetDeviceFeatures(&feats)) && feats != nullptr)
        for (auto* s = (const VkBaseInStructure*) feats->pNext; s != nullptr; s = s->pNext)
        {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            {
                haveV12 = true;
                f16 = ((const VkPhysicalDeviceVulkan12Features*) s)->shaderFloat16 != VK_FALSE;
            }
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES)
                f16 = f16 || ((const VkPhysicalDeviceShaderFloat16Int8Features*) s)->shaderFloat16 != VK_FALSE;
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
                dynRendering = ((const VkPhysicalDeviceVulkan13Features*) s)->dynamicRendering != VK_FALSE;
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
                dynRendering = dynRendering || ((const VkPhysicalDeviceDynamicRenderingFeatures*) s)->dynamicRendering;
        }
    _info = std::string("model ") + _m->name + (_m->intDot ? " (INT8)" : "") + ", device " + props.deviceName + ", vkd3d device features: " +
            (haveV12 ? "" : "(no 1.2 struct) ") + "shaderFloat16 " + (f16 ? "on" : "OFF") + ", dynamicRendering " +
            (dynRendering ? "on" : "OFF") + (_beginRendering ? "" : ", no vkCmdBeginRendering");
    LOG_INFO("SGSR3 student: {0}", _info);
    if (_beginRendering == nullptr || _endRendering == nullptr)
        return false;

    // per ring slot: a set per pass + resolve + blit; samplers / storage images as the recipe declares them
    uint32_t nSets = 2, nSmp = 1, nSto = 2;
    for (const auto& d : _m->passes)
    {
        nSets++;
        nSmp += (uint32_t) d.in.size();
        nSto += d.fragment ? 0 : (uint32_t) d.out.size();
    }
    nSmp += (uint32_t) _m->resolve.in.size();
    nSets = std::max(nSets, 16u);
    nSmp = std::max(nSmp, 160u);
    nSto = std::max(nSto, 80u);
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nSets * Ring },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nSmp * Ring },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nSto * Ring } };
    VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = nSets * Ring;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(_vk, &dpi, nullptr, &_pool) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo si { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(_vk, &si, nullptr, &_point) != VK_SUCCESS)
        return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(_vk, &si, nullptr, &_linear) != VK_SUCCESS)
        return false;

    VkShaderModuleCreateInfo mi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = _m->vsBytes;
    mi.pCode = _m->vs;
    if (vkCreateShaderModule(_vk, &mi, nullptr, &_vs) != VK_SUCCESS)
        return false;

    VkQueryPoolCreateInfo qi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = TsPerFrame * Ring;
    if (vkCreateQueryPool(_vk, &qi, nullptr, &_queries) != VK_SUCCESS)
        _queries = VK_NULL_HANDLE;

    // UBO ring in an upload heap; its VkBuffer and offset from the interop
    _ubo = CreateBuf(device, 256 * Ring, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    _flush[0] = CreateBuf(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    _flush[1] = CreateBuf(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (_ubo == nullptr || _flush[0] == nullptr || _flush[1] == nullptr)
        return false;
    D3D12_RANGE noRead { 0, 0 };
    if (FAILED(_ubo->Map(0, &noRead, (void**) &_uboMapped)))
        return false;
    UINT64 handle = 0;
    if (FAILED(_interop->GetVulkanResourceInfo1(_ubo, &handle, &_uboOffset, nullptr)) || handle == 0)
        return false;
    _uboBuffer = (VkBuffer) handle;

    if (_blitOnly)
    {
        if (!MakePass(_blit, _m->blit, _m->blitBytes, 1, 1, false, nullptr, 0))
            return false;
        _blit.gx = (_dw + 7) / 8;
        _blit.gy = (_dh + 7) / 8;
        _ready = true;
        return true;
    }

    // tensors and maps, an image per 4 channels. A tensor lives within the frame from the pass that writes it to the
    // last pass that reads it (the resolve is the last pass); tensors of one level and format whose lives do not
    // overlap share an image (S4s4v2s: 27 tensors in 23 images, D1 in S's, Z in E1's). Every pass is followed by a
    // compute barrier, so the reuse needs no more of them. OPTI_SGSR3_NOALIAS=1: an image for every tensor.
    const char* noAliasEnv = std::getenv("OPTI_SGSR3_NOALIAS");
    const bool alias = !(noAliasEnv != nullptr && noAliasEnv[0] == '1');
    const int nPasses = (int) _m->passes.size();
    auto life = [&](const std::string& n, int& first, int& last)
    {
        first = -1;
        last = -1;
        int firstRead = -1;
        auto scan = [&](const StModelPass& d, int k)
        {
            for (const auto& in : d.in)
                if (n == in.name)
                {
                    last = k;
                    if (firstRead < 0)
                        firstRead = k;
                }
            for (const auto& o : d.out)
                if (n == o && first < 0)
                    first = k;
        };
        for (int k = 0; k < nPasses; k++)
            scan(_m->passes[k], k);
        scan(_m->resolve, nPasses);
        // read before it is written in the frame (a state kept across frames) or never written: its own image
        if (first < 0 || (firstRead >= 0 && firstRead <= first))
            first = -1;
    };
    struct Slot
    {
        uint32_t w, h;
        VkFormat fmt;
        int last;  // the last pass reading the tensor in it so far; -2: not shared
        int img;
    };
    std::vector<Slot> slots;
    // format 2 recipes vela_exec): every tensor image is storage, sampled and a colour attachment
    // (fragment passes write them as MRT); format 1 keeps the bench's storage + sampled
    const VkImageUsageFlags tensorUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | (_m->format2 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    for (const auto& t : _m->tensors)
    {
        uint32_t w, h;
        LevelSize(t.level.c_str(), w, h);
        if (t.pp)
        {
            // a ping-pong pair carried to the next frame (doc 42 sect. 10): read as NAME.prev, written as NAME.next
            for (const auto& [key, fmt] : TensorImages(t))
                for (int c = 0; c < 2; c++)
                {
                    Img img;
                    if (!MakeImage(img, w, h, fmt, tensorUsage))
                        return false;
                    _tensorImgs.push_back(img);
                    _tensors[key + "#" + std::to_string(c)] = img;
                }
            continue;
        }
        for (const auto& [key, fmt] : TensorImages(t))
        {
            int first, last;
            life(key, first, last);
            Slot* use = nullptr;
            if (alias && first >= 0)
                for (auto& sl : slots)
                    if (sl.last != -2 && sl.w == w && sl.h == h && sl.fmt == fmt && sl.last < first)
                    {
                        use = &sl;
                        break;
                    }
            if (use == nullptr)
            {
                Img img;
                if (!MakeImage(img, w, h, fmt, tensorUsage))
                    return false;
                _tensorImgs.push_back(img);
                slots.push_back({ w, h, fmt, -2, (int) _tensorImgs.size() - 1 });
                use = &slots.back();
            }
            use->last = (alias && first >= 0) ? last : -2;
            _tensors[key] = _tensorImgs[use->img];
        }
    }
    // history: T rgb (R11G11B10) + state (R8), ping-pong, render targets of the resolve
    for (int k = 0; k < 2; k++)
    {
        if (!MakeImage(_hRgb[k], dw, dh, DXGI_FORMAT_R11G11B10_FLOAT, true, k ? L"SGSR3_StHist1" : L"SGSR3_StHist0") ||
            !MakeImage(_hState[k], dw, dh, StateFormat(*_m), true,
                       k ? L"SGSR3_StState1" : L"SGSR3_StState0"))
            return false;

    }

    for (size_t i = 0; i < _m->passes.size(); i++)
    {
        const StModelPass& d = _m->passes[i];
        Pass p;
        p.desc = (int) i;
        p.fragment = d.fragment;
        if (d.fragment)
        {
            // "fpass" vela_exec): a conv pass as a fragment shader, the outputs colour attachments
            std::vector<VkFormat> rtf;
            for (const auto& o : d.out)
                rtf.push_back(TensorImg(o, 0).format);
            if (!MakePass(p, d.spv, d.spvBytes, (uint32_t) d.in.size(), 0, true, rtf.data(), (uint32_t) rtf.size(), true))
                return false;
        }
        else if (!MakePass(p, d.spv, d.spvBytes, (uint32_t) d.in.size(), (uint32_t) d.out.size(), false, nullptr, 0))
            return false;
        uint32_t ox, oy, ix, iy;
        LevelSize(d.outLevel.c_str(), ox, oy);
        LevelSize(d.inLevel.c_str(), ix, iy);
        p.ow = ox;
        p.oh = oy;
        p.pc[0] = (int32_t) ox;
        p.pc[1] = (int32_t) oy;
        p.pc[2] = (int32_t) ix;
        p.pc[3] = (int32_t) iy;
        // the plan's workgroup size ("wg <file> <x> <y> [<ppx> <ppy>]", sec. 5: 16 x 8 for the
        // wide passes; pixels per thread,), the shader's local_size matches it; 8 x 8 without the line
        const uint32_t wx = (uint32_t) (d.wgx * d.ppx), wy = (uint32_t) (d.wgy * d.ppy);
        p.gx = (ox + wx - 1) / wx;
        p.gy = (oy + wy - 1) / wy;
        _passes.push_back(p);
    }
    // The image: by imageStore from the resolve into the game's output (default), or with /tmp/sgsr3_stcopy as a
    // third render target of ours (as in the bench) copied into the output afterwards.
    _copyOut = GetFileAttributesA("Z:\\tmp\\sgsr3_stcopy") != INVALID_FILE_ATTRIBUTES;
    if (_copyOut)
    {
        if (!MakeImage(_out, dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"SGSR3_StOut"))
            return false;
        const VkFormat rtf[3] = { _hRgb[0].format, _hState[0].format, _out.format };
        if (!MakePass(_resolve, _m->mrt, _m->mrtBytes, (uint32_t) _m->resolve.in.size(), 0, true, rtf, 3))
            return false;
    }
    else
    {
        const VkFormat rtf[2] = { _hRgb[0].format, _hState[0].format };
        if (!MakePass(_resolve, _m->resolve.spv, _m->resolve.spvBytes, (uint32_t) _m->resolve.in.size(), 1, true, rtf, 2))
            return false;
    }

    LOG_INFO("SGSR3 student: {0} passes + resolve ({9}), {1} tensors in {10} images, levels {2}x{3} / {4}x{5} / {6}x{7}, "
             "history layout {8}",
             _passes.size(), _tensors.size(), _L1[0], _L1[1], _L2[0], _L2[1], _L3[0], _L3[1], (int) _hRgb[0].layout,
             _copyOut ? "image as a render target + copy" : "image by imageStore", _tensorImgs.size());
    _ready = true;
    return true;
}

const SGSR3StudentVk::Img& SGSR3StudentVk::TensorImg(const std::string& n, int k) const
{
    auto ends = [&](const char* s)
    {
        const size_t l = strlen(s);
        return n.size() > l && n.compare(n.size() - l, l, s) == 0;
    };
    if (ends(".prev"))
        return _tensors.at(n.substr(0, n.size() - 5) + ".0#" + std::to_string(k));
    if (ends(".next"))
        return _tensors.at(n.substr(0, n.size() - 5) + ".0#" + std::to_string(1 - k));
    return _tensors.at(n);
}

void SGSR3StudentVk::NewViewFrame()
{
    // Views of the game's images live for one call only: the game may destroy an image and vkd3d-proton give its
    // VkImage handle to a new one, so a view kept by handle could point at a freed image (a suspect of Control's crash
    // with the student). A view is destroyed 2 x Ring calls later, when the command buffers that used it are done (the
    // assumption the UBO and descriptor rings already make).
    _viewCalls++;
    while (!_retiring.empty() && _retiring.front().first + 2 * Ring <= _viewCalls)
    {
        vkDestroyImageView(_vk, _retiring.front().second, nullptr);
        _retiring.pop_front();
    }
    _gameViews.clear();
}

VkImageView SGSR3StudentVk::GameView(ID3D12Resource* res, VkFormat& fmt, bool depth)
{
    UINT64 handle = 0, offset = 0;
    fmt = VK_FORMAT_UNDEFINED;
    if (res == nullptr || FAILED(_interop->GetVulkanResourceInfo1(res, &handle, &offset, &fmt)) || handle == 0)
        return VK_NULL_HANDLE;
    const auto key = std::make_tuple((uint64_t) handle, (int) fmt, depth ? 1 : 0);
    auto it = _gameViews.find(key);
    if (it != _gameViews.end())
        return it->second;
    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = (VkImage) handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    //: the depth input may be a colour-format copy: the depth aspect only for real depth formats
    depth = depth && (fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                      fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                      fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D16_UNORM_S8_UINT);
    vi.subresourceRange = { depth ? (VkImageAspectFlags) VK_IMAGE_ASPECT_DEPTH_BIT
                                  : (VkImageAspectFlags) VK_IMAGE_ASPECT_COLOR_BIT,
                            0, 1, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(_vk, &vi, nullptr, &v) != VK_SUCCESS)
    {
        LOG_ERROR("SGSR3 student: view of a game image (fmt {0}, depth {1}) failed", (int) fmt, depth);
        return VK_NULL_HANDLE;
    }
    if (_viewCalls <= 2)
    {
        auto d = res->GetDesc();
        LOG_INFO("SGSR3 student: game image {0}x{1} dxgi {2} -> vk fmt {3}{4}", d.Width, d.Height, (int) d.Format,
                 (int) fmt, depth ? " (depth aspect)" : "");
    }
    _gameViews[key] = v;
    _retiring.push_back({ _viewCalls, v });
    return v;
}

bool SGSR3StudentVk::Evaluate(ID3D12GraphicsCommandList* cl, const Input& color, const Input& depth, const Input& mv,
                              const Input& reactive, const Input& tc, const Input& output, Params prm, bool reset,
                              bool captureHistory)
{
    if (!_ready)
        return false;
    const uint32_t slot = _frame % Ring;
    const int k = _parity;

    VkFormat fc, fd, fm, fo, fr, ft;
    NewViewFrame();
    VkImageView vColor = GameView(color.res, fc, false);
    VkImageView vDepth = depth.res ? GameView(depth.res, fd, true) : VK_NULL_HANDLE;
    VkImageView vMv = GameView(mv.res, fm, false);
    VkImageView vOut = GameView(output.res, fo, false);
    VkImageView vReact = reactive.res ? GameView(reactive.res, fr, false) : VK_NULL_HANDLE;
    VkImageView vTc = tc.res ? GameView(tc.res, ft, false) : VK_NULL_HANDLE;
    if (vColor == VK_NULL_HANDLE || vMv == VK_NULL_HANDLE || vOut == VK_NULL_HANDLE)
        return false;
    VkImageLayout lColor, lDepth = VK_IMAGE_LAYOUT_GENERAL, lMv, lOut, lReact = VK_IMAGE_LAYOUT_GENERAL,
                          lTc = VK_IMAGE_LAYOUT_GENERAL;
    _interop->GetVulkanImageLayout(color.res, color.state, &lColor);
    if (depth.res)
        _interop->GetVulkanImageLayout(depth.res, depth.state, &lDepth);
    _interop->GetVulkanImageLayout(mv.res, mv.state, &lMv);
    _interop->GetVulkanImageLayout(output.res, output.state, &lOut);
    if (reactive.res)
        _interop->GetVulkanImageLayout(reactive.res, reactive.state, &lReact);
    if (tc.res)
        _interop->GetVulkanImageLayout(tc.res, tc.state, &lTc);
    // absent inputs: any float image of ours stands in (the flags tell the shaders not to read it)
    if (vDepth == VK_NULL_HANDLE)
    {
        const Img* standIn = &_hState[0];
        for (const auto& t : _tensors)
            if (!IsUintFormat(t.second.format))
            {
                standIn = &t.second;
                break;
            }
        vDepth = _blitOnly ? vColor : standIn->view;
        lDepth = _blitOnly ? lColor : standIn->layout;
    }
    if (vReact == VK_NULL_HANDLE)
    {
        vReact = vDepth;
        lReact = lDepth;
    }
    if (vTc == VK_NULL_HANDLE)
    {
        vTc = vDepth;
        lTc = lDepth;
    }

    if (_frame == 0)
        LOG_INFO("SGSR3 student: layouts colour {0} depth {1} mv {2} output {3}; ours {4}", (int) lColor,
                 (int) lDepth, (int) lMv, (int) lOut, (int) _hRgb[0].layout);

    prm.flags = (prm.flags & ~8u) | (reset ? 8u : 0u);
    prm.frameIndex = _frame;
    memcpy(_uboMapped + slot * 256, &prm, sizeof(prm));
    const VkDescriptorBufferInfo ubi { _uboBuffer, _uboOffset + slot * 256, 256 };

    // descriptors of this ring slot: the game's images may be different each frame, the history alternates
    std::vector<VkWriteDescriptorSet> wr;
    std::deque<VkDescriptorImageInfo> ii;  // stable addresses (S8: 100+ images a frame)
    auto put = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView v, VkImageLayout l,
                   VkSampler s)
    {
        ii.push_back({ s, v, l });
        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = &ii.back();
        wr.push_back(w);
    };
    auto putUbo = [&](VkDescriptorSet set)
    {
        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &ubi;
        wr.push_back(w);
    };
    auto input = [&](VkDescriptorSet set, uint32_t b, const StModelIn& in)
    {
        const std::string& n = in.name;
        VkSampler s = in.linear ? _linear : _point;
        const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (n == "color")
            put(set, b, S, vColor, lColor, s);
        else if (n == "depth")
            put(set, b, S, vDepth, lDepth, s);
        else if (n == "mv")
            put(set, b, S, vMv, lMv, s);
        else if (n == "reactive")
            put(set, b, S, vReact, lReact, s);
        else if (n == "tc")
            put(set, b, S, vTc, lTc, s);
        else if (n == "hist")
            put(set, b, S, _hRgb[k].view, _hRgb[k].layout, s);
        else if (n == "histstate")
            put(set, b, S, _hState[k].view, _hState[k].layout, s);
        else
        {
            const Img& im = TensorImg(n, k);
            put(set, b, S, im.view, im.layout, s);
        }
    };

    if (_blitOnly)
    {
        putUbo(_blit.set[slot]);
        put(_blit.set[slot], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, vColor, lColor, _linear);
        put(_blit.set[slot], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, vOut, lOut, VK_NULL_HANDLE);
    }
    else
    {
        for (auto& p : _passes)
        {
            const StModelPass& d = _m->passes[p.desc];
            putUbo(p.set[slot]);
            uint32_t b = 1;
            for (const auto& in : d.in)
                input(p.set[slot], b++, in);
            if (!d.fragment)  // a fragment pass writes its outputs as colour attachments
                for (const auto& o : d.out)
                {
                    const Img& im = TensorImg(o, k);
                    put(p.set[slot], b++, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, im.view, im.layout, VK_NULL_HANDLE);
                }
        }
        putUbo(_resolve.set[slot]);
        uint32_t b = 1;
        for (const auto& in : _m->resolve.in)
            input(_resolve.set[slot], b++, in);
        if (!_copyOut)
            put(_resolve.set[slot], b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, vOut, lOut, VK_NULL_HANDLE);
    }
    vkUpdateDescriptorSets(_vk, (uint32_t) wr.size(), wr.data(), 0, nullptr);

    // vkd3d-proton batches D3D12 barriers until the next command; a 4-byte copy makes it emit the transitions the
    // feature just recorded before the Vulkan commands below
    cl->CopyBufferRegion(_flush[1], 0, _flush[0], 0, 4);

    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (FAILED(_interop->BeginVkCommandBufferInterop(cl, &cb)) || cb == VK_NULL_HANDLE)
    {
        LOG_ERROR("SGSR3 student: BeginVkCommandBufferInterop failed");
        return false;
    }

    VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    RecordGpuPatches(cb, _patches, _uboBuffer, _uboOffset + (uint64_t) slot * 256); // per-frame fields from the game's GPU cbuffer
    if (!_imagesInitialised)
    {
        std::vector<VkImageMemoryBarrier> ib;
        for (VkImage im : _ownImages)
        {
            VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = im;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            ib.push_back(b);
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, (uint32_t) ib.size(), ib.data());
        _imagesInitialised = true;
        reset = true;
        prm.flags |= 8u;
        memcpy(_uboMapped + slot * 256, &prm, sizeof(prm));
    }
    const uint32_t q0 = slot * TsPerFrame;
    if (_queries != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cb, _queries, q0, TsPerFrame);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queries, q0);
    }

    VkMemoryBarrier cmb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    cmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // around fragment passes vela_exec full_barrier): attachment writes, shader reads and writes, and
    // the reuse of aliased images by either
    VkMemoryBarrier fmb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    fmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    fmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    auto fullBarrier = [&]()
    {
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &fmb, 0,
                             nullptr, 0, nullptr);
    };
    auto run = [&](Pass& p, bool nextFragment)
    {
        if (p.fragment)
        {
            const StModelPass& d = _m->passes[p.desc];
            VkRenderingAttachmentInfo att[16];
            for (size_t i = 0; i < d.out.size(); i++)
            {
                const Img& im = TensorImg(d.out[i], k);
                att[i] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                att[i].imageView = im.view;
                att[i].imageLayout = im.layout;
                att[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, { p.ow, p.oh } };
            ri.layerCount = 1;
            ri.colorAttachmentCount = (uint32_t) d.out.size();
            ri.pColorAttachments = att;
            _beginRendering(cb, &ri);
            VkViewport v { 0, 0, (float) p.ow, (float) p.oh, 0, 1 };
            VkRect2D sc { { 0, 0 }, { p.ow, p.oh } };
            vkCmdSetViewport(cb, 0, 1, &v);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pl, 0, 1, &p.set[slot], 0, nullptr);
            vkCmdPushConstants(cb, p.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, p.pc);
            vkCmdDraw(cb, 3, 1, 0, 0);
            _endRendering(cb);
            fullBarrier();
            return;
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.set[slot], 0, nullptr);
        vkCmdPushConstants(cb, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, p.pc);
        vkCmdDispatch(cb, p.gx, p.gy, 1);
        if (nextFragment && _m->format2)
            fullBarrier();
        else
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                                 &cmb, 0, nullptr, 0, nullptr);
    };

    if (_blitOnly)
    {
        run(_blit, false);
        if (_queries != VK_NULL_HANDLE)
            for (uint32_t i = 1; i < TsPerFrame; i++)
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queries, q0 + i);
    }
    else
    {
        int group = 0;  // 0 stem, 1 body, 2 head
        for (size_t i = 0; i < _passes.size(); i++)
        {
            Pass& p = _passes[i];
            const std::string& f = _m->passes[p.desc].file;
            const int g = f == "stem.comp" ? 0 : f == "head.comp" ? 2 : 1;
            if (g != group && _queries != VK_NULL_HANDLE)
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queries, q0 + g);
            group = g;
            // the resolve after the last pass is a fragment pass too
            run(p, i + 1 < _passes.size() ? _passes[i + 1].fragment : true);
        }
        if (_queries != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queries, q0 + 3);

        VkRenderingAttachmentInfo att[3] = { { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO },
                                             { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO },
                                             { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
        att[0].imageView = _hRgb[1 - k].view;
        att[0].imageLayout = _hRgb[1 - k].layout;
        att[1].imageView = _hState[1 - k].view;
        att[1].imageLayout = _hState[1 - k].layout;
        att[2].imageView = _out.view;
        att[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        for (auto& a : att)
        {
            a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { _dw, _dh } };
        ri.layerCount = 1;
        ri.colorAttachmentCount = _copyOut ? 3 : 2;
        ri.pColorAttachments = att;
        _beginRendering(cb, &ri);
        VkViewport v { 0, 0, (float) _dw, (float) _dh, 0, 1 };
        VkRect2D sc { { 0, 0 }, { _dw, _dh } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, _resolve.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, _resolve.pl, 0, 1, &_resolve.set[slot], 0,
                                nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        _endRendering(cb);
        if (_copyOut)
        {
            VkMemoryBarrier cb2 { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            cb2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            cb2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &cb2, 0, nullptr, 0, nullptr);
            VkImageCopy r {};
            r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.extent = { _dw, _dh, 1 };
            VkImage outImage = VK_NULL_HANDLE;
            UINT64 handle = 0, off = 0;
            if (SUCCEEDED(_interop->GetVulkanResourceInfo1(output.res, &handle, &off, nullptr)))
                outImage = (VkImage) handle;
            if (outImage != VK_NULL_HANDLE)
                vkCmdCopyImage(cb, _out.image, VK_IMAGE_LAYOUT_GENERAL, outImage, lOut, 1, &r);
        }
        if (_queries != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queries, q0 + 4);
    }

    // everything we wrote is visible to whatever the game records next
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);

    // dumps: the history just written, into D3D12 mirrors the dumper can copy (layout of their UAV state)
    if (captureHistory && !_blitOnly)
    {
        for (int m = 0; m < 2; m++)
            if (_mirror[m] == nullptr)
            {
                _mirror[m] = CreateTex(_device, _dw, _dh,
                                       m ? StateFormat(*_m)
                                         : DXGI_FORMAT_R11G11B10_FLOAT,
                                       false, m ? L"SGSR3_StStateMirror" : L"SGSR3_StHistMirror");
                UINT64 handle = 0, off = 0;
                if (_mirror[m] != nullptr)
                {
                    _interop->GetVulkanResourceInfo1(_mirror[m], &handle, &off, nullptr);
                    _interop->GetVulkanImageLayout(_mirror[m], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   &_mirrorLayout[m]);
                }
                _mirrorImage[m] = (VkImage) handle;
            }
        const Img* src[2] = { &_hRgb[1 - k], &_hState[1 - k] };
        for (int m = 0; m < 2; m++)
            if (_mirrorImage[m] != VK_NULL_HANDLE)
            {
                VkImageCopy r {};
                r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                r.extent = { _dw, _dh, 1 };
                vkCmdCopyImage(cb, src[m]->image, VK_IMAGE_LAYOUT_GENERAL, _mirrorImage[m], _mirrorLayout[m], 1, &r);
            }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    _interop->EndVkCommandBufferInterop(cl);

    _parity ^= 1;
    _frame++;
    return true;
}

bool SGSR3StudentVk::LastTiming(double out[4])
{
    if (_queries == VK_NULL_HANDLE || _frame < Ring)
        return false;
    // the slot written Ring - 1 frames ago has certainly retired
    const uint32_t slot = (_frame + 1) % Ring;
    uint64_t ts[TsPerFrame] {};
    if (vkGetQueryPoolResults(_vk, _queries, slot * TsPerFrame, TsPerFrame, sizeof(ts), ts, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return false;
    for (int i = 0; i < 4; i++)
        out[i] = ts[i + 1] >= ts[i] ? (double) (ts[i + 1] - ts[i]) * _tsPeriodNs * 1e-6 : 0.0;
    return true;
}

void SGSR3StudentVk::Release()
{
    if (_vk != VK_NULL_HANDLE)
    {
        auto pass = [&](Pass& p)
        {
            if (p.pipe)
                vkDestroyPipeline(_vk, p.pipe, nullptr);
            if (p.pl)
                vkDestroyPipelineLayout(_vk, p.pl, nullptr);
            if (p.dsl)
                vkDestroyDescriptorSetLayout(_vk, p.dsl, nullptr);
            p = Pass {};
        };
        for (auto& p : _passes)
            pass(p);
        _passes.clear();
        pass(_resolve);
        pass(_blit);
        auto img = [&](Img& i)
        {
            if (i.view)
                vkDestroyImageView(_vk, i.view, nullptr);
            if (i.image)
                vkDestroyImage(_vk, i.image, nullptr);
            if (i.mem)
                vkFreeMemory(_vk, i.mem, nullptr);
            i = Img {};
        };
        for (auto& t : _tensorImgs)
            img(t);
        _tensorImgs.clear();
        for (int k = 0; k < 2; k++)
        {
            img(_hRgb[k]);
            img(_hState[k]);
        }
        img(_out);
        _ownImages.clear();
        for (auto& v : _retiring)
            vkDestroyImageView(_vk, v.second, nullptr);
        _retiring.clear();
        _gameViews.clear();
        if (_pool)
            vkDestroyDescriptorPool(_vk, _pool, nullptr);
        if (_point)
            vkDestroySampler(_vk, _point, nullptr);
        if (_linear)
            vkDestroySampler(_vk, _linear, nullptr);
        if (_vs)
            vkDestroyShaderModule(_vk, _vs, nullptr);
        if (_queries)
            vkDestroyQueryPool(_vk, _queries, nullptr);
    }
    _tensors.clear();
    _tensorImgs.clear();
    for (int m = 0; m < 2; m++)
    {
        if (_mirror[m])
            _mirror[m]->Release();
        _mirror[m] = nullptr;
        _mirrorImage[m] = VK_NULL_HANDLE;
    }
    _imagesInitialised = false;
    _m = nullptr;
    if (_ubo)
        _ubo->Release();
    for (auto*& f : _flush)
        if (f)
        {
            f->Release();
            f = nullptr;
        }
    if (_interop)
        _interop->Release();
    _ubo = nullptr;
    _interop = nullptr;
    _ready = false;
}

// the main recipe's MK, for callers that do not include the SPIR-V header vela_fsr2.cpp); the running
// recipe's: SGSR3StudentVk::DefaultMk()
extern const float st_default_mk_value = st_main::st_default_mk;

// what the recipe needs from vkd3d-proton's VkDevice and the formats of its images
bool SGSR3StudentVk::DeviceCan(const StModel& m, const VkPhysicalDeviceFeatures2* feats, std::string& why) const
{
    why.clear();
    bool intDot = false;
    if (feats != nullptr)
        for (auto* s = (const VkBaseInStructure*) feats->pNext; s != nullptr; s = s->pNext)
        {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
                intDot = intDot || ((const VkPhysicalDeviceVulkan13Features*) s)->shaderIntegerDotProduct;
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES)
                intDot = intDot || ((const VkPhysicalDeviceShaderIntegerDotProductFeatures*) s)->shaderIntegerDotProduct;
        }
    if (m.intDot && !intDot)
        why += "shaderIntegerDotProduct not enabled on the vkd3d device; ";
    if (m.extFormats && (feats == nullptr || !feats->features.shaderStorageImageExtendedFormats))
        why += "shaderStorageImageExtendedFormats not enabled; ";
    std::vector<VkFormat> need;
    for (const auto& t : m.tensors)
        for (const auto& ti : TensorImages(t))
            if (std::find(need.begin(), need.end(), ti.second) == need.end())
                need.push_back(ti.second);
    // through vkGetInstanceProcAddr: the toolchain's vulkan-1 import library lists only what the fork uses
    auto getFormatProps = (PFN_vkGetPhysicalDeviceFormatProperties) vkGetInstanceProcAddr(
        _instance, "vkGetPhysicalDeviceFormatProperties");
    for (VkFormat f : need)
    {
        VkFormatProperties fp {};
        if (getFormatProps == nullptr)
        {
            why += "no vkGetPhysicalDeviceFormatProperties; ";
            break;
        }
        getFormatProps(_physical, f, &fp);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (m.format2)
            want |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if ((fp.optimalTilingFeatures & want) != want)
            why += "format " + std::to_string((int) f) + " lacks storage / sampled / attachment; ";
    }
    return why.empty();
}
