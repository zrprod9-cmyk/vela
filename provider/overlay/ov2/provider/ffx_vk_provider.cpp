#if !defined(_WIN32)
#define __declspec(x) __attribute__((visibility("default")))
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../amd-fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include <vulkan/vulkan.h>

#include "ffx_vk_device_requirements.h"
#include "ffx_vk_preset_query.h"
#include "vulkan_device_features.hpp"
#include "diagnostic-options.hpp"
#include "resource-diagnostics.hpp"
#include "device-feature-diagnostics.hpp"
#include "input-formats.hpp"
#include "ffx_vk_native_inputs.h"
#include "ffx_vk_create_device_features.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <limits>
#include "../native/quality-context.hpp"
#include "../native/fsr412lite-context.hpp"

namespace {
constexpr ffxStructType_t kBackendVkDescType = 3u;
constexpr ffxStructType_t kFsr4VulkanApiVersionDescType =
    0x46535234564b4150ull;
struct CreateBackendVkDesc {
    ffxCreateContextDescHeader header;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
};

struct Fsr4VulkanApiVersionDesc {
    ffxApiHeader header;
    std::uint32_t apiVersion;
};

// The FSR 4.0.2 model is not in this build (no 4.0.2 shaders or weights embedded or accepted).
constexpr std::uint64_t kFsr314CompatibilityVersionId =
    (0xF5A5CA1Eull << 32) | ((3ull << 22) | (1ull << 12) | 4ull);
constexpr std::uint64_t kVersion411Id =
    (0xF5A5CA1Eull << 32) | ((4ull << 22) | (1ull << 12) | 1ull);
// The installed VK INT8 selector prefixes these verbatim with "FSR".
// Keep the leading space here; version IDs, not labels, select the model.
constexpr const char* kVersion411Name = " 4.1.1 VK INT8";
// Vela 0.7.0: FSR 4.1.2 Lite (own recorder, fsr412lite-context.hpp); listed last so default selection is unchanged.
constexpr std::uint64_t kVersionLiteId =
    (0xF5A5CA1Eull << 32) | ((4ull << 22) | (1ull << 12) | 2ull);
constexpr const char* kVersionLiteName = " 4.1.2 VK INT8 Lite";
constexpr ffxStructType_t kVelaLossyDescType = 0x56454C414C535931ull;  // "VELALSY1" from the Vela OptiScaler bridge
struct VelaLossyDesc { ffxApiHeader header; std::uint32_t networkEvery; };
// Vela 0.8.0 (W-fork): FSR 4.1.2 Lite network on the Hexagon NPU through VK_LAYER_VELA_npu (0 off, 1 exact, 2 async)
constexpr ffxStructType_t kVelaNpuDescType = 0x56454C414E505531ull;  // "VELANPU1" from the Vela OptiScaler bridge
struct VelaNpuDesc { ffxApiHeader header; std::uint32_t mode; };

struct ProviderContext {
    // CPU API calls serialize; GPU submission/destruction remain host contracts.
    std::recursive_mutex api_mutex;
    inline static std::atomic<std::uint64_t> next_id{0};
    const std::uint64_t diagnostic_id=++next_id;
    std::uint64_t version = kVersion411Id;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    std::uint32_t apiVersion = VK_API_VERSION_1_1;
    std::optional<bool> robust_buffer_access, native_mixed_dot_enabled;
    uint32_t queue_family_index=UINT32_MAX;
    std::uint32_t flags = 0;
    FfxApiDimensions2D maxRenderSize{};
    FfxApiDimensions2D maxUpscaleSize{};
    int u15_mode=-1;   // VELA_FAST_UP15 with Fast, read at the first dispatch of this context
    ffxApiMessage message = nullptr;
    std::filesystem::path assets;
    std::map<std::string,std::unique_ptr<fsr4core::QualityContext>> cores;
    std::unique_ptr<fsr4core::LiteContext> lite;
    std::uint32_t lite_every = 1;
    std::uint32_t lite_npu = 0;
    std::string active_preset;
    std::uint32_t forced_preset = FSR4VK_PRESET_AUTO;
    fsr4core::QualityContext* active_core = nullptr;
    uint32_t last_output_width=0,last_output_height=0;
    std::filesystem::path diagnostic_path;
    bool first_dispatch = true;
    bool device_support_checked = false;
    bool sharpening_ignored_logged = false;
    bool optional_masks_ignored_logged = false;
    bool unsupported_config_logged = false;
    bool pending_color_reset = false;
    bool resource_snapshot_logged = false;
    uint64_t dispatch_attempts = 0;
    uint64_t dispatch_successes = 0;
    VkFormat motion_override = VK_FORMAT_UNDEFINED;
};

constexpr const char* presetName(std::uint32_t id) noexcept {
    switch (id) {
    case 0: return "native";
    case 1: return "quality";
    case 2: return "balanced";
    case 3: return "performance";
    case 4: return "drs";
    case 5: return "ultraperf";
    default: return nullptr;
    }
}

const char* selectedPreset(const ProviderContext& context, uint32_t rw, uint32_t ow) {
    return context.forced_preset == FSR4VK_PRESET_AUTO ? fsr4::resolution_preset(rw, ow,
               (context.flags & FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) != 0)
                                                     : presetName(context.forced_preset);
}

std::filesystem::path diagnostic_path(const std::filesystem::path& assets={}) noexcept {
#if defined(_WIN32)
    (void)assets;
#endif
    try {
        const char* log=std::getenv("FSR4_VK_LOG_PATH");
        if(!fsr4vk::provider_log_requested(std::getenv("FSR4_VK_LOG"),log)) return {};
        if(log && *log) return log;
#if defined(_WIN32)
        wchar_t executable[32768]{};
        const auto length=GetModuleFileNameW(nullptr,executable,32768);
        const auto host=(length && length<32768) ? std::filesystem::path(executable) : std::filesystem::path{};
        std::error_code ec;
        auto temp=std::filesystem::temp_directory_path(ec);
        if(ec) temp.clear();
        return fsr4vk::default_provider_log_path(host,temp,GetCurrentProcessId(),[](const auto& path) {
            std::ofstream stream(path,std::ios::app); return stream.good();
        });
#else
        return (assets.empty() ? std::filesystem::current_path() : assets)/"provider.log";
#endif
    } catch(...) { return {}; }
}

void write_diagnostic(const std::filesystem::path& path,uint64_t id,const std::string& text) noexcept {
    try {
        if (!path.empty()) {
            static std::mutex log_mutex;
            const std::lock_guard<std::mutex> lock(log_mutex);
            std::ofstream log(path,std::ios::app);
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            log << "[time_ms=" << ms << " context=" << id << "] " << text << '\n';
        }
    } catch (...) { }
}

void diagnostic(const ProviderContext& context,const std::string& text) noexcept {
    write_diagnostic(context.diagnostic_path,context.diagnostic_id,text);
}

void provider_message(const ProviderContext& context, uint32_t type,
                      const std::string& text) noexcept {
    diagnostic(context,text);
    if (!context.message) return;
    try {
        const std::wstring wide(text.begin(),text.end());
        context.message(type,wide.c_str());
    } catch (...) { }
}

std::filesystem::path asset_root() {
    if (const char* root=std::getenv("FSR4_VK_ASSET_ROOT")) return root;
#if defined(FSR4_EMBEDDED_ASSETS)
    return fsr4assets::root;
#endif
#if defined(_WIN32)
    HMODULE module{};
    wchar_t path[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ffxCreateContext),&module)) {
        const auto length=GetModuleFileNameW(module,path,32768);
        if (length && length<32768)
            return std::filesystem::path(path).parent_path()/"fsr4-vulkan-assets";
    }
