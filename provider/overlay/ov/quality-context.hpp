#pragma once
#include "quality-recorder.hpp"
#include "fsr411-recorder.hpp"  // FV: includes fv_npu.hpp
#include "fsr411-quad-bounds.hpp"
#include "embedded-assets.hpp"
#include "vulkan-compat.hpp"
#include "vulkan-result-diagnostics.hpp"
#include "external-exposure-spv.hpp"
#include "color-space.hpp"
#include "buffer-memory-policy.hpp"
#include "descriptor-layout.hpp"
#include "resource-adoption.hpp"
#include "timestamp-policy.hpp"
#include "model11-row-bounds.hpp"
#include "../provider/vulkan_device_features.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <thread>
#include <mutex>
#include <exception>
#include <numeric>
#include "vela-baked-pre.hpp"

namespace fsr4core {
namespace fs = std::filesystem;
using fsr4::kPasses;
using fsr4::image_barrier;
constexpr uint32_t kDescriptorCount = 128;
constexpr VkDeviceSize kPhysicalConstantBackingSize = 2048;
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    uint32_t memory_type_index = 0;
    VkMemoryPropertyFlags memory_properties = 0;
    bool preferred_memory_fallback = false;
    mutable VkDeviceAddress cached_address=0;
    VkDeviceSize allocation_size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkDeviceSize allocation_size = 0;
};

struct OptimizedConstants {
    float inv_size[2];
    float scale[2];
    float inv_scale[2];
    float jitter[2];
    float mv_scale[2];
    float tex_size[2];
    float max_render_size[2];
    float motion_vector_jitter_cancellation[2];
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t reset;
    std::uint32_t width_lr;
    std::uint32_t height_lr;
    float pre_exposure;
    float previous_pre_exposure;
    std::uint32_t rcas_enabled;
    float rcas_sharpness;
    float padding;
};

static_assert(sizeof(OptimizedConstants) == 104);

inline void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_FEATURE_NOT_PRESENT || result == VK_ERROR_EXTENSION_NOT_PRESENT)
            throw std::runtime_error(std::string("FSR4VK_ERROR_MISSING_DEVICE_FEATURES: ")+
                operation+" returned "+fsr4vk::result_description(result)+
                "; the driver did not identify the missing bit/extension. See the provider device-create contract and context=0 negotiation log; inspect the host VkDeviceCreateInfo. Enabled feature bits cannot be queried after device creation");
        throw std::runtime_error(std::string(operation) + " failed with " + fsr4vk::result_description(result));
    }
}

inline std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    if (fsr4assets::is_embedded(path)) return fsr4assets::read(path);
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open " + path.string());
    const auto size = stream.tellg();
    if (size <= 0 || size>64*1024*1024) throw std::runtime_error("asset size outside 1..64 MiB: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) throw std::runtime_error("short read from " + path.string());
    return bytes;
}

inline std::vector<std::uint32_t> read_spirv(const fs::path& path) {
    const auto bytes = read_bytes(path);
    if (bytes.size() % 4) throw std::runtime_error("invalid SPIR-V size");
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

inline std::uint32_t memory_type(VkPhysicalDevice physical_device,
                          std::uint32_t bits,
                          VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) &&
            (properties.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    throw std::runtime_error("no compatible Vulkan memory type");
}

inline Buffer create_buffer(VkPhysicalDevice physical_device,
                     VkDevice device,
                     VkDeviceSize size,
                     VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     bool device_address = false,
                     VkMemoryPropertyFlags preferred_properties = 0) {
    Buffer result{};
    result.size = size;
    try {
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
#if defined(FSR4_RESEARCH_CAPTURE)
    buffer_info.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
#endif
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
    result.allocation_size=requirements.size;
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags_info.flags = device_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = device_address ? &flags_info : nullptr;
    allocation.allocationSize = requirements.size;
    VkPhysicalDeviceMemoryProperties available{};
    vkGetPhysicalDeviceMemoryProperties(physical_device,&available);
    const auto choice=choose_buffer_memory(available,requirements.memoryTypeBits,
                                           properties,preferred_properties);
    const auto outcome=allocate_buffer_memory(allocation,choice,
        (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)!=0,size,result.memory,result.mapped,
        [&](const VkMemoryAllocateInfo& info,VkDeviceMemory& memory) {
            return vkAllocateMemory(device,&info,nullptr,&memory);
        },[&](VkDeviceMemory memory,VkDeviceSize bytes,void*& mapped) {
            return vkMapMemory(device,memory,0,bytes,0,&mapped);
        },[&](VkDeviceMemory memory) {vkFreeMemory(device,memory,nullptr);});
    check(outcome.result,"allocate/map buffer memory");
    result.memory_type_index=outcome.type;
    result.memory_properties=available.memoryTypes[outcome.type].propertyFlags;
    result.preferred_memory_fallback=outcome.fallback;
    check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory");
    return result;
    } catch (...) {
        if (result.mapped) vkUnmapMemory(device,result.memory);
        if (result.buffer) vkDestroyBuffer(device,result.buffer,nullptr);
        if (result.memory) vkFreeMemory(device,result.memory,nullptr);
        throw;
    }
}

inline Image create_image(VkPhysicalDevice physical_device,
                   VkDevice device,
                   std::uint32_t width,
                   std::uint32_t height,
                   VkFormat format,
                   VkImageUsageFlags usage) {
    Image result{};
    result.format = format;
    result.width = width;
    result.height = height;
    try {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
#if defined(FSR4_RESEARCH_CAPTURE)
    image_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device, &image_info, nullptr, &result.image), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, result.image, &requirements);
    result.allocation_size=requirements.size;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(physical_device, requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &allocation, nullptr, &result.memory),
          "vkAllocateMemory(image)");
    check(vkBindImageMemory(device, result.image, result.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = result.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    check(vkCreateImageView(device, &view_info, nullptr, &result.view), "vkCreateImageView");
    return result;
    } catch (...) {
        if (result.view) vkDestroyImageView(device,result.view,nullptr);
        if (result.image) vkDestroyImage(device,result.image,nullptr);
        if (result.memory) vkFreeMemory(device,result.memory,nullptr);
        throw;
    }
}

