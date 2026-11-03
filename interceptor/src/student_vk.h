#pragma once

#include <deque>
#include "interop_vkd3d.h"
#include "gpu_patch.h"

#include <d3d12.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// Ported from the plugin fork (patches 0004/0007 + 0.4.10/0.4.11 -- our
// own code): Vela AI. Changes: no OptiScaler pch/Util/logger (log.h), interop header ours, SetPatches()
// (per-frame fields copied on the GPU from the game's FSR 2 constant buffer, gpu_patch.h), depth aspect only
// for depth formats.
//============================================================================================================
// The Vela AI student upscaler (a small CNN that predicts the blend parameters of a
// temporal resolve) inside the D3D12 game, on Vulkan through vkd3d-proton's interop.
//
// Why Vulkan: the student's recipe is fp16 arithmetic (f16vec4 literals, fma). Through D3D12 that is impossible on
// Turnip: vkd3d-proton exposes neither native 16-bit ops nor relaxed min precision there (no fp16 denorm preserve).
// ID3D12DXVKInteropDevice1::BeginVkCommandBufferInterop hands out the Vulkan command
// buffer behind the game's D3D12 command list, so the student's SPIR-V is recorded in order with the game's work:
// no extra queue, no extra synchronisation beyond pipeline barriers.
//
// Resources: our images are plain Vulkan images on vkd3d-proton's device (as in the bench; images created as D3D12
// resources and taken through GetVulkanResourceInfo1 cost 1.2 ms more at 720p: vkd3d-proton makes them mutable /
// storage-capable, and Turnip loses framebuffer compression on them). For dumps, the written history is copied into
// two D3D12 mirrors. The game's colour / depth / MV / output come in the D3D12 states the feature put them in; their
// Vulkan layouts are asked from GetVulkanImageLayout.
//
// Shaders and plan: SGSR3_Student_spv.h (tools/gen_student_spv.py): compute passes stem .. head, the resolve as a
// full-screen fragment pass (history + state as render targets, the image by imageStore into the game's output).
//============================================================================================================

// ID3D12DXVKInteropDevice1: interop_vkd3d.h (our declaration from vkd3d-proton's public IDL)

// A recipe as the host runs it, built from the tables of a generated header (gen_student_spv.py, format 1 = fp16
// recipes, format 2 = INT8 / fragment recipes of: u8n and q8 tensors, "fpass", pixels per thread).
// student_vk.cpp holds the main header (SGSR3_Student_spv.h) and, when present, a fallback
// (SGSR3_Student_spv_fallback.h, an fp16 recipe) for devices without what the main one needs.
struct StModelIn
{
    std::string name;
    bool linear = false;
};
struct StModelPass
{
    std::string file, outLevel, inLevel;
    std::vector<StModelIn> in;
    std::vector<std::string> out;
    const uint32_t* spv = nullptr;
    size_t spvBytes = 0;
    int wgx = 8, wgy = 8, ppx = 1, ppy = 1;
    bool fragment = false;  // "fpass": out[] are colour attachments, push constants in the fragment stage
};
struct StModelTensor
{
    std::string name, level, fmt;
    int channels = 0;
    bool pp = false;  // format 3: a ping-pong pair (NAME.prev / NAME.next), switched with the history
};
struct StModel
{
    std::string name;
    float defaultMk = 8.0f;
    int stateChannels = 1;
    bool intDot = false;      // a shader uses SPV_KHR_integer_dot_product (shaderIntegerDotProduct)
    bool extFormats = false;  // a q8 image is RG32UI (shaderStorageImageExtendedFormats)
    bool format2 = false;     // u8n / q8 / fpass: every tensor is also a colour attachment
    std::vector<StModelTensor> tensors;
    std::vector<StModelPass> passes;
    StModelPass resolve;
    const uint32_t *vs = nullptr, *mrt = nullptr, *blit = nullptr;
    size_t vsBytes = 0, mrtBytes = 0, blitBytes = 0;
};
// the main recipe / the fallback (nullptr without SGSR3_Student_spv_fallback.h)
const StModel& StMainModel();
const StModel* StFallbackModel();

class SGSR3StudentVk
{
  public:
    // the student's Params UBO (st_common.glsl), 80 bytes (the resolve also reads alphaK)
    struct Params
    {
        int32_t renderSize[2];
        int32_t displaySize[2];
        float jitter[2];
        float mvToUv[2];
        float renderRcp[2];
        float displayRcp[2];
        float preExposure;
        float invScale;
        uint32_t flags;  // 1 depth, 2 depth inverted, 4 low-res MV, 8 reset, 16 reactive, 32 T&C
        uint32_t frameIndex;
        float alphaK = 1.0f;  // the resolve's damping of the nearest current sample (OPTI_SGSR3_ALPHAK, 0.4.1)
        float mk = 8.0f;      // the motion term of the accumulation cap (OPTI_SGSR3_MK, 0.4.4; st_default_mk)
        float amax = 0.0f;    // the current sample's weight cap w <= amax, 0 = none (OPTI_SGSR3_AMAX, 0.4.11)
        float pad = 0.0f;
    };
    static_assert(sizeof(Params) == 80, "student Params");

    struct Input
    {
        ID3D12Resource* res = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    // blitOnly: the interop test pass (the frame bilinearly into the output) instead of the student
    bool Init(ID3D12Device* device, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, bool blitOnly);
    // Records the student into InCommandList. The inputs are in the given D3D12 states; the output in UAV state.
    // Our own resources stay in UNORDERED_ACCESS throughout.
    bool Evaluate(ID3D12GraphicsCommandList* InCommandList, const Input& color, const Input& depth, const Input& mv,
                  const Input& reactive, const Input& tc, const Input& output, Params prm, bool reset,
                  bool captureHistory = false);
    void Release();