#endif
    throw std::invalid_argument("FSR4_VK_ASSET_ROOT must name the shader/model sidecar directory");
}

bool validAllocator(const ffxAllocationCallbacks* callbacks) {
    return callbacks == nullptr || (callbacks->alloc != nullptr && callbacks->dealloc != nullptr);
}

void* allocate(const ffxAllocationCallbacks* callbacks, std::size_t size) {
    return callbacks ? callbacks->alloc(callbacks->pUserData, size) : std::malloc(size);
}

void deallocate(const ffxAllocationCallbacks* callbacks, void* memory) {
    if (callbacks) callbacks->dealloc(callbacks->pUserData, memory);
    else std::free(memory);
}

void validateApiChain(const ffxApiHeader* header) {
    std::array<const ffxApiHeader*,64> seen{};
    std::size_t count=0;
    for (auto* node=header;node;node=node->pNext) {
        if(count==seen.size() || std::find(seen.begin(),seen.begin()+count,node)!=seen.begin()+count)
            throw std::invalid_argument("FFX descriptor chain is cyclic or exceeds 64 nodes");
        seen[count++]=node;
    }
}

const CreateBackendVkDesc* findBackend(const ffxApiHeader* header) {
    for (auto* current = header; current; current = current->pNext) {
        if (current->type == kBackendVkDescType) {
            return reinterpret_cast<const CreateBackendVkDesc*>(current);
        }
    }
    return nullptr;
}

std::uint32_t findVulkanApiVersion(const ffxApiHeader* header) {
    for (auto* current = header; current; current = current->pNext) {
        if (current->type == kFsr4VulkanApiVersionDescType) {
            return reinterpret_cast<const Fsr4VulkanApiVersionDesc*>(current)->apiVersion;
        }
    }
    // Standard FFX Vulkan backend descriptors do not carry the instance API
    // version. Unknown callers take the conservative compatibility path.
    return VK_API_VERSION_1_1;
}

std::uint64_t findVersion(const ffxApiHeader* header) {
    for (auto* current = header; current; current = current->pNext) {
        if (current->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION) {
            return reinterpret_cast<const ffxOverrideVersion*>(current)->versionId;
        }
    }
    return 0;
}

