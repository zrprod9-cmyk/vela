#include "exposure_fix.h"
#include "log.h"
#include "misc_spv.h"

static ID3D12Resource* SmallBuf(ID3D12Device* dev, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

bool ExposureFix::Init(ID3D12Device* dev)
{
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), (void**) &_interop)) || !_interop) return false;
    VkInstance inst;
    VkPhysicalDevice phys;
    if (FAILED(_interop->GetVulkanHandles(&inst, &phys, &_vk)) || !_vk) return false;

    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(_vk, &dl, nullptr, &_dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &_dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(_vk, &pli, nullptr, &_pl) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(misc_spv_exposure_div_comp);
    mi.pCode = misc_spv_exposure_div_comp;
    VkShaderModule sm;
    if (vkCreateShaderModule(_vk, &mi, nullptr, &sm) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"};
    ci.layout = _pl;
    VkResult vr = vkCreateComputePipelines(_vk, VK_NULL_HANDLE, 1, &ci, nullptr, &_pipe);
    vkDestroyShaderModule(_vk, sm, nullptr);
    if (vr != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Ring}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Ring}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = Ring;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(_vk, &dpi, nullptr, &_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout ls[Ring];
    for (auto& l : ls) l = _dsl;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = Ring;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(_vk, &ai, _set) != VK_SUCCESS) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(_vk, &si, nullptr, &_point) != VK_SUCCESS) return false;
    _flush[0] = SmallBuf(dev, D3D12_RESOURCE_STATE_COPY_SOURCE);
    _flush[1] = SmallBuf(dev, D3D12_RESOURCE_STATE_COPY_DEST);
    _ready = _flush[0] && _flush[1];
    LOGI("exposure fix: %s", _ready ? "ready" : "failed");
    return _ready;
}

VkImageView ExposureFix::View(ID3D12Resource* r)
{
    UINT64 h = 0, off = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    if (!r || FAILED(_interop->GetVulkanResourceInfo1(r, &h, &off, &fmt)) || !h) return VK_NULL_HANDLE;
    auto it = _views.find(h);
    if (it != _views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = (VkImage) h;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(_vk, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    _views[h] = v;
    return v;
}

bool ExposureFix::Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* img, ID3D12Resource* exposure, uint32_t w,
                      uint32_t h)
{
    if (!_ready) return false;
    VkImageView vImg = View(img), vExp = View(exposure);
    if (!vImg || !vExp) return false;
    VkImageLayout lImg, lExp;
    _interop->GetVulkanImageLayout(img, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &lImg);
    _interop->GetVulkanImageLayout(exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &lExp);
    const uint32_t slot = _frame++ % Ring;
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, vImg, lImg}, {_point, vExp, lExp}};
    VkWriteDescriptorSet wr[2] = {};
    for (int i = 0; i < 2; i++)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = _set[slot];
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(_vk, 2, wr, 0, nullptr);
    cl->CopyBufferRegion(_flush[1], 0, _flush[0], 0, 4); // make vkd3d emit its batched barriers first
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (FAILED(_interop->BeginVkCommandBufferInterop(cl, &cb)) || !cb) return false;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _pl, 0, 1, &_set[slot], 0, nullptr);
    int32_t pc[2] = {(int32_t) w, (int32_t) h};
    vkCmdPushConstants(cb, _pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    _interop->EndVkCommandBufferInterop(cl);
    return true;
}

void ExposureFix::Release()
{
    if (_vk)
    {
        for (auto& kv : _views) vkDestroyImageView(_vk, kv.second, nullptr);
        if (_point) vkDestroySampler(_vk, _point, nullptr);
        if (_pool) vkDestroyDescriptorPool(_vk, _pool, nullptr);
        if (_pipe) vkDestroyPipeline(_vk, _pipe, nullptr);
        if (_pl) vkDestroyPipelineLayout(_vk, _pl, nullptr);
        if (_dsl) vkDestroyDescriptorSetLayout(_vk, _dsl, nullptr);
    }
    for (auto& f : _flush) if (f) f->Release();
    if (_interop) _interop->Release();
    *this = ExposureFix{};
}
