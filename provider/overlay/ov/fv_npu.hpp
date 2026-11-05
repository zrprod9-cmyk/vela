// More output sizes on the HTP: kSizes lists output sizes with their own graphs, on top of 1080p / 900p / 720p, under the 720p path's conditions (k1h, fused pre,
// input ping-pong, 1080p model capacity, which is every maxUpscaleSize <= 1920x1080). Each x tensor keeps the fused pre's literal 960-px rows, each k1 is dense at K2's
// stride value[4].x / 2. A size or network change re-probes, as a 720p/900p change does. VELA_NPU_SIZES=0 turns these sizes off.
// Vela 0.8.10: the state line names the schedule really run ("on, exact" / "on, async", say_on, exact_run set in plan()).
// Vela 0.8.10: exact NPU mode (default on; live key npuexact=0|1 > env VELA_NPU_EXACT > 1). The async k1h path applies the network
// of frame N-1 to frame N (K2(N) reads k1(N-1)): fine detail is filtered with the previous frame's per-pixel kernels (tested: true vs
// FV_DELAY=1). Exact mode runs the whole post of frame N-1 in frame N instead: TAIL(N-1) = WAIT(HTP(N-1)) + select + [GPU network on
// X0(N-1) on fallback] + border_12/K1 (fallback) + K2(N-1) on a copy of colour(N-1) with frame N-1's constants, written to frame N's
// output; then HEAD(N) = pre(N) + border_0 + colour copy + the HTP(N) kick. Every output frame is true FSR 4.1.1, one frame late
// (+1 frame latency; the game's HUD/post of frame N is drawn over the image of N-1). Entering: one GPU frame (E) whose output is kept
// and shown again by the first HEAD frame (D). Leaving / size or colour change: the pending TAIL is dropped (its layout transitions kept).
// Only k1h + fused pre + input ping-pong + NMS inputs (the HTP sizes 1080p/900p/720p). plan(): 0 FULL, 1 TAIL+HEAD, 2 drop+FULL, 3 dup+HEAD, 4 E.
// Vela 0.8.9: indirect triples on a 16-byte stride (kStride words; select push word n = count | stride<<16). Turnip takes a
// slow path for an indirect dispatch whose offset is not 16-byte aligned (tu_emit_compute_driver_params: WFI, 3 x CP_MEM_TO_MEM,
// WAIT_MEM_WRITES, CACHE_INVALIDATE, WFI), which 3 of 4 of the 12-byte triples hit. E4_ALIGN=0 (test only) = the 0.8.8 12-byte layout.
// Vela 0.8.9: 1600x900 output with capacity 1920x1080 (network grid 800x450, tensors padded to 800x452 by 4.1.1's
// round_up(height, 8)) on the HTP, k1h mode only: graph nf4_k1h_450 (FV_GRAPH_K1H_450), daemon fsr4npud_nf4_k1h_450. x uint8
// 1x450x480x32 (rows of 960 px, the fused pre's literal stride; the pre writes grid rows < 450 only), the graph slices cols 0..799 and
// pads 2 zero rows itself; k1 dense 1x450x400x32 (K2 stride width/2 = 800). Same conditions as 720p (fused pre + input ping-pong + k1h),
// else the GPU path. VELA_NPU_900=0 turns it off. 1920x1080 and 1280x720 are unchanged.
// Vela 0.8.7: 1280x720 output with capacity 1920x1080 (render 640x360, network grid 640x360) runs on the HTP too, k1h mode
// only: graph nf4_k1h_360 (FV_GRAPH_K1H_360), daemon fsr4npud_nf4_k1h_360. The fused pre (pass-01 f1g/d) stores the HTP input with a
// LITERAL 960-pixel row stride (its index = scratch index - 8R - 3844, from the literal 962 scratch stride), so the 360 graph takes
// x uint8 1x360x480x32 (rows of 960 px, columns 640..959 unused) and slices columns 0..639 itself; its output k1 is dense 1x360x320x32,
// the stride K2 (k2lu) reads (constants value[4].x / 2 = 640). Needs the fused pre + input ping-pong (no pack at 720p) and k1h; else
// the GPU path ("fallback size"). 1920x1080 is unchanged. VELA_NPU_720=0 turns the 720p path off.
// FV_DUMP=dir FV_DUMP_FRAMES=a,b (test only): X0 after border_0, OUT after model_12 (scratch region 0) and K1 (InterBuf) of those frames.
// Vela 0.8.6: NPU start = where the HTP(N) kick (UPSCALE marker, layer E1) sits in frame N. early = 0.8.5: before model_1, the
// HTP runs during model..post(N) and its DDR traffic slows K2 (measured 1.53 ms vs 1.30 ms). late (default): after post(N) (recorder
// after_post), the HTP runs during whatever follows the FSR dispatch (in game: the rest of the frame and the next one). Slots, seq,
// WAIT(N-1) and the K2a/K2b select are the same in both: HTP(N) reads input slot S&1 (written by pre(N), before either kick point)
// and writes output slot S&1, K2a(N) reads the slot of N-1 only when WAIT(N-1) is OK, K1/K2b fall back on the scratch InterBuf.
// Setting: live key npukick=early|late in Z:/tmp/vela-live/<appid>.ini (the plugin's live file, re-read every 120 frames, applied
// at a begin_frame) > env VELA_NPU_KICK=early|late|0|1 > late. FVSTATS adds kick (last mode), kicks early/late, packs.
// Vela 0.8.5: k1h mode (bundle with K2P k1x/k2lu, layer CAP_OUTPINGPONG): graph nf4_k1h exports K1's result k1 (uint8 = int8^0x80,
// [y*960+x] uvec4) instead of OUT. HTP(N) writes output slot S&1 (S = its UPSCALE seq); post K2(N) reads the slot of N-1 directly
// (push words 4..5 = that slot's address); the unpack is gone, border_12 and K1 are indirect: 0 groups when WAIT(N-1) is OK, else the
// GPU network + border_12 + K1 (k1x, its xor store into the same slot) = the fallback in the same command buffer. Without OUTPINGPONG
// or the nf4_k1h daemon: the 0.8.4 network-graph path (re-probe after 300 frames). FV_TEST_FALLBACK=1 (test only): select as if WAIT failed.
// Vela 0.8.3: the full FSR 4.1.1 network (passes 2..24 = model_1..12) on the Hexagon NPU, graph nf4_exact
// (byte-exact), through VK_LAYER_VELA_npu (vela_npu_abi.h v1 + vela_npu_ext.h, layer build 5). Port of an earlier test bridge
// (a direct fsr4npud client with its own dma-buf import) to the layer handshake; async mode only.
// Frame N, recorder hook before pass 2 (pre + border_0 have written X0(N) into scratch region 0):
//   WAIT(seq N-1) marker (layer: GPU waits E2 of HTP(N-1), the final status lands in the ack on the GPU timeline)
//   -> select (GPU: ack OK -> unpack 60x34 groups, passes 2..24 0 groups; else unpack 0, passes 2..24 their groups = the GPU
//      network on X0(N), the fallback) -> pack X0(N) into the layer's dma-buf input -> unpack OUT(N-1) from the dma-buf output into
//   region 0 (indirect) -> UPSCALE(seq N, TENSORS|ASYNC|ZEROCOPY|GRAPH): E1 = HTP(N) kick -> passes 2..24 (indirect) -> border_12 + post.
//   HTP(N) overlaps post(N) and pre(N+1). Output = the GPU-only schedule with the network one frame late (FV_DELAY=1 reference).
// Host: acks are read NR-2 frames later (PROBE: layer present, CAP_ZEROCOPY + CAP_BDA, gen, offsets; WAIT: final status, HTP us);
// "Vela NPU: ..." lines for the panel via take_lines().
// Scratch layout: X0 and OUT share region 0, uvec4 per padded pixel (962 x 542, halo 1); HTP tensors uint8 NHWC 960x540x16
// = int8 ^ 0x80. Only for output 1920x1080 with capacity 1920x1080 (render 960x540; else the GPU path, "fallback size").
// Env (test only): FV_NPU=2 without a provider request; FV_DELAY=1 GPU-only async-schedule reference; FV_GRAPH (nf4_exact).
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "vela_npu_abi.h"
#include "vela_npu_ext.h"
#include "fv_spv.hpp"
namespace fv {
constexpr uint32_t kRegionVec = 962u * 542u;
constexpr uint32_t NA = 27;   // [24] border_12, [25] post K1 (k1h mode); [26] K2b (k1h fallback: K2 on the scratch InterBuf)
    // indirect triples: [0] unpack, [j] pass j+1 (j = 1..23 -> passes 2..24)
constexpr uint32_t NR = 8;     // marker ring (frames); acks read NR-2 frames later
constexpr VkDeviceSize kLive = 512;   // args buffer: full (host) at 0, live (select) at 512
inline std::atomic<uint32_t> g_request{0};   // provider (VelaNpuDesc for 4.1.1): 0 off, else async
// AMD's 4.1.1 1080p presets native/quality/balanced/performance/drs ship byte-identical network shaders and initializers.bin
// (the graph's weights); only ultraperf (selector: 1920/render width >= 2.99f, i.e. width <= 642) has its own. The network grid
// is the output's (960x540 for 1920x1080) at every render size. The provider sets these per dispatch (render size, preset != ultraperf).
inline std::atomic<uint32_t> g_render_w{0}, g_render_h{0};
inline std::atomic<bool> g_render_ok{true};
// Vela 0.8.14: the dispatch runs the Ultra Performance network (preset ultraperf, own weights): its own graphs nf4up_k1h*
inline std::atomic<bool> g_net_up{false};
inline int env_i(const char* n, int d) { const char* v = std::getenv(n); return v && *v ? std::atoi(v) : d; }
// NPU start. 1 late, 0 early, -1 not set
inline int kick_word(const char* v) {
    if (!v) return -1; while (*v == ' ') ++v;
    const bool one = v[0] && (v[1] == 0 || v[1] == '\r' || v[1] == '\n' || v[1] == ' ');
    if (!std::strncmp(v, "late", 4) || (v[0] == '1' && one)) return 1;
    if (!std::strncmp(v, "early", 5) || (v[0] == '0' && one)) return 0;
    return -1;
}
inline const std::string& live_path() {   // the fork's VelaLive.h Path(): the same file, the same appid order
    static const std::string p = [] { for (const char* k : {"SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID"})
        if (const char* e = std::getenv(k); e && e[0] >= '0' && e[0] <= '9') return std::string("Z:\\tmp\\vela-live\\") + e + ".ini";
        return std::string(); }();
    return p;
}
inline int live_kick() {
    const std::string& p = live_path(); if (p.empty()) return -1;
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return -1;
    char l[160]; int r = -1; while (std::fgets(l, sizeof l, f)) if (!std::strncmp(l, "npukick=", 8)) r = kick_word(l + 8);
    std::fclose(f); return r;
}
inline int live_int(const char* key) {   // "key=" (with '=') -> its integer, -1 not set
    const std::string& p = live_path(); if (p.empty()) return -1;
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return -1;
    const size_t n = std::strlen(key); char l[160]; int r = -1;
    while (std::fgets(l, sizeof l, f)) if (!std::strncmp(l, key, n) && l[n] >= '0' && l[n] <= '9') r = std::atoi(l + n);
    std::fclose(f); return r;
}
// Vela 0.8.16 "FSR 4 Fast": one product switch. Each FSR 4 context sets g_fast at create from VELA_FAST=1 or the live file's
// model=fsr4fast (the panel's "FSR 4 Fast"; the fork / RE4 interceptor treat fsr4fast as FSR 4 and re-create the context on a change).
inline std::atomic<int> g_fast{0};
// VELA_FAST_UP15=1|2 (with Fast): 1080p UP served by the P network at 720p + fused 1.5x K2. g_u15 = mode of the core being created
inline std::atomic<int> g_u15{0};
// u15_setting() is defined after fast_setting() (live key fastup15=, fused modes = legacy Fast only)
inline bool live_fast() {
    const std::string& p = live_path(); if (p.empty()) return false;
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
    char l[160]; bool r = false; while (std::fgets(l, sizeof l, f)) if (!std::strncmp(l, "model=", 6)) r = !std::strncmp(l + 6, "fsr4fast", 8) && (l[14] == 0 || l[14] == '\r' || l[14] == '\n' || l[14] == ' ');
    std::fclose(f); return r;
}
// 1 = Fast (q1mkb: per-pixel recurrent taps, full-res recurrent, shipped K2; static-edge shimmer fixed), 2 = the previous
// Fast (q3kb + K2 rha + half-size recurrent: -0.04..-0.1 ms but static edges shimmer 0.57 vs exact 0.12 sRGB levels)
inline int fast_setting() { const char* e = std::getenv("VELA_FAST"); if (e && *e) return e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0; return live_fast() ? 1 : 0; }
// "FSR 4 Fast UP15": VELA_FAST_UP15=1..4 (experimental; 3 = P720 + separate cubic 1.5x, 4 = bilinear, 1 / 2 = fused K2) wins;
// Without the env mode 3 is the DEFAULT of Fast; the live file's hidden key fastup15=0 turns it
// off (plain Fast UP); VELA_FAST_UP15=0 (or any non-empty value outside 1..4) = off too. Not in the panel. 1 / 2 need K2 rha (legacy Fast 2).
inline int u15_setting() { const char* e = std::getenv("VELA_FAST_UP15"); int m = (e && e[0] >= '1' && e[0] <= '4') ? e[0] - '0' : 0;
    if (!e || !*e) m = live_int("fastup15=") == 0 ? 0 : 3;
    if ((m == 1 || m == 2) && fast_setting() != 2) m = 0;
    return m; }
inline int fp_bits() { return g_fast.load() ? 7 : env_i("VELA_FP", 0); }   // VELA_FP bits: 1 slim, 2 history 10 bit, 4 reprojected 10 bit
inline int fx_bits() { const int f = g_fast.load(); return f == 2 ? 2 : f ? 0 : env_i("VELA_FX", 0); }   // VELA_FX bit 2: half-size recurrent (legacy Fast only)
inline bool exact_setting() { const int lv = live_int("npuexact="); if (lv >= 0) return lv != 0; return env_i("VELA_NPU_EXACT", 1) != 0; }
inline bool kick_setting() { const int lv = live_kick(); if (lv >= 0) return lv == 1; return kick_word(std::getenv("VELA_NPU_KICK")) != 0; }
inline void chk(VkResult r, const char* w) { if (r != VK_SUCCESS) throw std::runtime_error(std::string("FV: ") + w + " " + std::to_string(int(r))); }

struct SizeDim { uint32_t w, h; };
inline constexpr SizeDim kSizes[] = {{1600, 904}, {1440, 810}, {1280, 800}, {1152, 648}, {960, 540}};
inline const std::string kSizeGraph[][2] = {{"nf4_k1h_452", "nf4up_k1h_452"}, {"nf4_k1h_405", "nf4up_k1h_405"},
                                            {"nf4_k1h_400", "nf4up_k1h_400"}, {"nf4_k1h_324", "nf4up_k1h_324"}, {"nf4_k1h_270", "nf4up_k1h_270"}};
inline int size_index(uint32_t w, uint32_t h) { int i = 0; for (const SizeDim& d : kSizes) { if (d.w == w && d.h == h) return i; ++i; } return -1; }

struct Bridge {
    const uint32_t kStride = env_i("E4_ALIGN", 1) ? 4u : 3u;   // Words per indirect triple (4 = 16 B aligned)
    VkPhysicalDevice pd{}; VkDevice dev{}; int mode = 0; bool delay = false; std::string graph;
    VkShaderModule sx{}, ss{}; VkPipelineLayout pl{}; VkPipeline px{}, ps{};
    VkBuffer mb{}; VkDeviceMemory mm{}; uint8_t* mk = nullptr; VkDeviceAddress ma = 0;     // markers: NR x 2 x 256 B
    VkBuffer ab{}; VkDeviceMemory am{}; uint32_t* af = nullptr; VkDeviceAddress aa = 0;    // indirect args
    VkImage dimg{}; VkDeviceMemory dmem{};                                                  // 1x1 R32_UINT (layer img_ok; zero-copy never touches it)
    VkBuffer tb[2]{}; VkDeviceMemory tm[2]{}; VkDeviceAddress ta[2]{};                      // FV_DELAY
    uint32_t netg[NA][2]{};
    uint32_t k2g[2]{};   // K2 groups (14x14 own blocks per WG) when non-zero
    uint64_t frame = 0, seq = 0, pend_seq = 0, probe_frame = 0; bool pend = false, active = false, sized = false;
    uint32_t gate_dbg = ~0u;   // Last printed gate state (FV gate line on every change)
    int layer = 0;   // 0 not probed, 1 PROBE in flight, 2 up, -1 no layer (this process), -2 refused (re-probe after 300 frames)
    uint32_t gen = 0, qin = 0, qout = 0; VkDeviceAddress zbase = 0;
    // Fused prepass (pass-01 = f1g: X0 store + guarded HTP-input store at push bytes 16..23). Only with CAP_PINGPONG:
    // pre(N) writes in[pf_slot], slot = parity of UPSCALE seq N; else the address is 0 (no store) and the pack after WAIT stays.
    uint32_t qin1 = 0, pf_slot = 0; bool pp = false, pf_used = false; const bool pf = env_i("VELA_PF_FUSED", 1) != 0;
    bool pre_fused = false;   // Set per record by the context (pipelines[0] is f1g)
    // k1h mode
    bool k1h_want = false, k1h = false, ppo = false, k1h_frame = false; uint32_t qout1 = 0, po_slot = 0, pend_oslot = 0;
    uint64_t k1h_off_until = 0; VkDeviceAddress k1_addr = 0; std::string graph_k1h; const bool tfb = env_i("FV_TEST_FALLBACK", 0) != 0;
    const bool tsame = env_i("FV_TEST_SAMESLOT", 0) != 0;   // Test only: HTP(N) writes the slot K2a(N) would read (late-write race)
    bool k1h_used = false;   // a k1h frame was recorded (FVSTATS graph)
    // NPU start. kick_late = this frame's mode (read at begin_frame); a late kick is recorded by after_post()
    bool kick_late = true, late_pend = false; uint32_t late_ri = 0, late_fl = 0; uint64_t kicks_early = 0, kicks_late = 0, packs = 0;
    bool s720 = false, s720_used = false; std::string graph_k1h_360; const bool n7_720 = env_i("VELA_NPU_720", 1) != 0;   // 720p path
    bool s900 = false, s900_used = false; std::string graph_k1h_450; const bool n9_900 = env_i("VELA_NPU_900", 1) != 0;   // 900p path
    bool sup = false, sup_used = false; std::string graph_up, graph_up_450, graph_up_360;   // UP network (k1h only)
    int szi = -1, szi_used = -1; const bool sizes_on = env_i("VELA_NPU_SIZES", 1) != 0;   // kSizes index of this frame, -1 none
    const bool up_1080 = env_i("VELA_NPU_UP", 1) != 0, up_900 = env_i("VELA_NPU_UP_900", 1) != 0, up_720 = env_i("VELA_NPU_UP_720", 1) != 0;   // 900p/720p UP on by default
    const std::string& gk1h() const { if (szi >= 0) return kSizeGraph[szi][sup ? 1 : 0]; return sup ? (s900 ? graph_up_450 : s720 ? graph_up_360 : graph_up) : (s900 ? graph_k1h_450 : s720 ? graph_k1h_360 : graph_k1h); }
    // Exact mode state (see plan())
    bool exact_set = true, begun = false, tail_pend = false, dup_pend = false; int pcase = 0;
    bool exact_run = false;   // The last planned frame ran the exact pipeline (plan() case 1/3/4): the panel's "on, exact|async"
    uint64_t tail_seq = 0, pipe_tails = 0, pipe_starts = 0, pipe_drops = 0; uint32_t tail_oslot = 0, tail_w = 0, tail_h = 0, tail_rw = 0, tail_rh = 0;
    std::string dump_dir; std::vector<uint64_t> dump_frames; VkBuffer hb{}; VkDeviceMemory hm{}; uint8_t* hp = nullptr; VkDeviceAddress ha = 0;
    bool dump_now = false; uint64_t dump_f = 0; uint32_t dump_nk = 0;   // Debug dump
    VkDeviceAddress pf_din() { pf_used = pf && pre_fused && pp && active && zbase && mode; if (!pf_used) return 0; pf_slot = uint32_t((seq + 1) & 1);
        return zbase + VkDeviceAddress(pf_slot ? qin1 : qin) * 16; }
    struct Ring { uint32_t mask = 0; uint32_t op[2]{}; uint64_t s[2]{}; } ring[NR];
    uint64_t ok = 0, fb = 0, line_ok = 0, line_fb = 0; double line_htp = 0; std::vector<double> htp;
    // Slim NPU frames (VELA_FP bit 1). fp_pre: pipelines[0] is an X0-flag pre (set per record by the context). fp_prev_head: the
    // last recorded HEAD skipped X0 (its TAIL must not run the GPU network). Slim only after 8 OK acks and 2*NR frames after a failed one.
    int fp_mode = 0; bool fp_pre = false, fp_prev_head = false; uint64_t fp_last_fb = 0, fp_fb_seen = 0, fp_heads = 0, fp_tails = 0;
    bool fp_allow() { fp_mode = fp_bits(); if (fb != fp_fb_seen) { fp_fb_seen = fb; fp_last_fb = frame; } return (fp_mode & 1) && fp_pre && ok >= 8 && frame > fp_last_fb + 2 * NR; }
    VkDeviceAddress k1_other() const { return zbase + VkDeviceAddress(tail_oslot ? qout : qout1) * 16; }   // k1 slot of N-2
    std::vector<std::string> lines; std::string last_key;

    uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return UINT32_MAX;
    }
    VkDeviceAddress addr(VkBuffer b) { VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; i.buffer = b; return vkGetBufferDeviceAddress(dev, &i); }
    void mkbuf(VkDeviceSize sz, VkBufferUsageFlags u, VkMemoryPropertyFlags f, VkBuffer& b, VkDeviceMemory& m, void** map) {
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = sz; bc.usage = u | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        chk(vkCreateBuffer(dev, &bc, nullptr, &b), "buffer");
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
        VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mi.pNext = &fl; mi.allocationSize = mr.size;
        mi.memoryTypeIndex = mtype(mr.memoryTypeBits, f); if (mi.memoryTypeIndex == UINT32_MAX) throw std::runtime_error("FV: no memory type");
        chk(vkAllocateMemory(dev, &mi, nullptr, &m), "alloc"); chk(vkBindBufferMemory(dev, b, m, 0), "bind");
        if (map) chk(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map), "map");
    }
    VkPipeline mkpipe(const uint32_t* code, size_t bytes, VkShaderModule& sm) {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = bytes; si.pCode = code;
        chk(vkCreateShaderModule(dev, &si, nullptr, &sm), "shader");
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pc.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr}; pc.layout = pl;
        VkPipeline p{}; chk(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pc, nullptr, &p), "pipeline"); return p;
    }
    Bridge(VkPhysicalDevice p, VkDevice d, int m) : pd(p), dev(d), mode(m) {
        delay = env_i("FV_DELAY", 0) != 0; const char* g = std::getenv("FV_GRAPH"); graph = g && *g ? g : "nf4_exact";
        { const char* k = std::getenv("FV_GRAPH_K1H"); graph_k1h = k && *k ? k : "nf4_k1h"; }
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; lc.pushConstantRangeCount = 1; lc.pPushConstantRanges = &pr;
        chk(vkCreatePipelineLayout(dev, &lc, nullptr, &pl), "layout");
        px = mkpipe(kXferSpv, sizeof kXferSpv, sx);
        { const char* k = std::getenv("FV_GRAPH_K1H_360"); graph_k1h_360 = k && *k ? k : "nf4_k1h_360"; }
        { const char* k = std::getenv("FV_GRAPH_K1H_450"); graph_k1h_450 = k && *k ? k : "nf4_k1h_450"; }
        { const char* k = std::getenv("FV_GRAPH_K1H_UP"); graph_up = k && *k ? k : "nf4up_k1h"; }
        { const char* k = std::getenv("FV_GRAPH_K1H_UP_450"); graph_up_450 = k && *k ? k : "nf4up_k1h_450"; }
        { const char* k = std::getenv("FV_GRAPH_K1H_UP_360"); graph_up_360 = k && *k ? k : "nf4up_k1h_360"; }
        if (const char* dd = std::getenv("FV_DUMP"); dd && *dd) { dump_dir = dd; const char* fl = std::getenv("FV_DUMP_FRAMES"); const std::string fs = fl && *fl ? fl : "40,80";
            for (size_t p0 = 0; p0 < fs.size();) { size_t p1 = fs.find(',', p0); if (p1 == std::string::npos) p1 = fs.size(); if (p1 > p0) dump_frames.push_back(std::strtoull(fs.substr(p0, p1 - p0).c_str(), nullptr, 10)); p0 = p1 + 1; }
            mkbuf(VkDeviceSize(kRegionVec) * 16 * 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, hb, hm, (void**)&hp); ha = addr(hb); }
        if (delay) for (int k = 0; k < 2; k++) { mkbuf(VkDeviceSize(kRegionVec) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tb[k], tm[k], nullptr); ta[k] = addr(tb[k]); }
        if (!mode) return;
        ps = mkpipe(kSelectSpv, sizeof kSelectSpv, ss);
        mkbuf(NR * 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mb, mm, (void**)&mk);
        std::memset(mk, 0, NR * 512); ma = addr(mb);
        mkbuf(1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ab, am, (void**)&af);
        std::memset(af, 0, 1024); aa = addr(ab);
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R32_UINT; ic.extent = {1, 1, 1};
        ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL; ic.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        chk(vkCreateImage(dev, &ic, nullptr, &dimg), "image");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, dimg, &mr);
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mi.allocationSize = mr.size; mi.memoryTypeIndex = mtype(mr.memoryTypeBits, 0);
        chk(vkAllocateMemory(dev, &mi, nullptr, &dmem), "image memory"); chk(vkBindImageMemory(dev, dimg, dmem, 0), "image bind");
    }
    ~Bridge() {
        vkDeviceWaitIdle(dev); dump_write();
        if (hb) vkDestroyBuffer(dev, hb, nullptr); if (hm) vkFreeMemory(dev, hm, nullptr);
        if (mode) for (uint64_t f = frame > NR - 2 ? frame - (NR - 2) : 0; f < frame; f++) drain(f);   // acks not read yet
        if (mode || delay) {
            std::vector<double> v = htp; std::sort(v.begin(), v.end()); double mean = 0; for (double x : v) mean += x; if (!v.empty()) mean /= v.size();
            auto pc = [&](double q) { return v.empty() ? 0.0 : v[std::min<size_t>(v.size() - 1, size_t(q * v.size()))]; };
            std::printf("FVSTATS mode %d delay %d graph %s frames %llu htp_ok %llu fallbacks %llu layer %d htp_ms mean %.3f p50 %.3f p90 %.3f max %.3f"
                        " kick %s kicks early %llu late %llu packs %llu exact %d tails %llu starts %llu drops %llu\n",
                        mode, int(delay), (k1h_used ? szi_used >= 0 ? kSizeGraph[szi_used][sup_used ? 1 : 0] : (sup_used ? (s900_used ? graph_up_450 : s720_used ? graph_up_360 : graph_up) : (s900_used ? graph_k1h_450 : s720_used ? graph_k1h_360 : graph_k1h)) : graph).c_str(), (unsigned long long)frame, (unsigned long long)ok, (unsigned long long)fb, layer,
                        mean / 1000, pc(0.5) / 1000, pc(0.9) / 1000, v.empty() ? 0.0 : v.back() / 1000,
                        kick_late ? "late" : "early", (unsigned long long)kicks_early, (unsigned long long)kicks_late, (unsigned long long)packs,
                        int(exact_set), (unsigned long long)pipe_tails, (unsigned long long)pipe_starts, (unsigned long long)pipe_drops);
            if (fp_mode) std::printf("FPSTATS mode %d slim heads %llu tails %llu fallbacks %llu\n", fp_mode, (unsigned long long)fp_heads, (unsigned long long)fp_tails, (unsigned long long)fb);
            std::fflush(stdout);
        }
        for (auto& [b, m] : {std::pair{mb, mm}, std::pair{ab, am}, std::pair{tb[0], tm[0]}, std::pair{tb[1], tm[1]}}) { if (b) vkDestroyBuffer(dev, b, nullptr); if (m) vkFreeMemory(dev, m, nullptr); }
        if (dimg) vkDestroyImage(dev, dimg, nullptr); if (dmem) vkFreeMemory(dev, dmem, nullptr);
        for (VkPipeline p : {px, ps}) if (p) vkDestroyPipeline(dev, p, nullptr);
        for (VkShaderModule s : {sx, ss}) if (s) vkDestroyShaderModule(dev, s, nullptr);
        if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
    }
    // ---- panel lines (one per state change, plus a running line every 600 waited frames)
    void say(const std::string& key, const char* text) { if (key != last_key) { last_key = key; lines.emplace_back(text); } }
    void say_on(bool x) { say(x ? "on_exact" : "on_async", x ? "Vela NPU: on, exact" : "Vela NPU: on, async"); }
    static const char* why(uint32_t st) {
        return st == VELA_NPU_ACK_NODAEMON ? "nodaemon" : st == VELA_NPU_ACK_BADARGS ? "badargs" : st == VELA_NPU_ACK_BUSY ? "busy" : "error";
    }
    void fallback(const char* w) { char b[96]; std::snprintf(b, sizeof b, "Vela NPU: fallback %s, GPU path", w); say(w, b); }
    void adopt(const VelaNpuAck& a, uint32_t caps) {
        if (!(caps & VELA_NPU_CAP_ZEROCOPY) || !(caps & VELA_NPU_CAP_BDA)) return;
        uint32_t g, q[3]; uint64_t b; std::memcpy(&g, a.reserved + 16, 4); std::memcpy(q, a.reserved + 20, 12); std::memcpy(&b, a.reserved + 32, 8);
        if (b) { gen = g; zbase = b; qin = q[0]; qout = q[1]; }
        pp = b && (caps & VELA_NPU_CAP_PINGPONG); if (pp) std::memcpy(&qin1, a.reserved + 40, 4);   // input ping-pong
        ppo = b && (caps & VELA_NPU_CAP_OUTPINGPONG); if (ppo) std::memcpy(&qout1, a.reserved + 44, 4);   // output ping-pong
    }
    void drain(uint64_t f) {
        Ring& r = ring[f % NR];
        for (uint32_t k = 0; k < 2; k++) {
            if (!(r.mask >> k & 1)) continue;
            VelaNpuAck a; std::memcpy(&a, mk + (f % NR) * 512 + k * 256, sizeof a);
            const uint32_t op = r.op[k];
            if (a.magic == VELA_NPU_MAGIC_CMD) { if (op == VELA_NPU_OP_PROBE) { layer = -1; fallback("nolayer"); } else ++fb; continue; }   // no layer below us
            if (a.magic != VELA_NPU_MAGIC_ACK || a.seq != r.s[k]) { if (op == VELA_NPU_OP_PROBE) layer = 0; else if (op == VELA_NPU_OP_WAIT) { ++fb; ++line_fb; } continue; }
            uint32_t caps; std::memcpy(&caps, a.reserved, 4); adopt(a, caps);
            if (op == VELA_NPU_OP_PROBE) {
                if (k1h && !(a.status == VELA_NPU_ACK_OK && (caps & VELA_NPU_CAP_ZEROCOPY) && (caps & VELA_NPU_CAP_BDA) && zbase && ppo)) {
                    k1h_off_until = frame + 300; layer = 0;   // no nf4_k1h daemon or no output slot 1: the network graph (0.8.4 path) now
                } else if (a.status == VELA_NPU_ACK_OK && (caps & VELA_NPU_CAP_ZEROCOPY) && (caps & VELA_NPU_CAP_BDA) && zbase) {
                    layer = 2; say_on(exact_run || (exact_set && k1h)); lines.emplace_back("Vela NPU: zero-copy tensors");
                } else { layer = -2; probe_frame = frame; fallback(a.status == VELA_NPU_ACK_OK ? "badargs" : why(a.status)); }
            } else if (op == VELA_NPU_OP_WAIT) {
                if (a.status == VELA_NPU_ACK_OK) { uint32_t us; std::memcpy(&us, a.reserved + 4, 4); ++ok; ++line_ok; line_htp += us / 1000.0; htp.push_back(us); if (layer == 2) say_on(exact_run); }
                else { ++fb; ++line_fb; fallback(why(a.status)); }
            }
        }
        r.mask = 0;
        if (line_ok + line_fb >= 600) {
            char b[160]; std::snprintf(b, sizeof b, "Vela NPU: %llu frames, HTP %.2f ms, fallbacks %llu", (unsigned long long)(line_ok + line_fb),
                                      line_ok ? line_htp / line_ok : 0.0, (unsigned long long)line_fb);
            lines.emplace_back(b); line_ok = line_fb = 0; line_htp = 0;
        }
    }
    // ---- recording (called by record_fsr411_frame)
    void bar(VkCommandBuffer cb, VkPipelineStageFlags s, VkAccessFlags sa, VkPipelineStageFlags d, VkAccessFlags da) {
        VkMemoryBarrier mbr{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mbr.srcAccessMask = sa; mbr.dstAccessMask = da;
        vkCmdPipelineBarrier(cb, s, d, 0, 1, &mbr, 0, nullptr, 0, nullptr);
    }
    void xfer(VkCommandBuffer cb, VkDeviceAddress a, VkDeviceAddress b, uint32_t m, bool indirect = false) {
        struct { VkDeviceAddress a, b; uint32_t m, pad[3]; } p{a, b, m, {}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, px);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &p);
        if (indirect) vkCmdDispatchIndirect(cb, ab, kLive); else if (m == 2) vkCmdDispatch(cb, (kRegionVec + 255) / 256, 1, 1); else vkCmdDispatch(cb, 60, 34, 1);
    }
    void marker(VkCommandBuffer cb, uint32_t ri, uint32_t k, uint32_t op, uint64_t s, uint32_t flags) {
        VelaNpuCmd m{}; m.magic = VELA_NPU_MAGIC_CMD; m.version = VELA_NPU_VERSION; m.op = op; m.seq = s; m.flags = flags | VELA_NPU_F_GRAPH;
        if (op == VELA_NPU_OP_UPSCALE) {
            const VelaNpuImage im{(uint64_t)(uintptr_t)dimg, VK_FORMAT_R32_UINT, VK_IMAGE_LAYOUT_GENERAL, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT, 0};
            m.color = im; m.output = im; m.pad0 = gen;
        }
        std::snprintf(reinterpret_cast<char*>(m.reserved), 32, "%s", (k1h ? gk1h() : graph).c_str());   // Graph per (network, output)
        if (flags & VELA_NPU_F_OUTSLOT) m.reserved[33] = uint8_t(po_slot);   // output slot
        if (flags & VELA_NPU_F_INSLOT) m.reserved[32] = uint8_t(pf_slot);   // input slot
        vkCmdUpdateBuffer(cb, mb, VkDeviceSize(ri) * 512 + k * 256, sizeof m, &m);
        ring[ri].mask |= 1u << k; ring[ri].op[k] = op; ring[ri].s[k] = s;
    }
    VkDeviceSize arg_off(uint32_t pass) const { return kLive + VkDeviceSize(pass - 1) * kStride * 4; }   // pass 2..24 (16 B stride)
    // Debug dump: k 0 X0 / 1 OUT (region 0, kRegionVec uvec4), 2 K1 (InterBuf, nvec uvec4) of the listed frames -> host buffer; true = recorded
    bool dump(VkCommandBuffer cb, int k, VkDeviceAddress src, uint32_t nvec) {
        if (!hp || !src || std::find(dump_frames.begin(), dump_frames.end(), frame) == dump_frames.end()) return false;
        constexpr VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; constexpr VkAccessFlags RW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bar(cb, CS, RW, CS, RW);
        struct { VkDeviceAddress a, b; uint32_t m, pad[3]; } p{src, ha + VkDeviceAddress(kRegionVec) * 16 * VkDeviceAddress(k), 2, {}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, px); vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &p);
        vkCmdDispatch(cb, (std::min(nvec, kRegionVec) + 255) / 256, 1, 1);
        bar(cb, CS, RW, CS | VK_PIPELINE_STAGE_HOST_BIT, RW | VK_ACCESS_HOST_READ_BIT);
        dump_now = true; dump_f = frame; if (k == 2) dump_nk = std::min(nvec, kRegionVec); return true;
    }
    void dump_write() {   // after the frame's queue wait (the bench waits idle per frame)
        if (!dump_now || !hp) return; dump_now = false; const char* nm[3] = {"x0", "out", "k1"};
        for (int k = 0; k < 3; k++) { const std::string p = dump_dir + "/" + nm[k] + "_" + std::to_string(dump_f) + ".raw";
            if (FILE* f = std::fopen(p.c_str(), "wb")) { std::fwrite(hp + size_t(kRegionVec) * 16 * k, 1, k == 2 ? size_t(dump_nk) * 16 : size_t(kRegionVec) * 16, f); std::fclose(f); } }
        std::fprintf(stderr, "FV dump frame %llu nk %u -> %s\n", (unsigned long long)dump_f, dump_nk, dump_dir.c_str());
    }
    void begin_frame(uint32_t w, uint32_t h, uint32_t cw, uint32_t ch) {
        begun = true; dump_write();
        if (!mode) return;
        if (frame >= NR - 2) drain(frame - (NR - 2));
        // the recorder gets the OUTPUT size and capacity; the network runs at half (960x540, region 0 = 962 x 542 uvec4 with
        // halo, scratch layout): only 960x540 -> 1920x1080 with capacity 1920x1080 (test bench: FV size 1920x1080 capacity 1920x1080)
        const bool s1080 = w == 1920 && h == 1080 && cw == 1920 && ch == 1080;
        // 1280x720 only as k1h (nf4_k1h_360) with the fused pre writing the HTP input; a size change re-probes (other graph = other daemon)
        const bool s7 = w == 1280 && h == 720 && cw == 1920 && ch == 1080 && n7_720 && k1h_want && frame >= k1h_off_until && pre_fused && pf;
        if (s7 != s720 && layer != 1) { s720 = s7; if (layer == 2 || layer == -2) layer = 0; pend = false; }
        // 1600x900 as 720p (k1h nf4_k1h_450, fused pre, input ping-pong); its own graph = its own daemon, a size change re-probes
        const bool s9 = w == 1600 && h == 900 && cw == 1920 && ch == 1080 && n9_900 && k1h_want && frame >= k1h_off_until && pre_fused && pf;
        if (s9 != s900 && layer != 1) { s900 = s9; if (layer == 2 || layer == -2) layer = 0; pend = false; }
        // The other sizes with their own graphs, same conditions; a size change re-probes
        const int sn = sizes_on && cw == 1920 && ch == 1080 && k1h_want && frame >= k1h_off_until && pre_fused && pf ? size_index(w, h) : -1;
        if (sn != szi && layer != 1) { szi = sn; if (layer == 2 || layer == -2) layer = 0; pend = false; }
        // Network change (P <-> UP) = other graph = other daemon: re-probe like a size change. UP runs only as k1h (no UP
        // network-only graph), only for outputs whose UP graph exists (1080p, 900p, 720p; all on by default, VELA_NPU_UP*=0 turns one off).
        const bool nu = g_net_up.load();
        if (nu != sup && layer != 1) { sup = nu; if (layer == 2 || layer == -2) layer = 0; pend = false; }
        const bool up_graph = !sup || ((s1080 ? up_1080 : s9 ? up_900 : s7 ? up_720 : sn >= 0) && k1h_want && frame >= k1h_off_until);
        sized = (s1080 || s7 || s9 || sn >= 0) && g_render_ok.load() && sup == nu && up_graph;
        { const uint32_t dg = uint32_t(s1080) | uint32_t(s7) << 1 | uint32_t(s9) << 2 | uint32_t(k1h_want) << 3 | uint32_t(sup) << 4 | uint32_t(nu) << 5
              | uint32_t(up_graph) << 6 | uint32_t(g_render_ok.load()) << 7 | uint32_t(sized) << 8 | uint32_t(frame < k1h_off_until) << 9 | uint32_t(pre_fused) << 10 | uint32_t(pf) << 11;
          static const bool fv_dbg = env_i("FV_DEBUG", 0) != 0; if (fv_dbg && dg != gate_dbg) { gate_dbg = dg;   // Why the NPU is on/off (one line per change); only with FV_DEBUG=1
              std::fprintf(stderr, "FV gate frame %llu out %ux%u cap %ux%u render %ux%u s1080 %d s7 %d s9 %d k1h_want %d net_up %d sup %d up_graph %d up_1080 %d render_ok %d k1h_off %d pre_fused %d pf %d layer %d -> sized %d\n",
                  (unsigned long long)frame, w, h, cw, ch, g_render_w.load(), g_render_h.load(), int(s1080), int(s7), int(s9), int(k1h_want), int(nu), int(sup), int(up_graph), int(up_1080),
                  int(g_render_ok.load()), int(frame < k1h_off_until), int(pre_fused), int(pf), layer, int(sized)); } }
        if (!sized) {   // The line names the render and output size (plugin: "this model on the NPU does not take this size")
            char b[128]; std::snprintf(b, sizeof b, "Vela NPU: fallback size %ux%u->%ux%u, GPU path", g_render_w.load(), g_render_h.load(), w, h);
            say(std::string("size ") + b, b);
            static bool once = false; if (!once) { once = true; std::fprintf(stderr, "FV size %ux%u capacity %ux%u render %ux%u: GPU path\n", w, h, cw, ch, g_render_w.load(), g_render_h.load()); } }
        if (layer == -2 && frame - probe_frame > 300) layer = 0;
        k1h_frame = false; k1_addr = 0; late_pend = false;
        if (frame % 120 == 0) { kick_late = kick_setting(); exact_set = exact_setting(); }   // NPU start (live key / env), changes between frames only
        { const bool want = k1h_want && frame >= k1h_off_until;   // Graph follows the bound post (re-probe on a change)
          if (want != k1h && layer != 1) { k1h = want; if (layer == 2 || layer == -2) layer = 0; pend = false; } }
        if (layer == 1 && frame - probe_frame > NR) layer = 0;   // PROBE ack never came back: probe again
        active = sized && layer == 2 && zbase && (!k1h || ppo) && s720 == s7 && (!s720 || (k1h && pp)) && s900 == s9 && (!s900 || (k1h && pp)) && szi == sn && (szi < 0 || (k1h && pp));   // 720p/900p need k1h + input ping-pong
        const uint32_t ri = uint32_t(frame % NR); ring[ri].mask = 0; std::memset(mk + ri * 512, 0, 512);
        if (!active) { pend = false; return; }
        af[0] = k1h ? (k2g[0] ? k2g[0] : netg[25][0]) : 60; af[1] = k1h ? (k2g[0] ? k2g[1] : netg[25][1]) : 34; af[2] = 1; af[3] = 0;   // no unpack in k1h mode; triple 0 (OK only) = K2a
        netg[26][0] = k1h ? (k2g[0] ? k2g[0] : netg[25][0]) : 0; netg[26][1] = k1h ? (k2g[0] ? k2g[1] : netg[25][1]) : 0;   // Triple 26 (fallback only) = K2b
        for (uint32_t j = 1; j < NA; j++) { af[j * kStride] = netg[j][0]; af[j * kStride + 1] = netg[j][1]; af[j * kStride + 2] = 1; if (kStride == 4) af[j * 4 + 3] = 0; }
    }
    // Before pass 2. true: passes 2..24 dispatch indirect from arg_off(pass).
    bool before_net(VkCommandBuffer cb, VkDeviceAddress sadr) {
        if (!mode) return false;
        const uint32_t ri = uint32_t(frame % NR);
        if (layer == 0 && sized) { marker(cb, ri, 0, VELA_NPU_OP_PROBE, ++seq, 0); layer = 1; probe_frame = frame; return false; }
        if (!active) return false;
        constexpr VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags RW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        if (pend) marker(cb, ri, 0, VELA_NPU_OP_WAIT, pend_seq, 0);
        bar(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT | CS, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            CS | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, RW | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        struct { VkDeviceAddress ack, full, live; uint32_t have, n; } sp{ma + VkDeviceAddress(ri) * 512, aa, aa + kLive, (pend && !tfb) ? 1u : 0u, (NA * kStride) | (kStride << 16)};   // n = count | stride << 16
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ps);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &sp);
        vkCmdDispatch(cb, 1, 1, 1);
        bar(cb, CS, VK_ACCESS_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, RW | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        if (!pf_used) { xfer(cb, sadr, zbase + VkDeviceAddress(qin) * 16, 0); ++packs; }   // Fused prepass already wrote in[pf_slot]                // pack X0(N) -> HTP input (HTP(N-1) is done: WAIT above)
        bar(cb, CS, RW, CS, RW);
        if (k1h) { k1h_frame = true; k1h_used = true; s720_used = s720; s900_used = s900; sup_used = sup; szi_used = szi; k1_addr = pend ? zbase + VkDeviceAddress(pend_oslot ? qout1 : qout) * 16 : 0; }   // K2a reads k1(N-1) there (only K2a)
        else { xfer(cb, sadr, zbase + VkDeviceAddress(qout) * 16, 1, true);         // unpack OUT(N-1) -> region 0 (0 groups on fallback)
        bar(cb, CS, RW, CS, RW); }
        po_slot = (tsame && k1h && pend) ? pend_oslot : uint32_t((seq + 1) & 1);   // Output slot of this kick (parity of its seq, as the input slot)
        const uint32_t kfl = VELA_NPU_F_TENSORS | VELA_NPU_F_ASYNC | VELA_NPU_F_ZEROCOPY | (pf_used ? VELA_NPU_F_INSLOT : 0u) | (k1h ? VELA_NPU_F_OUTSLOT : 0u);
        ++seq;   // HTP(N)'s seq: its input slot (pre(N) wrote it) and output slot are fixed here for both kick points
        if (kick_late) { late_pend = true; late_ri = ri; late_fl = kfl; ++kicks_late; }   // E1 after post(N), in after_post()
        else { marker(cb, ri, 1, VELA_NPU_OP_UPSCALE, seq, kfl); ++kicks_early; }   // E1: HTP(N) after the unpack read OUT(N-1)
        pend = true; pend_seq = seq; pf_used = false; pend_oslot = po_slot;
        return true;
    }
    // After pass 24: the GPU-only reference of the async schedule swaps OUT(N) with OUT(N-1) (FV_DELAY semantics).
    void after_net(VkCommandBuffer cb, VkDeviceAddress sadr) {
        if (mode || !delay) return;
        constexpr VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags RW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        // FV_DELAY_FROM = first frame that uses OUT(N-1) (1 = the GPU-only reference; the layer path starts at NR-1 = 7: PROBE at 0, its ack read
        // at 6, first kick at 6, first unpack at 7), so the reference matches the async run from frame 0 on
        static const uint64_t from = uint64_t(std::max(1, env_i("FV_DELAY_FROM", 1)));
        if (frame + 1 >= from) { xfer(cb, sadr, ta[frame & 1], 2); bar(cb, CS, RW, CS, RW); }
        if (frame >= from) { xfer(cb, ta[(frame - 1) & 1], sadr, 2); bar(cb, CS, RW, CS, RW); }
    }
    // Late NPU start: the HTP(N) kick after post(N) (K2(N) has read k1(N-1); pre(N) wrote HTP(N)'s input long before). The layer
    // puts its own all-writes -> HOST barrier before E1. po_slot / pf_slot / gen still hold frame N's values (pf_din() of N+1 runs later).
    void after_post(VkCommandBuffer cb) { if (!late_pend) return; late_pend = false; marker(cb, late_ri, 1, VELA_NPU_OP_UPSCALE, pend_seq, late_fl); }
    void end_frame() { ++frame; begun = false; }
    // This frame's exact-mode case (after begin_frame). ok = the context can pipeline (NMS inputs, split post, no size change);
    // cc / dup = the colour copy / kept output match this frame's colour / output image.
    int plan(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, bool ok, bool cc, bool dup) {
        const bool pipe = ok && mode && exact_set && active && k1h && pp && pf && pre_fused;
        const bool same = tail_w == w && tail_h == h && tail_rw == rw && tail_rh == rh;
        if (tail_pend) pcase = pipe && same && cc ? 1 : 2;
        else if (dup_pend && pipe && same && dup) pcase = 3;
        else if (pipe) pcase = 4;
        else pcase = 0;
        exact_run = pcase == 1 || pcase == 3 || pcase == 4;   // Report only
        if (pcase != 3) dup_pend = false;
        if (pcase == 2) { tail_pend = false; pend = false; ++pipe_drops; }
        if (pcase == 4) { pend = false; active = false; dup_pend = true; ++pipe_starts; }   // GPU frame, no kick; its output is shown twice
        tail_w = w; tail_h = h; tail_rw = rw; tail_rh = rh;
        return pcase;
    }
    // TAIL(N-1) before pass 2: WAIT(HTP(N-1)), select (K2a on k1(N-1), else the GPU network on X0(N-1) + border_12 + K1 + K2b)
    bool tail_net(VkCommandBuffer cb) {
        const uint32_t ri = uint32_t(frame % NR);
        constexpr VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags RW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        marker(cb, ri, 0, VELA_NPU_OP_WAIT, tail_seq, 0);
        bar(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT | CS, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            CS | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, RW | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        struct { VkDeviceAddress ack, full, live; uint32_t have, n; } sp{ma + VkDeviceAddress(ri) * 512, aa, aa + kLive, tfb ? 0u : 1u, (NA * kStride) | (kStride << 16)};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ps);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, &sp);
        vkCmdDispatch(cb, 1, 1, 1);
        bar(cb, CS, VK_ACCESS_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, RW | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        k1h_frame = true; k1h_used = true; s720_used = s720; s900_used = s900; sup_used = sup; szi_used = szi;
        k1_addr = zbase + VkDeviceAddress(tail_oslot ? qout1 : qout) * 16;
        tail_pend = false; ++pipe_tails;
        return true;
    }
    // HEAD(N) after border_0: the HTP(N) kick (pre(N) wrote its input slot; K2(N-1) has already read k1(N-1))
    void head_kick(VkCommandBuffer cb, VkDeviceAddress sadr) {
        constexpr VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags RW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        const uint32_t ri = uint32_t(frame % NR);
        bar(cb, CS, RW, CS, RW);
        if (!pf_used) { xfer(cb, sadr, zbase + VkDeviceAddress(qin) * 16, 0); ++packs; bar(cb, CS, RW, CS, RW); }
        po_slot = uint32_t((seq + 1) & 1);
        const uint32_t kfl = VELA_NPU_F_TENSORS | VELA_NPU_F_ASYNC | VELA_NPU_F_ZEROCOPY | (pf_used ? VELA_NPU_F_INSLOT : 0u) | VELA_NPU_F_OUTSLOT;
        ++seq; marker(cb, ri, 1, VELA_NPU_OP_UPSCALE, seq, kfl); ++kicks_early;
        tail_pend = true; tail_seq = seq; tail_oslot = po_slot; pf_used = false; pend = false;
    }
};
inline Bridge* g_bridge = nullptr;
inline bool g_failed = false;
inline std::vector<std::string> g_lines;
inline void shutdown() { delete g_bridge; g_bridge = nullptr; }
inline Bridge* get(VkPhysicalDevice pd, VkDevice d) {
    const int m = (g_request.load() || env_i("FV_NPU", 0)) ? 2 : 0;
    const bool dl = env_i("FV_DELAY", 0) != 0;
    if (g_bridge && (g_bridge->dev != d || g_bridge->mode != m)) shutdown();
    if (!m && !dl && !(std::getenv("FV_DUMP") && *std::getenv("FV_DUMP"))) return nullptr;   // + debug dump
    if (!g_bridge && !g_failed) {
        try { g_bridge = new Bridge(pd, d, m); }
        catch (const std::exception& e) { g_failed = true; g_bridge = nullptr; g_lines.emplace_back("Vela NPU: fallback error, GPU path");
            std::fprintf(stderr, "%s\n", e.what()); }
    }
    return g_bridge;
}
inline std::vector<std::string> take_lines() {
    std::vector<std::string> v; v.swap(g_lines);
    if (g_bridge) { for (auto& l : g_bridge->lines) v.push_back(l); g_bridge->lines.clear(); }
    return v;
}
}
