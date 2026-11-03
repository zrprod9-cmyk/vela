// One small compute pass on the game's command list through vkd3d interop (shaders/hdr_wrap.comp):
// mode 0 = reversible tonemap c / (1 + max c) (Vela's input), 1 = its inverse (Vela's output), 2 = copy.
// src is sampled (any colour format), dst is written without a format qualifier (RGBA16F or the game's R11G11B10).
#pragma once
#include "interop_vkd3d.h"
#include <deque>
#include <utility>

class HdrWrap
{
  public:
    enum Mode { Forward = 0, Inverse = 1, Copy = 2 };
    bool Init(ID3D12Device* dev);
    bool Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, ID3D12Resource* dst,
             D3D12_RESOURCE_STATES dstState, uint32_t w, uint32_t h, Mode mode);
    bool Ready() const { return _ready; }

  private:
    static constexpr uint32_t Ring = 32; // several runs per frame
    ID3D12DXVKInteropDevice1* _interop = nullptr;
    VkDevice _vk = VK_NULL_HANDLE;
    VkDescriptorSetLayout _dsl = VK_NULL_HANDLE;
    VkPipelineLayout _pl = VK_NULL_HANDLE;
    VkPipeline _pipe = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set[Ring] = {};
    VkSampler _point = VK_NULL_HANDLE;
    ID3D12Resource* _flush[2] = {};
    // views of the game's images live for one call: the game may free an image (a level load) and Turnip give
    // its VkImage handle to a new one, so a view cached by handle would read freed memory (an RE4 GPU fault)
    std::deque<std::pair<uint32_t, VkImageView>> _retiring;
    uint32_t _n = 0;
    bool _ready = false;
    VkImageView View(ID3D12Resource* r);
};
