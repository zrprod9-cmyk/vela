// After Vela writes its frame into FSR 2's upscaled-history texture (RGBA16F), divide it by FSR's live
// auto-exposure (the game's 1x1 R32G32_FLOAT exposure texture), so RCAS and RE4's post-processing get the values
// FSR itself would have written (shaders/exposure_div.comp). Recorded through vkd3d-proton interop on the game's
// command list, like the upscalers.
#pragma once
#include "interop_vkd3d.h"
#include <map>

class ExposureFix
{
  public:
    bool Init(ID3D12Device* dev);
    // img: the history texture in UNORDERED_ACCESS; exposure: the 1x1 texture in a shader-read state
    bool Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* img, ID3D12Resource* exposure, uint32_t w, uint32_t h);
    void Release();
    bool Ready() const { return _ready; }

  private:
    static constexpr uint32_t Ring = 8;
    ID3D12DXVKInteropDevice1* _interop = nullptr;
    VkDevice _vk = VK_NULL_HANDLE;
    VkDescriptorSetLayout _dsl = VK_NULL_HANDLE;
    VkPipelineLayout _pl = VK_NULL_HANDLE;
    VkPipeline _pipe = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set[Ring] = {};
    VkSampler _point = VK_NULL_HANDLE;
    ID3D12Resource* _flush[2] = {};
    std::map<uint64_t, VkImageView> _views;
    uint32_t _frame = 0;
    bool _ready = false;
    VkImageView View(ID3D12Resource* r);
};