float upscaleRatio(std::uint32_t qualityMode) {
    constexpr float ratios[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return qualityMode < std::size(ratios) ? ratios[qualityMode] : 0.0f;
}

float radicalInverse(std::uint32_t index, std::uint32_t base) {
    float value = 0.0f;
    const float inverseBase = 1.0f / static_cast<float>(base);
    float fraction = inverseBase;
    while (index) {
        value += static_cast<float>(index % base) * fraction;
        index /= base;
        fraction *= inverseBase;
    }
    return value;
}

const char* dispatchScalarProblem(const ffxDispatchDescUpscale& d) noexcept {
    if(!std::isfinite(d.jitterOffset.x) || !std::isfinite(d.jitterOffset.y))
        return "jitterOffset must contain finite values";
    if(!std::isfinite(d.motionVectorScale.x) || !std::isfinite(d.motionVectorScale.y))
        return "motionVectorScale must contain finite values";
    return nullptr;
}

ffxReturnCode_t prepareVulkanDevice(ffxQueryDescVkPrepareDevice* query) noexcept {
    if (!query || !query->physicalDevice || !query->sourceCreateInfo) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    query->outputCreateInfo = nullptr;
    query->token = nullptr;
    query->errorMessage = nullptr;
    static thread_local std::string error;

    try {
        auto requirements = std::make_unique<fsr4vk::DeviceFeatures>(
            query->physicalDevice, *query->sourceCreateInfo, query->apiVersion,
            query->getPhysicalDeviceFeatures2,
            query->enumerateDeviceExtensionProperties);
        // Negotiation happens before a provider context exists; still record the
        // exact disabled/missing request fields in the same opt-in game log.
        try {
            const auto path=diagnostic_path();
            if(!path.empty()) write_diagnostic(path,0,fsr4vk::device_enablement_diagnostic(
                *query->sourceCreateInfo,*requirements->get()));
        } catch(...) { }
        query->outputCreateInfo = requirements->get();
        query->token = requirements.release();
        return FFX_API_RETURN_OK;
    } catch (const std::exception& exception) {
        error = exception.what();
        error += " (device feature chain:";
        auto* node = static_cast<const VkBaseInStructure*>(query->sourceCreateInfo->pNext);
        for (unsigned i=0; node && i<64; ++i, node=node->pNext)
            error += " " + std::to_string(node->sType);
        error += ")";
        query->errorMessage = error.c_str();
        write_diagnostic(diagnostic_path(),0,"device negotiation failed: "+error);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    } catch (...) {
        error = "unknown Vulkan device-requirements failure";
        query->errorMessage = error.c_str();
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
}

ffxReturnCode_t releaseVulkanDevice(ffxQueryDescVkReleaseDevice* query) noexcept {
    if (!query || !query->token) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    delete static_cast<fsr4vk::DeviceFeatures*>(query->token);
    query->token = nullptr;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t queryUpscale(ffxContext* context, ffxQueryDescHeader* header) {
    switch (header->type) {
    case FSR4VK_QUERY_DESC_TYPE_DEVICE_FEATURES:
        reinterpret_cast<Fsr4VkQueryDeviceFeatures*>(header)->version=1;
        return FFX_API_RETURN_OK;
    case FSR4VK_QUERY_DESC_TYPE_NATIVE_INPUTS: {
        auto* query = reinterpret_cast<Fsr4VkQueryNativeInputs*>(header);
        query->version = 0;
        if (context && !*context) return FFX_API_RETURN_ERROR_PARAMETER;
        query->version = 1;
        return FFX_API_RETURN_OK;
    }
    case FSR4VK_QUERY_DESC_TYPE_PRESET_CAPABILITIES: {
        auto* query = reinterpret_cast<Fsr4VkQueryPresetCapabilities*>(header);
        query->forcedPresetMask = 0;
        if (!context || !*context) return FFX_API_RETURN_ERROR_PARAMETER;
        query->forcedPresetMask = 0x3fu; // Presets 0 through 5, including DRS.
        return FFX_API_RETURN_OK;
    }
    case FSR4VK_QUERY_DESC_TYPE_ACTIVE_PRESET: {
        auto* query = reinterpret_cast<Fsr4VkQueryActivePreset*>(header);
        query->activePreset = FSR4VK_PRESET_UNKNOWN;
        if (!context || !*context) return FFX_API_RETURN_ERROR_PARAMETER;
        const auto& preset = static_cast<ProviderContext*>(*context)->active_preset;
        if (preset == "native") query->activePreset = 0;
        else if (preset == "quality") query->activePreset = 1;
        else if (preset == "balanced") query->activePreset = 2;
        else if (preset == "performance") query->activePreset = 3;
        else if (preset == "drs") query->activePreset = 4;
        else if (preset == "ultraperf" || preset == "performance/u15") query->activePreset = 5;   // The game asked UP
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
        auto* query = reinterpret_cast<ffxQueryGetProviderVersion*>(header);
        if (!context || !*context) return FFX_API_RETURN_ERROR_PARAMETER;
        query->versionId = static_cast<ProviderContext*>(*context)->version;
        query->versionName = query->versionId==kVersionLiteId ? kVersionLiteName : kVersion411Name;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*>(header);
        const float ratio = upscaleRatio(query->qualityMode);
        if (!query->pOutUpscaleRatio || ratio == 0.0f) return FFX_API_RETURN_ERROR_PARAMETER;
        *query->pOutUpscaleRatio = ratio;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(header);
        const float ratio = upscaleRatio(query->qualityMode);
        if (!query->pOutRenderWidth || !query->pOutRenderHeight || ratio == 0.0f) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        if(!query->displayWidth || !query->displayHeight) return FFX_API_RETURN_ERROR_PARAMETER;
        const double width=std::ceil(double(query->displayWidth)/double(ratio));
        const double height=std::ceil(double(query->displayHeight)/double(ratio));
        if(width>UINT32_MAX || height>UINT32_MAX) return FFX_API_RETURN_ERROR_PARAMETER;
        *query->pOutRenderWidth = static_cast<std::uint32_t>(width);
        *query->pOutRenderHeight = static_cast<std::uint32_t>(height);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetJitterPhaseCount*>(header);
        if (!query->pOutPhaseCount || !query->renderWidth || !query->displayWidth)
            return FFX_API_RETURN_ERROR_PARAMETER;
        const double ratio = double(query->displayWidth)/double(query->renderWidth);
        const double phases=8.0*ratio*ratio;
        if(phases<1.0 || phases>double(INT32_MAX)) return FFX_API_RETURN_ERROR_PARAMETER;
        *query->pOutPhaseCount = static_cast<std::int32_t>(phases);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetJitterOffset*>(header);
        if (!query->pOutX || !query->pOutY || query->phaseCount <= 0 || query->index < 0) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        const std::uint32_t sample = static_cast<std::uint32_t>(query->index % query->phaseCount) + 1;
        *query->pOutX = radicalInverse(sample, 2) - 0.5f;
        *query->pOutY = radicalInverse(sample, 3) - 0.5f;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetResourceRequirements*>(header);
        query->required_resources = FFX_API_QUERY_RESOURCE_INPUT_COLOR |
                                    FFX_API_QUERY_RESOURCE_INPUT_DEPTH |
                                    FFX_API_QUERY_RESOURCE_INPUT_MV |
                                    FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE;
        query->optional_resources = 0;
        if (context && *context && (static_cast<ProviderContext*>(*context)->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE))
            query->required_resources &= ~FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsage*>(header);
        if (!context || !*context || !query->gpuMemoryUsageUpscaler) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        *query->gpuMemoryUsageUpscaler={};
        for(const auto& [preset,core]:static_cast<ProviderContext*>(*context)->cores)
            if(core) query->gpuMemoryUsageUpscaler->totalUsageInBytes+=core->owned_allocation_bytes();
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2: {
        auto* query = reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsageV2*>(header);
        if (!query->gpuMemoryUsageUpscaler) return FFX_API_RETURN_ERROR_PARAMETER;
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

}  // namespace

static ffxReturnCode_t createContextImpl(
    ffxContext* context,
    ffxCreateContextDescHeader* header,
    const ffxAllocationCallbacks* callbacks) {
    if (!context || !header || !validAllocator(callbacks)) return FFX_API_RETURN_ERROR_PARAMETER;
    *context = nullptr;
    if (header->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    validateApiChain(header);
    for(const auto type : std::array<ffxStructType_t,4>{kBackendVkDescType,FFX_API_DESC_TYPE_OVERRIDE_VERSION,kFsr4VulkanApiVersionDescType,FSR4VK_CREATE_DESC_TYPE_DEVICE_FEATURES}) {
        bool found=false;
        for(auto* node=header;node;node=node->pNext) if(node->type==type) {
            if(found) return FFX_API_RETURN_ERROR_PARAMETER;
            found=true;
        }
    }
    const auto version = findVersion(header);
    // An explicit FSR 4.0.2 request is refused (not in this build); unversioned callers get 4.1.1. Some games
    // loading amd_fidelityfx_vk.dll pin the stock FSR 3.1.4 provider ID
    // instead; accept that ABI-compatible selection as a drop-in alias.
    if (version != 0 && version != kVersion411Id && version != kVersionLiteId &&
        version != kFsr314CompatibilityVersionId) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    const auto* backend = findBackend(header);
    if (!backend || !backend->device || !backend->physicalDevice ||
        !backend->getDeviceProcAddr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const auto* create = reinterpret_cast<const ffxCreateContextDescUpscale*>(header);
    void* storage = allocate(callbacks, sizeof(ProviderContext));
    if (!storage) return FFX_API_RETURN_ERROR_MEMORY;
    ProviderContext* provider=nullptr;
    try { provider=new (storage) ProviderContext{}; }
    catch (...) { deallocate(callbacks,storage); throw; }
    provider->version = version==kVersionLiteId ? kVersionLiteId : kVersion411Id;
    for(auto* node=header->pNext;node;node=node->pNext) if(node->type==kVelaLossyDescType)
        provider->lite_every=reinterpret_cast<const VelaLossyDesc*>(node)->networkEvery;
    for(auto* node=header->pNext;node;node=node->pNext) if(node->type==kVelaNpuDescType)
        provider->lite_npu=reinterpret_cast<const VelaNpuDesc*>(node)->mode;
    provider->device = backend->device;
    provider->physicalDevice = backend->physicalDevice;
    provider->getDeviceProcAddr = backend->getDeviceProcAddr;
    provider->apiVersion = findVulkanApiVersion(header);
    for(auto* node=header;node;node=node->pNext) if(node->type==FSR4VK_CREATE_DESC_TYPE_DEVICE_FEATURES) {
        const auto& info=*reinterpret_cast<const Fsr4VkCreateDeviceFeatures*>(node);
        if(info.enabledFlags & ~(FSR4VK_DEVICE_ROBUST_BUFFER_ACCESS|FSR4VK_DEVICE_NATIVE_MIXED_DOT)) {
            provider->~ProviderContext(); deallocate(callbacks,provider);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        provider->robust_buffer_access=(info.enabledFlags & FSR4VK_DEVICE_ROBUST_BUFFER_ACCESS)!=0;
        provider->queue_family_index=info.queueFamilyIndex;
        provider->native_mixed_dot_enabled=(info.enabledFlags & FSR4VK_DEVICE_NATIVE_MIXED_DOT)!=0;
        if(info.apiVersion>=VK_API_VERSION_1_1) provider->apiVersion=info.apiVersion;
    }
    provider->flags = create->flags;
    provider->maxRenderSize = create->maxRenderSize;
    provider->maxUpscaleSize = create->maxUpscaleSize;
    provider->message = create->fpMessage;
    try {
        const auto assets=asset_root()/"fsr411";   // the only asset bundle (4.1.1); Lite reads none
        provider->assets=assets;
        const bool embedded=fsr4assets::is_embedded(assets);
        if (!embedded) {
            if (!std::filesystem::is_directory(assets/"general"))
                throw std::invalid_argument("general-resolution asset bundles are missing");
        }
        provider->diagnostic_path=diagnostic_path(assets);
        // Vela 0.8.3: the full FSR 4.1.1 network on the NPU (nf4_exact through VK_LAYER_VELA_npu, async), 960x540 -> 1920x1080 only
        if(provider->version==kVersion411Id) {
            // Any render size whose preset carries the 540p weights (all but ultraperf = render width <= 642 at 1080p, fv_npu.hpp);
            // each dispatch re-checks its own render size (fv::g_render_ok)
            const char* const max_preset=selectedPreset(*provider,create->maxRenderSize.width,create->maxUpscaleSize.width);
            // Vela 0.8.10: the NPU path needs the 1080p model layout (QualityContext model_capacity 1920x1080 = every
            // maxUpscaleSize <= 1920x1080; scratch_capacity is the same 1080p buffer), not maxUpscaleSize == 1920x1080: a game at
            // 1280x720 / 1600x900 creates the context with maxUpscaleSize = its output, and 0.8.9 turned the NPU off for it here
            // ("fallback size 640x360->1280x720"). The output size itself is checked per frame (fv_npu.hpp begin_frame: 1080p/900p/720p).
            // Vela 0.8.14: ultraperf is no longer excluded here: the network (P or UP) is chosen per dispatch (fv::g_net_up) and
            // fv_npu.hpp turns the NPU on only where a graph exists for (network, output); else "fallback size", GPU path
            (void)max_preset;
            const bool fit=create->maxUpscaleSize.width<=1920 && create->maxUpscaleSize.height<=1080;
            fv::g_request=provider->lite_npu && fit ? 2u : 0u;
            if(provider->lite_npu && !fit) provider_message(*provider,FFX_API_MESSAGE_TYPE_WARNING,"Vela NPU: fallback size "+
                std::to_string(create->maxRenderSize.width)+"x"+std::to_string(create->maxRenderSize.height)+"->"+
                std::to_string(create->maxUpscaleSize.width)+"x"+std::to_string(create->maxUpscaleSize.height)+", GPU path");
            diagnostic(*provider,"FSR 4.1.1 full network on the NPU: request="+std::to_string(provider->lite_npu)+" fit="+std::to_string(fit)+
                " graph nf4_exact, async");
        }
        if(provider->version==kVersionLiteId) {
            if(!fsr4core::LiteContext::supports(create->maxRenderSize.width,create->maxRenderSize.height,
                                                create->maxUpscaleSize.width,create->maxUpscaleSize.height))
                throw std::invalid_argument("FSR 4.1.2 Lite supports only maxUpscaleSize 1920x1080 with maxRenderSize width 645..1920 (AMD's 540p model)");
            if(provider->lite_every<1 || provider->lite_every>16) throw std::invalid_argument("FSR 4.1.2 Lite: networkEvery must be 1..16");
            provider->lite=std::make_unique<fsr4core::LiteContext>(provider->physicalDevice,provider->device,provider->lite_every,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,provider->lite_npu,create->maxRenderSize.width,create->maxRenderSize.height);
            diagnostic(*provider,"FSR 4.1.2 Lite ready: render="+std::to_string(provider->lite->render_width())+"x"+std::to_string(provider->lite->render_height())+" npu="+std::to_string(provider->lite->npu_mode())+" network_every="+std::to_string(provider->lite->network_every())+
                " subgroup32="+std::to_string(provider->lite->subgroup32())+" subgroup="+std::to_string(provider->lite->subgroup_min())+".."+
                std::to_string(provider->lite->subgroup_max())+" owned_bytes="+std::to_string(provider->lite->owned_bytes())+
                " kernels "+fsr4lite::kKernelMd5);
        }
        diagnostic(*provider,"diagnostics_schema=2 provider_build=" __DATE__ " " __TIME__);
        provider->motion_override=fsr4vk::motion_format_override(std::getenv("FSR4_VK_MOTION_FORMAT"));
        if(provider->motion_override!=VK_FORMAT_UNDEFINED)
            diagnostic(*provider,"explicit motion format override=VK_FORMAT_R16G16_SNORM; applies only to FFX R8G8B8A8_SNORM metadata; original VkImage format must match; no automatic alias guessing");
        diagnostic(*provider,"context request flags="+std::to_string(create->flags)+
                   " max_render="+std::to_string(create->maxRenderSize.width)+"x"+
                   std::to_string(create->maxRenderSize.height)+" max_output="+
                   std::to_string(create->maxUpscaleSize.width)+"x"+
                   std::to_string(create->maxUpscaleSize.height));
        diagnostic(*provider,std::string("version=")+
                   (provider->version==kVersionLiteId ? kVersionLiteName : kVersion411Name));
        const uint32_t required_flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        const uint32_t permutation = create->flags & ~(FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
                                                        FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                                                        FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE);
        fsr4::validate_dimensions(create->maxRenderSize.width,create->maxRenderSize.height,
                                 create->maxUpscaleSize.width,create->maxUpscaleSize.height);
        if(permutation!=required_flags)
            throw std::invalid_argument("provider requires inverted depth and low-resolution non-jittered motion vectors");
        fsr4vk::validate_descriptor_commands(backend->device,backend->getDeviceProcAddr);
        (void)fsr4vk::DeviceDispatch(backend->device,backend->getDeviceProcAddr,provider->apiVersion,false);
        diagnostic(*provider,"context created flags="+std::to_string(create->flags)+
                   " api="+std::to_string(VK_API_VERSION_MAJOR(provider->apiVersion))+"."+
                   std::to_string(VK_API_VERSION_MINOR(provider->apiVersion)));
        diagnostic(*provider,"input contract: inverted_depth=true motion_vectors=render-resolution,non-jittered exposure="+
            std::string((create->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) ? "automatic (external exposure ignored)" : "external (1x1 R16_FLOAT or R32_FLOAT required)")+
            " HDR="+std::to_string(bool(create->flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE)));
    } catch (const std::exception& error) {
        try { provider_message(*provider,FFX_API_MESSAGE_TYPE_ERROR,std::string("context rejected: ")+error.what()); } catch (...) { }
        provider->~ProviderContext();
        deallocate(callbacks,provider);
        return dynamic_cast<const std::bad_alloc*>(&error) ? FFX_API_RETURN_ERROR_MEMORY : FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    } catch (...) {
        provider->~ProviderContext();deallocate(callbacks,provider);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    *context = provider;
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t destroyContextImpl(
    ffxContext* context,
    const ffxAllocationCallbacks* callbacks) {
    if (!context || !*context || !validAllocator(callbacks)) return FFX_API_RETURN_ERROR_PARAMETER;
    auto* provider = reinterpret_cast<ProviderContext*>(*context);
    try { diagnostic(*provider,"context destroyed cached_models="+std::to_string(provider->cores.size())+
        " dispatch_attempts="+std::to_string(provider->dispatch_attempts)+
        " dispatches_recorded="+std::to_string(provider->dispatch_successes)+
        " rejected_or_failed="+std::to_string(provider->dispatch_attempts-provider->dispatch_successes)+
        " (recorded does not imply GPU completion)"); } catch (...) { }
    provider->~ProviderContext();
    deallocate(callbacks, provider);
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t configureImpl(
    ffxContext* context,
    const ffxConfigureDescHeader* header) {
    if (!context || !*context || !header) return FFX_API_RETURN_ERROR_PARAMETER;
    const std::lock_guard<std::recursive_mutex> lock(static_cast<ProviderContext*>(*context)->api_mutex);
    validateApiChain(header);
    if (header->type == FSR4VK_CONFIGURE_DESC_TYPE_PRESET) {
        const auto preset = reinterpret_cast<const Fsr4VkConfigurePreset*>(header)->preset;
        if (preset != FSR4VK_PRESET_AUTO && !presetName(preset))
            return FFX_API_RETURN_ERROR_PARAMETER;
        // No allocations or resource destruction: cached cores may still be in
        // flight. Dispatch lazily creates the chosen model and resets history
        // when active_preset changes. Reporting stays tied to actual dispatch.
        static_cast<ProviderContext*>(*context)->forced_preset = preset;
        return FFX_API_RETURN_OK;
    }
    if (header->type == FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE) {
        auto& provider=*static_cast<ProviderContext*>(*context);
        if(!provider.unsupported_config_logged) {
            provider_message(provider,FFX_API_MESSAGE_TYPE_WARNING,
                "FSR4VK_WARNING_UNSUPPORTED_CONFIGURATION: standard key/value controls are not applied by the captured INT8 models; preset control is supported separately");
            provider.unsupported_config_logged=true;
        }
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

static ffxReturnCode_t queryImpl(
    ffxContext* context,
    ffxQueryDescHeader* header) {
    if (!header) return FFX_API_RETURN_ERROR_PARAMETER;
    validateApiChain(header);
    std::unique_lock<std::recursive_mutex> lock;
    if(context && *context) lock=std::unique_lock<std::recursive_mutex>(static_cast<ProviderContext*>(*context)->api_mutex);
    if (!context && header->type == FFX_API_QUERY_DESC_TYPE_VK_PREPARE_DEVICE) {
        return prepareVulkanDevice(reinterpret_cast<ffxQueryDescVkPrepareDevice*>(header));
    }
    if (!context && header->type == FFX_API_QUERY_DESC_TYPE_VK_RELEASE_DEVICE) {
        return releaseVulkanDevice(reinterpret_cast<ffxQueryDescVkReleaseDevice*>(header));
    }
    if (!context && header->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* query = reinterpret_cast<ffxQueryDescGetVersions*>(header);
        if (!query->outputCount) return FFX_API_RETURN_ERROR_PARAMETER;
        if (query->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
            *query->outputCount = 0;
            return FFX_API_RETURN_OK;
        }
        const std::uint64_t capacity = *query->outputCount;
        if (capacity > 0) {   // 4.1.1 first (no 4.0.2 in this build)
            if (query->versionIds) query->versionIds[0] = kVersion411Id;
            if (query->versionNames) query->versionNames[0] = kVersion411Name;
        }
        if (capacity > 1) {
            if (query->versionIds) query->versionIds[1] = kVersionLiteId;
            if (query->versionNames) query->versionNames[1] = kVersionLiteName;
        }
        *query->outputCount = 2;
        return FFX_API_RETURN_OK;
    }
    return queryUpscale(context, header);
}

static ffxReturnCode_t dispatchImpl(
    ffxContext* context,
    const ffxDispatchDescHeader* header) {
    if (!context || !*context || !header) return FFX_API_RETURN_ERROR_PARAMETER;
    const std::lock_guard<std::recursive_mutex> lock(static_cast<ProviderContext*>(*context)->api_mutex);
    validateApiChain(header);
    if (header->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    auto* provider = static_cast<ProviderContext*>(*context);
    const auto& d = *reinterpret_cast<const ffxDispatchDescUpscale*>(header);
    ++provider->dispatch_attempts;
    const char* stage="dispatch validation";
    const auto rw=d.renderSize.width,rh=d.renderSize.height;
    const auto gow=d.upscaleSize.width ? d.upscaleSize.width : provider->maxUpscaleSize.width;
    const auto goh=d.upscaleSize.height ? d.upscaleSize.height : provider->maxUpscaleSize.height;
    // Fast + VELA_FAST_UP15: a 3x request (1080p UP) runs the FSR 4 core as Performance at 2 x render; the fused K2 writes the game's 3x output
    if(provider->u15_mode<0) { provider->u15_mode=provider->version==kVersion411Id && fv::fast_setting() ? fv::u15_setting() : 0;
        if(provider->u15_mode) provider_message(*provider,FFX_API_MESSAGE_TYPE_WARNING,std::string(provider->u15_mode>=3 ? "Fast 1.5x: Fast UP as P720 + separate 1.5x pass (only when output = 3 x render; off: fastup15=0 / VELA_FAST_UP15=0), mode " : "Fast 1.5x: Fast UP as P720 + fused 1.5x K2, mode ")+std::to_string(provider->u15_mode)); }
    const bool u15=provider->u15_mode>0 && gow==3*rw && goh==3*rh && gow==provider->maxUpscaleSize.width && goh==provider->maxUpscaleSize.height;
    const auto ow=u15 ? 2*rw : gow;
    const auto oh=u15 ? 2*rh : goh;
    const uint32_t capw=u15 ? 2*rw : provider->maxUpscaleSize.width, caph=u15 ? 2*rh : provider->maxUpscaleSize.height;
    const auto reject = [provider](const char* reason) {
        provider_message(*provider,FFX_API_MESSAGE_TYPE_ERROR,
                         "FSR4 Vulkan dispatch rejected: attempt="+std::to_string(provider->dispatch_attempts)+" "+reason);
        return FFX_API_RETURN_ERROR_PARAMETER;
    };
    if (!d.commandList) return reject("null command list");
    if(const auto* problem=dispatchScalarProblem(d)) return reject(problem);
    if (d.enableSharpening && !provider->sharpening_ignored_logged) {
        provider_message(*provider, FFX_API_MESSAGE_TYPE_WARNING,
                         "FSR4 Vulkan internal sharpening is unavailable; use host RCAS if desired");
        provider->sharpening_ignored_logged = true;
    }
    if (d.flags & ~(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB | FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ))
        return reject("dispatch flags other than sRGB/PQ color selection are unsupported");
    const auto color_space=fsr4core::select_color_space(
        (provider->flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE)!=0,
        (d.flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB)!=0,
        (d.flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ)!=0);
    // FSR2/3-compatible hosts may supply a reactive/composition mask (including
    // a converted DLSS bias mask). The current FSR4 model path has no external
    // mask inputs. Accept their presence without reading, binding or changing
    // the caller's resources; optional inputs must not prevent upscaling.
    if ((d.reactive.resource || d.transparencyAndComposition.resource) &&
        !provider->optional_masks_ignored_logged) {
        provider_message(*provider, FFX_API_MESSAGE_TYPE_WARNING,
                         "FSR4 Vulkan ignores optional reactive/transparency masks; "
                         "the current model path does not consume them");
        provider->optional_masks_ignored_logged = true;
    }
    std::string resource_errors;
    const auto resource = [&resource_errors](const char* name, const FfxApiResource& r, uint32_t width, uint32_t height,
                             uint32_t format, VkFormat vkformat, uint32_t state,
                             const char* accepted, uint32_t depth_usage = 0) {
        const auto problem=fsr4vk::resource_problem(name,r,width,height,format,state,depth_usage,accepted);
        if(!problem.empty()) {
            if(!resource_errors.empty()) resource_errors+=" | ";
            resource_errors+=problem;
        }
        return fsr4core::Image{reinterpret_cast<VkImage>(r.resource),VK_NULL_HANDLE,
                              VK_NULL_HANDLE,vkformat,width,height};
    };
    try {
        if(!provider->resource_snapshot_logged && !provider->diagnostic_path.empty()) {
            diagnostic(*provider,"first-dispatch inputs: render="+std::to_string(rw)+"x"+std::to_string(rh)+
                " output="+std::to_string(ow)+"x"+std::to_string(oh)+
                " preExposure="+std::to_string(d.preExposure)+" reset="+std::to_string(d.reset)+
                " dispatch_flags="+std::to_string(d.flags));
            const std::pair<const char*,const FfxApiResource*> inputs[]={
                {"color",&d.color},{"depth",&d.depth},{"motionVectors",&d.motionVectors},
                {"exposure",&d.exposure},{"output",&d.output}};
            for(const auto& entry : inputs)
                diagnostic(*provider,fsr4vk::resource_snapshot(entry.first,*entry.second));
            provider->resource_snapshot_logged=true;
        }
        fsr4::validate_dimensions(rw,rh,ow,oh);
        if(rw>provider->maxRenderSize.width || rh>provider->maxRenderSize.height ||
           ow>provider->maxUpscaleSize.width || oh>provider->maxUpscaleSize.height)
            throw std::invalid_argument("dispatch dimensions exceed context capacity");
        stage="resource validation";
        const bool packed_color=d.color.description.format==FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
        const bool shared_exponent_color=
            d.color.description.format==FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;
        const bool packed_output=d.output.description.format==FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
        const uint32_t depth_stencil=FFX_API_RESOURCE_USAGE_DEPTHTARGET | FFX_API_RESOURCE_USAGE_STENCILTARGET;
        const uint32_t depth_usage=d.depth.description.usage & depth_stencil;
        // The host marks real depth images separately from R32 color images.
        // D32-only and D32S8 must both be sampled through the DEPTH aspect.
        const auto depth_format=depth_usage==FFX_API_RESOURCE_USAGE_STENCILTARGET
            ? VK_FORMAT_UNDEFINED : fsr4vk::depth_view_format(depth_usage);
        const auto color_format=shared_exponent_color ? FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP :
            (packed_color ? FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT : FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT);
        const auto color_vk_format=shared_exponent_color ? VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 :
            (packed_color ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT);
        const auto color = resource("color",d.color,rw,rh,color_format,color_vk_format,
            FFX_API_RESOURCE_STATE_COMPUTE_READ,"R16G16B16A16_FLOAT, R11G11B10_FLOAT, R9G9B9E5_SHAREDEXP");
        const auto depth = resource("depth",d.depth,rw,rh,FFX_API_SURFACE_FORMAT_R32_FLOAT,
            depth_format,FFX_API_RESOURCE_STATE_COMPUTE_READ,
            "R32_FLOAT (R32_SFLOAT with no depth flags; D32_SFLOAT with DEPTHTARGET; D32_SFLOAT_S8_UINT with both depth/stencil flags)",
            depth_format==VK_FORMAT_UNDEFINED ? 0 : depth_usage);
        // Native metadata wins over the legacy environment override. It belongs
        // to this dispatch, so resource/preset changes cannot reuse stale formats.
        VkFormat native_motion=VK_FORMAT_UNDEFINED;
        bool native_motion_supplied=false;
        unsigned chain_length=0;
        for(auto* node=d.header.pNext;node;node=node->pNext) {
            if(++chain_length>64) throw std::invalid_argument("dispatch extension chain is cyclic or too long");
            if(node->type!=FSR4VK_DISPATCH_DESC_TYPE_NATIVE_INPUTS) continue;
            if(native_motion_supplied) throw std::invalid_argument("duplicate native input format descriptor");
            native_motion_supplied=true;
            native_motion=static_cast<VkFormat>(reinterpret_cast<const Fsr4VkDispatchNativeInputs*>(node)->motionFormat);
            if(native_motion!=VK_FORMAT_R16G16_SNORM && native_motion!=VK_FORMAT_R16G16_SFLOAT)
                throw std::invalid_argument("motionVectors: native VkFormat="+std::to_string(native_motion)+
                    " incompatible; accepted VK_FORMAT_R16G16_SNORM(78), VK_FORMAT_R16G16_SFLOAT(83)");
            const auto expected=native_motion==VK_FORMAT_R16G16_SNORM ? FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM : FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
            if(d.motionVectors.description.format!=expected)
                throw std::invalid_argument("motionVectors: native VkFormat contradicts FFX format="+
                    fsr4vk::surface_format(d.motionVectors.description.format));
        }
        const auto motion_view=native_motion_supplied ? native_motion : provider->motion_override;
        const bool snorm_motion=d.motionVectors.description.format==FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM &&
            motion_view!=VK_FORMAT_UNDEFINED;
        const auto motion = resource("motionVectors",d.motionVectors,rw,rh,
            snorm_motion ? FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM : FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
            snorm_motion ? motion_view : VK_FORMAT_R16G16_SFLOAT,FFX_API_RESOURCE_STATE_COMPUTE_READ,
            snorm_motion ? "R16G16_FLOAT; R8G8B8A8_SNORM metadata with explicit original-VkImage format override" :
                "R16G16_FLOAT (VK_FORMAT_R16G16_SFLOAT); ambiguous SNORM host aliases require an explicit FSR4_VK_MOTION_FORMAT matching the original VkImage");
        // The adapter samples floating-point exposure and writes our internal
        // R32 history. Preserve the caller's real format: R16 sampling expands
        // the value to float without reinterpreting its bits or adding a pass.
        const bool half_exposure=d.exposure.description.format==FFX_API_SURFACE_FORMAT_R16_FLOAT;
        const auto exposure = (provider->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) ? fsr4core::Image{} : resource("exposure",d.exposure,1,1,
            half_exposure ? FFX_API_SURFACE_FORMAT_R16_FLOAT : FFX_API_SURFACE_FORMAT_R32_FLOAT,
            half_exposure ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R32_SFLOAT,FFX_API_RESOURCE_STATE_COMPUTE_READ,"R16_FLOAT, R32_FLOAT");
        const auto output = resource("output",d.output,gow,goh,packed_output ? FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT : FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
            packed_output ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,"R16G16B16A16_FLOAT, R11G11B10_FLOAT");
        if(!resource_errors.empty()) throw std::invalid_argument(resource_errors);
        if(provider->first_dispatch && !provider->diagnostic_path.empty())
            diagnostic(*provider,"resolved input views: depth VkFormat="+std::to_string(depth.format)+
                " motion VkFormat="+std::to_string(motion.format)+
                (native_motion_supplied ? " source=host-native-metadata" : snorm_motion ? " source=legacy-environment-override" : " source=FFX")+
                (snorm_motion ? " (native SNORM sampling, not bit reinterpretation)" : ""));
        if(provider->version==kVersionLiteId) {
            stage="FSR 4.1.2 Lite recording";
            if(!fsr4core::LiteContext::supports(rw,rh,ow,oh) || rw!=provider->lite->render_width() || rh!=provider->lite->render_height())
                throw std::invalid_argument("FSR 4.1.2 Lite: render size must equal the context's maxRenderSize (width 645..1920) -> 1920x1080");
            if(color_space!=fsr4core::ColorSpace::Linear)
                throw std::invalid_argument("FSR 4.1.2 Lite supports linear colour only");
            if(!provider->lite) throw std::runtime_error("FSR 4.1.2 Lite context missing");
            fsr4core::OptimizedConstants c{};
            c.inv_size[0]=1.f/ow; c.inv_size[1]=1.f/oh;
            c.scale[0]=float(ow)/rw; c.scale[1]=float(oh)/rh;
            c.inv_scale[0]=1.f/c.scale[0]; c.inv_scale[1]=1.f/c.scale[1];
            c.jitter[0]=d.jitterOffset.x; c.jitter[1]=d.jitterOffset.y;
            c.mv_scale[0]=d.motionVectorScale.x/d.renderSize.width;
            c.mv_scale[1]=d.motionVectorScale.y/d.renderSize.height;
            c.tex_size[0]=provider->maxUpscaleSize.width; c.tex_size[1]=provider->maxUpscaleSize.height;
            c.max_render_size[0]=provider->maxRenderSize.width; c.max_render_size[1]=provider->maxRenderSize.height;
            c.width=ow; c.height=oh; c.width_lr=rw; c.height_lr=rh;
            c.reset=d.reset || provider->last_output_width!=ow || provider->last_output_height!=oh;
            c.pre_exposure=fsr4::sanitize_pre_exposure(d.preExposure);
            provider->lite->record(static_cast<VkCommandBuffer>(d.commandList),color,depth,motion,output,c);
            for(const auto& line : provider->lite->take_npu_lines())   // "Vela NPU: ..." lines for the panel
                provider_message(*provider,FFX_API_MESSAGE_TYPE_WARNING,line);
            provider->last_output_width=ow; provider->last_output_height=oh;
            if(provider->first_dispatch) { diagnostic(*provider,"first FSR 4.1.2 Lite dispatch recorded"); provider->first_dispatch=false; }
            ++provider->dispatch_successes;
            return FFX_API_RETURN_OK;
        }
        const std::string selected_preset=selectedPreset(*provider,rw,ow);
        if(provider->version==kVersion411Id) { fv::g_render_w=rw; fv::g_render_h=rh; fv::g_render_ok=true; fv::g_net_up=selected_preset=="ultraperf"; }   // Network per dispatch (UP graphs in fv_npu.hpp)
        // Repeated dispatches use the same owned core without a map lookup.
        // std::map keeps the other cores alive; switching never frees in-flight work.
        const std::string core_key=u15 ? selected_preset+"/u15" : selected_preset;   // Own core (capacity 2 x render, fused K2)
        auto* core=provider->active_preset==core_key ? provider->active_core : nullptr;
        if (!core) core=provider->cores[core_key].get();
        if(!core) {
            if (!provider->device_support_checked) {
                stage="device capability check";
                // This queries physical support, NOT the bits enabled on the
                // application's already-created VkDevice. Vulkan exposes no
                // general post-creation query for those bits.
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(provider->physicalDevice,&properties);
                diagnostic(*provider,"device="+std::string(properties.deviceName)+
                    " vendorID="+std::to_string(properties.vendorID)+" deviceID="+std::to_string(properties.deviceID)+
                    " driverVersion_raw="+std::to_string(properties.driverVersion)+
                    " physical_API="+std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion))+"."+
                    std::to_string(VK_API_VERSION_MINOR(properties.apiVersion))+"."+
                    std::to_string(VK_API_VERSION_PATCH(properties.apiVersion))+
                    " (properties reported by loader; host may spoof identity)");
                VkDeviceCreateInfo empty{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                try {
                    fsr4vk::DeviceFeatures supported(provider->physicalDevice,empty,properties.apiVersion,
                        vkGetPhysicalDeviceFeatures2,vkEnumerateDeviceExtensionProperties);
                    if(!provider->diagnostic_path.empty()) {
                        // Use the application's API version for promoted-extension requirements.
                        try {
                            fsr4vk::DeviceFeatures contract(provider->physicalDevice,empty,provider->apiVersion,
                                vkGetPhysicalDeviceFeatures2,vkEnumerateDeviceExtensionProperties);
                            diagnostic(*provider,fsr4vk::device_contract_diagnostic(*contract.get()));
                        } catch(const std::exception& error) {
                            diagnostic(*provider,std::string("application API contract check: ")+error.what());
                        }
                    }
                } catch(const std::exception& error) {
                    throw std::runtime_error(std::string("FSR4VK_ERROR_DEVICE_CAPABILITY_CHECK: ")+error.what()+
                        "; check GPU/driver support; unsupported capabilities cannot be enabled by the provider");
                }
                if(provider->robust_buffer_access.has_value())
                    diagnostic(*provider,"host device snapshot: robustBufferAccess="+std::to_string(*provider->robust_buffer_access)+
                        " nativeMixedDotEnabled="+std::to_string(provider->native_mixed_dot_enabled.value_or(false))+
                        " (snapshot from successful device creation; other required bits still rely on negotiation contract)");
                else diagnostic(*provider,"physical capability check passed; enabled feature status=UNKNOWN (Vulkan cannot query enabled bits after vkCreateDevice). "
                        "See context=0 device negotiation or supply the optional successful device-create snapshot. Physical support is not proof of enablement.");
                provider->device_support_checked=true;
            }
            diagnostic(*provider,"creating model="+selected_preset+
                       " exposure="+((provider->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) ? "auto" : "external"));
            stage="model/pipeline creation";
            const auto build_started=std::chrono::steady_clock::now();
            const auto bucket=provider->maxUpscaleSize.width>1920 || provider->maxUpscaleSize.height>1080 ? "2160" : "1080";
            const auto bundle=provider->assets/"general"/bucket/selected_preset;
            fv::g_u15=u15 ? provider->u15_mode : 0;   // Read by the QualityContext constructor (K2 choice)
            auto created=std::make_unique<fsr4core::QualityContext>(provider->physicalDevice,provider->device,
                bundle,bundle/"initializers.bin",bundle/"weights.bin",true,
                provider->getDeviceProcAddr,
                provider->maxRenderSize.width,provider->maxRenderSize.height,
                capw,caph,
                provider->apiVersion,
                (provider->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE)==0,provider->version==kVersion411Id,
                provider->robust_buffer_access,provider->native_mixed_dot_enabled,
                provider->diagnostic_path.empty() ? std::function<void(const std::string&)>{} :
                    std::function<void(const std::string&)>{[path=provider->diagnostic_path,id=provider->diagnostic_id](const auto& text){write_diagnostic(path,id,text);}},
                provider->queue_family_index);
            core=created.get();
            fv::g_u15=0;
            provider->cores[core_key]=std::move(created);
            if(!provider->diagnostic_path.empty()) {
                const auto& allocation=core->initializer_allocation();
                diagnostic(*provider,"initializer_memory bytes="+std::to_string(allocation.size)+
                    " type="+std::to_string(allocation.memory_type_index)+
                    " flags="+std::to_string(allocation.memory_properties)+
                    " device_local="+std::to_string(bool(allocation.memory_properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))+
                    " fallback="+std::to_string(allocation.preferred_memory_fallback));
            }
            diagnostic(*provider,"model ready="+selected_preset+" cpu_build_ms="+
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now()-build_started).count())+
                " owned_device_memory_bytes="+std::to_string(core->owned_allocation_bytes()));
        }
        stage="color-space pipeline selection";
        const bool color_changed=core->set_color_space(color_space);
        provider->pending_color_reset |= color_changed;
        if(color_changed || provider->first_dispatch)
            diagnostic(*provider,std::string("color_space=")+fsr4core::color_space_name(color_space)+
                       " history_reset=1");
        const bool force_reset=provider->pending_color_reset || provider->active_preset!=core_key ||
                               provider->last_output_width!=ow ||
                               provider->last_output_height!=oh;
        stage="dispatch recording";
        fsr4core::OptimizedConstants c{};
        const bool fsr411=provider->version==kVersion411Id;
        // Unlike 4.0.2, 4.1.1 keeps image sampling at the exact output size.
        // Its recorder separately pads the neural tensors to eight pixels.
        const auto padded_w=fsr411 ? ow : fsr4::round_up(ow,8);
        const auto padded_h=fsr411 ? oh : fsr4::round_up(oh,8);
        c.inv_size[0]=1.f/padded_w; c.inv_size[1]=1.f/padded_h;
        c.scale[0]=float(padded_w)/rw; c.scale[1]=float(padded_h)/rh;
        c.inv_scale[0]=1.f/c.scale[0]; c.inv_scale[1]=1.f/c.scale[1];
        c.jitter[0]=d.jitterOffset.x; c.jitter[1]=d.jitterOffset.y;
        // FFX API scales convert input vectors to pixels; the optimized shader
        // consumes normalized UV displacement. Only low-resolution MV is supported.
        c.mv_scale[0]=d.motionVectorScale.x/d.renderSize.width;
        c.mv_scale[1]=d.motionVectorScale.y/d.renderSize.height;
        c.tex_size[0]=fsr411 ? capw : fsr4::round_up(provider->maxUpscaleSize.width,8);
        c.tex_size[1]=fsr411 ? caph : fsr4::round_up(provider->maxUpscaleSize.height,8);
        c.max_render_size[0]=provider->maxRenderSize.width;
        c.max_render_size[1]=provider->maxRenderSize.height;
        c.width=padded_w; c.height=padded_h; c.width_lr=rw; c.height_lr=rh;
        c.reset=d.reset || force_reset;
        c.pre_exposure=fsr4::sanitize_pre_exposure(d.preExposure);
        if (c.pre_exposure!=d.preExposure && provider->first_dispatch)
            provider_message(*provider,FFX_API_MESSAGE_TYPE_WARNING,
                             "FSR4 Vulkan replaced invalid pre-exposure with 1.0");
        core->record(static_cast<VkCommandBuffer>(d.commandList),color,depth,motion,exposure,output,c);
        for(const auto& line : fv::take_lines())   // "Vela NPU: ..." lines of the full network
            provider_message(*provider,FFX_API_MESSAGE_TYPE_WARNING,line);
        provider->pending_color_reset=false;
        provider->active_preset=core_key;
        provider->active_core=core;
        provider->last_output_width=ow;provider->last_output_height=oh;
        if (provider->first_dispatch) {
            diagnostic(*provider,"preset="+selected_preset);
            diagnostic(*provider, std::string("shader_backend=") + core->shader_backend());
            diagnostic(*provider,"first dispatch recorded: "+std::to_string(rw)+"x"+
                std::to_string(rh)+" -> "+std::to_string(ow)+"x"+std::to_string(oh));
            diagnostic(*provider,"motion scale API="+std::to_string(d.motionVectorScale.x)+","+
                std::to_string(d.motionVectorScale.y)+" shader="+std::to_string(c.mv_scale[0])+","+
                std::to_string(c.mv_scale[1]));
            provider->first_dispatch=false;
        }
        ++provider->dispatch_successes;
        return FFX_API_RETURN_OK;
    } catch (const std::invalid_argument& error) {
        provider_message(*provider,FFX_API_MESSAGE_TYPE_ERROR,
                         "FSR4 Vulkan dispatch rejected: attempt="+std::to_string(provider->dispatch_attempts)+
                         " stage="+stage+"; "+error.what());
        return FFX_API_RETURN_ERROR_PARAMETER;
    } catch (const std::exception& error) {
        provider_message(*provider,FFX_API_MESSAGE_TYPE_ERROR,
                         "FSR4 Vulkan dispatch failed: attempt="+std::to_string(provider->dispatch_attempts)+
                         " stage="+stage+"; "+error.what());
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    } catch (...) {
        provider_message(*provider,FFX_API_MESSAGE_TYPE_ERROR,
                         "FSR4 Vulkan dispatch failed: unknown exception");
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
}

// No exception may cross the public C ABI, including failures while formatting
// another error or calling a noncompliant user allocation callback.
template<class Call> static ffxReturnCode_t apiBoundary(Call&& call) noexcept {
    try { return call(); }
    catch(const std::bad_alloc&) { return FFX_API_RETURN_ERROR_MEMORY; }
    catch(const std::invalid_argument&) { return FFX_API_RETURN_ERROR_PARAMETER; }
    catch(...) { return FFX_API_RETURN_ERROR_RUNTIME_ERROR; }
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context,ffxCreateContextDescHeader* desc,const ffxAllocationCallbacks* callbacks) {
    return apiBoundary([&]{return createContextImpl(context,desc,callbacks);});
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context,const ffxAllocationCallbacks* callbacks) {
    // Exclusive destruction after GPU completion is required by the host ABI.
    return apiBoundary([&]{return destroyContextImpl(context,callbacks);});
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context,const ffxConfigureDescHeader* desc) {
    return apiBoundary([&]{return configureImpl(context,desc);});
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context,ffxQueryDescHeader* desc) {
    return apiBoundary([&]{return queryImpl(context,desc);});
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context,const ffxDispatchDescHeader* desc) {
    return apiBoundary([&]{return dispatchImpl(context,desc);});
}
