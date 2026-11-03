// SGSR 3 v1f on Vulkan through vkd3d-proton's interop, standalone (no OptiScaler deps).
//
// Ported from our patch 0005) -- our own code -- with the
// OptiScaler Config/Logger/menu/env-file toggles removed and the variant fixed at construction.
// convert (fragment, render res): depth+MV+reactive+colour -> MDR (RGBA16F) + working colour.
// upscale (display res): fragment, history as render target(s), the image by imageStore into the
// game's output; or compute. Own images are plain Vulkan images (keeps UBWC). The game's resources
// come in through GetVulkanResourceInfo1 in the layouts GetVulkanImageLayout reports.
#pragma once
#include "interop_vkd3d.h"
#include "gpu_patch.h"
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class VkUpscalerV1f
{
  public:
    struct Input
    {
        ID3D12Resource* res = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    // bitSet: 0 v1a (x<1.75), 1 v1b, 2 v1c (x>=2.5)
    // Rgba16: RGBA16F history (N in alpha), v1 heuristics; Lock: compact A2B10G10R10 + N (RG16F),
    //         low-frequency lock + thin-feature locks; Lf: compact + low-frequency lock only.
    enum class Variant { Rgba16, Lf, Lock };
    bool Init(ID3D12Device* device, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, int bitSet, Variant v);
    bool Evaluate(ID3D12GraphicsCommandList* cl, const Input& color, const Input& depth, const Input& mv,
                  const Input& reactive, const Input& tc, const Input& output, const void* constants, size_t bytes,
                  bool reset);
    void Release();
    bool LastTiming(double out[2]);
    bool Ready() const { return _ready; }
    bool Compute() const { return _compute; }
    bool SizesMatch(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) const
    {
        return rw == _rw && rh == _rh && dw == _dw && dh == _dh;
    }
    const std::string& Info() const { return _info; }
    // fields of this frame's SGSR3Constants copied on the GPU from another buffer before the passes run
    void SetPatches(const GpuPatches& p) { _patches = p; }

  private:
    static constexpr uint32_t Ring = 8;
    static constexpr uint32_t TsPerFrame = 3;

    struct Img
    {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t w = 0, h = 0;
    };
    struct Pass
    {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkDescriptorSet set[Ring] = {};
    };

    ID3D12Device* _device = nullptr;
    ID3D12DXVKInteropDevice1* _interop = nullptr;
    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDevice _vk = VK_NULL_HANDLE;
    PFN_vkCmdBeginRendering _beginRendering = nullptr;
    PFN_vkCmdEndRendering _endRendering = nullptr;
    double _tsPeriodNs = 1.0;

    uint32_t _rw = 0, _rh = 0, _dw = 0, _dh = 0;
    bool _compute = false, _compact = false, _lock = false, _ready = false, _imagesInitialised = false;
    std::string _info;
    GpuPatches _patches;

    Img _mdr, _work, _hist[2], _histN[2];
    ID3D12Resource* _ubo = nullptr;
    uint8_t* _uboMapped = nullptr;
    VkBuffer _uboBuffer = VK_NULL_HANDLE;
    uint64_t _uboOffset = 0;
    ID3D12Resource* _flush[2] = {};

    VkSampler _point = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkShaderModule _vs = VK_NULL_HANDLE;
    Pass _convert, _upscale;
    VkQueryPool _queries = VK_NULL_HANDLE;
    std::map<std::tuple<uint64_t, int, int>, VkImageView> _gameViews; // this call's views of game images only
    std::vector<std::pair<uint32_t, VkImageView>> _retiring;         // destroyed 2 x Ring calls later
    uint32_t _frame = 0;
    int _parity = 0;

    bool MakeImage(Img& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage);
    bool MakePass(Pass& p, const uint32_t* spv, size_t bytes,
                  const std::vector<std::pair<uint32_t, VkDescriptorType>>& bindings, bool fragment,
                  const VkFormat* rtFormats, uint32_t nRt);
    VkImageView GameView(ID3D12Resource* res, VkFormat& fmt, bool depth);
};
