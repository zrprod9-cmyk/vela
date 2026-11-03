#pragma once
// Vela 0.7.0): FSR 4.1.2 Lite recorder. Mirrors the capture replayer (fsr4lite/gen5/replay5.cpp with the split
// postpass twin, replayJ.cpp for the lossy schedule): 11 captured dispatches + K2 (module 13, postpass layout and
// grid), classic descriptor sets with the captured layouts (immutable linear clamp sampler at b320), one UBO slot per pass
// (aux 32 B SPD constants, prepass/postpass 104 B OptimizedConstants, network passes 144 B captured constants), scratch
// 25,027,392 B, weights 27,392 B, internal images zero-initialised on the first frame. Kernels: fsr412lite-blobs.hpp.
// Lossy (network_every = N > 1): a skipped frame is aux + reproject-only prepass (J pre_r2c, 3x3 clamp) + K2; K1 output
// (scratch plane 0) and the rest of scratch stay untouched across skipped frames). N = 1 is exact.
// SR4 (0.8.2, default when N > 1; VELA_SR3=0 = the old pre_r2c path): a skipped frame is aux + R sr_r4: warp the K1
// feature plane cp -> cp^1 with the round-2 rest / uncovered / edge rules, reprojected colour) + W round-2 sr_w = Lite K2
// on plane cp^1), history ping-pong kHistory/kXHist, feature planes at scratch bytes 0 and 8388608 seqJ.inc mode kr).
// Supported sizes (since 0.8.2): any render size -> 1920x1080 for which AMD's Lite picks the same model as at 960x540
//: same 13 modules and weights from 672x378 up to 1920x1080; the network always runs at 960x540 = output / 2,
// prepass grid 120x68, network grids and K2 60x34 unchanged; render size reaches the kernels only through the 104 B constants).
// Size-dependent here: aux grid ceil(render / 64) and SPD constants, the colour copy, the exposure mip. SR4 skipped frames index
// the feature plane by render pixel (sr_r4 assumes render == 960x540), so other sizes run the network every frame (N = 1).
// 640x360 (ratio 3.0, width <= 641) is another AMD model (4 modules + weights differ): rejected, the host falls back.
#include "fsr412lite-blobs.hpp"
#include "vela_npu_abi.h"
#include "vela_npu_ext.h"
#include "fsr412lite-npu-spv.hpp"
#include "fsr4fu-spv.hpp"
#include "fsr4sr4-spv.hpp"   // GPL-2.0-or-later skipped-frame kernels, separate file on purpose
#include <iterator>
#include <stdexcept>
#include <cstdio>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsr4core {

// Bench-only instrumentation (stays null in the DLL): two timestamps per dispatch.
inline VkQueryPool g_lite_profile_pool = VK_NULL_HANDLE;
inline std::uint32_t g_lite_profile_used = 0;
inline const char* g_lite_profile_names[16] = {};
// Test only (stays false in the DLL): the HTP input carries the GPU network result (npu_netref.comp)
inline bool g_lite_npu_loopback = false;
struct LiteNpuStats { std::uint64_t npu_frames = 0, gpu_frames = 0, async_frames = 0, acks_ok = 0, acks_fail = 0, acks_absent = 0, probes = 0;
                      std::uint64_t fu_frames = 0, htp_n = 0; double htp_ms = 0;
                      std::uint32_t last_status = 0xffffffffu, layer_build = 0, caps = 0; int layer = 0; /* 0 unknown, 1 ok, -1 absent, -2 back-off */ };
inline LiteNpuStats g_lite_npu_stats{};   // copy of the last recorder counters (bench readout)
inline std::uint32_t g_lite_npu_mode = 0;