    // GPU time of the last readable frame, ms: [0] stem, [1] body, [2] head, [3] resolve; false until available
    bool LastTiming(double out[4]);
    // the history written by the last Evaluate (for the dumper): T rgb (R11G11B10) and state (R8); the parity has
    // flipped after Evaluate, so it is _histRgb[_parity]
    // (copies made during the last Evaluate with captureHistory, as D3D12 resources in UNORDERED_ACCESS)
    ID3D12Resource* WrittenHistory() const { return _mirror[0]; }
    ID3D12Resource* WrittenState() const { return _mirror[1]; }
    bool Ready() const { return _ready; }
    // the PC stand (tools/student_host_test.cpp): the history images written by the last Evaluate
    VkImage WrittenHistoryImage(bool state) const { return state ? _hState[_parity].image : _hRgb[_parity].image; }
    // the sizes the student's images were made for (a frame with other sizes must not reach its passes)
    bool SizesMatch(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) const
    {
        return rw == _rw && rh == _rh && dw == _dw && dh == _dh;
    }
    bool BlitOnly() const { return _blitOnly; }
    const std::string& Info() const { return _info; }
    // the running recipe's MK (Params.mk default); valid after Init
    float DefaultMk() const { return _m ? _m->defaultMk : StMainModel().defaultMk; }
    const char* ModelName() const { return _m ? _m->name.c_str() : StMainModel().name.c_str(); }
    void SetPatches(const GpuPatches& p) { _patches = p; }

  private:
    static constexpr uint32_t Ring = 8;  // frames whose descriptor sets / UBO may still be in flight

    struct Img
    {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool attachment = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
        uint32_t w = 0, h = 0;
    };

    struct Pass
    {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkDescriptorSet set[Ring] = {};
        int32_t pc[4] = {};
        uint32_t gx = 0, gy = 0, ow = 0, oh = 0;
        int desc = -1;  // index into the model's passes, -1 = resolve / blit
        bool fragment = false;
    };

    const StModel* _m = nullptr;  // the recipe chosen at Init (main, or the fallback when the device lacks a feature)
    ID3D12Device* _device = nullptr;
    ID3D12DXVKInteropDevice1* _interop = nullptr;
    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDevice _vk = VK_NULL_HANDLE;
    PFN_vkCmdBeginRendering _beginRendering = nullptr;
    PFN_vkCmdEndRendering _endRendering = nullptr;
    double _tsPeriodNs = 1.0;

    uint32_t _rw = 0, _rh = 0, _dw = 0, _dh = 0;
    uint32_t _L1[2] = {}, _L2[2] = {}, _L3[2] = {};
    bool _blitOnly = false;
    bool _ready = false;
    std::string _info;
    GpuPatches _patches;

    std::map<std::string, Img> _tensors;  // by tensor name ("E1.0"); tensors may share an image
    std::vector<Img> _tensorImgs;         // the images themselves (released once)
    std::vector<VkImage> _ownImages;
    Img _out;               // /tmp/sgsr3_stcopy: the image as a render target of ours, then copied
    bool _copyOut = false;
    bool _imagesInitialised = false;
    ID3D12Resource* _mirror[2] = {};  // dumps: the written history (R11G11B10) and state (R8) as D3D12 resources
    VkImage _mirrorImage[2] = {};
    VkImageLayout _mirrorLayout[2] = { VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL };
    Img _hRgb[2], _hState[2];
    ID3D12Resource* _ubo = nullptr;  // upload heap, Ring x 256 B
    uint8_t* _uboMapped = nullptr;
    VkBuffer _uboBuffer = VK_NULL_HANDLE;
    uint64_t _uboOffset = 0;
    ID3D12Resource* _flush[2] = {};  // two tiny buffers: a copy between them flushes vkd3d-proton's barrier batch

    VkSampler _point = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkShaderModule _vs = VK_NULL_HANDLE;
    std::vector<Pass> _passes;
    Pass _resolve, _blit;
    VkQueryPool _queries = VK_NULL_HANDLE;
    static constexpr uint32_t TsPerFrame = 5;

    // views of the game's images, by (image, format, aspect)
    std::map<std::tuple<uint64_t, int, int>, VkImageView> _gameViews;  // this call's views of game images
    std::deque<std::pair<uint64_t, VkImageView>> _retiring;           // (call made in, view)
    uint64_t _viewCalls = 0;

    uint32_t _frame = 0;
    int _parity = 0;  // _hRgb[_parity] = previous history

    bool MakeImage(Img& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage);
    bool MakeImage(Img& img, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool renderTarget, const wchar_t* name);
    // pcFragment: push constants (osize / isize) in the fragment stage (a fragment conv pass)
    bool MakePass(Pass& p, const uint32_t* spv, size_t bytes, uint32_t nSampled, uint32_t nStorage, bool fragment,
                  const VkFormat* rtFormats, uint32_t nRt, bool pcFragment = false);
    bool DeviceCan(const StModel& m, const VkPhysicalDeviceFeatures2* feats, std::string& why) const;
    VkImageView GameView(ID3D12Resource* res, VkFormat& fmt, bool depth);
    void NewViewFrame();  // a new call: this call's views made afresh, old ones retired
    void LevelSize(const char* level, uint32_t& w, uint32_t& h) const;
    // NAME.prev / NAME.next of a ping-pong tensor -> its image of this frame's parity k / of the other one
    const Img& TensorImg(const std::string& name, int k) const;
};