inline void destroy_buffer(VkDevice device, Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

inline void destroy_image(VkDevice device, Image& image) {
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = {};
}


inline VkDeviceAddress device_address(PFN_vkGetBufferDeviceAddress get_buffer_device_address,
                                      VkDevice device, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return get_buffer_device_address(device, &info);
}

// One model/preset core. Caller submits frames in recording order
// on one queue, and completes all submissions before destroying this object.
class QualityContext {
    VkPhysicalDevice physical_device;
    VkDevice device;
    fsr4vk::DeviceDispatch dispatch;
    fsr4vk::DescriptorDispatch descriptor_dispatch;
    fsr4vk::DescriptorLayout descriptor_layout;
    VkSampler sampler{};
    VkPipelineLayout pipeline_layout{};
    std::array<VkDescriptorSetLayout,4> set_layouts{};
    std::vector<VkShaderModule> modules;
    std::vector<VkPipeline> pipelines;
    std::vector<VkPipeline> owned_pipelines;
    VkPipelineCache pipeline_cache{};
    std::function<void(const std::string&)> build_message;
    std::array<std::array<VkPipeline,2>,4> color_pipelines{};
    fs::path color_shader_dir;
    ColorSpace active_color = ColorSpace::Linear;
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    std::vector<uint8_t> weights;
    bool fsr411 = false;
    uint32_t model_capacity_width = 1920, model_capacity_height = 1080;
    bool initialized = false;
    uint32_t vela_fill_w = 0, vela_fill_h = 0;
    bool vela_always_fill = std::getenv("FSR4_VELA_FILL") != nullptr;
    // Vela 0.7.0: post split in K1 (pass-27) + K2 (pass-28) when the bundle carries pass-28.spv
    bool x_split = true;  // a bundle with pass-28.spv has K1 in pass-27: the split is not optional
    bool x_fk_k2=false;
    int x_u15=0;   // 1 / 2 = this core serves 1080p UP at 720p with the fused 1.5x K2
    // 3 / 4 = normal Fast P K2 into u15_mid (internal, core output size) + separate 1.5x pass x_u15_up (cubic / bilinear) to the game image
    Image u15_mid{}; VkPipeline x_u15_up{}; bool u15_fresh=false;
    bool x_w64=false;   // pass-01 with requiredSubgroupSize 64 (set before the compile jobs); x_fk_k2: K2 replaced by VELA_FK_K2 (k2lu-derived, reads the HTP k1 tensor)
    bool x_k1h_ok=false;   // pass-27 = K2P k1x and pass-28 = K2P k2lu: K2 can read the HTP k1 tensor itself (nf4_k1h)
    VkPipeline x_pre_fx{};   // pipelines[0] when it is the fx1 pre (no reprojected store)
    VkPipeline x_pre_fpx{};   // pipelines[0] when it is an X0-flag Fast pre
    VkPipeline x_k1{}, x_k2{}, x_pre_fused{};   // x_pre_fused = pipelines[0] when it is the f1g pre
    VkPipeline x_pre_b1080{};   // pipelines[0] when it is the plain baked 1080-tier pre (0x067a3d15, ultraperf without a fused pre)
    Buffer x_inter{};
    Image dl_mv{}; int fx_lat = 0;   // MV copy for the exact-mode TAIL; which of images[0]/[1] holds the latest history
    Image dl_cc{}, dl_dup{}; bool dl_open = false;   // Colour copy (TAIL reads colour(N-1)), kept output of the exact-mode entry frame
    bool nms_inputs = false;
    bool external_exposure = false;
    bool native_mixed_dot = false;
    VkQueryPool profile_queries{};
    double timestamp_period = 0;
    std::string profile_path;
    std::ofstream profile_output;
    uint32_t profile_query_count=0,profile_queue_family=UINT32_MAX,timestamp_bits=0;
    uint32_t last_profile_base=0;
    uint64_t profile_serial=0;
    std::string profile_preset;
    float previous_exposure = 0;
    struct Frame {
        std::array<Buffer,3> descriptors{};
        Buffer constants{};
        VkEvent done{};
        bool used = false;
        bool descriptors_initialized=false;
        bool profile_pending=false;
        uint32_t profile_base=0,width=0,height=0,render_width=0,render_height=0;
        uint64_t serial=0;
        ColorSpace color=ColorSpace::Linear;
        std::vector<VkImageView> views;
    };
    std::array<Frame,8> frames{};
    size_t next_frame = 0;
    void collect_profile(Frame& frame) noexcept {
        if(!profile_queries || !frame.profile_pending) return;
        frame.profile_pending=false;
        try {
            std::array<uint64_t,64> values{};
            const auto result=vkGetQueryPoolResults(device,profile_queries,frame.profile_base,profile_query_count,
                sizeof(values),values.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
            profile_output << std::setprecision(12) << "{\"schema\":2,\"frame\":" << frame.serial
                << ",\"model\":\"" << (fsr411 ? "4.1.1" : "4.0.2") << "\",\"backend\":\"" << shader_backend()
                << "\",\"preset\":\"" << profile_preset << "\",\"color\":\"" << color_space_name(frame.color)
                << "\",\"render_size\":[" << frame.render_width << ',' << frame.render_height
                << "],\"output_size\":[" << frame.width << ',' << frame.height
                << "],\"queue_family\":" << profile_queue_family << ",\"timestamp_valid_bits\":" << timestamp_bits
                << ",\"timestamp_period_ns\":" << timestamp_period << ",\"vk_result\":" << int(result)
                << ",\"available\":" << (result==VK_SUCCESS ? "true" : "false") << ",\"passes\":";
            if(result!=VK_SUCCESS) profile_output << "null";
            else {
                profile_output << '{';
                for(uint32_t i=0;i<profile_query_count/2;++i) {
                    if(i) profile_output << ',';
                    const char* name=fsr411 && i==29 ? "u15_up" : fsr411 && i>=30 ? "u15_pad" : fsr411 && i>=28 ? "post_k2" : nms_inputs && i==0 ? "auto_exposure" : fsr411 ? fsr4::k411PassNames[i-1] : kPasses[i-(nms_inputs ? 1 : 0)].name;
                    const auto ticks=fsr4vk::timestamp_delta(values[i*2],values[i*2+1],timestamp_bits);
                    profile_output << '"' << name << "\":" << double(ticks)*timestamp_period/1e6;
                }
                profile_output << '}';
            }
            profile_output << "}\n";
        } catch(...) { /* Optional measurement never changes upscaler success. */ }
    }
    void release() {
        fv::shutdown();
        if (profile_queries) {
            // No waits here. Caller must complete GPU work before destruction.
            std::array<Frame*,8> ordered{};
            for(size_t i=0;i<frames.size();++i) ordered[i]=&frames[i];
            std::sort(ordered.begin(),ordered.end(),[](auto* a,auto* b){return a->serial<b->serial;});
            for(auto* frame:ordered) collect_profile(*frame);
            profile_output.close();
            vkDestroyQueryPool(device,profile_queries,nullptr);
        }
        for (auto& f : frames) {
            for (auto view : f.views) vkDestroyImageView(device,view,nullptr);
            if (f.done) vkDestroyEvent(device,f.done,nullptr);
            for (auto& b : f.descriptors) destroy_buffer(device,b);
            destroy_buffer(device,f.constants);
        }
        for (auto p : owned_pipelines) vkDestroyPipeline(device,p,nullptr);
        if(pipeline_cache) vkDestroyPipelineCache(device,pipeline_cache,nullptr);
        for (auto m : modules) vkDestroyShaderModule(device,m,nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
        for (auto l : set_layouts) if (l) vkDestroyDescriptorSetLayout(device,l,nullptr);
        if (sampler) vkDestroySampler(device,sampler,nullptr);
        for (auto& i : images) destroy_image(device,i);
        for (auto& b : buffers) destroy_buffer(device,b);
        if(x_inter.buffer) destroy_buffer(device,x_inter);
        if(dl_mv.image) destroy_image(device,dl_mv); if(dl_cc.image) destroy_image(device,dl_cc); if(dl_dup.image) destroy_image(device,dl_dup); if(u15_mid.image) destroy_image(device,u15_mid);
    }
public:
    // Only the pre/post shaders depend on color space. Keep old pipelines alive
    // for in-flight command buffers; do not allocate another model/scratch set.
    bool set_color_space(ColorSpace color) {
        const auto name=color_space_name(color); // Validate before indexing.
        if (color==active_color) return false;
        if (!nms_inputs) throw std::invalid_argument("color variants require general model layout");
        auto& pair=color_pipelines[static_cast<unsigned>(color)];
        for (unsigned i=0;i<2;++i) {
            if(pair[i]) continue;
            const auto file=std::string(i==0 ? "pass-01" : fsr411 ? "pass-27" : "pass-14")+
                (fsr411 || native_mixed_dot ? ".spv" : ".portable.spv");
            auto code=read_spirv(color_shader_dir/name/file);
            if(fsr411 && i==0 && !std::getenv("FSR4_J_NOGUARD") && !vela_baked_pre(code))fsr4::guard_fsr411_prepass_quads(code);
            pair[i]=create_pipeline(code,name);
        }
        pipelines[0]=pair[0]; pipelines[fsr411 ? 26 : 13]=pair[1];
        active_color=color;
        return true; // Caller must reset temporal history on the transition.
    }
private:
    // Vela 0.8.9: compile_pipeline touches no member container, so the constructor can run it on worker threads
    // (vkCreateShaderModule / vkCreateComputePipelines need no external sync on the device; the VkPipelineCache is
    // created without EXTERNALLY_SYNCHRONIZED). Same SPIR-V, layout, flags and cache as before: same binaries.
    VkResult compile_pipeline(const std::vector<std::uint32_t>& code,const std::string& name,VkShaderModule& module,VkPipeline& pipeline) const {
        const auto started=std::chrono::steady_clock::now();
        if(build_message) build_message("pipeline_enter name="+name+" words="+std::to_string(code.size()));
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize=code.size()*sizeof(std::uint32_t);
        module_info.pCode=code.data();
        module=VK_NULL_HANDLE; pipeline=VK_NULL_HANDLE;
        auto result=vkCreateShaderModule(device,&module_info,nullptr,&module);
        if(result!=VK_SUCCESS) return result;
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; stage.module=module; stage.pName="main";
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo w64{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        w64.requiredSubgroupSize=64;   // pass-01 at wave64 (ir3 would pick wave128 at 12 regs); same quad-op results, -22 % prepass Fast
        if(x_w64 && name=="pass-01.spv") stage.pNext=&w64;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.flags=VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        info.stage=stage; info.layout=pipeline_layout;
        result=vkCreateComputePipelines(device,pipeline_cache,1,&info,nullptr,&pipeline);
        if(stage.pNext && build_message) build_message("pipeline_w64 name="+name+" result="+std::to_string(int(result)));
        if(result==VK_SUCCESS && build_message) build_message("pipeline_complete name="+name+" cpu_ms="+
            std::to_string(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()));
        return result;
    }
    struct PipelineJob { std::vector<std::uint32_t> code; std::string name; VkShaderModule module{}; VkPipeline pipeline{}; VkResult result=VK_NOT_READY; };
    // Cold first start (empty driver cache, e.g. a game's first FSR 4 frame under Steam's per-game Mesa cache) compiled the
    // 29 pipelines one by one on the game's thread: 9.85 s on A740/Turnip 26.2.4, 9.0 s of it in pass-13/15/17/19.
    // Longest SPIR-V first over FSR4_VK_PIPELINE_THREADS workers (default min(4, cores); 1 = the old sequential order).
    void compile_jobs(std::vector<PipelineJob>& jobs) const {
        unsigned threads=std::min(4u,std::max(1u,std::thread::hardware_concurrency()));
        if(const char* v=std::getenv("FSR4_VK_PIPELINE_THREADS")) { const int n=std::atoi(v); if(n>=1 && n<=16) threads=unsigned(n); }
        threads=std::min<unsigned>(threads,unsigned(jobs.size()));
        std::vector<std::size_t> order(jobs.size()); std::iota(order.begin(),order.end(),std::size_t{0});
        if(threads>1) std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return jobs[a].code.size()>jobs[b].code.size();});
        std::atomic<std::size_t> next{0};
        const auto work=[&]{ for(std::size_t k;(k=next.fetch_add(1))<order.size();) { auto& j=jobs[order[k]];
            try { j.result=compile_pipeline(j.code,j.name,j.module,j.pipeline); } catch(...) { j.result=VK_ERROR_OUT_OF_HOST_MEMORY; } } };
        if(threads<=1) { work(); return; }
        std::vector<std::thread> pool;
        try { for(unsigned t=1;t<threads;++t) pool.emplace_back(work); } catch(...) { }  // thread start failure: fewer workers
        work();
        for(auto& t:pool) t.join();
    }
    // Adopt in index order on this thread (all created handles, also after a failure, so the destructor frees them).
    void adopt_job(PipelineJob& j) {
        if(j.module) adopt_resource(modules,j.module,[&](auto handle){vkDestroyShaderModule(device,handle,nullptr);});
        if(j.pipeline) adopt_resource(owned_pipelines,j.pipeline,[&](auto handle){vkDestroyPipeline(device,handle,nullptr);});
    }
    VkPipeline create_pipeline(const std::vector<std::uint32_t>& original,const char* name) {
        const auto started=std::chrono::steady_clock::now();
        if(build_message) build_message(std::string("pipeline_enter name=")+name+" words="+std::to_string(original.size()));
        const auto& code=original;
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize=code.size()*sizeof(std::uint32_t);
        module_info.pCode=code.data();
        VkShaderModule module{};
        check(vkCreateShaderModule(device,&module_info,nullptr,&module),name);
        adopt_resource(modules,module,[&](auto handle){vkDestroyShaderModule(device,handle,nullptr);});
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; stage.module=module; stage.pName="main";
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.flags=VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        info.stage=stage; info.layout=pipeline_layout;
        VkPipeline pipeline{};
        const auto result=vkCreateComputePipelines(device,pipeline_cache,1,&info,nullptr,&pipeline);
        if(pipeline) adopt_resource(owned_pipelines,pipeline,[&](auto handle){vkDestroyPipeline(device,handle,nullptr);});
        check(result,name);
        if(build_message) build_message(std::string("pipeline_complete name=")+name+" cpu_ms="+
            std::to_string(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()));
        return pipeline;
    }
    VkDeviceAddress buffer_address(const Buffer& buffer) const {
        if(!buffer.cached_address) {
            buffer.cached_address=device_address(dispatch.get_buffer_device_address,device,buffer.buffer);
            if(!buffer.cached_address) throw std::runtime_error("FSR4VK_ERROR_MISSING_DEVICE_FEATURES: owned buffer returned a null device address; bufferDeviceAddress must be enabled");
        }
        return buffer.cached_address;
    }
public:
    uint32_t last_profile_query_base() const noexcept {return last_profile_base;}
    const Buffer& initializer_allocation() const { return buffers.at(4); }
    uint64_t owned_allocation_bytes() const noexcept {
        uint64_t bytes=0;
        for(const auto& b:buffers) bytes+=b.allocation_size;
        for(const auto& image:images) bytes+=image.allocation_size;
        for(const auto& frame:frames) {
            bytes+=frame.constants.allocation_size;
            for(const auto& b:frame.descriptors) bytes+=b.allocation_size;
        }
        return bytes; // VkDeviceMemory allocations; excludes driver-owned caches.
    }
    const char* shader_backend() const {
        return native_mixed_dot ? "native-mixed-dot" : "portable-int8";
    }
    QualityContext(const QualityContext&) = delete;
    QualityContext& operator=(const QualityContext&) = delete;
    QualityContext(VkPhysicalDevice physical, VkDevice logical, const fs::path& shader_dir,
                   const fs::path& initializer_path, const fs::path& weights_path,
                   bool nms,
                   PFN_vkGetDeviceProcAddr get_device_proc_addr,
                   uint32_t max_input_width,
                   uint32_t max_input_height,
                   uint32_t max_output_width,
                   uint32_t max_output_height,
                   uint32_t api_version,
                   bool external_exposure_input = false, bool use_fsr411 = false,
                   std::optional<bool> robust_buffer_access=std::nullopt,
                   std::optional<bool> enabled_native_mixed_dot=std::nullopt,
                   std::function<void(const std::string&)> progress={},
                   uint32_t queue_family_index=UINT32_MAX)
        : physical_device(physical), device(logical),
          dispatch(logical, get_device_proc_addr, api_version,false),
          descriptor_dispatch(logical,get_device_proc_addr),build_message(std::move(progress)),nms_inputs(nms),
          external_exposure(external_exposure_input) {
        fsr411=use_fsr411;
        if(!fsr411) throw std::invalid_argument("FSR 4.0.2 is not in this build (Vela release provider): 4.1.1 only");
        if(fsr411 && !nms) throw std::invalid_argument("4.1.1 requires the general layout");
        if(max_output_width>1920 || max_output_height>1080) { model_capacity_width=3840;model_capacity_height=2160; }
      try {
        color_shader_dir=shader_dir.parent_path().parent_path().parent_path()/"colors"/
                         shader_dir.parent_path().filename()/shader_dir.filename();
        fsr4vk::validate_descriptor_commands(device,get_device_proc_addr);
        if(external_exposure && !nms_inputs)
            throw std::invalid_argument("external exposure adapter requires general model layout");
        native_mixed_dot = !fsr411 && enabled_native_mixed_dot.value_or(true) && fsr4vk::supportsNativeMixedDot(
            physical, vkGetPhysicalDeviceFeatures2, vkEnumerateDeviceExtensionProperties);
        VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        const auto cache_result=vkCreatePipelineCache(device,&cache_info,nullptr,&pipeline_cache);
        if(cache_result==VK_ERROR_DEVICE_LOST) check(cache_result,"vkCreatePipelineCache");
        if(cache_result!=VK_SUCCESS) {
            if(pipeline_cache) vkDestroyPipelineCache(device,pipeline_cache,nullptr);
            pipeline_cache=VK_NULL_HANDLE;
            if(build_message) build_message("pipeline cache unavailable: "+fsr4vk::result_description(cache_result)+"; using uncached creation");
        }
        if (const char* path=std::getenv("FSR4_VK_PROFILE_PATH")) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical_device,&properties);
            timestamp_period=properties.limits.timestampPeriod;
            uint32_t family_count=0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device,&family_count,nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device,&family_count,families.data());
            if(queue_family_index<family_count) timestamp_bits=families[queue_family_index].timestampValidBits;
            if(!timestamp_bits || timestamp_bits>64 || !std::isfinite(timestamp_period) || timestamp_period<=0) {
                if(build_message) build_message("profiling disabled: recording queue family unknown or timestampValidBits unavailable");
            } else {
                static std::atomic<uint64_t> identities{0};
                profile_path=std::string(path)+'.'+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+
                    '.'+std::to_string(identities.fetch_add(1))+".jsonl";
                profile_output.open(profile_path,std::ios::app); // Never truncate another core/session.
                profile_preset=shader_dir.filename().string();
                if(profile_preset.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_")!=std::string::npos) profile_preset="unknown";
                profile_queue_family=queue_family_index;
                profile_query_count=fsr411 ? (fv::g_u15.load()>=3 ? 64 : 58) : nms_inputs ? 30 : 28;   // +6 = u15_up + 2 pad pairs: the bench tracks only 58/64-query pools
                VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                query.queryType=VK_QUERY_TYPE_TIMESTAMP; query.queryCount=profile_query_count*uint32_t(frames.size());
                const auto result=profile_output ? vkCreateQueryPool(device,&query,nullptr,&profile_queries) : VK_ERROR_INITIALIZATION_FAILED;
                if(result!=VK_SUCCESS) {
                    if(profile_queries) vkDestroyQueryPool(device,profile_queries,nullptr);
                    profile_queries=VK_NULL_HANDLE;
                    if(build_message) build_message("profiling disabled: query pool/file initialization failed: "+fsr4vk::result_description(result));
                } else if(build_message) build_message("profile samples="+profile_path);
            }
        }
        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxAnisotropy = 16.0f;
        sampler_info.compareOp = VK_COMPARE_OP_NEVER;
        sampler_info.maxLod = std::numeric_limits<float>::max();
        sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        check(vkCreateSampler(device, &sampler_info, nullptr, &sampler),
              "vkCreateSampler");

        const VkDescriptorBindingFlags variable_descriptor_count =
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        const std::array<VkDescriptorType, 6> mutable_types{{
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        }};
        const VkMutableDescriptorTypeListEXT mutable_list{
            static_cast<std::uint32_t>(mutable_types.size()), mutable_types.data()};

        const auto create_set_layout = [&](std::uint32_t index,
                                           const VkDescriptorSetLayoutBinding* bindings,
                                           std::uint32_t binding_count,
                                           const void* next,
                                           VkDescriptorSetLayoutCreateFlags flags) {
            VkDescriptorSetLayoutCreateInfo layout_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout_info.pNext = next;
            layout_info.flags = flags;
            layout_info.bindingCount = binding_count;
            layout_info.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
                                               &set_layouts[index]),
                  "vkCreateDescriptorSetLayout");
        };

        const VkDescriptorSetLayoutBinding sampler_heap_binding{
            0, VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorCount,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutBindingFlagsCreateInfo sampler_heap_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        sampler_heap_flags.bindingCount = 1;
        sampler_heap_flags.pBindingFlags = &variable_descriptor_count;
        create_set_layout(0, &sampler_heap_binding, 1, &sampler_heap_flags,
                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);

        const std::array<VkDescriptorSetLayoutBinding, 2> image_heap_bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, kDescriptorCount,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        const std::array<VkDescriptorBindingFlags, 2> image_heap_flag_values{{
            0, variable_descriptor_count}};
        const std::array<VkMutableDescriptorTypeListEXT, 2> image_heap_lists{{
            {0, nullptr}, mutable_list}};
        VkMutableDescriptorTypeCreateInfoEXT image_heap_mutable_info{
            VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
        image_heap_mutable_info.mutableDescriptorTypeListCount = image_heap_lists.size();
        image_heap_mutable_info.pMutableDescriptorTypeLists = image_heap_lists.data();
        VkDescriptorSetLayoutBindingFlagsCreateInfo image_heap_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        image_heap_flags.pNext = &image_heap_mutable_info;
        image_heap_flags.bindingCount = image_heap_flag_values.size();
        image_heap_flags.pBindingFlags = image_heap_flag_values.data();
        create_set_layout(1, image_heap_bindings.data(), image_heap_bindings.size(),
                          &image_heap_flags,
                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);

        const VkDescriptorSetLayoutBinding buffer_heap_binding{
            0, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, kDescriptorCount,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkMutableDescriptorTypeCreateInfoEXT buffer_heap_mutable_info{
            VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
        buffer_heap_mutable_info.mutableDescriptorTypeListCount = 1;
        buffer_heap_mutable_info.pMutableDescriptorTypeLists = &mutable_list;
        VkDescriptorSetLayoutBindingFlagsCreateInfo buffer_heap_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        buffer_heap_flags.pNext = &buffer_heap_mutable_info;
        buffer_heap_flags.bindingCount = 1;
        buffer_heap_flags.pBindingFlags = &variable_descriptor_count;
        create_set_layout(2, &buffer_heap_binding, 1, &buffer_heap_flags,
                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);

        const VkDescriptorSetLayoutBinding embedded_sampler_binding{
            0, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, &sampler};
        create_set_layout(
            3, &embedded_sampler_binding, 1, nullptr,
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT);

        VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
        VkPipelineLayoutCreateInfo pipeline_layout_info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = set_layouts.size();
        pipeline_layout_info.pSetLayouts = set_layouts.data();
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                     &pipeline_layout),
              "vkCreatePipelineLayout");

        const auto model_pass_count=fsr411 ? 27u : unsigned(kPasses.size());
        const auto pipeline_count=model_pass_count+(nms_inputs ? 1u : 0u);
        modules.reserve(pipeline_count);
        pipelines.reserve(pipeline_count);
        const bool general_bundle=fsr4assets::is_embedded(shader_dir) || fs::exists(shader_dir/"pass-00.spv");
        std::vector<PipelineJob> jobs; jobs.reserve(pipeline_count+1);
        for (unsigned index=0;index<pipeline_count;++index) {
            const bool spd=index==model_pass_count;
            const auto canonical=spd ? 0u : index+1;
            const char* hash=spd ? "996136f00380aba8" : nms_inputs && index==0 ? "c853540cef1e8d64" :
                             fsr411 ? "" : kPasses[index].hash;
            auto name=general_bundle ? "pass-"+std::string(canonical<10 ? "0" : "")+std::to_string(canonical)+".spv"
                                     : std::string(hash)+".spv";
            if (!fsr411 && !native_mixed_dot) name.insert(name.size()-4,".portable");
            auto code=external_exposure && spd
                ? std::vector<std::uint32_t>(std::begin(kExternalExposureSpv),std::end(kExternalExposureSpv))
                : read_spirv(shader_dir/name);
            if(fsr411 && index==0) if(const char* fp=std::getenv("VELA_FP_PRE")) if(*fp && std::string(fp)!="0") {   // "FSR 4 Fast" prepass (A/B switch, default off)
                const char* fd=std::getenv("VELA_FP_DIR"); const auto f=(fd ? fs::path(fd) : shader_dir/"fp")/(std::string(fp)+".spv"); if(fs::exists(f)) { auto c=read_spirv(f); if(vela_fp_pre_hash(vela_fnv(c))) code=std::move(c); } }
            if(fsr411 && index==0) if(const char* lr=std::getenv("VELA_LR")) if(*lr && std::string(lr)!="0") {   // Lossy reprojection variant of the prepass (A/B switch, default off)
                const char* ld=std::getenv("VELA_LR_DIR"); const auto f=(ld ? fs::path(ld) : shader_dir/"lr")/(std::string(lr)+".spv"); if(fs::exists(f)) { auto c=read_spirv(f); if(vela_lr_pre_hash(vela_fnv(c))) code=std::move(c); } }
            if(fsr411 && index==0) { fv::g_fast=fv::fast_setting(); std::fprintf(stderr,"Fast: mode %s\n",fv::g_fast==1 ? "fsr4fast" : fv::g_fast==2 ? "fsr4fast-rha (legacy)" : "fsr4 (exact)");
                if(!std::getenv("VELA_FP_PRE") && !std::getenv("VELA_LR") && !std::getenv("VELA_R3")) vela_ff_pre(code,read_bytes(initializer_path),fv::g_fast.load()); }   // Embedded (experimental env switches win)
            if(fsr411 && index==0) vela_r3_select(code,shader_dir,initializer_path);   // baked-weight prepass (guarded, default off)
            if(!fsr411 && index==11) fsr4::guard_model11_rows(code);
            if(fsr411 && index==0 && !std::getenv("FSR4_J_NOGUARD") && !vela_baked_pre(code))fsr4::guard_fsr411_prepass_quads(code);
            jobs.push_back({std::move(code),spd ? "auto_exposure" : fsr411 ? name : std::string(kPasses[index].name)});
        }
        if(fsr411) { const char* e=std::getenv("VELA_W64"); if(!(e && e[0]=='0')) {   // wave64 on pass-01 when the device allows it
            VkPhysicalDeviceSubgroupSizeControlProperties sp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
            VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; pp.pNext=&sp; vkGetPhysicalDeviceProperties2(physical_device,&pp);
            x_w64=pp.properties.apiVersion>=VK_API_VERSION_1_3 && (sp.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT) && sp.minSubgroupSize<=64 && sp.maxSubgroupSize>=64;
            std::fprintf(stderr,"Fast: wave64 pass-01 %s (subgroup %u..%u)\n",x_w64 ? "on" : "off",sp.minSubgroupSize,sp.maxSubgroupSize); } }
        x_u15=fsr411 && fv::g_fast ? fv::g_u15.load() : 0;
        std::size_t k2_job=SIZE_MAX;
        if(fsr411 && x_split) {
            try { auto code=read_spirv(shader_dir/"pass-28.spv");
                // VELA_FK_K2=<name> loads (VELA_FK_DIR or <shader_dir>/fk)/<name>_<p|u>.spv (p = performance tier, u = ultraperf) as K2; it keeps k2lu's K1 read, so the NPU path stays on
                if(const char* fk=std::getenv("VELA_FK_K2")) if(*fk && std::string(fk)!="0") { const char* fd=std::getenv("VELA_FK_DIR");
                    const auto f=(fd ? fs::path(fd) : shader_dir/"fk")/(std::string(fk)+(vela_fnv(code)==0x72169abf0b2d80c1ull ? "_u.spv" : "_p.spv"));
                    if(vela_k2u(code) && fs::exists(f)) { code=read_spirv(f); x_fk_k2=true; } }
                if(!x_fk_k2 && (x_u15==1 || x_u15==2) && vela_fnv(code)==0xe28d58d95e8cdca6ull) {   // P bundle k2lu -> fused P720 + 1.5x K2
                    if(x_u15==2) code.assign(kU15sP,kU15sP+std::size(kU15sP)); else code.assign(kU15P,kU15P+std::size(kU15P));
                    x_fk_k2=true; std::fprintf(stderr,"Fast 1.5x: K2 %s (P720 + fused 1.5x to the 1080p output)\n",x_u15==2 ? "rha15s_p" : "rha15_p"); }
                if((x_u15==1 || x_u15==2) && !x_fk_k2) std::fprintf(stderr,"Fast 1.5x: WARNING K2 not replaced (bundle not P k2lu): output only 720p\n");
                if(!x_fk_k2 && fv::g_fast==2 && vela_ff_k2(code)) x_fk_k2=true;   // Legacy Fast K2 rha (embedded); Fast 1 keeps k2lu (full-res recurrent)
                k2_job=jobs.size(); jobs.push_back({std::move(code),"post_k2"}); }
            catch(const std::exception&) { }
        }
        compile_jobs(jobs);
        for(auto& j:jobs) adopt_job(j);
        for (unsigned index=0;index<pipeline_count;++index) {
            check(jobs[index].result,jobs[index].name.c_str());
            pipelines.push_back(jobs[index].pipeline);
            if(fsr411 && index==0 && vela_fused_pre(jobs[index].code)) x_pre_fused=pipelines.back();
            if(fsr411 && index==0 && vela_fp_x0f(vela_fnv(jobs[index].code))) x_pre_fpx=pipelines.back();   // X0-flag prepass
            if(fsr411 && index==0 && vela_fx_pre(vela_fnv(jobs[index].code))) x_pre_fx=pipelines.back();   // no-reprojected-store prepass
            if(fsr411 && index==0 && vela_fnv(jobs[index].code)==0x067a3d152a1be205ull) x_pre_b1080=pipelines.back();   // baked 1080-tier prepass
        }
        color_pipelines[0]={pipelines[0],pipelines[fsr411 ? 26 : 13]};
        if(k2_job!=SIZE_MAX) {
            try { auto& j=jobs[k2_job]; check(j.result,"post_k2"); x_k2=j.pipeline; x_k1=pipelines[26]; x_k1h_ok=(vela_k2u(j.code) || x_fk_k2) && vela_k1x(read_spirv(shader_dir/"pass-27.spv")); }
            catch(const std::exception&) { x_k2=VK_NULL_HANDLE; }
        }
        if(!x_k2) x_split=false;
        if(x_u15>=3) { const std::uint32_t* b=x_u15==3 ? kU15C : kU15B; const std::size_t n=x_u15==3 ? std::size(kU15C) : std::size(kU15B);
            x_u15_up=create_pipeline(std::vector<std::uint32_t>(b,b+n),"u15_up");
            std::fprintf(stderr,"Fast 1.5x: separate 1.5x pass %s (K2 = Fast P at the core size -> internal image -> game output)\n",x_u15==3 ? "cubic a=-0.75" : "bilinear"); }
        if(fsr411 && fv::env_i("FV_DEBUG",0)!=0) std::fprintf(stderr,"FV k1h load: dir %s k2_job %d k2 %d k2u %d k1x %d k1h_ok %d split %d\n",shader_dir.string().c_str(),int(k2_job!=SIZE_MAX),int(x_k2!=VK_NULL_HANDLE),
            int(k2_job!=SIZE_MAX && vela_k2u(jobs[k2_job].code)),int(fs::exists(shader_dir/"pass-27.spv") && vela_k1x(read_spirv(shader_dir/"pass-27.spv"))),int(x_k1h_ok),int(x_split));


        auto get_layout_size = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(
            get_device_proc_addr(device, "vkGetDescriptorSetLayoutSizeEXT"));
        auto get_binding_offset =
            reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
                get_device_proc_addr(device,
                                    "vkGetDescriptorSetLayoutBindingOffsetEXT"));
        auto get_descriptor = reinterpret_cast<PFN_vkGetDescriptorEXT>(
            get_device_proc_addr(device, "vkGetDescriptorEXT"));
        auto bind_descriptor_buffers =
            reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(
                get_device_proc_addr(device, "vkCmdBindDescriptorBuffersEXT"));
        auto set_descriptor_offsets =
            reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
                get_device_proc_addr(device,
                                    "vkCmdSetDescriptorBufferOffsetsEXT"));
        auto bind_embedded_samplers =
            reinterpret_cast<PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT>(
                get_device_proc_addr(
                    device, "vkCmdBindDescriptorBufferEmbeddedSamplersEXT"));
        if (!get_layout_size || !get_binding_offset || !get_descriptor ||
            !bind_descriptor_buffers || !set_descriptor_offsets ||
            !bind_embedded_samplers) {
            throw std::runtime_error("descriptor buffer entry point is unavailable");
        }

        VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_properties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &descriptor_properties;
        vkGetPhysicalDeviceProperties2(physical_device, &properties2);
        std::array<VkDeviceSize, 3> descriptor_layout_sizes{};
        std::array<VkDeviceSize, 3> descriptor_binding_offsets{};
        constexpr std::array<std::uint32_t, 3> descriptor_bindings{{0, 1, 0}};
        for (std::size_t index = 0; index < descriptor_layout_sizes.size(); ++index) {
            get_layout_size(device, set_layouts[index], &descriptor_layout_sizes[index]);
            get_binding_offset(device, set_layouts[index], descriptor_bindings[index],
                               &descriptor_binding_offsets[index]);
        }
        // Properties determine descriptor strides; queried set sizes only bound
        // the allocation. Drivers are allowed to pad the overall layouts.
        descriptor_layout=fsr4vk::DescriptorLayout(descriptor_properties,descriptor_layout_sizes,
                                                   descriptor_binding_offsets,robust_buffer_access);



        buffers.reserve(9);
        // Preserve stable scratch/initializer indices without allocating three
        // descriptor buffers that recording never uses (slots own their storage).
        buffers.resize(3);

        const auto add_buffer=[&](Buffer buffer){adopt_resource(buffers,buffer,[&](auto& value){destroy_buffer(device,value);});};
        add_buffer(create_buffer(
            physical_device, device, fsr4::scratch_capacity(max_output_width,max_output_height),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true));
        const auto initializer_bytes = read_bytes(initializer_path);
        add_buffer(create_buffer(
            physical_device, device, initializer_bytes.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        Buffer& initializer = buffers.back();
        std::memcpy(initializer.mapped, initializer_bytes.data(), initializer_bytes.size());
        const auto weights_bytes = fsr411 ? std::vector<uint8_t>(1024) : read_bytes(weights_path);
        if (weights_bytes.size() != 1024) throw std::runtime_error("weights must be 1024 bytes");
        add_buffer(create_buffer(
            physical_device, device, kPhysicalConstantBackingSize,
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true));
        Buffer& constants = buffers.back();
        if(x_split) x_inter=create_buffer(physical_device,device,VkDeviceSize(16)*((max_output_width+1)/2+2)*((max_output_height+1)/2+2),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true);

        // Immutable template descriptors are prepared per frame below.
        frames[0].constants = constants;
        buffers.back() = {};
        weights = weights_bytes;
        for (auto& f : frames) {
            for (size_t i=0;i<3;++i)
                f.descriptors[i] = create_buffer(physical_device,device,descriptor_layout_sizes[i],
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    (i==0 ? VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT :
                            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT),
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
            if (!f.constants.buffer)
                f.constants = create_buffer(physical_device,device,2048,
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
            VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            check(vkCreateEvent(device,&ei,nullptr,&f.done),"vkCreateEvent");
        }
        images.reserve(6);
        const auto add_image=[&](Image image){adopt_resource(images,image,[&](auto& value){destroy_image(device,value);});};
        // VELA_FP bits 2/4: history / reprojected as A2B10G10R10_UNORM (half the bytes; log-domain values clamp at 1.0)
        const int fp_bits=fv::fp_bits();   // Fast = 7
        const auto fp_fmt=[&](int bit){ if(!(fp_bits&bit)) return VK_FORMAT_R16G16B16A16_SFLOAT; VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(physical_device,VK_FORMAT_A2B10G10R10_UNORM_PACK32,&p);
            const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if((p.optimalTilingFeatures&need)==need) { std::fprintf(stderr,"Fast image %d A2B10G10R10_UNORM\n",bit); return VK_FORMAT_A2B10G10R10_UNORM_PACK32; }
            std::fprintf(stderr,"Fast image %d: A2B10G10R10 not storage+sampled+linear, kept R16G16B16A16\n",bit); return VK_FORMAT_R16G16B16A16_SFLOAT; };
        const auto fx_half=[](VkFormat f)->uint32_t{ return (vela_fx_bits()&2) && f==VK_FORMAT_R8G8B8A8_UNORM ? 2u : 1u; };
        // VELA_FK_HIST / VELA_FK_REPR = a2b10 | b10g11 (experimental): narrow history / reprojected (shader unchanged); VELA_FP bits win when set
        const auto fk_fmt=[](const char* e,VkFormat d){ const char* v=std::getenv(e); if(!v || d!=VK_FORMAT_R16G16B16A16_SFLOAT) return d; const std::string s(v);
            return s=="a2b10" ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : s=="b10g11" ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : d; };
        for (auto format : {fsr411 ? fk_fmt("VELA_FK_HIST",fp_fmt(2)) : VK_FORMAT_R16G16B16A16_SFLOAT,
                            fsr411 ? ((vela_fx_bits()&1) ? fp_fmt(2) : fk_fmt("VELA_FK_REPR",fp_fmt(4))) : VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM})   // VELA_FX bit 1: [1] = 2nd history
            add_image(create_image(physical_device,device,   // VELA_FX bit 2: recurrent (the only RGBA8 one) at ceil(output/2)
                fsr411 ? (max_output_width+fx_half(format)-1)/fx_half(format) : fsr4::round_up(max_output_width,8),
                fsr411 ? (max_output_height+fx_half(format)-1)/fx_half(format) : fsr4::round_up(max_output_height,8),format,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        if (nms_inputs) {
            const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            add_image(create_image(physical_device,device,2,1,VK_FORMAT_R32_SFLOAT,usage));
            add_image(create_image(physical_device,device,1,1,VK_FORMAT_R32_UINT,usage));
            add_image(create_image(physical_device,device,fsr4::divide_up(max_input_width,16),fsr4::divide_up(max_input_height,16),VK_FORMAT_R32_SFLOAT,usage));
        }
      } catch (...) { release(); throw; }
    }
    ~QualityContext() { release(); }
    // External views must outlive GPU completion. Sampled inputs are READ_ONLY,
    // output is GENERAL. Input dimensions are selected by each dispatch.
    void record(VkCommandBuffer command_buffer, Image color, Image depth,
                Image motion, Image exposure_image, Image output,
                OptimizedConstants main_constants) {
        auto& frame = frames[next_frame];
        Image u15_game{};   // Modes 3 / 4: the game's image; everything of the core that writes / copies "output" uses u15_mid
        if(x_u15>=3 && x_u15_up) { u15_game=output;
            if(!u15_mid.image || u15_mid.width!=main_constants.width || u15_mid.height!=main_constants.height) {
                if(u15_mid.image) { vkDeviceWaitIdle(device); destroy_image(device,u15_mid); }
                u15_mid=create_image(physical_device,device,main_constants.width,main_constants.height,VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                u15_fresh=true; }
            output=u15_mid; }
        if (frame.used) {
            auto status = vkGetEventStatus(device,frame.done);
            if (status == VK_EVENT_RESET)
                throw std::runtime_error("Quality context has eight frames still in flight");
            if (status != VK_EVENT_SET)
                throw std::runtime_error("vkGetEventStatus failed with "+fsr4vk::result_description(status));
            collect_profile(frame); // Retired slot only; never WAIT_BIT.
            check(vkResetEvent(device,frame.done),"vkResetEvent");
            frame.used = false;
        }
        for (auto view : frame.views) vkDestroyImageView(device,view,nullptr);
        frame.views.clear();
        frame.views.reserve(6);
        const uint32_t profile_base=uint32_t(next_frame)*profile_query_count;
        Image supplied_exposure = exposure_image;
        if (nms_inputs) exposure_image = images[3];
        for (auto* image : {&color,&depth,&motion,&exposure_image,&output,
                           external_exposure ? &supplied_exposure : &exposure_image}) {
            if (!image->view) {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image=image->image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=image->format;
                const bool depth_format=image->format==VK_FORMAT_D32_SFLOAT_S8_UINT || image->format==VK_FORMAT_D32_SFLOAT;
                vi.subresourceRange={depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                check(vkCreateImageView(device,&vi,nullptr,&image->view),"external image view");
                adopt_resource(frame.views,image->view,[&](auto view){vkDestroyImageView(device,view,nullptr);});
            }
        }
        if(u15_game.image) {   // View of the game's image (this frame only, like the other external views)
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image=u15_game.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=u15_game.format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            check(vkCreateImageView(device,&vi,nullptr,&u15_game.view),"Fast 1.5x game output view");
            adopt_resource(frame.views,u15_game.view,[&](auto view){vkDestroyImageView(device,view,nullptr);});
        }
        const bool fx=fsr411 && (vela_fx_bits()&1) && x_fk_k2 && x_pre_fx && pipelines[0]==x_pre_fx;   // K2 reprojects, history ping-pong
        { static int said=0; if(!said++) std::fprintf(stderr,"VELA_FX bits %d active %d (pre %d k2 %d)\n",vela_fx_bits(),int(fx),int(x_pre_fx && pipelines[0]==x_pre_fx),int(x_fk_k2)); }
        auto& history = images[fx ? fx_lat : 0];          // history L
        auto& reprojected = images[fx ? (fx_lat^1) : 1];  // reprojected O
        auto& recurrent = images[2];
        auto& scratch = buffers[3];
        auto& initializer = buffers[4];
        // Exact NPU mode case of this frame (fv_npu.hpp plan())
        fv::Bridge* const fv_bridge=fsr411 ? fv::get(physical_device,device) : nullptr;
        if(fv_bridge) fv_bridge->pre_fused=x_pre_fused && pipelines[0]==x_pre_fused;
        if(fv_bridge) fv_bridge->fp_pre=x_pre_fpx && pipelines[0]==x_pre_fpx;   // X0-flag prepass
        if(fv_bridge) fv_bridge->k1h_want=x_k1h_ok && x_split && x_k2 && pipelines[26]==x_k1;   // Colour variants / other presets: network graph
        int dl_case=0;
        const auto dl_match=[](const Image& a,const Image& b){ return a.image && a.format==b.format && a.width==b.width && a.height==b.height; };
        if(fv_bridge) {
            fv_bridge->k2g[0]=(x_u15==1 || x_u15==2) ? (main_constants.width/2+13)/14 : 0; fv_bridge->k2g[1]=(x_u15==1 || x_u15==2) ? (main_constants.height/2+13)/14 : 0;   // 1.5x mode
            fsr4::fv_prepare(fv_bridge,main_constants.width,main_constants.height,model_capacity_width,model_capacity_height);
            dl_case=fv_bridge->plan(main_constants.width,main_constants.height,main_constants.width_lr,main_constants.height_lr,
                initialized && nms_inputs && x_split && x_k2 && pipelines[26]==x_k1 && !vela_always_fill && color.width && output.width &&
                vela_fill_w==main_constants.width_lr && vela_fill_h==main_constants.height_lr, dl_match(dl_cc,color), dl_match(dl_dup,output));
        }
        if(dl_open && dl_case!=1 && dl_case!=2) dl_case=-1;   // a HEAD is open but no TAIL follows (bridge gone): layout transitions only
        const auto dl_make=[&](Image& im,const Image& like){ if(dl_match(im,like)) return; vkDeviceWaitIdle(device); if(im.image) destroy_image(device,im);
            im=create_image(physical_device,device,like.width,like.height,like.format,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT); };
        if(dl_case==3 || dl_case==1) dl_make(dl_cc,color);
        if(fx && (dl_case==3 || dl_case==1)) dl_make(dl_mv,motion);   // MV copy for the exact-mode TAIL
        if(dl_case==4) dl_make(dl_dup,output);
        const auto& descriptor_properties=descriptor_layout.properties;
        const auto get_descriptor=descriptor_dispatch.get_descriptor;
        const auto bind_descriptor_buffers=descriptor_dispatch.bind_descriptor_buffers;
        const auto set_descriptor_offsets=descriptor_dispatch.set_descriptor_offsets;
        const auto bind_embedded_samplers=descriptor_dispatch.bind_embedded_samplers;
        // Redirect writes to this frame's descriptor storage.
        const auto write_image_descriptor_buffer = [&](uint32_t index, VkDescriptorType type, VkImageView view) {
            VkDescriptorImageInfo ii{};
            ii.imageView=view;
            ii.imageLayout=type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ?
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorGetInfoEXT di{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            di.type=type;
            size_t size;
            if (type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                di.data.pSampledImage=&ii; size=descriptor_properties.sampledImageDescriptorSize;
            } else { di.data.pStorageImage=&ii; size=descriptor_properties.storageImageDescriptorSize; }
            get_descriptor(device,&di,size,static_cast<uint8_t*>(frame.descriptors[1].mapped)+
                descriptor_layout.offset(1,index,size));
        };
        const auto write_buffer_descriptor_buffer = [&](uint32_t index,const Buffer& buffer) {
            VkDescriptorAddressInfoEXT ai{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            ai.address=buffer_address(buffer); ai.range=buffer.size;
            VkDescriptorGetInfoEXT di{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            di.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; di.data.pStorageBuffer=&ai;
            get_descriptor(device,&di,descriptor_layout.storage_buffer_size,
                static_cast<uint8_t*>(frame.descriptors[2].mapped)+
                descriptor_layout.offset(2,index,descriptor_layout.storage_buffer_size));
        };
        if(!frame.descriptors_initialized)
            for (auto& b:frame.descriptors) std::memset(b.mapped,0,b.size);
        // Pre-pass image table: SRV base 12 and UAV base 0.
        write_image_descriptor_buffer(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      fx && dl_case==1 ? reprojected.view : history.view);       // t0 (what the exact-mode TAIL writes)
        write_image_descriptor_buffer(13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      motion.view);        // t1
        write_image_descriptor_buffer(14, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      depth.view);         // t2
        write_image_descriptor_buffer(15, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      color.view);         // t3
        write_image_descriptor_buffer(16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      recurrent.view);     // t4
        write_image_descriptor_buffer(29, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      exposure_image.view);  // t17
        write_image_descriptor_buffer(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      reprojected.view);  // u3
        // Post-pass tables: UAV base 51 and SRV base 63.
        write_image_descriptor_buffer(66, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      dl_case==1 ? dl_cc.view : color.view);          // t3 (exact-mode TAIL: colour of N-1)
        write_image_descriptor_buffer(72, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      reprojected.view);    // t9
        write_image_descriptor_buffer(80, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      exposure_image.view);  // t17
        write_image_descriptor_buffer(52, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      fx ? reprojected.view : history.view);       // u1 (O)
        if(fx) { write_image_descriptor_buffer(63,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,history.view);   // post t0 = L
                 write_image_descriptor_buffer(64,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,dl_case==1 ? dl_mv.view : motion.view); }   // post t1 = MV (TAIL: of N-1)
        write_image_descriptor_buffer(53, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      output.view);        // u2
        write_image_descriptor_buffer(57, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      recurrent.view);     // u6
        if (nms_inputs) {
            write_image_descriptor_buffer(90,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,images[4].view);
            write_image_descriptor_buffer(91,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,images[5].view);
            write_image_descriptor_buffer(92,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,images[3].view);
            write_image_descriptor_buffer(93,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          external_exposure ? supplied_exposure.view : color.view);
        }

        if(!frame.descriptors_initialized) {
        if(fsr411) {
            write_buffer_descriptor_buffer(11,scratch);
            write_buffer_descriptor_buffer(30,initializer);
        } else {
        for (const auto index : {
                 11u, 20u, 22u, 24u, 26u, 28u, 30u,
                 34u, 38u, 42u, 46u, 48u, 50u, 62u}) {
            write_buffer_descriptor_buffer(index, scratch);
        }
        for (const auto index : {32u, 36u, 40u, 44u}) {
            write_buffer_descriptor_buffer(index, initializer);
        }
        }
        write_buffer_descriptor_buffer(62, scratch);  // post u11
        frame.descriptors_initialized=true;
        }


        main_constants.previous_pre_exposure = previous_exposure;
        main_constants.reset = main_constants.reset || !initialized;
        std::memset(frame.constants.mapped,0,2048);
        std::memcpy(frame.constants.mapped,&main_constants,sizeof(main_constants));
        if (nms_inputs) {
            struct SpdConstants { uint32_t mips, groups, offset[2]; float inverse_size[2], pre_exposure, pad; };
            const auto rw=main_constants.width_lr, rh=main_constants.height_lr;
            const auto gx=(rw+63)/64, gy=(rh+63)/64;
            unsigned mips=0;for(auto size=std::max(rw,rh);size>1;size>>=1)++mips;
            mips=std::min(mips,12u);
            const SpdConstants spd{mips,gx*gy,{0,0},{1.0f/rw,1.0f/rh},main_constants.pre_exposure,0};
            static_assert(sizeof(spd)==32);
            std::memcpy(static_cast<uint8_t*>(frame.constants.mapped)+512,&spd,sizeof(spd));
        }
        if(fsr411) {
            std::array<std::array<uint32_t,4>,17> tensors{};
            for(size_t i=0;i<tensors.size();++i) {
                tensors[i][0]=fsr4::round_up(main_constants.width,8)/fsr4::k411TensorDivisors[i];
                tensors[i][1]=fsr4::round_up(main_constants.height,8)/fsr4::k411TensorDivisors[i];
            }
            std::memcpy(static_cast<uint8_t*>(frame.constants.mapped)+1024,tensors.data(),sizeof(tensors));
        } else std::memcpy(static_cast<uint8_t*>(frame.constants.mapped)+1024,weights.data(),weights.size());
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(u15_game.image && u15_fresh) {
            VkClearColorValue zero{};
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,u15_mid.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(command_buffer,u15_mid.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,u15_mid.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
            u15_fresh=false;
        } else if(u15_game.image)
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,u15_mid.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,0,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,0);
        bool dl_reset=false;
        if(dl_case==1 || dl_case==2 || dl_case==-1) {   // TAIL of frame N-1 (constants slot N-1, post tables of this slot), or its transitions
            if(profile_queries) { vkCmdResetQueryPool(command_buffer,profile_queries,profile_base,profile_query_count); dl_reset=true; }
            std::array<VkDeviceAddress,3> ad{}; for(size_t i=0;i<3;++i) ad[i]=buffer_address(frame.descriptors[i]);
            auto& prev=frames[(next_frame+frames.size()-1)%frames.size()];
            fsr4::record_fsr411_frame(command_buffer,pipeline_layout,std::span<const VkPipeline>(pipelines).first(27),ad,
                buffer_address(prev.constants),history.image,recurrent.image,reprojected.image,
                bind_descriptor_buffers,set_descriptor_offsets,bind_embedded_samplers,profile_queries,profile_base+2,
                main_constants.width,main_constants.height,model_capacity_width,model_capacity_height,
                dispatch.cmd_pipeline_barrier2,dispatch.cmd_write_timestamp2,x_k2,x_inter.buffer ? buffer_address(x_inter) : 0,
                fv_bridge,buffer_address(scratch),dl_case==1 ? 2 : 3,fx,(x_u15==1 || x_u15==2) ? 14u : 0u);
            dl_open=false;
            if(fx && dl_case==1) fx_lat^=1;   // The TAIL's K2 wrote O
        }
        if(dl_case==3) {   // The first HEAD frame shows the entry frame's output again
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,output.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy c{}; c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.dstSubresource=c.srcSubresource; c.extent={output.width,output.height,1};
            vkCmdCopyImage(command_buffer,dl_dup.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,output.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
        }
        if(dl_case==1 || dl_case==3) {   // colour(N) -> dl_cc for TAIL(N) in the next frame (same format and extent: bit-exact sampling)
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,color.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,dl_cc.image,dl_case==3 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy c{}; c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.dstSubresource=c.srcSubresource; c.extent={color.width,color.height,1};
            vkCmdCopyImage(command_buffer,color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dl_cc.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,0,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,dl_cc.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        if(fx && (dl_case==1 || dl_case==3)) {   // MV(N) -> dl_mv (as colour -> dl_cc) for TAIL(N) in the next frame (same format and extent: bit-exact sampling)
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,motion.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,dl_mv.image,dl_case==3 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy c{}; c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.dstSubresource=c.srcSubresource; c.extent={motion.width,motion.height,1};
            vkCmdCopyImage(command_buffer,motion.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dl_mv.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,motion.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,0,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,dl_mv.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        if (!initialized) {
            VkClearColorValue zero{};
            for (auto& i:images) {
                image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,i.image,VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdClearColorImage(command_buffer,i.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
                image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,i.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    i.image!=history.image && i.image!=recurrent.image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            }
        } else {
            for (auto* i : {&images[fx ? fx_lat : 0],&recurrent})   // L after a TAIL swap
                image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,i->image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,images[fx ? (fx_lat^1) : 1].image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT);
        }
        // Retain the zero-initialized scratch behavior of the reference runner.
        VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        before.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        before.dstStageMask=VK_PIPELINE_STAGE_2_CLEAR_BIT;
        before.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount=1; dep.pMemoryBarriers=&before;
        dispatch.cmd_pipeline_barrier2(command_buffer,&dep);
        // Vela: the zero fill is needed on the first frame and when the render size changes (edge elements are never written)
        if(vela_always_fill || !initialized || vela_fill_w!=main_constants.width_lr || vela_fill_h!=main_constants.height_lr)
            vkCmdFillBuffer(command_buffer,scratch.buffer,0,VK_WHOLE_SIZE,0);
        // 4.1.1 pads the neural tensors to round_up(output, 8) / 2, the pre writes X0 only for output rows < (height & ~1). At a
        // height that is not a multiple of 8 (900 -> X0 rows 450..451 of 452) those pad rows kept the previous frame's region-0 data (OUT of
        // model_12) since 0.8.x skips the per-frame clear. Zero them every frame (= the reference runner's per-frame scratch clear) when the
        // fused pre's fixed layout is in use (region 0 at offset 0, 962 x uvec4 rows, halo 1) and the width needs no pad; 1080p/720p: no pad rows.
        // Also for the plain baked 1080-tier pre (same X0 layout; 900p UP without the fill 0/119 vs the reference runner).
        else if(fsr411 && ((x_pre_fused && pipelines[0]==x_pre_fused) || (x_pre_b1080 && pipelines[0]==x_pre_b1080))
                && (main_constants.width&~1u)==fsr4::round_up(main_constants.width,8)
                && fsr4::round_up(main_constants.height,8)>(main_constants.height&~1u) && fsr4::round_up(main_constants.height,8)/2+2<=542u) {
            const VkDeviceSize r0=(main_constants.height&~1u)/2+1,r1=fsr4::round_up(main_constants.height,8)/2+1;
            vkCmdFillBuffer(command_buffer,scratch.buffer,15392ull*r0,15392ull*(r1-r0),0);
        }
        vela_fill_w=main_constants.width_lr; vela_fill_h=main_constants.height_lr;
        before.srcStageMask=VK_PIPELINE_STAGE_2_CLEAR_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
        before.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
        before.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        before.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT;
        dispatch.cmd_pipeline_barrier2(command_buffer,&dep);
        std::array<VkDeviceAddress,3> addresses{};
#if defined(FSR4_RESEARCH_CAPTURE)
        if(nms_inputs) {
            fsr4capture::begin(physical_device,device,scratch.buffer,scratch.size,images[3].image,
                reprojected.image,reprojected.width,reprojected.height,frame.constants.mapped,fsr411 ? 28 : 15);
            fsr4capture::after_pass(command_buffer,-1);
        }
#endif
        for(size_t i=0;i<3;++i) addresses[i]=buffer_address(frame.descriptors[i]);
        if (nms_inputs) {
            if (initialized)
                image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,images[3].image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
            fsr4::compute_barrier(dispatch.cmd_pipeline_barrier2,command_buffer);
            std::array<VkDescriptorBufferBindingInfoEXT,3> bindings{};
            for (size_t i=0;i<3;++i) {
                bindings[i].sType=VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                bindings[i].address=addresses[i];
                bindings[i].usage=i==0 ? VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT :
                                       VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
            }
            bind_descriptor_buffers(command_buffer,3,bindings.data());
            const uint32_t indices[]{0,1,2}; const VkDeviceSize offsets[]{0,0,0};
            set_descriptor_offsets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,3,indices,offsets);
            const auto address=buffer_address(frame.constants)+512;
            const uint32_t push[]{external_exposure ? uint32_t(main_constants.reset!=0) : uint32_t(address),
                                  uint32_t(address>>32),90,93};
            vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines.back());
            vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
            if (profile_queries) {
                if(!dl_reset) vkCmdResetQueryPool(command_buffer,profile_queries,profile_base,profile_query_count);
                dispatch.cmd_write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,profile_queries,profile_base);
            }
            vkCmdDispatch(command_buffer,external_exposure ? 1 : (main_constants.width_lr+63)/64,
                          external_exposure ? 1 : (main_constants.height_lr+63)/64,1);
#if defined(FSR4_RESEARCH_CAPTURE)
            fsr4capture::after_pass(command_buffer,0);
#endif
            if (profile_queries) dispatch.cmd_write_timestamp2(
                command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,profile_queries,profile_base+1);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,images[3].image,VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
        }
        if (profile_queries && !nms_inputs && !dl_reset) vkCmdResetQueryPool(command_buffer,profile_queries,profile_base,profile_query_count);
        if(fsr411) fsr4::record_fsr411_frame(command_buffer,pipeline_layout,std::span<const VkPipeline>(pipelines).first(27),addresses,
            buffer_address(frame.constants),history.image,recurrent.image,reprojected.image,
            bind_descriptor_buffers,set_descriptor_offsets,bind_embedded_samplers,profile_queries,profile_base+2,
            main_constants.width,main_constants.height,model_capacity_width,model_capacity_height,
            dispatch.cmd_pipeline_barrier2,dispatch.cmd_write_timestamp2,
            x_split && pipelines[26]==x_k1 ? x_k2 : VK_NULL_HANDLE, x_inter.buffer ? buffer_address(x_inter) : 0,
            fv_bridge,buffer_address(scratch),dl_case==1 ? 1 : dl_case==3 ? 4 : 0,fx,(x_u15==1 || x_u15==2) ? 14u : 0u);
        else fsr4::record_quality_frame(command_buffer,pipeline_layout,std::span<const VkPipeline>(pipelines).first(kPasses.size()),addresses,
            buffer_address(frame.constants),history.image,recurrent.image,reprojected.image,
            bind_descriptor_buffers,set_descriptor_offsets,bind_embedded_samplers,profile_queries,profile_base+(nms_inputs ? 2 : 0),
            main_constants.width,main_constants.height,dispatch.cmd_pipeline_barrier2,
            dispatch.cmd_write_timestamp2);
        if(dl_case==1 || dl_case==3) dl_open=true;
        if(fx && dl_case!=1 && dl_case!=3) fx_lat^=1;   // This call's K2 (FULL frame) wrote O
        if(dl_case==4) {   // Keep the entry frame's output (shown again by the first HEAD frame)
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,output.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,dl_dup.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,0,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy c{}; c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.dstSubresource=c.srcSubresource; c.extent={output.width,output.height,1};
            vkCmdCopyImage(command_buffer,output.image,VK_IMAGE_LAYOUT_GENERAL,dl_dup.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,dl_dup.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        if(u15_game.image) {   // The ONLY writer of the game's image in modes 3 / 4: u15_mid (latest K2 / K2b / exact-mode copy) -> 1.5x
            image_barrier(dispatch.cmd_pipeline_barrier2,command_buffer,u15_mid.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            const auto put=[&](uint32_t index,VkDescriptorType type,VkImageView view) {   // GENERAL layout for both (u15_mid stays GENERAL)
                VkDescriptorImageInfo ii{}; ii.imageView=view; ii.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
                VkDescriptorGetInfoEXT di{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT}; di.type=type; size_t size;
                if(type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) { di.data.pSampledImage=&ii; size=descriptor_properties.sampledImageDescriptorSize; }
                else { di.data.pStorageImage=&ii; size=descriptor_properties.storageImageDescriptorSize; }
                get_descriptor(device,&di,size,static_cast<uint8_t*>(frame.descriptors[1].mapped)+descriptor_layout.offset(1,index,size)); };
            put(100,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,u15_mid.view); put(101,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,u15_game.view);
            std::array<VkDescriptorBufferBindingInfoEXT,3> ub{};
            for(size_t i=0;i<3;++i) { ub[i].sType=VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT; ub[i].address=buffer_address(frame.descriptors[i]);
                ub[i].usage=i==0 ? VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT : VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT; }
            bind_descriptor_buffers(command_buffer,3,ub.data());
            const uint32_t ui[]{0,1,2}; const VkDeviceSize uo[]{0,0,0};
            set_descriptor_offsets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,3,ui,uo);
            vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,x_u15_up);
            const uint32_t up[6]{0,0,101,100,u15_mid.width,u15_mid.height};
            vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(up),up);
            if(profile_queries) dispatch.cmd_write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,profile_queries,profile_base+58);
            vkCmdDispatch(command_buffer,(u15_mid.width/2+15)/16,(u15_mid.height/2+15)/16,1);
            if(profile_queries) dispatch.cmd_write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,profile_queries,profile_base+59);
            if(profile_queries) for(uint32_t q=60;q<64;++q) dispatch.cmd_write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,profile_queries,profile_base+q);
        }
        VkMemoryBarrier2 end{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        end.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        end.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo done{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        done.memoryBarrierCount=1; done.pMemoryBarriers=&end;
        dispatch.cmd_set_event2(command_buffer,frame.done,&done);
        frame.used=true;
        frame.profile_pending=profile_queries!=VK_NULL_HANDLE;
        frame.profile_base=profile_base;
        frame.serial=profile_serial++;
        frame.width=main_constants.width;frame.height=main_constants.height;
        frame.render_width=main_constants.width_lr;frame.render_height=main_constants.height_lr;
        frame.color=active_color;
        last_profile_base=profile_base;
        next_frame=(next_frame+1)%frames.size();
        initialized=true;
        previous_exposure=main_constants.pre_exposure;
    }
};
} // namespace fsr4core