class LiteContext {
public:
    static constexpr std::uint32_t kRenderW = 960, kRenderH = 540, kOutW = 1920, kOutH = 1080, kSlots = 8, kPasses = 11;
    static constexpr VkDeviceSize kScratchBytes = 25027392, kWeightsBytes = sizeof(fsr4lite::kWeights);
    // AMD's model choice follows the render width (measured on captures): 645 and up = the 540p model
    // (700x360, 960x360 too), 641 and below = another model (641x361, 640x400, 640x540). kMinRenderW/H stay on the measured side.
    static constexpr std::uint32_t kMinRenderW = 645, kMinRenderH = 360;
    static bool supports(std::uint32_t rw, std::uint32_t rh, std::uint32_t ow, std::uint32_t oh) {
        return ow == kOutW && oh == kOutH && rw >= kMinRenderW && rh >= kMinRenderH && rw <= kOutW && rh <= kOutH;
    }
    std::uint32_t render_width() const { return rw_; }
    std::uint32_t render_height() const { return rh_; }
    LiteContext(VkPhysicalDevice physical, VkDevice logical, std::uint32_t network_every,
                VkImageLayout sampled_input_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, std::uint32_t npu_mode = 0,
                std::uint32_t render_w = kRenderW, std::uint32_t render_h = kRenderH)
        : device(logical), every(network_every < 1 ? 1 : network_every > 16 ? 16 : network_every), input_layout(sampled_input_layout),
          rw_(render_w), rh_(render_h) {
        if (rw_ != kRenderW || rh_ != kRenderH) every = 1;   // Skipped frames (SR4 / pre_r2c) are 540p-only
        npu = every == 1 && npu_mode <= 2 ? npu_mode : 0;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical, &props);
        const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 1;
        ubo_stride = ((256 + align - 1) / align) * align;
        VkPhysicalDeviceSubgroupSizeControlProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &sg;
        if (props.apiVersion >= VK_API_VERSION_1_3) vkGetPhysicalDeviceProperties2(physical, &p2);
        min_subgroup = sg.minSubgroupSize; max_subgroup = sg.maxSubgroupSize;
        // The captured pipelines require subgroup size 32 (AMD/NVIDIA). Adreno offers 64/128 only and runs the default
        // size, exactly like the console replayer runs (no REPLAY_SG32).
        sg32 = sg.minSubgroupSize && sg.minSubgroupSize <= 32 && sg.maxSubgroupSize >= 32 &&
               (sg.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) && !std::getenv("FSR4_LITE_NO_SG32");
        try { build(); build_npu(); } catch (...) { release(); throw; }
    }
    LiteContext(const LiteContext&) = delete;
    LiteContext& operator=(const LiteContext&) = delete;
    ~LiteContext() { release(); }
    std::uint32_t network_every() const { return every; }
    bool subgroup32() const { return sg32; }
    std::uint32_t subgroup_min() const { return min_subgroup; }
    std::uint32_t subgroup_max() const { return max_subgroup; }
    VkDeviceSize owned_bytes() const { return owned; }

    // Inputs: sampled in input_layout; output: GENERAL storage. Returns true for a network frame.
    std::uint32_t npu_mode() const { return npu; }
    const LiteNpuStats& npu_stats() const { return nstats; }
    // State lines for OptiScaler.log; the provider forwards them via fpMessage.
    std::vector<std::string> take_npu_lines() { std::vector<std::string> r; r.swap(lines); return r; }

    // Inputs: sampled in input_layout; output: GENERAL storage. Returns true for a network frame.
    // npu = 0: unchanged v2 recorder. npu = 1 (exact): aux, prepass, pass0_post, pack -> marker (layer: tensor -> HTP ->
    // tensor, GPU waits inside the command buffer) -> select (reads the ack on the GPU timeline) -> GPU network passes
    // 3..9 OR unpack, by indirect dispatch -> K1, K2. npu = 2 (async): this call records POST(N-1) (select, net|unpack,
    // K1, K2 with N-1's constants and colour copy, into this call's output) and then PRE(N) + pack + kick; the HTP runs
    // frame N between calls. The CPU sends no marker while the last completed frame showed no ack (GPU path + PROBE).
    bool record(VkCommandBuffer cb, Image color, Image depth, Image motion, Image output, OptimizedConstants c) {
        poll_markers();
        std::vector<VkImageView> made;
        for (auto* im : {&color, &depth, &motion, &output}) {
            if (im->view) continue;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im->image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = im->format;
            const bool dfmt = im->format == VK_FORMAT_D32_SFLOAT_S8_UINT || im->format == VK_FORMAT_D32_SFLOAT;
            vi.subresourceRange = {dfmt ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(device, &vi, nullptr, &im->view), "Lite external image view");
            made.push_back(im->view);
        }
        c.previous_pre_exposure = previous_exposure;
        c.reset = c.reset || !initialized;
        if (c.reset) frame = 0;
        const bool net = frame % every == 0;
        if (!initialized) init_resources(cb);
        if (npu && npu_backoff && --npu_backoff == 0) nstats.layer = 0;   // retry the PROBE after a back-off
        const bool npu_on = npu && every == 1 && nstats.layer == 1;
        const bool want_async = npu_on && npu == 2;
        const bool probe = npu && every == 1 && nstats.layer == 0;
        bool out_written = false;
        if (pending) {   // async: POST(N-1) into this call's output
            const std::uint32_t ps = take_slot();
            set_ubo(ps, prev_c);
            write_sets(ps, color.view, depth.view, motion.view, output.view, color_copy.view);
            const bool wait = (nstats.caps & VELA_NPU_CAP_ASYNC) != 0;
            if (wait) write_marker(cb, ps, 1, VELA_NPU_OP_WAIT, pending_seq, 0, prev_c);
            fullb(cb);
            select(cb, word_of(pending_slot, 0), wait ? word_of(ps, 1) : 0xffffffffu, pending_seq);
            if (pending_fu) net_k1_fu(cb, pending_zc);   // network (0 groups) + K1 GPU (0 groups) | K1 fused
            else { net_or_unpack(cb, pending_zc); run(cb, pipelines[10], 10, "k1"); }
            run(cb, k2, 10, "k2");
            finish_slot(cb, ps, {});
            pending = false; out_written = true;
        }
        const std::uint32_t si = take_slot();
        set_ubo(si, c);
        if (want_async && (out_written || keep_valid)) {
            ensure_async_images(cb, color, output);
            write_sets(si, color.view, depth.view, motion.view, output.view, color.view);
            fullb(cb);
            run(cb, pipelines[0], 0, "aux");
            fu_pre_frame = p_fu_pre && fu_enabled() && !g_lite_npu_loopback && cur_zc >= 0;
            if (fu_pre_frame) run_fu(cb, p_fu_pre, fu_pre_layout, 1, cur_zc, "pre", nullptr);   // prepass + HTP input store
            else run(cb, pipelines[1], 1, "pre");
            run(cb, pipelines[2], 2, "net");
            const std::uint64_t seq = ++npu_seq;
            npu_handoff(cb, si, seq, VELA_NPU_F_ASYNC, c);
            fu_pre_frame = false;
            copy_color(cb, color);
            if (!out_written) copy_image(cb, keep.image, output.image, kOutW, kOutH);   // switch-on: repeat the last frame once
            pending = true; pending_slot = si; pending_seq = seq; prev_c = c; pending_zc = frame_zc; pending_fu = frame_zc >= 0 && p_fu_k1 && fu_enabled();
            if (pending_fu) ++nstats.fu_frames;
            ++nstats.npu_frames; ++nstats.async_frames;
        } else {
            write_sets(si, color.view, depth.view, motion.view, output.view, color.view);
            fullb(cb);
            run(cb, pipelines[0], 0, "aux");
            if (net) {
                run(cb, pipelines[1], 1, "pre");
                run(cb, pipelines[2], 2, "net");
                if (npu_on) {
                    const std::uint64_t seq = ++npu_seq;
                    npu_handoff(cb, si, seq, 0, c);
                    fullb(cb);
                    select(cb, word_of(si, 0), 0xffffffffu, seq);
                    net_or_unpack(cb, frame_zc);
                    ++nstats.npu_frames;
                } else {
                    for (int p = 3; p <= 9; ++p) run(cb, pipelines[p], p, "net");
                    if (probe) { write_marker(cb, si, 0, VELA_NPU_OP_PROBE, ++npu_seq, 0, c); ++nstats.probes; }
                    ++nstats.gpu_frames;
                }
                run(cb, pipelines[10], 10, "k1");
            } else if (sr) {
                record_sr(cb, si, color.view, depth.view, motion.view, output.view);
            } else {
                run(cb, pre_skip, 1, "pre");
            }
            if (net || !sr) run(cb, k2, 10, "k2");
            if (net) { sr_h = 0; sr_cp = 0; }   // K1 wrote plane 0, K2 wrote kHistory
            if (want_async) { ensure_async_images(cb, color, output); fullb(cb); copy_image(cb, output.image, keep.image, kOutW, kOutH); keep_valid = true; }
            else keep_valid = false;
        }
        finish_slot(cb, si, std::move(made));
        previous_exposure = c.pre_exposure;
        initialized = true;
        ++frame;
        g_lite_npu_stats = nstats; g_lite_npu_mode = npu;
        return net;
    }

private:
    struct OwnedImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    struct OwnedBuffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; std::uint8_t* map = nullptr; };
    struct Slot { std::array<VkDescriptorSet, kPasses> sets{}; VkEvent done = VK_NULL_HANDLE; bool used = false; std::vector<VkImageView> views;
                  std::uint32_t marker_mask = 0; std::uint64_t marker_seq[2] = {}; };
    enum { kHistory, kReprojected, kRecurrent, kCounter, kMip, kExposure, kXHist, kImageCount };

    VkDevice device;
    std::uint32_t every;
    VkImageLayout input_layout;
    std::uint32_t rw_ = kRenderW, rh_ = kRenderH;   // Render size of this context (supports())
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkDeviceSize ubo_stride = 256, owned = 0;
    bool sg32 = false;
    std::uint32_t min_subgroup = 0, max_subgroup = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, kPasses> set_layouts{};
    std::array<VkPipelineLayout, kPasses> layouts{};
    std::array<VkPipeline, kPasses> pipelines{};
    VkPipeline k2 = VK_NULL_HANDLE, pre_skip = VK_NULL_HANDLE;
    // SR4 skipped frames: [0] = R (sr_r4), [1] = W (sr_w); per-slot sets; sr_h = history read next (1 = kXHist), sr_cp = feature plane
    bool sr = false; std::uint32_t sr_h = 0, sr_cp = 0;
    VkDescriptorSetLayout sr_set_layout[2] = {}; VkPipelineLayout sr_layout[2] = {}; VkPipeline sr_pipe[2] = {}; VkDescriptorPool sr_pool = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, 2>, kSlots> sr_sets{};
    static bool sr_enabled() { static const bool on = [] { const char* v = std::getenv("VELA_SR3"); return !(v && v[0] == '0'); }(); return on; }
    std::array<OwnedImage, kImageCount> images{};
    OwnedBuffer scratch, weights, staging, ubo;
    std::uint8_t* ubo_map = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots{};
    std::uint32_t next = 0;
    std::uint64_t frame = 0;
    float previous_exposure = 0;
    bool initialized = false;
    VkCommandBuffer current = VK_NULL_HANDLE;
    bool build_npu_deferred = false;
    // ------------------------------------------------------------------ NPU (W-fork)
    std::uint32_t npu = 0, cur = 0, npu_backoff = 0, fail_streak = 0, last_line_state = 0xffffffffu;
    std::uint64_t npu_seq = 0, pending_seq = 0, line_frames = 0, line_fallbacks = 0; double line_htp = 0;
    bool pending = false, keep_valid = false;
    std::uint32_t pending_slot = 0;
    OptimizedConstants prev_c{};
    LiteNpuStats nstats{};
    std::vector<std::string> lines;
    OwnedImage tin{}, out0{}, out1{}, color_copy{}, keep{};
    struct Zc { std::uint64_t buf = 0; std::uint32_t gen = 0, q[4] = {}; VkDescriptorSet set = VK_NULL_HANDLE; };
    static constexpr int kMaxZc = 8;
    Zc zcs[kMaxZc]{}; int nzc = 0, cur_zc = -1, frame_zc = -1, pending_zc = -1;
    VkDescriptorSetLayout zc_set_layout = VK_NULL_HANDLE; VkPipelineLayout zc_layout = VK_NULL_HANDLE; VkDescriptorPool zc_pool = VK_NULL_HANDLE;
    VkPipeline p_pack_zc = VK_NULL_HANDLE, p_unpack_zc = VK_NULL_HANDLE;
    // Fused prepass+pack (set 0 = prepass set, set 1 = dma-buf) and K1+unpack (set 0 = postpass set, set 1 = dma-buf)
    VkPipelineLayout fu_pre_layout = VK_NULL_HANDLE, fu_k1_layout = VK_NULL_HANDLE;
    VkPipeline p_fu_pre = VK_NULL_HANDLE, p_fu_k1 = VK_NULL_HANDLE;
    bool fu_pre_frame = false, pending_fu = false;
    static bool fu_enabled() { static const bool on = [] { const char* v = std::getenv("VELA_FU_FUSE"); return !(v && v[0] == '0'); }(); return on; }
    VkFormat color_copy_format = VK_FORMAT_UNDEFINED, keep_format = VK_FORMAT_UNDEFINED;
    OwnedBuffer marker{}, args_full{}, args_live{};
    VkDescriptorSetLayout npu_set_layout = VK_NULL_HANDLE; VkPipelineLayout npu_layout = VK_NULL_HANDLE;
    VkDescriptorPool npu_pool = VK_NULL_HANDLE; VkDescriptorSet npu_set = VK_NULL_HANDLE;
    VkPipeline p_pack = VK_NULL_HANDLE, p_unpack = VK_NULL_HANDLE, p_select = VK_NULL_HANDLE, p_netref = VK_NULL_HANDLE;
    static constexpr std::uint32_t kUnpackGroups = (521404 + 63) / 64, kPackGroups = 518400 / 64;

    static std::uint32_t word_of(std::uint32_t slot, std::uint32_t k) { return (slot * 2 + k) * 64; }   // 256 B = 64 words
    static void fullb(VkCommandBuffer cb) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    std::uint32_t take_slot() {
        auto& s = slots[next];
        if (s.used) {
            const auto status = vkGetEventStatus(device, s.done);
            if (status == VK_EVENT_RESET) throw std::runtime_error("Lite context has eight recordings still in flight");
            if (status != VK_EVENT_SET) throw std::runtime_error("Lite vkGetEventStatus failed");
            read_marker(next);
            check(vkResetEvent(device, s.done), "vkResetEvent");
            s.used = false;
        }
        for (auto v : s.views) vkDestroyImageView(device, v, nullptr);
        s.views.clear();
        cur = next; next = (next + 1) % kSlots;
        return cur;
    }
    void finish_slot(VkCommandBuffer cb, std::uint32_t si, std::vector<VkImageView> views) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkCmdSetEvent(cb, slots[si].done, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        slots[si].used = true;
        slots[si].views = std::move(views);
    }
    void set_ubo(std::uint32_t si, const OptimizedConstants& c) {
        std::uint8_t* u = ubo_map + VkDeviceSize(si) * kPasses * ubo_stride;
        struct SpdConstants { std::uint32_t mips, groups, offset[2]; float inverse_size[2], pre_exposure, pad; };
        const std::uint32_t gx = (c.width_lr + 63) / 64, gy = (c.height_lr + 63) / 64;
        const SpdConstants spd{std::uint32_t(std::floor(std::log2(double(c.width_lr > c.height_lr ? c.width_lr : c.height_lr)))), gx * gy, {0, 0},
                               {1.0f / c.width_lr, 1.0f / c.height_lr}, c.pre_exposure, 0};
        static_assert(sizeof(SpdConstants) == 32);
        std::memcpy(u, &spd, sizeof spd);
        std::memcpy(u + 1 * ubo_stride, &c, sizeof c);
        std::memcpy(u + 10 * ubo_stride, &c, sizeof c);
    }
    static VelaNpuImage npu_image(VkImage im) {
        VelaNpuImage r{}; r.image = std::uint64_t(im); r.format = VK_FORMAT_R32_UINT; r.layout = VK_IMAGE_LAYOUT_GENERAL;
        r.width = kOutW; r.height = kOutH; r.aspect = VK_IMAGE_ASPECT_COLOR_BIT; return r;
    }
    void write_marker(VkCommandBuffer cb, std::uint32_t si, std::uint32_t k, std::uint32_t op, std::uint64_t seq, std::uint32_t flags,
                      const OptimizedConstants& c) {
        VelaNpuCmd m{}; m.magic = VELA_NPU_MAGIC_CMD; m.version = VELA_NPU_VERSION; m.op = op; m.seq = seq;
        m.flags = flags | (c.reset ? VELA_NPU_F_RESET : 0u);
        if (op == VELA_NPU_OP_UPSCALE) { m.flags |= VELA_NPU_F_TENSORS; m.color = npu_image(tin.image); m.output = npu_image(out0.image); m.motion = npu_image(out1.image); }
        if (op == VELA_NPU_OP_UPSCALE && frame_zc >= 0) { m.flags |= VELA_NPU_F_ZEROCOPY; m.pad0 = zcs[frame_zc].gen; }   // DX: no layer copies
        m.jitter[0] = c.jitter[0]; m.jitter[1] = c.jitter[1]; m.mvScale[0] = c.mv_scale[0]; m.mvScale[1] = c.mv_scale[1];
        m.preExposure = c.pre_exposure;
        const bool pr = op != VELA_NPU_OP_PROBE && prof_b(cb, "lay");   // the layer's recording (copies, E1, E2 wait) lands in here
        vkCmdUpdateBuffer(cb, marker.buffer, VkDeviceSize(word_of(si, k)) * 4, sizeof m, &m);
        if (pr) prof_e(cb);
        slots[si].marker_mask |= 1u << k; slots[si].marker_seq[k] = seq;
    }
    // GPU -> HTP: pack (or, test only, the GPU network + its result in the HTP output layout) then the marker
    void npu_handoff(VkCommandBuffer cb, std::uint32_t si, std::uint64_t seq, std::uint32_t flags, const OptimizedConstants& c) {
        // DX: zero-copy for async kicks only. Exact keeps 0.8.0's copy path: its GPU waits for the HTP anyway (no saving),
        // and exact+zero-copy hashed differently run to run on the test device (open finding).
        frame_zc = (g_lite_npu_loopback || !(flags & VELA_NPU_F_ASYNC)) ? -1 : cur_zc;
        if (g_lite_npu_loopback) {
            for (int p = 3; p <= 9; ++p) run(cb, pipelines[p], p, "net");
            run_npu(cb, p_netref, kPackGroups, "netref");
            fullb(cb);
            vkCmdFillBuffer(cb, scratch.buffer, 16684928, kScratchBytes - 16684928, 0);   // unpack must deliver it again
        } else {
            if (frame_zc >= 0 && fu_pre_frame) { /* Written by the prepass */ }
            else if (frame_zc >= 0) run_zc(cb, p_pack_zc, kPackGroups, "pack", frame_zc);
            else run_npu(cb, p_pack, kPackGroups, "pack");
        }
        fullb(cb);
        write_marker(cb, si, 0, VELA_NPU_OP_UPSCALE, seq, flags, c);
    }
    void run_npu(VkCommandBuffer cb, VkPipeline p, std::uint32_t groups, const char* name, const void* push = nullptr) {
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const bool prof = g_lite_profile_pool && g_lite_profile_used < 32;
        if (prof) { g_lite_profile_names[g_lite_profile_used / 2] = name; vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++); }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, npu_layout, 0, 1, &npu_set, 0, nullptr);
        if (push) vkCmdPushConstants(cb, npu_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
        vkCmdDispatch(cb, groups, 1, 1);
        if (prof) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++);
    }
    void select(VkCommandBuffer cb, std::uint32_t kick_word, std::uint32_t wait_word, std::uint64_t seq) {
        const std::uint32_t pc[4] = {kick_word, wait_word, std::uint32_t(seq), std::uint32_t(seq >> 32)};
        run_npu(cb, p_select, 1, "sel", pc);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    // the GPU network (passes 3..9) or the unpack: exactly one of them gets non-zero groups (npu_select.comp)
    void net_or_unpack(VkCommandBuffer cb, int zc) {
        const bool pr = prof_b(cb, "nz");   // the 7 indirect GPU-network dispatches (0 groups on NPU frames) + their barriers
        for (int p = 3; p <= 9; ++p) {
            barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[p]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layouts[p], 0, 1, &slots[cur].sets[p], 0, nullptr);
            vkCmdDispatchIndirect(cb, args_live.buffer, VkDeviceSize(p - 3) * 12);
        }
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        if (pr) prof_e(cb);
        const bool pu = prof_b(cb, "unp");
        if (zc >= 0) { bind_zc(cb, p_unpack_zc, zc); }
        else { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_unpack);
               vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, npu_layout, 0, 1, &npu_set, 0, nullptr); }
        vkCmdDispatchIndirect(cb, args_live.buffer, 7 * 12);
        if (pu) prof_e(cb);
    }
    // ---- DX (0.8.1): per-section timestamps (bench only: g_lite_profile_pool is null in the game) + zero-copy tensors
    static bool prof_b(VkCommandBuffer cb, const char* n) {
        if (!g_lite_profile_pool || g_lite_profile_used >= 31) return false;
        g_lite_profile_names[g_lite_profile_used / 2] = n;
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++);
        return true;
    }
    static void prof_e(VkCommandBuffer cb) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++); }
    void bind_zc(VkCommandBuffer cb, VkPipeline p, int zc) {
        const VkDescriptorSet sets[2] = {npu_set, zcs[zc].set};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, zc_layout, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cb, zc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, zcs[zc].q);
    }
    void run_zc(VkCommandBuffer cb, VkPipeline p, std::uint32_t groups, const char* name, int zc) {
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const bool pr = prof_b(cb, name);
        bind_zc(cb, p, zc); vkCmdDispatch(cb, groups, 1, 1);
        if (pr) prof_e(cb);
    }
    // a captured-layout pass with set 1 = the dma-buf and the zc push constants (qin, q0, q1)
    void bind_fu(VkCommandBuffer cb, VkPipeline p, VkPipelineLayout l, std::uint32_t pass, int zc) {
        const VkDescriptorSet sets[2] = {slots[cur].sets[pass], zcs[zc].set};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, l, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cb, l, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, zcs[zc].q);
    }
    void run_fu(VkCommandBuffer cb, VkPipeline p, VkPipelineLayout l, std::uint32_t pass, int zc, const char* name, const void*) {
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        const bool pr = prof_b(cb, name);
        bind_fu(cb, p, l, pass, zc);
        const auto& g = fsr4lite::kPasses[pass].groups; vkCmdDispatch(cb, g[0], g[1], g[2]);
        if (pr) prof_e(cb);
    }
    void net_k1_fu(VkCommandBuffer cb, int zc) {
        const bool pr = prof_b(cb, "nz");
        for (int p = 3; p <= 9; ++p) {
            barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[p]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layouts[p], 0, 1, &slots[cur].sets[p], 0, nullptr);
            vkCmdDispatchIndirect(cb, args_live.buffer, VkDeviceSize(p - 3) * 12);
        }
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        if (pr) prof_e(cb);
        const bool pk = prof_b(cb, "k1");   // exactly one of the two K1 dispatches has groups (npu_select_fu.comp)
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[10]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layouts[10], 0, 1, &slots[cur].sets[10], 0, nullptr);
        vkCmdDispatchIndirect(cb, args_live.buffer, 25 * 4);
        bind_fu(cb, p_fu_k1, fu_k1_layout, 10, zc);
        vkCmdDispatchIndirect(cb, args_live.buffer, 28 * 4);
        if (pk) prof_e(cb);
    }
    // ack.reserved: [0..3] caps, [8..15] VkBuffer, [16..19] gen, [20..31] in/out0/out1 in 16-byte units (vela_npu_ext.h).
    // A newer gen (reconnect) gets a new descriptor set; older sets stay valid (the layer keeps retired buffers alive).
    void adopt_zc(const std::uint8_t* r) {
        static const bool off = [] { const char* v = std::getenv("VELA_NPU_ZEROCOPY"); return v && v[0] == '0'; }();
        if (off || g_lite_npu_loopback || !zc_layout) return;
        Zc z{}; std::memcpy(&z.buf, r + 8, 8); std::memcpy(&z.gen, r + 16, 4); std::memcpy(z.q, r + 20, 12);
        if (!z.buf || (cur_zc >= 0 && z.gen <= zcs[cur_zc].gen)) return;
        if (nzc >= kMaxZc) { cur_zc = -1; return; }   // too many reconnects: stay on the copy path
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sa.descriptorPool = zc_pool; sa.descriptorSetCount = 1; sa.pSetLayouts = &zc_set_layout;
        if (vkAllocateDescriptorSets(device, &sa, &z.set) != VK_SUCCESS) { cur_zc = -1; return; }
        VkDescriptorBufferInfo bi{}; std::memcpy(&bi.buffer, &z.buf, sizeof bi.buffer); bi.offset = 0; bi.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = z.set; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        zcs[nzc] = z; cur_zc = nzc++;
        lines.emplace_back("Vela NPU: zero-copy tensors");
    }
    void copy_image(VkCommandBuffer cb, VkImage src, VkImage dst, std::uint32_t w, std::uint32_t h) {   // both GENERAL
        VkImageCopy r{}; r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.extent = {w, h, 1};
        vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        fullb(cb);
    }
    void layout_barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // async: K1/K2 of frame N run in the next call, after the game may have reused its colour image -> keep a copy
    void copy_color(VkCommandBuffer cb, const Image& color) {
        fullb(cb);
        const bool pr = prof_b(cb, "cpy");
        layout_barrier(cb, color.image, input_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageCopy r{}; r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.extent = {rw_, rh_, 1};
        vkCmdCopyImage(cb, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        layout_barrier(cb, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input_layout);
        if (pr) prof_e(cb);
    }
    void ensure_async_images(VkCommandBuffer cb, const Image& color, const Image& output) {
        auto mk = [&](OwnedImage& im, VkFormat& have, VkFormat f, std::uint32_t w, std::uint32_t h) {
            if (im.image && have == f) return;
            if (im.image) { vkDestroyImageView(device, im.view, nullptr); vkDestroyImage(device, im.image, nullptr); vkFreeMemory(device, im.memory, nullptr); im = {}; }
            im = make_image(f, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            have = f; layout_barrier(cb, im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        };
        mk(color_copy, color_copy_format, color.format, rw_, rh_);
        mk(keep, keep_format, output.format, kOutW, kOutH);
    }
    void poll_markers() {
        for (std::uint32_t i = 0; i < kSlots; ++i)
            if (slots[i].used && slots[i].marker_mask && vkGetEventStatus(device, slots[i].done) == VK_EVENT_SET) read_marker(i);
        if (nstats.layer == 1 && line_frames >= 600) {
            char b[160]; std::snprintf(b, sizeof b, "Vela NPU: %llu frames, HTP %.2f ms, fallbacks %llu", (unsigned long long)line_frames,
                                       line_frames ? line_htp / double(line_frames) : 0.0, (unsigned long long)line_fallbacks);
            lines.emplace_back(b); line_frames = 0; line_htp = 0; line_fallbacks = 0;
        }
    }
    static const char* reason(std::uint32_t st) {
        return st == VELA_NPU_ACK_NODAEMON ? "nodaemon" : st == VELA_NPU_ACK_BADARGS ? "badargs" : st == VELA_NPU_ACK_BUSY ? "busy" : "error";
    }
    // F1/F2 on every change of state; key 0 = on, else the reason's key (printed once per change)
    void state_line(std::uint32_t key, const char* why = nullptr) {
        if (key == last_line_state) return;
        last_line_state = key;
        if (key == 0) lines.emplace_back(npu == 2 ? "Vela NPU: on, async" : "Vela NPU: on, exact");
        else lines.emplace_back(std::string("Vela NPU: fallback ") + why + ", GPU path");
    }
    void read_marker(std::uint32_t si) {
        Slot& s = slots[si];
        for (std::uint32_t k = 0; k < 2; ++k) {
            if (!(s.marker_mask >> k & 1u)) continue;
            const std::uint8_t* m = marker.map + VkDeviceSize(word_of(si, k)) * 4;
            std::uint64_t magic; std::memcpy(&magic, m, 8);
            if (magic == VELA_NPU_MAGIC_CMD) {   // payload came back unchanged: no layer below us, for this process
                ++nstats.acks_absent; nstats.layer = -1; state_line(100, "nolayer");
            } else if (magic == VELA_NPU_MAGIC_ACK) {
                VelaNpuAck ack; std::memcpy(&ack, m, sizeof ack);
                std::uint32_t caps = 0, htp_us = 0; std::memcpy(&caps, ack.reserved, 4); std::memcpy(&htp_us, ack.reserved + 4, 4);
                nstats.last_status = ack.status; nstats.layer_build = ack.layerBuild; nstats.caps = caps;
                if (caps & VELA_NPU_CAP_ZEROCOPY) adopt_zc(ack.reserved);
                if (ack.seq != s.marker_seq[k]) { ++nstats.acks_fail; continue; }
                if (ack.status == VELA_NPU_ACK_OK) {
                    ++nstats.acks_ok; fail_streak = 0;
                    if (nstats.layer != -1) { nstats.layer = 1; state_line(0); }
                    if (k == 0 || (caps & VELA_NPU_CAP_ASYNC)) { ++line_frames; line_htp += htp_us / 1000.0; }
                    if (htp_us) { ++nstats.htp_n; nstats.htp_ms += htp_us / 1000.0; }   // Bench readout
                } else {
                    ++nstats.acks_fail; ++line_fallbacks;
                    if (nstats.layer != -1) { nstats.layer = 0; state_line(1 + ack.status, reason(ack.status)); }
                    if (++fail_streak >= 8) { nstats.layer = -2; npu_backoff = 240; fail_streak = 0; }   // probe again in 240 frames
                }
            } else { ++nstats.acks_fail; if (nstats.layer == 1) { nstats.layer = 0; state_line(101, "timeout"); } }
        }
        s.marker_mask = 0;
    }
    void build_npu() {
        if (!npu) return;
        const VkImageUsageFlags tu = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        tin = make_image(VK_FORMAT_R32_UINT, kOutW, kOutH, tu); out0 = make_image(VK_FORMAT_R32_UINT, kOutW, kOutH, tu);
        out1 = make_image(VK_FORMAT_R32_UINT, kOutW, kOutH, tu);
        marker = make_buffer(VkDeviceSize(kSlots) * 2 * 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        std::memset(marker.map, 0, std::size_t(kSlots) * 2 * 256);
        args_full = make_buffer(128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        args_live = make_buffer(128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
        std::uint32_t* f = reinterpret_cast<std::uint32_t*>(args_full.map); std::memset(f, 0, 128);
        for (int p = 3; p <= 9; ++p) for (int k = 0; k < 3; ++k) f[(p - 3) * 3 + k] = fsr4lite::kPasses[p].groups[k];
        f[21] = kUnpackGroups; f[22] = 1; f[23] = 1;
        for (int k = 0; k < 3; ++k) f[25 + k] = f[28 + k] = fsr4lite::kPasses[10].groups[k];   // K1 GPU / K1 fused
        VkDescriptorSetLayoutBinding bs[7]{};
        const VkDescriptorType ty[7] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (std::uint32_t i = 0; i < 7; ++i) { bs[i].binding = i; bs[i].descriptorType = ty[i]; bs[i].descriptorCount = 1; bs[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lc.bindingCount = 7; lc.pBindings = bs;
        check(vkCreateDescriptorSetLayout(device, &lc, nullptr, &npu_set_layout), "NPU set layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &npu_set_layout;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &npu_layout), "NPU pipeline layout");
        const VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 1; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
        check(vkCreateDescriptorPool(device, &dp, nullptr, &npu_pool), "NPU descriptor pool");
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sa.descriptorPool = npu_pool; sa.descriptorSetCount = 1; sa.pSetLayouts = &npu_set_layout;
        check(vkAllocateDescriptorSets(device, &sa, &npu_set), "NPU descriptor set");
        VkDescriptorBufferInfo bi[4] = {{scratch.buffer, 0, VK_WHOLE_SIZE}, {marker.buffer, 0, VK_WHOLE_SIZE}, {args_full.buffer, 0, VK_WHOLE_SIZE}, {args_live.buffer, 0, VK_WHOLE_SIZE}};
        VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, tin.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, out0.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, out1.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[7]{};
        for (std::uint32_t i = 0; i < 7; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = npu_set; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = ty[i]; }
        w[0].pBufferInfo = &bi[0]; w[1].pImageInfo = &ii[0]; w[2].pImageInfo = &ii[1]; w[3].pImageInfo = &ii[2];
        w[4].pBufferInfo = &bi[1]; w[5].pBufferInfo = &bi[2]; w[6].pBufferInfo = &bi[3];
        vkUpdateDescriptorSets(device, 7, w, 0, nullptr);
        auto mkp = [&](const std::uint32_t* code, std::size_t bytes) {
            VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sc.codeSize = bytes; sc.pCode = code;
            VkShaderModule module = VK_NULL_HANDLE; check(vkCreateShaderModule(device, &sc, nullptr, &module), "NPU shader module");
            VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pc.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pc.stage.module = module; pc.stage.pName = "main"; pc.layout = npu_layout;
            VkPipeline p = VK_NULL_HANDLE; const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &p);
            vkDestroyShaderModule(device, module, nullptr); check(r, "NPU pipeline"); return p;
        };
        p_pack = mkp(fsr4npu_spv::k_npu_pack, sizeof fsr4npu_spv::k_npu_pack);
        p_unpack = mkp(fsr4npu_spv::k_npu_unpack, sizeof fsr4npu_spv::k_npu_unpack);
        p_select = mkp(fsr4fu_spv::k_npu_select_fu, sizeof fsr4fu_spv::k_npu_select_fu);   // Words 0..24 as 0.8.1
        p_netref = mkp(fsr4npu_spv::k_npu_netref, sizeof fsr4npu_spv::k_npu_netref);
        {   // DX: zero-copy pipelines (set 0 = npu_set, set 1 = the layer's dma-buf buffer)
            VkDescriptorSetLayoutBinding zb{}; zb.binding = 0; zb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; zb.descriptorCount = 1; zb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo zl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; zl.bindingCount = 1; zl.pBindings = &zb;
            check(vkCreateDescriptorSetLayout(device, &zl, nullptr, &zc_set_layout), "NPU zc set layout");
            const VkDescriptorSetLayout two[2] = {npu_set_layout, zc_set_layout};
            VkPipelineLayoutCreateInfo zp{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; zp.setLayoutCount = 2; zp.pSetLayouts = two; zp.pushConstantRangeCount = 1; zp.pPushConstantRanges = &pr;
            check(vkCreatePipelineLayout(device, &zp, nullptr, &zc_layout), "NPU zc pipeline layout");
            const VkDescriptorPoolSize zs{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::uint32_t(kMaxZc)};
            VkDescriptorPoolCreateInfo zd{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; zd.maxSets = kMaxZc; zd.poolSizeCount = 1; zd.pPoolSizes = &zs;
            check(vkCreateDescriptorPool(device, &zd, nullptr, &zc_pool), "NPU zc descriptor pool");
            const VkPipelineLayout keep_layout = npu_layout; npu_layout = zc_layout;   // mkp() builds with npu_layout
            p_pack_zc = mkp(fsr4npu_spv::k_npu_pack_zc, sizeof fsr4npu_spv::k_npu_pack_zc);
            p_unpack_zc = mkp(fsr4npu_spv::k_npu_unpack_zc, sizeof fsr4npu_spv::k_npu_unpack_zc);
            npu_layout = keep_layout;
            const VkDescriptorSetLayout pre2[2] = {set_layouts[1], zc_set_layout}, k12[2] = {set_layouts[10], zc_set_layout};
            zp.pSetLayouts = pre2; check(vkCreatePipelineLayout(device, &zp, nullptr, &fu_pre_layout), "fused pre layout");
            zp.pSetLayouts = k12; check(vkCreatePipelineLayout(device, &zp, nullptr, &fu_k1_layout), "fused k1 layout");
            p_fu_pre = make_pipeline(fsr4fu_spv::k_fu_pre, sizeof fsr4fu_spv::k_fu_pre / 4, fsr4lite::kPasses[1].entry, fu_pre_layout);
            const char* kv = std::getenv("VELA_FU_K1"); const char m = kv ? kv[0] : 'l';   // K1 variant: l = LDS uvec4 (default), s = LDS SoA, d = direct
            if (m == 'l') p_fu_k1 = make_pipeline(fsr4fu_spv::k_fu_k1, sizeof fsr4fu_spv::k_fu_k1 / 4, fsr4lite::kPasses[10].entry, fu_k1_layout);
            else if (m == 'd') p_fu_k1 = make_pipeline(fsr4fu_spv::k_fu_k1d, sizeof fsr4fu_spv::k_fu_k1d / 4, fsr4lite::kPasses[10].entry, fu_k1_layout);
            else p_fu_k1 = make_pipeline(fsr4fu_spv::k_fu_k1s, sizeof fsr4fu_spv::k_fu_k1s / 4, fsr4lite::kPasses[10].entry, fu_k1_layout);
        }
    }
    void release_npu() {
        for (auto* p : {&p_fu_pre, &p_fu_k1}) { if (*p) vkDestroyPipeline(device, *p, nullptr); *p = VK_NULL_HANDLE; }
        for (auto* l : {&fu_pre_layout, &fu_k1_layout}) { if (*l) vkDestroyPipelineLayout(device, *l, nullptr); *l = VK_NULL_HANDLE; }
        for (auto* p : {&p_pack, &p_unpack, &p_select, &p_netref, &p_pack_zc, &p_unpack_zc}) { if (*p) vkDestroyPipeline(device, *p, nullptr); *p = VK_NULL_HANDLE; }
        if (zc_pool) vkDestroyDescriptorPool(device, zc_pool, nullptr); zc_pool = VK_NULL_HANDLE; nzc = 0; cur_zc = frame_zc = pending_zc = -1;
        if (zc_layout) vkDestroyPipelineLayout(device, zc_layout, nullptr); zc_layout = VK_NULL_HANDLE;
        if (zc_set_layout) vkDestroyDescriptorSetLayout(device, zc_set_layout, nullptr); zc_set_layout = VK_NULL_HANDLE;
        if (npu_pool) vkDestroyDescriptorPool(device, npu_pool, nullptr); npu_pool = VK_NULL_HANDLE;
        if (npu_layout) vkDestroyPipelineLayout(device, npu_layout, nullptr); npu_layout = VK_NULL_HANDLE;
        if (npu_set_layout) vkDestroyDescriptorSetLayout(device, npu_set_layout, nullptr); npu_set_layout = VK_NULL_HANDLE;
        for (auto* b : {&marker, &args_full, &args_live}) {
            if (b->buffer) vkDestroyBuffer(device, b->buffer, nullptr);
            if (b->memory) vkFreeMemory(device, b->memory, nullptr);
            *b = {};
        }
        for (auto* im : {&tin, &out0, &out1, &color_copy, &keep}) { if (im->view) vkDestroyImageView(device, im->view, nullptr); if (im->image) vkDestroyImage(device, im->image, nullptr);
                                  if (im->memory) vkFreeMemory(device, im->memory, nullptr); *im = {}; }
    }

    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags want) const {
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & want) == want) return i;
        throw std::runtime_error("Lite: no memory type");
    }
    OwnedBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        OwnedBuffer b;
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = size; bc.usage = usage;
        check(vkCreateBuffer(device, &bc, nullptr, &b.buffer), "Lite buffer");
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size;
        ma.memoryTypeIndex = memory_type(mr.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &ma, nullptr, &b.memory), "Lite buffer memory");
        owned += mr.size;
        check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "Lite bind buffer");
        if (host) { void* p = nullptr; check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &p), "Lite map"); b.map = static_cast<std::uint8_t*>(p); }
        return b;
    }
    OwnedImage make_image(VkFormat format, std::uint32_t w, std::uint32_t h,
                          VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        OwnedImage im;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = format; ic.extent = {w, h, 1};
        ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = usage; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device, &ic, nullptr, &im.image), "Lite image");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, im.image, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size;
        ma.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &ma, nullptr, &im.memory), "Lite image memory");
        owned += mr.size;
        check(vkBindImageMemory(device, im.image, im.memory, 0), "Lite bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &im.view), "Lite image view");
        return im;
    }
    VkPipeline make_pipeline(const std::uint32_t* code, std::size_t words, const char* entry, VkPipelineLayout layout) {
        VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sc.codeSize = words * 4; sc.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE; check(vkCreateShaderModule(device, &sc, nullptr, &module), "Lite shader module");
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        rs.requiredSubgroupSize = 32;
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pc.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pc.stage.pNext = sg32 ? &rs : nullptr;
        pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pc.stage.module = module; pc.stage.pName = entry; pc.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &p);
        vkDestroyShaderModule(device, module, nullptr);
        check(r, (std::string("Lite pipeline ") + entry).c_str());
        return p;
    }
    void build() {
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sc.maxLod = VK_LOD_CLAMP_NONE;
        check(vkCreateSampler(device, &sc, nullptr, &sampler), "Lite sampler");
        for (std::uint32_t i = 0; i < kPasses; ++i) {
            const auto& pass = fsr4lite::kPasses[i];
            std::vector<VkDescriptorSetLayoutBinding> bs;
            for (std::uint32_t k = 0; k < pass.binding_count; ++k) {
                VkDescriptorSetLayoutBinding b{}; b.binding = pass.bindings[k].binding; b.descriptorType = VkDescriptorType(pass.bindings[k].type);
                b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                if (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) b.pImmutableSamplers = &sampler;
                bs.push_back(b);
            }
            VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lc.bindingCount = std::uint32_t(bs.size()); lc.pBindings = bs.data();
            check(vkCreateDescriptorSetLayout(device, &lc, nullptr, &set_layouts[i]), "Lite set layout");
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &set_layouts[i];
            check(vkCreatePipelineLayout(device, &pl, nullptr, &layouts[i]), "Lite pipeline layout");
            pipelines[i] = make_pipeline(pass.spv, pass.spv_words, pass.entry, layouts[i]);
        }
        k2 = make_pipeline(fsr4lite::kSpv13, sizeof(fsr4lite::kSpv13) / 4, fsr4lite::kPasses[10].entry, layouts[10]);
        pre_skip = make_pipeline(fsr4lite::kSpvPreR2c, sizeof(fsr4lite::kSpvPreR2c) / 4, fsr4lite::kPasses[1].entry, layouts[1]);
        images[kHistory] = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kOutW, kOutH);
        images[kReprojected] = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kOutW, kOutH);
        images[kRecurrent] = make_image(VK_FORMAT_R8G8B8A8_UNORM, kOutW, kOutH);
        images[kCounter] = make_image(VK_FORMAT_R32_UINT, 1, 1);
        images[kMip] = make_image(VK_FORMAT_R32_SFLOAT, (rw_ + 15) / 16, (rh_ + 15) / 16);
        images[kExposure] = make_image(VK_FORMAT_R32_SFLOAT, 2, 1);
        sr = every > 1 && sr_enabled();
        if (sr) build_sr();
        scratch = make_buffer(kScratchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        weights = make_buffer(kWeightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        staging = make_buffer(kWeightsBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.map, fsr4lite::kWeights, kWeightsBytes);
        ubo = make_buffer(ubo_stride * kPasses * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        ubo_map = ubo.map;
        std::memset(ubo_map, 0, std::size_t(ubo_stride * kPasses * kSlots));
        for (std::uint32_t s = 0; s < kSlots; ++s)
            for (std::uint32_t i = 2; i < 10; ++i)
                if (fsr4lite::kPasses[i].ubo_size == 144)
                    std::memcpy(ubo_map + (VkDeviceSize(s) * kPasses + i) * ubo_stride, fsr4lite::kPasses[i].constants, 144);
        const VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 * kSlots},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32 * kSlots},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, 16 * kSlots}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = kPasses * kSlots; dp.poolSizeCount = 5; dp.pPoolSizes = ps;
        check(vkCreateDescriptorPool(device, &dp, nullptr, &pool), "Lite descriptor pool");
        build_npu_deferred = true;
        for (auto& s : slots) {
            VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sa.descriptorPool = pool; sa.descriptorSetCount = kPasses; sa.pSetLayouts = set_layouts.data();
            check(vkAllocateDescriptorSets(device, &sa, s.sets.data()), "Lite descriptor sets");
            VkEventCreateInfo ec{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; check(vkCreateEvent(device, &ec, nullptr, &s.done), "Lite event");
        }
    }
    void write_sets(std::uint32_t slot, VkImageView color, VkImageView depth, VkImageView motion, VkImageView output, VkImageView post_color) {
        std::vector<VkWriteDescriptorSet> ws; std::vector<VkDescriptorImageInfo> ii; std::vector<VkDescriptorBufferInfo> bi;
        ii.reserve(64); bi.reserve(64);
        for (std::uint32_t i = 0; i < kPasses; ++i) {
            const auto& pass = fsr4lite::kPasses[i];
            for (std::uint32_t k = 0; k < pass.binding_count; ++k) {
                const auto bn = pass.bindings[k].binding; const auto type = VkDescriptorType(pass.bindings[k].type);
                if (type == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = slots[slot].sets[i]; w.dstBinding = bn; w.descriptorCount = 1; w.descriptorType = type;
                if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    bi.push_back({bn == 21 ? weights.buffer : scratch.buffer, 0, VK_WHOLE_SIZE}); w.pBufferInfo = &bi.back();
                } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    bi.push_back({ubo.buffer, (VkDeviceSize(slot) * kPasses + i) * ubo_stride, pass.ubo_size}); w.pBufferInfo = &bi.back();
                } else {
                    VkImageView v = VK_NULL_HANDLE; bool external = false;
                    if (i == 0) { if (bn == 0) { v = color; external = true; } else if (bn == 128) v = images[kCounter].view;
                                  else if (bn == 129) v = images[kMip].view; else if (bn == 130) v = images[kExposure].view; }
                    else if (i == 1) { switch (bn) { case 0: v = images[sr_h ? kXHist : kHistory].view; break; case 1: v = motion; external = true; break;
                                       case 2: v = depth; external = true; break; case 3: v = color; external = true; break;
                                       case 4: v = images[kRecurrent].view; break; case 17: v = images[kExposure].view; break;
                                       case 131: v = images[kReprojected].view; break; default: break; } }
                    else if (i == 10) { switch (bn) { case 3: v = post_color; external = post_color == color; break; case 9: v = images[kReprojected].view; break;
                                        case 17: v = images[kExposure].view; break; case 129: v = images[kHistory].view; break;
                                        case 130: v = output; break; case 134: v = images[kRecurrent].view; break; default: break; } }
                    if (!v) throw std::logic_error("Lite: unmapped binding " + std::to_string(bn) + " in pass " + std::to_string(i));
                    const bool sampled_external = external && type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    ii.push_back({VK_NULL_HANDLE, v, sampled_external ? input_layout : VK_IMAGE_LAYOUT_GENERAL}); w.pImageInfo = &ii.back();
                }
                ws.push_back(w);
            }
        }
        vkUpdateDescriptorSets(device, std::uint32_t(ws.size()), ws.data(), 0, nullptr);
    }
    void init_resources(VkCommandBuffer cb) {
        std::vector<VkImageMemoryBarrier> ib;
        for (auto& im : images) {
            if (!im.image) continue;   // kXHist exists only with SR4
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ib.push_back(b);
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, std::uint32_t(ib.size()), ib.data());
        const VkClearColorValue zero{}; const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto& im : images) if (im.image) vkCmdClearColorImage(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        vkCmdFillBuffer(cb, scratch.buffer, 0, VK_WHOLE_SIZE, 0);
        if (npu) for (auto* im : {&tin, &out0, &out1}) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = im->image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        const VkBufferCopy copy{0, 0, kWeightsBytes}; vkCmdCopyBuffer(cb, staging.buffer, weights.buffer, 1, &copy);
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    static void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags src_access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = src_access;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier(cb, src, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    void run(VkCommandBuffer cb, VkPipeline p, std::uint32_t pass, const char* name) {
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        const bool prof = g_lite_profile_pool && g_lite_profile_used < 32;
        if (prof) { g_lite_profile_names[g_lite_profile_used / 2] = name; vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++); }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layouts[pass], 0, 1, &slots[cur].sets[pass], 0, nullptr);
        const auto& g = fsr4lite::kPasses[pass].groups;
        if (pass == 0) vkCmdDispatch(cb, (rw_ + 63) / 64, (rh_ + 63) / 64, 1);   // aux (SPD): render-size grid, as AMD's at that size
        else vkCmdDispatch(cb, g[0], g[1], g[2]);
        if (prof) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++);
    }
    // ------------------------------------------------------------------ SR4 skipped frame (R + W), bindings as seqJ.inc kr mode
    struct SrBind { std::uint32_t binding; VkDescriptorType type; };
    static constexpr SrBind kSrR[] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {131, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {141, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        {257, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, {320, VK_DESCRIPTOR_TYPE_SAMPLER}};
    static constexpr SrBind kSrW[] = {{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {134, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {141, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        {257, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}};
    void build_sr() {
        images[kXHist] = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kOutW, kOutH);
        for (int k = 0; k < 2; ++k) {
            std::vector<VkDescriptorSetLayoutBinding> bs;
            const SrBind* sb = k ? kSrW : kSrR; const std::size_t n = k ? std::size(kSrW) : std::size(kSrR);
            for (std::size_t j = 0; j < n; ++j) { VkDescriptorSetLayoutBinding b{}; b.binding = sb[j].binding; b.descriptorType = sb[j].type; b.descriptorCount = 1;
                b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; if (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) b.pImmutableSamplers = &sampler; bs.push_back(b); }
            VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lc.bindingCount = std::uint32_t(bs.size()); lc.pBindings = bs.data();
            check(vkCreateDescriptorSetLayout(device, &lc, nullptr, &sr_set_layout[k]), "SR4 set layout");
            const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &sr_set_layout[k];
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
            check(vkCreatePipelineLayout(device, &pl, nullptr, &sr_layout[k]), "SR4 pipeline layout");
        }
        sr_pipe[0] = make_pipeline(fsr4sr4_spv::k_sr_r4, std::size(fsr4sr4_spv::k_sr_r4), "main", sr_layout[0]);
        sr_pipe[1] = make_pipeline(fsr4sr4_spv::k_sr_w, std::size(fsr4sr4_spv::k_sr_w), "main", sr_layout[1]);
        const VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kSlots},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSlots}, {VK_DESCRIPTOR_TYPE_SAMPLER, kSlots}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 2 * kSlots; dp.poolSizeCount = 5; dp.pPoolSizes = ps;
        check(vkCreateDescriptorPool(device, &dp, nullptr, &sr_pool), "SR4 descriptor pool");
        for (auto& s : sr_sets) { VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sa.descriptorPool = sr_pool; sa.descriptorSetCount = 2;
                                  sa.pSetLayouts = sr_set_layout; check(vkAllocateDescriptorSets(device, &sa, s.data()), "SR4 descriptor sets"); }
    }
    void record_sr(VkCommandBuffer cb, std::uint32_t si, VkImageView color, VkImageView depth, VkImageView motion, VkImageView output) {
        std::vector<VkWriteDescriptorSet> ws; std::vector<VkDescriptorImageInfo> ii; std::vector<VkDescriptorBufferInfo> bi; ii.reserve(16); bi.reserve(4);
        const VkDescriptorBufferInfo ubk{ubo.buffer, (VkDeviceSize(si) * kPasses + 10) * ubo_stride, fsr4lite::kPasses[10].ubo_size};   // postpass constants
        for (int k = 0; k < 2; ++k) {
            const SrBind* sb = k ? kSrW : kSrR; const std::size_t n = k ? std::size(kSrW) : std::size(kSrR);
            for (std::size_t j = 0; j < n; ++j) {
                const auto bn = sb[j].binding; const auto type = sb[j].type; if (type == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = sr_sets[si][k]; w.dstBinding = bn; w.descriptorCount = 1; w.descriptorType = type;
                if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) { bi.push_back({scratch.buffer, 0, VK_WHOLE_SIZE}); w.pBufferInfo = &bi.back(); }
                else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) { bi.push_back(ubk); w.pBufferInfo = &bi.back(); }
                else {
                    VkImageView v = VK_NULL_HANDLE; bool external = false;
                    if (k == 0) { switch (bn) { case 0: v = images[sr_h ? kXHist : kHistory].view; break; case 1: v = motion; external = true; break;
                                                case 2: v = depth; external = true; break; case 3: v = color; external = true; break;
                                                case 17: v = images[kExposure].view; break; case 131: v = images[kReprojected].view; break; default: break; } }
                    else { switch (bn) { case 3: v = color; external = true; break; case 9: v = images[kReprojected].view; break; case 17: v = images[kExposure].view; break;
                                         case 129: v = images[sr_h ? kHistory : kXHist].view; break; case 130: v = output; break;
                                         case 134: v = images[kRecurrent].view; break; default: break; } }
                    if (!v) throw std::logic_error("SR4: unmapped binding " + std::to_string(bn));
                    ii.push_back({VK_NULL_HANDLE, v, external && type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? input_layout : VK_IMAGE_LAYOUT_GENERAL}); w.pImageInfo = &ii.back();
                }
                ws.push_back(w);
            }
        }
        vkUpdateDescriptorSets(device, std::uint32_t(ws.size()), ws.data(), 0, nullptr);
        static constexpr std::uint32_t kPlane[2] = {0u, 8388608u / 4u};   // scratch words: K1 feature plane, ping-pong plane in free scratch
        const std::uint32_t pr[4] = {kPlane[sr_cp], kPlane[sr_cp ^ 1u], 0u, 0u}, pw[4] = {kPlane[sr_cp ^ 1u], 0u, 0u, 0u};
        const auto& g = fsr4lite::kPasses[10].groups;
        run_sr(cb, 0, si, pr, (kRenderW + 7) / 8, (kRenderH + 7) / 8, 1, "pre");
        run_sr(cb, 1, si, pw, g[0], g[1], g[2], "k2");
        sr_h ^= 1u; sr_cp ^= 1u;
    }
    void run_sr(VkCommandBuffer cb, int k, std::uint32_t si, const std::uint32_t* push, std::uint32_t gx, std::uint32_t gy, std::uint32_t gz, const char* name) {
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        const bool prof = g_lite_profile_pool && g_lite_profile_used < 32;
        if (prof) { g_lite_profile_names[g_lite_profile_used / 2] = name; vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++); }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, sr_pipe[k]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, sr_layout[k], 0, 1, &sr_sets[si][k], 0, nullptr);
        vkCmdPushConstants(cb, sr_layout[k], VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
        vkCmdDispatch(cb, gx, gy, gz);
        if (prof) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_lite_profile_pool, g_lite_profile_used++);
    }
    void release() {
        release_npu();
        for (auto& s : slots) { for (auto v : s.views) vkDestroyImageView(device, v, nullptr); s.views.clear(); if (s.done) vkDestroyEvent(device, s.done, nullptr); s.done = VK_NULL_HANDLE; }
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE;
        for (auto* b : {&scratch, &weights, &staging, &ubo}) {
            if (b->buffer) vkDestroyBuffer(device, b->buffer, nullptr);
            if (b->memory) vkFreeMemory(device, b->memory, nullptr);
            *b = {};
        }
        for (auto& im : images) { if (im.view) vkDestroyImageView(device, im.view, nullptr); if (im.image) vkDestroyImage(device, im.image, nullptr);
                                  if (im.memory) vkFreeMemory(device, im.memory, nullptr); im = {}; }
        for (auto* p : {&k2, &pre_skip}) { if (*p) vkDestroyPipeline(device, *p, nullptr); *p = VK_NULL_HANDLE; }
        for (int k = 0; k < 2; ++k) { if (sr_pipe[k]) vkDestroyPipeline(device, sr_pipe[k], nullptr); if (sr_layout[k]) vkDestroyPipelineLayout(device, sr_layout[k], nullptr);
                                      if (sr_set_layout[k]) vkDestroyDescriptorSetLayout(device, sr_set_layout[k], nullptr);
                                      sr_pipe[k] = VK_NULL_HANDLE; sr_layout[k] = VK_NULL_HANDLE; sr_set_layout[k] = VK_NULL_HANDLE; }
        if (sr_pool) vkDestroyDescriptorPool(device, sr_pool, nullptr); sr_pool = VK_NULL_HANDLE;
        for (auto& p : pipelines) { if (p) vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; }
        for (auto& l : layouts) { if (l) vkDestroyPipelineLayout(device, l, nullptr); l = VK_NULL_HANDLE; }
        for (auto& l : set_layouts) { if (l) vkDestroyDescriptorSetLayout(device, l, nullptr); l = VK_NULL_HANDLE; }
        if (sampler) vkDestroySampler(device, sampler, nullptr); sampler = VK_NULL_HANDLE;
    }
};

}  // namespace fsr4core
