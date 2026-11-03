// VK_LAYER_VELA_npu: implicit Vulkan layer (aarch64 Linux, enabled by VELA_NPU=1) that runs the FSR 4.1 Lite and the full
// FSR 4.1.1 networks on the Hexagon NPU through the fsr4npud daemon.
//
// What it does (contract: vela_npu_abi.h v1 + vela_npu_ext.h):
//  * recognises the provider's vkCmdUpdateBuffer marker (256 B, magic VELAVPC1) and writes a VelaNpuAck in its place;
//  * PROBE: connects to fsr4npud, which allocates one dma-buf (input at offset 0, the outputs after it); the layer imports it
//    into the game's VkDevice (VK_EXT_external_memory_dma_buf, added at vkCreateDevice). The provider never sees that buffer;
//  * UPSCALE + F_TENSORS, exact: copy input image -> dma-buf, barrier (all writes -> HOST), SetEvent(E1), wait for E2 (set by
//    the worker), copy dma-buf -> out0/out1 images, FINAL ack.status + HTP us copied on the GPU timeline;
//    worker: E1 seen -> eventfd kick -> done -> status slot -> vkSetEvent(E2);
//  * F_ASYNC: the kick only (ack OK = kicked); OP_WAIT(seq) in a later command buffer does the wait + copy-out + final status;
//  * E2 is ALWAYS set (E1 timeout, HTP timeout, daemon gone, teardown, dropped producer): the game never hangs;
//    a non-OK final status makes the provider's select shader run the GPU network for that frame;
//  * E2 is reset on the host only after a later job's E1 proves the GPU passed the wait (Turnip: a vkCmdResetEvent recorded
//    before vkCmdWaitEvents is not yet visible to that wait, so the GPU ran ahead);
//  * a slow HTP execute (longer than VELA_NPU_SLOW_MS) releases its frame to the GPU network at once and keeps the connection;
//    until it ends, every queued job is released too and new markers get BUSY, so no frame waits for the HTP;
//  * daemon gone or HTP hung (no answer in VELA_NPU_NPU_TIMEOUT_MS) -> "no NPU" (NODAEMON / status 5), the worker reconnects
//    every VELA_NPU_RECONNECT_MS;
//  * zero overhead without markers: VELA_NPU=0 -> no hooks; else a size compare in vkCmdUpdateBuffer and an atomic
//    load in the submit/begin hooks.
// Graph choice: VELA_NPU_F_GRAPH picks the daemon per PROBE (fsr4npud_<graph>, VELA_NPUD_GRAPH_NAME overrides); 2-tensor
// sets (the full FSR 4.1.1 graph) get zero-copy and their VkDeviceAddress (CAP_BDA). Lite (no flag, 3 tensors) is unchanged.
// Bounded GPU wait: the GPU does not wait on E2 in the game's command buffer, because E2 is set only by this process: a game
// killed with a job in flight would leave the command processor in vkCmdWaitEvents until the kernel's hangcheck resets the GPU
// and gamescope dies. Instead the layer records KSTEP doubling steps of vkCmdDispatchIndirect whose x the command processor
// reads from the slot's CPU line (1 = armed at submit, 0 = the worker's "done"), each followed by a compute -> indirect
// barrier; all steps run = ~50 ms (680 MHz) .. ~160 ms (220 MHz), far below hangcheck.
// A shader cannot poll host memory on the A740 (its cache keeps the first line it read); the command processor does read
// indirect arguments from memory. Needs bufferDeviceAddress (vkd3d-proton, fsr4vk); otherwise the vkCmdWaitEvents wait is used.
// VELA_NPU_BOUNDED=0 forces that wait.
// Env: VELA_NPU (manifest enable; DISABLE_VELA_NPU=1 disables), VELA_NPUD_NAME (fsr4npud),
//      VELA_NPU_LOG_FILE (append; none -> stderr; the plugin parses the four "Vela NPU layer: " lines), VELA_NPU_CONNECT_MS (100),
//      VELA_NPU_RECONNECT_MS (150), VELA_NPU_E1_TIMEOUT_MS (200), VELA_NPU_SLOW_MS (20: a longer execute -> that frame on the GPU),
//      VELA_NPU_NPU_TIMEOUT_MS (400: no answer -> the HTP counts as hung, fresh connection).
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fsr4npu.h"
#include "vela_npu_abi.h"
#include "vela_npu_ext.h"
#include "vela_npu_wait_spv.h"   // kWaitSpv (vela_npu_wait.comp)
#define VELA_NPU_ACK_ERROR_TIMEOUT 5u   /* layer-private final status: GPU/HTP timeout or device lost (provider reason "error") */

#define VL_EXPORT extern "C" __attribute__((visibility("default")))
#define VL_BUILD 11u   /* layer build number: reported in every ack and in the log; the manifest's implementation_version is the same number */
#define VL_BUILD_PREV 10u   /* previous layer build number */
#define NSLOT 8
#define KSTEP 11   /* steps of the bounded wait (111 << k iterations, k = 0..10: 227k in all) */

static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int env_int(const char* n, int def) { const char* v = getenv(n); return v && *v ? atoi(v) : def; }
static void* key_of(const void* h) { return *(void* const*)h; }

// ---------------------------------------------------------------- log (file under the game's /tmp)
static std::mutex g_logm; static FILE* g_log = nullptr; static volatile int g_logfd = -1;   // fd for the async-signal-safe line
static FILE* log_file() {  // VELA_NPU_LOG_FILE (plugin contract, append); no env -> stderr
  if (!g_log) { const char* p = getenv("VELA_NPU_LOG_FILE"); if (!p || !*p) p = getenv("VELA_NPU_LOG"); g_log = p && *p ? fopen(p, "a") : nullptr; if (!g_log) g_log = stderr; g_logfd = fileno(g_log); }
  return g_log;
}
// Lines the plugin parses (loaded / daemon connected / daemon not reachable / job statistics): printed exactly, nothing before them on the line.
static void vline(const char* f, ...) {
  std::lock_guard<std::mutex> l(g_logm); FILE* o = log_file();
  fputs("Vela NPU layer: ", o); va_list a; va_start(a, f); vfprintf(o, f, a); va_end(a); fputc(10, o); fflush(o);
}
// Diagnostic lines: a different prefix, so the plugin's parser ignores them.
static void vlog(const char* f, ...) {
  std::lock_guard<std::mutex> l(g_logm); FILE* o = log_file();
  fprintf(o, "[vela-npu %d %.3f] ", (int)getpid(), now_ms());
  va_list a; va_start(a, f); vfprintf(o, f, a); va_end(a); fputc(10, o); fflush(o);
}

// ---------------------------------------------------------------- state
// Contract: vela_npu_abi.h v1 + vela_npu_ext.h:
//   UPSCALE + F_TENSORS: color = HTP input, output = out0, motion = out1 (R32_UINT, GENERAL; word j at pixel (j%w, j/w)).
//   exact (no F_ASYNC): copy-in, E1, wait E2, copy-out; ack.status is the FINAL status, written on the GPU after the wait.
//   F_ASYNC: copy-in + E1 only, ack OK = "kicked".  OP_WAIT(seq): wait E2 of that kick, copy-out, final status after the wait.
//   ack bytes 32..35 = caps (CAP_ASYNC | CAP_TENSORS), bytes 36..39 = HTP time in us (GPU-written with the status).
static const uint32_t kCaps = VELA_NPU_CAP_ASYNC | VELA_NPU_CAP_TENSORS;
static const uint32_t kPending = 0x7FFFFFFFu;  // record-time status of a marker whose final status is copied after the wait
struct Inst { PFN_vkGetInstanceProcAddr gipa; VkInstance inst; };
struct TSet { fsr4npu* npu = nullptr; VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress bda = 0; uint64_t offAlt = 0, offOut1 = 0; };   // ping-pong: input slot 1, output slot 1 (0 = none)
enum { P_FREE = 0, P_RECORDED, P_QUEUED, P_DONE, P_DROPPED };
struct Slot {
  VkEvent e1 = VK_NULL_HANDLE, e2 = VK_NULL_HANDLE;
  int refs = 0;            // recording hold + queued jobs + async wait hold + E2 hold + quarantine
  int prod = P_FREE; uint64_t seq = 0; uint32_t gen = 0;
  VkImage out0 = VK_NULL_HANDLE, out1 = VK_NULL_HANDLE; uint32_t w = 0, h = 0;   // copy-out targets (async: used by OP_WAIT)
  bool quarantine = false; double qSince = 0;
  bool zc = false;          // zero-copy job (the provider's pack/unpack use the dma-buf directly)
  bool e2Set = false; uint64_t e2After = 0;   // E2 left set: host-reset once a later job's E1 proves the GPU passed its wait
};
struct Job { int slot; uint64_t seq; uint32_t gen; double tSubmit; uint64_t qn; bool async = false; int in = 0, out = 0; bool released = false; };   // in / out = ping-pong input / output slot of the kick   // async: its wait is a frame later   // released: frame already on the GPU network (slow execute), no kick
struct Consumer { int slot; uint64_t seq; };
struct CbRec { std::vector<Job> jobs; std::vector<Consumer> cons; bool submitted = false; };

struct Dev {
  VkDevice dev; PFN_vkGetDeviceProcAddr gdpa; VkPhysicalDeviceMemoryProperties mp{};
  bool on = false, extOk = false, bda = false;   // bda = the app enabled bufferDeviceAddress
  char want[64] = "";                           // daemon name of the last PROBE
#define F(n) PFN_vk##n n = nullptr;
  F(DestroyDevice) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(AllocateMemory) F(FreeMemory)
  F(BindBufferMemory) F(MapMemory) F(GetMemoryFdPropertiesKHR) F(CreateEvent) F(DestroyEvent) F(SetEvent) F(ResetEvent)
  F(GetEventStatus) F(CmdSetEvent) F(CmdResetEvent) F(CmdWaitEvents) F(CmdPipelineBarrier) F(CmdUpdateBuffer) F(CmdCopyBuffer)
  F(CmdCopyImageToBuffer) F(CmdCopyBufferToImage)
  F(QueueSubmit) F(QueueSubmit2) F(QueueSubmit2KHR) F(BeginCommandBuffer) F(AllocateCommandBuffers) F(FreeCommandBuffers)
  F(ResetCommandPool) F(DeviceWaitIdle) F(GetBufferDeviceAddress)
  F(CreateShaderModule) F(DestroyShaderModule) F(CreatePipelineLayout) F(DestroyPipelineLayout) F(CreateComputePipelines) F(DestroyPipeline)
  F(CmdBindPipeline) F(CmdDispatchIndirect)
#undef F
  // bounded wait (bw): GPU-only scratch for the step atomics, one pipeline per step length (no sets, no push constants)
  bool bw = false; VkBuffer wsBuf = VK_NULL_HANDLE; VkDeviceMemory wsMem = VK_NULL_HANDLE; VkShaderModule wsm = VK_NULL_HANDLE;
  VkPipelineLayout wpl = VK_NULL_HANDLE; VkPipeline wp[KSTEP] = {}; long bwWaits = 0, bwRestores = 0, bwNoRestore = 0;
  std::mutex mtx;
  int state = 0;                      // 0 never probed, 1 up, -1 down (reconnect pending), -2 lost/disabled
  uint32_t gen = 0; TSet set; std::vector<TSet> retired;
  uint32_t nt = 0; uint64_t off[3] = {}, bytes[3] = {};   // t[0] input, t[1] out0, t[2] out1 (graph may have 1 or 2 outputs)
  VkBuffer stBuf = VK_NULL_HANDLE; VkDeviceMemory stMem = VK_NULL_HANDLE; volatile uint32_t* st = nullptr;  // per slot: status, htp_us (CPU writes only)
  Slot slots[NSLOT]; uint32_t nextSlot = 0; int lastKick = -1;   // async kick waiting for its OP_WAIT
  std::unordered_map<VkCommandBuffer, CbRec> recs; std::atomic<int> nRec{0};
  std::unordered_map<VkCommandBuffer, VkCommandPool> cbPool;
  uint64_t qCounter = 0; std::thread worker; std::deque<Job> q; std::condition_variable cv; bool quit = false; double nextRetry = 0;
  bool slow = false; long slowJobs = 0, relJobs = 0;   // slow: the worker waits for a slow HTP execute (frames go to the GPU meanwhile)
  long l4At = 0, connFails = 0; long jobs = 0, okJobs = 0, e1Timeouts = 0, npuErrors = 0, noDaemon = 0, reconnects = 0, markers = 0, forced = 0;
  double sumE1 = 0, maxE1 = 0, sumNpu = 0, maxNpu = 0, sumRec = 0, maxRec = 0;
  // worker only: low envelope of submit->E1 (the worker sleeps through most of it instead of polling) + a diag window
  double e1Low = 0; long wJobs = 0, wPolls = 0; double wE1 = 0, wE1Max = 0, wNpu = 0, wNpuMax = 0, wSlept = 0;
};

static std::mutex g_mtx;
static std::unordered_map<void*, Inst*> g_inst;
struct DevEnt { std::atomic<void*> key{nullptr}; Dev* d = nullptr; };
static DevEnt g_dev[16];                                // lock-free lookup on the hot path
// A process that ends with a job in flight leaves behind the worker that sets E2 (and the done word of the bounded wait). With the
// vkCmdWaitEvents wait the GPU would then stay in the wait until the kernel's hangcheck ("hangcheck detected gpu lockup",
// "offending task: unknown") and the recovery kills gamescope (seen when Resident Evil 4 and Deep Rock Galactic were closed with
// FSR 4 on the NPU). vkDestroyDevice already sets every E2, but games end without it. Now every E2 is set when the process ends:
//  * exit() (the game's own quit, ExitProcess): the layer's destructor;
//  * SIGTERM / SIGHUP (anything but SIG_IGN) and SIGINT with the default action: a handler that chains to the previous one;
//  * after a release the worker never resets an E2 again (the GPU must not wait any more), the frames after it may show
//    a stale NPU output: only while the process ends.
// SIGKILL cannot be caught: with the vkCmdWaitEvents wait a SIGKILL with a job in flight still hangs; the bounded wait does not
// depend on this process.
static std::atomic<int> g_dying{0};
static void safe_line(const char* a, long v, const char* b) {   // async-signal-safe: write(2) of "[vela-npu exit] <a><v><b>\n"
  int fd = g_logfd; if (fd < 0) fd = 2;
  char buf[192]; size_t n = 0; auto put = [&](const char* s) { while (s && *s && n < sizeof buf - 1) buf[n++] = *s++; };
  put("[vela-npu exit] "); put(a);
  if (v >= 0) { char t[24]; int k = 0; long x = v; do { t[k++] = char('0' + x % 10); x /= 10; } while (x && k < 23); while (k && n < sizeof buf - 2) buf[n++] = t[--k]; }
  put(b); buf[n++] = 10; ssize_t r = write(fd, buf, n); (void)r;
}
static void release_all(const char* why, int sig) {   // async-signal-safe (atomics + vkSetEvent = a store in the event's BO)
  g_dying.store(1, std::memory_order_seq_cst);
  long n = 0, devs = 0;
  for (auto& e : g_dev) {
    if (!e.key.load(std::memory_order_acquire)) continue;
    Dev* d = e.d; if (!d || !d->stBuf || !d->SetEvent) continue;   // never probed: no events, nothing waits
    devs++;
    for (int i = 0; i < NSLOT; i++) if (d->slots[i].e2) { if (d->bw && d->st) d->st[i * 16 + 4] = 0; d->SetEvent(d->dev, d->slots[i].e2); n++; }   // and the done word
  }
  if (devs) { safe_line(why, sig, ""); safe_line("released E2 events: ", n, ""); }
}
static Dev* dev_of(const void* h) {
  void* k = key_of(h);
  for (auto& e : g_dev) if (e.key.load(std::memory_order_acquire) == k) return e.d;
  return nullptr;
}
static int g_enabled = -1;
static bool enabled() { if (g_enabled < 0) { const char* v = getenv("VELA_NPU"); g_enabled = !(v && !strcmp(v, "0")); } return g_enabled; }

// ---------------------------------------------------------------- tensor set: daemon ALLOC -> import into the game device
static uint32_t mem_type(Dev* d, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < d->mp.memoryTypeCount; i++) if ((bits >> i & 1) && (d->mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}
static void set_free(Dev* d, TSet& s) {
  if (s.npu) fsr4npu_close(s.npu);
  if (s.buf) d->DestroyBuffer(d->dev, s.buf, nullptr);
  if (s.mem) d->FreeMemory(d->dev, s.mem, nullptr);
  s = TSet{};
}
// Connect + ALLOC + import. No d->mtx held (blocks up to connect_ms). 0 or a status.
static bool pp_want(uint32_t nt) { static const bool off = [] { const char* v = getenv("VELA_NPU_PINGPONG"); return v && v[0] == '0'; }(); return !off && nt == 2; }
static int set_open(Dev* d, const char* name, int connect_ms, TSet& s, uint32_t* nt, uint64_t off[3], uint64_t bytes[3]) {
  bool bda = false;
  fsr4npu_info info{}; int fd = -1; uint64_t total = 0, o[FSR4NPU_MAXT] = {};
  VkMemoryRequirements mr{}; uint32_t mt = UINT32_MAX; VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  int st = fsr4npu_connect(name, connect_ms, &s.npu, &info);
  if (st) { s.npu = nullptr; return st; }
  if (info.ntensors < 2) { vlog("daemon graph '%s' has %u tensors", info.graph, info.ntensors); st = E_SIZE; goto fail; }
  s.offAlt = 0; s.offOut1 = 0;
  if ((st = pp_want(info.ntensors) ? fsr4npu_alloc_pp2(s.npu, 1, &fd, &total, o, &s.offAlt, &s.offOut1) : fsr4npu_alloc(s.npu, 1, &fd, &total, o))) { vlog("ALLOC: %s", fsr4npu_strerror(st)); goto fail; }
  *nt = std::min<uint32_t>(info.ntensors, 3);
  bda = d->bda && d->GetBufferDeviceAddress && info.ntensors == 2;   // only 1-output graphs (Lite unchanged)
  for (uint32_t k = 0; k < 3; k++) { off[k] = k < *nt ? o[k] : 0; bytes[k] = k < *nt ? info.bytes[k] : 0; }
  {
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO}; eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.pNext = &eb; bc.size = total;
    bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (bda) bc.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (d->CreateBuffer(d->dev, &bc, nullptr, &s.buf) != VK_SUCCESS) { close(fd); vlog("vkCreateBuffer(external) failed"); st = E_SYS; goto fail; }
  }
  d->GetBufferMemoryRequirements(d->dev, s.buf, &mr);
  if (d->GetMemoryFdPropertiesKHR(d->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS) { close(fd); vlog("vkGetMemoryFdPropertiesKHR failed"); st = E_FD; goto fail; }
  mt = mem_type(d, mr.memoryTypeBits & fp.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits & fp.memoryTypeBits, 0);
  {
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR}; imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = fd;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; fl.pNext = &imp; fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.pNext = bda ? (const void*)&fl : (const void*)&imp; ma.allocationSize = std::max<uint64_t>(total, mr.size); ma.memoryTypeIndex = mt;
    if (mt == UINT32_MAX || d->AllocateMemory(d->dev, &ma, nullptr, &s.mem) != VK_SUCCESS) { close(fd); vlog("dma-buf import failed (type %u)", mt); st = E_FD; goto fail; }
    if (d->BindBufferMemory(d->dev, s.buf, s.mem, 0) != VK_SUCCESS) { vlog("bind failed"); st = E_SYS; goto fail; }   // fd owned by the driver now
    if (bda) { VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; bi.buffer = s.buf; s.bda = d->GetBufferDeviceAddress(d->dev, &bi); }
  }
  vlog("daemon '%s' graph '%s' backend %u: dma-buf %llu B imported; in @%llu %llu B, out0 @%llu %llu B, out1 @%llu %llu B", name, info.graph, info.backend,
       (unsigned long long)total, (unsigned long long)off[0], (unsigned long long)bytes[0], (unsigned long long)off[1], (unsigned long long)bytes[1],
       (unsigned long long)off[2], (unsigned long long)bytes[2]);
  return 0;
fail:
  set_free(d, s);
  return st ? st : E_SYS;
}
static void worker_main(Dev* d);
// the signals whose arrival ends the process; previous handlers are chained (Wine has its own for SIGINT / SIGQUIT)
static const int kExitSigs[3] = {SIGTERM, SIGHUP, SIGINT};
static struct sigaction g_oldsa[3];
static void exit_sig(int sig, siginfo_t* info, void* uc) {
  int k = sig == SIGTERM ? 0 : sig == SIGHUP ? 1 : 2; const struct sigaction& o = g_oldsa[k];
  const bool dfl = !(o.sa_flags & SA_SIGINFO) && o.sa_handler == SIG_DFL, ign = !(o.sa_flags & SA_SIGINFO) && o.sa_handler == SIG_IGN;
  if (!ign && (sig != SIGINT || dfl)) release_all("signal ", sig);
  if (dfl) { struct sigaction d{}; d.sa_handler = SIG_DFL; sigemptyset(&d.sa_mask); sigaction(sig, &d, nullptr); raise(sig); return; }   // delivered on return
  if (ign) return;
  if (o.sa_flags & SA_SIGINFO) o.sa_sigaction(sig, info, uc); else o.sa_handler(sig);
}
static void install_exit_hooks() {
  static std::once_flag once;
  std::call_once(once, [] {
    int ok = 0;
    for (int k = 0; k < 3; k++) {
      struct sigaction sa{}; sa.sa_sigaction = exit_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK; sigemptyset(&sa.sa_mask);
      if (sigaction(kExitSigs[k], nullptr, &g_oldsa[k]) == 0 && sigaction(kExitSigs[k], &sa, nullptr) == 0) ok++;
    }
    vlog("exit hooks: E2 release at exit() and on SIGTERM/SIGHUP/SIGINT (%d/3 handlers; previous SIGTERM %s)", ok,
         (g_oldsa[0].sa_flags & SA_SIGINFO) ? "handler" : g_oldsa[0].sa_handler == SIG_DFL ? "default" : g_oldsa[0].sa_handler == SIG_IGN ? "ignored" : "handler");
  });
}
__attribute__((destructor)) static void vl_fini() { release_all("exit() / unload, sig ", 0); }
static void slot_unref(Dev* d, int i) { Slot& s = d->slots[i]; if (s.refs > 0 && --s.refs == 0 && s.prod == P_RECORDED) s.prod = P_DROPPED; }
// bounded wait setup (d->mtx held, first PROBE). Any failure -> d->bw stays false = the vkCmdWaitEvents wait.
static void bw_free(Dev* d) {
  for (VkPipeline& p : d->wp) { if (p) d->DestroyPipeline(d->dev, p, nullptr); p = VK_NULL_HANDLE; }
  if (d->wpl) d->DestroyPipelineLayout(d->dev, d->wpl, nullptr); if (d->wsm) d->DestroyShaderModule(d->dev, d->wsm, nullptr);
  if (d->wsBuf) d->DestroyBuffer(d->dev, d->wsBuf, nullptr); if (d->wsMem) d->FreeMemory(d->dev, d->wsMem, nullptr);
  d->wpl = VK_NULL_HANDLE; d->wsm = VK_NULL_HANDLE; d->wsBuf = VK_NULL_HANDLE; d->wsMem = VK_NULL_HANDLE; d->bw = false;
}
static void bw_init(Dev* d) {
  const char* v = getenv("VELA_NPU_BOUNDED");
  if (v && v[0] == '0') { vlog("bounded GPU wait off (VELA_NPU_BOUNDED=0): host-event wait"); return; }
  if (!d->bda || !d->GetBufferDeviceAddress || !d->CreateComputePipelines || !d->CmdDispatchIndirect || !d->CmdBindPipeline) {
    vlog("bounded GPU wait unavailable (bufferDeviceAddress %s): host-event wait", d->bda ? "on" : "off"); return; }
  const char* why = nullptr; void* p = nullptr; VkDeviceAddress a = 0; uint32_t total = 0;
  {
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = 256; bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkMemoryRequirements mr{}; VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.pNext = &fl;
    if (d->CreateBuffer(d->dev, &bc, nullptr, &d->wsBuf) != VK_SUCCESS) { why = "scratch buffer"; goto fail; }
    d->GetBufferMemoryRequirements(d->dev, d->wsBuf, &mr); ma.allocationSize = mr.size;
    ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);   // host-visible memory: the step timings in the header comment were measured with it
    if (ma.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->dev, &ma, nullptr, &d->wsMem) != VK_SUCCESS || d->BindBufferMemory(d->dev, d->wsBuf, d->wsMem, 0) != VK_SUCCESS) { why = "scratch memory"; goto fail; }
    if (d->MapMemory(d->dev, d->wsMem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p) memset(p, 0, 256);
    VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; bi.buffer = d->wsBuf; a = d->GetBufferDeviceAddress(d->dev, &bi);
    if (!a) { why = "scratch address"; goto fail; }
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize = sizeof kWaitSpv; sm.pCode = kWaitSpv;
    if (d->CreateShaderModule(d->dev, &sm, nullptr, &d->wsm) != VK_SUCCESS) { why = "shader module"; goto fail; }
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};   // empty: binding it disturbs no set and no push constant
    if (d->CreatePipelineLayout(d->dev, &pl, nullptr, &d->wpl) != VK_SUCCESS) { why = "pipeline layout"; goto fail; }
    for (int k = 0; k < KSTEP; k++) {
      const uint32_t sc[3] = {uint32_t(a), uint32_t(a >> 32), 111u << k}; total += sc[2];
      VkSpecializationMapEntry me[3]; for (int i = 0; i < 3; i++) { me[i].constantID = uint32_t(i); me[i].offset = uint32_t(i * 4); me[i].size = 4; }
      VkSpecializationInfo si{3, me, sizeof sc, sc};
      VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = d->wsm; ci.stage.pName = "main"; ci.stage.pSpecializationInfo = &si; ci.layout = d->wpl;
      if (d->CreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &d->wp[k]) != VK_SUCCESS) { why = "compute pipeline"; goto fail; }
    }
  }
  d->bw = true;
  vlog("bounded GPU wait on: %d steps, %u iterations in all (~50 ms at 680 MHz, ~160 ms at 220 MHz; then the GPU network for that frame)", KSTEP, total);
  return;
fail:
  vlog("bounded GPU wait setup failed (%s): host-event wait", why); bw_free(d);
}
// the app's compute pipeline, restored after the steps. Thread-local (a command buffer records on one thread at a time);
// valid only for the recording this thread began (begin epoch in a small table keyed by the command buffer: a collision or a
// command buffer begun on another thread -> no restore, counted).
static std::atomic<uint64_t> g_epoch{0};
struct BeginEnt { std::atomic<VkCommandBuffer> cb{VK_NULL_HANDLE}; std::atomic<uint64_t> ep{0}; };
static BeginEnt g_begin[256];
static thread_local VkCommandBuffer tl_bcb = VK_NULL_HANDLE; static thread_local uint64_t tl_bep = 0;      // last Begin on this thread
static thread_local VkCommandBuffer tl_pcb = VK_NULL_HANDLE; static thread_local uint64_t tl_pep = 0; static thread_local VkPipeline tl_pipe = VK_NULL_HANDLE;
static unsigned cb_hash(VkCommandBuffer cb) { uintptr_t x = (uintptr_t)cb; return unsigned((x >> 4) ^ (x >> 12)) & 255u; }
static void note_begin(VkCommandBuffer cb) {
  const uint64_t e = g_epoch.fetch_add(1, std::memory_order_relaxed) + 1; BeginEnt& b = g_begin[cb_hash(cb)];
  b.cb.store(cb, std::memory_order_relaxed); b.ep.store(e, std::memory_order_release); tl_bcb = cb; tl_bep = e; if (tl_pcb == cb) tl_pcb = VK_NULL_HANDLE;
}
static VkPipeline app_pipe(VkCommandBuffer cb) {
  if (tl_pcb != cb || !tl_pipe) return VK_NULL_HANDLE;
  const BeginEnt& b = g_begin[cb_hash(cb)];
  return b.cb.load(std::memory_order_relaxed) == cb && b.ep.load(std::memory_order_acquire) == tl_pep ? tl_pipe : VK_NULL_HANDLE;
}
static bool ctx_init(Dev* d) {  // d->mtx held; first PROBE: status buffer, events, worker
  if (d->stBuf) return true;
  VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = NSLOT * 64; bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (d->CreateBuffer(d->dev, &bc, nullptr, &d->stBuf) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; d->GetBufferMemoryRequirements(d->dev, d->stBuf, &mr);
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size;
  ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  void* p = nullptr;
  if (ma.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->dev, &ma, nullptr, &d->stMem) != VK_SUCCESS ||
      d->BindBufferMemory(d->dev, d->stBuf, d->stMem, 0) != VK_SUCCESS || d->MapMemory(d->dev, d->stMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
  d->st = (volatile uint32_t*)p;
  for (Slot& s : d->slots) {
    VkEventCreateInfo ec{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    if (d->CreateEvent(d->dev, &ec, nullptr, &s.e1) != VK_SUCCESS || d->CreateEvent(d->dev, &ec, nullptr, &s.e2) != VK_SUCCESS) return false;
  }
  for (int i = 0; i < NSLOT; i++) { d->st[i * 16 + 4] = 0; d->st[i * 16 + 5] = 1; d->st[i * 16 + 6] = 1; }   // indirect dispatch args (x, 1, 1)
  bw_init(d);             // bounded wait setup
  install_exit_hooks();   // E2 release at process end
  d->worker = std::thread(worker_main, d);
  pthread_setname_np(d->worker.native_handle(), "vela-npu-w");   // visible in /proc/<pid>/task/*/comm for per-thread CPU
  if (const char* cpus = getenv("VELA_NPU_WORKER_CPUS")) {
    cpu_set_t cs; CPU_ZERO(&cs); int a = -1, n = 0; char* e = nullptr;
    for (const char* q = cpus; *q;) { long v = strtol(q, &e, 10); if (e == q) break; q = e;
      if (a >= 0) { for (long c = a; c <= v && c < CPU_SETSIZE; c++) { CPU_SET(c, &cs); n++; } a = -1; } else if (*q == '-') { a = int(v); } else { CPU_SET(v, &cs); n++; }
      if (*q == '-' || *q == ',') q++; }
    if (n) vlog("worker pinned to %s: %s", cpus, pthread_setaffinity_np(d->worker.native_handle(), sizeof cs, &cs) ? "failed" : "ok");
  }
  return true;
}
static void go_down(Dev* d, const char* why) {  // d->mtx held: retire the set (kept alive until vkDestroyDevice: GPU work may still use it)
  if (d->state != 1) return;
  vlog("NPU DOWN (%s): gen %u retired, GPU path until fsr4npud is back", why, d->gen);
  if (d->set.npu) { fsr4npu_close(d->set.npu); d->set.npu = nullptr; }
  d->retired.push_back(d->set); d->set = TSet{};
  if (d->lastKick >= 0) { slot_unref(d, d->lastKick); d->lastKick = -1; }
  d->state = -1; d->nextRetry = now_ms() + env_int("VELA_NPU_RECONNECT_MS", 150);
}
static void try_up(Dev* d, int connect_ms) {  // d->mtx NOT held
  TSet ns; uint32_t nt = 0; uint64_t off[3], bytes[3]; char nm[64];
  { std::lock_guard<std::mutex> l(d->mtx); memcpy(nm, d->want, sizeof nm); }   // the daemon of the last PROBE
  double t0 = now_ms(); int st = set_open(d, nm, connect_ms, ns, &nt, off, bytes);
  std::lock_guard<std::mutex> l(d->mtx);
  if (st) {
    if (d->connFails++ % 200 == 0) vline("daemon not reachable (%s)", fsr4npu_strerror(st));   // about every 30 s
    if (d->state == 0) d->state = -1;
    d->nextRetry = now_ms() + env_int("VELA_NPU_RECONNECT_MS", 150); return;
  }
  if (d->state == -2 || d->retired.size() > 64) { set_free(d, ns); return; }
  d->set = ns; d->nt = nt; for (int k = 0; k < 3; k++) { d->off[k] = off[k]; d->bytes[k] = bytes[k]; }
  if (d->state == -1) d->reconnects++;
  d->gen++; d->state = 1; d->connFails = 0;
  vline("daemon connected");
  vlog("NPU UP: gen %u, %u tensors, connect+alloc+import %.3f ms", d->gen, nt, now_ms() - t0);
}
// absolute sleep on CLOCK_MONOTONIC (now_ms() base)
static void sleep_until_ms(double t) {
  timespec ts; ts.tv_sec = time_t(t / 1e3); ts.tv_nsec = long((t - ts.tv_sec * 1e3) * 1e6); if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}
// the worker's wake-up time for a job: the GPU reaches E1 no earlier than ~ the shortest recent submit->E1, so sleep
// through 85 % (async: its OP_WAIT is a frame later) / 70 % (exact: the GPU waits right after E1) of it, minus 0.25 ms.
static double e1_wake(double tSubmit, double e1Low, bool async) { return e1Low > 1.0 ? tSubmit + e1Low * (async ? 0.85 : 0.70) - 0.25 : 0.0; }
static double e1_low_next(double low, double e1) { return low <= 0 ? e1 : e1 < low ? 0.5 * low + 0.5 * e1 : 0.97 * low + 0.03 * e1; }  // fast down, slow up
static void line_diag(Dev* d) {  // d->mtx held; diag prefix: the plugin parser ignores it
  if (!d->wJobs) return;
  vlog("window %ld ok jobs: submit->E1 mean %.2f max %.2f ms (low %.2f, slept %.2f ms/job) | kick->done mean %.2f max %.2f ms | polls/job %.1f",
       d->wJobs, d->wE1 / d->wJobs, d->wE1Max, d->e1Low, d->wSlept / d->wJobs, d->wNpu / d->wJobs, d->wNpuMax, double(d->wPolls) / d->wJobs);
  d->wJobs = d->wPolls = 0; d->wE1 = d->wE1Max = d->wNpu = d->wNpuMax = d->wSlept = 0;
}
static void line_bw(Dev* d) {  // diagnostic prefix
  if (d->bw) vlog("bounded waits %ld (app pipeline rebound %ld, not known %ld)", d->bwWaits, d->bwRestores, d->bwNoRestore);
}
static void line_slow(Dev* d) {  // d->mtx held; diagnostic prefix
  if (d->slowJobs || d->relJobs) vlog("slow NPU executes %ld (frames released to the GPU network while they ran: %ld)", d->slowJobs, d->relJobs);
}
static void line_l4(Dev* d) {  // d->mtx held
  vline("%ld jobs, HTP %.2f ms, timeouts %ld, errors %ld", d->jobs, d->okJobs ? d->sumNpu / d->okJobs : 0.0, d->e1Timeouts, d->npuErrors);
}

// ---------------------------------------------------------------- worker: E1 seen -> NPU -> final status -> E2 (always)
static void put_status(Dev* d, int slot, uint32_t ack, uint32_t htp_us) { d->st[slot * 16 + 0] = ack; d->st[slot * 16 + 1] = htp_us; }
// words 4..6 of the slot line = the steps' indirect args (x, 1, 1). Status first, then x (release + dsb: the CP reads memory).
static void put_x(Dev* d, int slot, uint32_t x) { __atomic_store_n((uint32_t*)&d->st[slot * 16 + 4], x, __ATOMIC_RELEASE); __asm__ volatile("dsb st" ::: "memory"); }
static void put_done(Dev* d, int slot) { if (d->bw) put_x(d, slot, 0); }
static void put_arm(Dev* d, int slot) { if (d->bw) { put_status(d, slot, VELA_NPU_ACK_ERROR_TIMEOUT, 0); put_x(d, slot, 1); } }   // before the driver submit
// final status TIMEOUT (the GPU network runs for that frame), then the GPU's wait ends: x = 0 and E2
static void put_release(Dev* d, int slot) { put_status(d, slot, VELA_NPU_ACK_ERROR_TIMEOUT, 0); put_done(d, slot); d->SetEvent(d->dev, d->slots[slot].e2); }
static void worker_main(Dev* d) {
  const double e1Tmo = env_int("VELA_NPU_E1_TIMEOUT_MS", 200); const int npuTmo = std::max(1, env_int("VELA_NPU_NPU_TIMEOUT_MS", 400));
  const int slowMs = std::min(npuTmo, std::max(1, env_int("VELA_NPU_SLOW_MS", 20)));
  std::unique_lock<std::mutex> lk(d->mtx);
  for (;;) {
    d->cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return d->quit || !d->q.empty(); });
    for (int i = 0; i < NSLOT; i++) { Slot& s = d->slots[i];   // E1 timed out earlier: release once the GPU set it, or after 2 s
      if (s.quarantine && (d->GetEventStatus(d->dev, s.e1) == VK_EVENT_SET || now_ms() - s.qSince > 2000)) { d->ResetEvent(d->dev, s.e1); s.quarantine = false; slot_unref(d, i); } }
    if (d->q.empty()) {
      if (d->quit) return;
      if (d->state == -1 && now_ms() >= d->nextRetry) { d->nextRetry = now_ms() + 1e9; lk.unlock(); try_up(d, 20); lk.lock(); }
      continue;
    }
    Job j = d->q.front(); d->q.pop_front();
    Slot& s = d->slots[j.slot];
    fsr4npu* npu = (d->state == 1 && j.gen == d->gen) ? d->set.npu : nullptr;
    lk.unlock();
    double t0 = now_ms(), deadline = t0 + e1Tmo; bool seen = false; VkResult er = VK_SUCCESS;
    // in a game the GPU reaches E1 10-20 ms after the submit (the frame's own work queued ahead); polling all of it (20 us
    // nanosleep steps + timer slack) keeps a game-process thread waking for that whole time. Sleep through the certain part, then poll (async: 8 tight + 100 us steps; exact: as before).
    const double wake = e1_wake(j.tSubmit, d->e1Low, j.async); long polls = 0;
    if (wake > t0) { sleep_until_ms(std::min(wake, deadline)); }
    const double slept = std::max(0.0, now_ms() - t0);
    for (int spin = 0; !d->quit && now_ms() < deadline; ++spin) {
      er = d->GetEventStatus(d->dev, s.e1); polls++;
      if (er == VK_EVENT_SET) { seen = true; break; }
      if (er < 0) break;                                                          // device lost
      if (spin > (j.async ? 8 : 200)) { timespec ts{0, j.async ? 100000L : 20000L}; nanosleep(&ts, nullptr); }   // tight, then steps
    }
    double tE1 = now_ms(); int st = S_OK; uint32_t ack; bool slow = false;
    if (seen) d->ResetEvent(d->dev, s.e1);
    if (!seen || j.released) ack = VELA_NPU_ACK_ERROR_TIMEOUT;
    else if (!npu) ack = VELA_NPU_ACK_NODAEMON;
    else { st = (j.in || j.out) ? fsr4npu_kick_slots(npu, j.in, j.out) : fsr4npu_kick(npu); if (st == S_OK) st = fsr4npu_wait(npu, slowMs);
           // the poll ran out (the daemon's own E_TIMEOUT comes earlier): slow execute. Its frame and every queued one go to the GPU
           // network now; the wait for this execute goes on up to npuTmo on the same connection, so its "done" pairs with this kick.
           if (st == E_TIMEOUT && slowMs < npuTmo && now_ms() - tE1 >= slowMs - 0.5) {
             slow = true; put_release(d, j.slot);
             lk.lock(); d->slow = true; for (Job& o : d->q) if (!o.released) { put_release(d, o.slot); o.released = true; } lk.unlock();
             st = fsr4npu_wait(npu, std::max(1, int(tE1 + npuTmo - now_ms())));
           }
           ack = st == S_OK && !slow ? VELA_NPU_ACK_OK : st == E_DISCONNECTED || st == E_PROTO ? VELA_NPU_ACK_NODAEMON : VELA_NPU_ACK_ERROR_TIMEOUT; }
    double tDone = now_ms(), np = tDone - tE1;
    put_status(d, j.slot, ack, ack == VELA_NPU_ACK_OK ? uint32_t(np * 1000.0) : 0);   // coherent host memory, before the event
    put_done(d, j.slot);                                                          // the bounded wait's x = 0 (after the status)
    d->SetEvent(d->dev, s.e2);                                                    // ALWAYS: the GPU never waits on a dead or hung daemon
    lk.lock();
    d->slow = false; d->jobs++; s.prod = P_DONE;
    if (seen && !g_dying.load(std::memory_order_acquire)) for (int i = 0; i < NSLOT; i++) { Slot& o = d->slots[i];   // never after a release; a later E1 seen: the CP passed every earlier wait
      if (i != j.slot && o.e2Set && o.e2After < j.qn) { d->ResetEvent(d->dev, o.e2); o.e2Set = false; slot_unref(d, i); } }
    if (!s.e2Set) { s.e2Set = true; s.e2After = j.qn; s.refs++; }
    if (ack == VELA_NPU_ACK_OK) { d->okJobs++; double e1 = tE1 - j.tSubmit; d->sumE1 += e1; d->sumNpu += np; d->maxE1 = std::max(d->maxE1, e1); d->maxNpu = std::max(d->maxNpu, np);
      d->e1Low = e1_low_next(d->e1Low, e1); d->wJobs++; d->wPolls += polls; d->wE1 += e1; d->wE1Max = std::max(d->wE1Max, e1); d->wNpu += np; d->wNpuMax = std::max(d->wNpuMax, np); d->wSlept += slept; }
    else if (!seen) { d->e1Timeouts++; s.quarantine = true; s.qSince = now_ms(); s.refs++;
      vlog("job seq %llu slot %d: pack-done event not seen in %.0f ms%s", (unsigned long long)j.seq, j.slot, e1Tmo, er < 0 ? " (DEVICE LOST)" : "");
      if (er == VK_ERROR_DEVICE_LOST && d->state != -2) { go_down(d, "device lost"); d->state = -2; } }
    else if (j.released) d->relJobs++;
    else if (!npu) d->noDaemon++;
    else if (slow && st == S_OK) { d->slowJobs++;
      vlog("job seq %llu: slow NPU execute %.1f ms: frame on the GPU network from %d ms, connection kept", (unsigned long long)j.seq, np, slowMs); }
    else { d->npuErrors++; vlog("job seq %llu: NPU %s after %.1f ms -> fallback", (unsigned long long)j.seq, fsr4npu_strerror(st), np);
      if (j.gen == d->gen) go_down(d, fsr4npu_strerror(st)); }   // fresh connection: a late "done" must never pair with the next kick
    slot_unref(d, j.slot);
    if (d->jobs - d->l4At >= 600) { d->l4At = d->jobs; line_l4(d); line_diag(d); line_bw(d); line_slow(d); }
  }
}

// ---------------------------------------------------------------- recording at the marker
static const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_TRANSFER_BIT, HOST = VK_PIPELINE_STAGE_HOST_BIT;
static void mbar(Dev* d, VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkAccessFlags sa, VkAccessFlags da) {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = sa; b.dstAccessMask = da;
  d->CmdPipelineBarrier(cb, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
}
static int slot_take(Dev* d) {
  for (int k = 0; k < NSLOT; k++) { int i = int((d->nextSlot + k) % NSLOT); if (d->slots[i].refs == 0 && !d->slots[i].quarantine) { d->nextSlot = i + 1; return i; } }
  return -1;
}
// buffer <-> R32_UINT image (word j at pixel (j % w, j / w)): full rows + one partial row
static void tensor_copy(Dev* d, VkCommandBuffer cb, bool toImage, VkImage im, uint32_t w, uint32_t h, VkBuffer buf, uint64_t off, uint64_t bytes) {
  uint64_t words = std::min<uint64_t>(bytes / 4, uint64_t(w) * h); uint32_t rows = uint32_t(words / w), rest = uint32_t(words % w);
  VkBufferImageCopy r[2]{}; uint32_t n = 0;
  if (rows) { r[n].bufferOffset = off; r[n].bufferRowLength = w; r[n].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r[n].imageExtent = {w, rows, 1}; n++; }
  if (rest) { r[n].bufferOffset = off + uint64_t(rows) * w * 4; r[n].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r[n].imageOffset = {0, int32_t(rows), 0}; r[n].imageExtent = {rest, 1, 1}; n++; }
  if (!n) return;
  if (toImage) d->CmdCopyBufferToImage(cb, buf, im, VK_IMAGE_LAYOUT_GENERAL, n, r);
  else d->CmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_GENERAL, buf, n, r);
}
// The bounded wait. Steps: indirect dispatch (x from the slot line, read by the CP) + compute -> indirect barrier (the next
// x is read only after the step ran). Then the same HOST_WRITE -> reads barrier as the old wait (Turnip invalidates the caches:
// the HTP's output and the worker's status line are read fresh). If x never turned 0 (process gone, worker late), the status
// stays the armed TIMEOUT -> the fork's select runs the GPU network for that frame. The app's compute pipeline is rebound.
static void bounded_wait(Dev* d, VkCommandBuffer cb, int si, VkPipelineStageFlags ds, VkAccessFlags da) {
  VkMemoryBarrier ib{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; ib.srcAccessMask = 0; ib.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
  for (int k = 0; k < KSTEP; k++) {
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->wp[k]);
    d->CmdDispatchIndirect(cb, d->stBuf, uint64_t(si) * 64 + 16);
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &ib, 0, nullptr, 0, nullptr);
  }
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; b.dstAccessMask = da;
  d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | HOST, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
  const VkPipeline ap = app_pipe(cb);
  if (ap) { d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ap); d->bwRestores++; } else d->bwNoRestore++;
  d->bwWaits++;
}
static void wait_and_copy_out(Dev* d, VkCommandBuffer cb, int si) {  // host-signalled E2: legacy vkCmdWaitEvents, srcStage HOST (outside a render pass)
  Slot& s = d->slots[si];
  if (s.zc) {   // zero-copy: no copy-out; make the HTP's writes visible to the provider's unpack (shader reads of the dma-buf)
    VkMemoryBarrier z{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; z.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; z.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    if (d->bw) bounded_wait(d, cb, si, ALL, z.dstAccessMask); else
    d->CmdWaitEvents(cb, 1, &s.e2, HOST, ALL, 1, &z, 0, nullptr, 0, nullptr);
    return;
  }
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
  if (d->bw) bounded_wait(d, cb, si, TR | ALL, b.dstAccessMask); else
  d->CmdWaitEvents(cb, 1, &s.e2, HOST, TR | ALL, 1, &b, 0, nullptr, 0, nullptr);
  const TSet& t = d->set;   // same gen as the kick (checked by the caller)
  tensor_copy(d, cb, true, s.out0, s.w, s.h, t.buf, d->off[1], d->bytes[1]);
  if (d->nt >= 3 && s.out1) tensor_copy(d, cb, true, s.out1, s.w, s.h, t.buf, d->off[2], d->bytes[2]);
  mbar(d, cb, TR, ALL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
// zero-copy is offered when the set is up with in/out0/out1 at 16-byte offsets (uvec4 SSBO indexing); VELA_NPU_ZEROCOPY=0 disables.
static bool zc_ok(Dev* d) {  // d->mtx held
  static const bool off = [] { const char* v = getenv("VELA_NPU_ZEROCOPY"); return v && v[0] == '0'; }();
  return !off && d->state == 1 && d->set.buf && d->nt >= 2 && !(d->off[0] % 16) && !(d->off[1] % 16) && !(d->off[2] % 16);
}
static void zc_info(Dev* d, VelaNpuAck& a) {  // d->mtx held; every ack carries the CURRENT set (vela_npu_ext.h)
  if (!zc_ok(d)) return;
  uint32_t caps; memcpy(&caps, a.reserved, 4); caps |= VELA_NPU_CAP_ZEROCOPY; memcpy(a.reserved, &caps, 4);
  const uint64_t b = (uint64_t)(uintptr_t)d->set.buf; const uint32_t q[3] = {uint32_t(d->off[0] / 16), uint32_t(d->off[1] / 16), uint32_t(d->off[2] / 16)};
  memcpy(a.reserved + 8, &b, 8); memcpy(a.reserved + 16, &d->gen, 4); memcpy(a.reserved + 20, q, 12);
  if (d->set.bda) { caps |= VELA_NPU_CAP_BDA; memcpy(a.reserved, &caps, 4); memcpy(a.reserved + 32, &d->set.bda, 8); }   // device address
  if (d->set.offAlt && !(d->set.offAlt % 16)) { caps |= VELA_NPU_CAP_PINGPONG; memcpy(a.reserved, &caps, 4); const uint32_t qa = uint32_t(d->set.offAlt / 16); memcpy(a.reserved + 40, &qa, 4); }   // input ping-pong
  if (d->set.offOut1 && !(d->set.offOut1 % 16)) { caps |= VELA_NPU_CAP_OUTPINGPONG; memcpy(a.reserved, &caps, 4); const uint32_t qo = uint32_t(d->set.offOut1 / 16); memcpy(a.reserved + 44, &qo, 4); }   // output ping-pong
}
static bool img_ok(const VelaNpuImage& i) { return i.image && i.format == VK_FORMAT_R32_UINT && i.layout == VK_IMAGE_LAYOUT_GENERAL && i.width && i.height; }

// The daemon a PROBE asks for. VELA_NPU_F_GRAPH + cmd.reserved = graph ([a-z0-9_], <= 31) -> fsr4npud_<graph>
// (env VELA_NPUD_GRAPH_NAME overrides, for testing); no flag or a bad name -> VELA_NPUD_NAME or fsr4npud (Lite, unchanged).
static void probe_name(const VelaNpuCmd* c, char nm[64]) {
  const char* base = getenv("VELA_NPUD_NAME"); if (!base || !*base) base = FSR4NPU_DEFAULT_NAME;
  snprintf(nm, 64, "%s", base);
  if (!(c->flags & VELA_NPU_F_GRAPH)) return;
  char g[32]; size_t n = 0;
  for (; n < 31 && c->reserved[n]; n++) { const char ch = (char)c->reserved[n];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return; g[n] = ch; }
  if (!n) return; g[n] = 0;
  const char* ov = getenv("VELA_NPUD_GRAPH_NAME");
  if (ov && *ov) snprintf(nm, 64, "%s", ov); else snprintf(nm, 64, "fsr4npud_%s", g);
}
static void record_marker(Dev* d, VkCommandBuffer cb, const VelaNpuCmd* c, VkBuffer mk, VkDeviceSize off) {
  VelaNpuAck a{}; a.magic = VELA_NPU_MAGIC_ACK; a.version = VELA_NPU_VERSION; a.seq = c->seq; a.layerBuild = VL_BUILD; a.slot = ~0u;
  memcpy(a.reserved, &kCaps, 4);
  int copySlot = -1;
  if (c->version != VELA_NPU_VERSION) a.status = VELA_NPU_ACK_BADARGS;
  else if (c->op == VELA_NPU_OP_PROBE) {
    char nm[64]; probe_name(c, nm); bool first, sw = false;
    { std::lock_guard<std::mutex> l(d->mtx); first = d->state == 0 && d->extOk && ctx_init(d); if (!d->extOk) d->state = -2;
      if (strcmp(nm, d->want)) { const bool had = d->want[0] != 0; memcpy(d->want, nm, sizeof nm);   // another daemon (graph)
        if (had && d->state == 1) go_down(d, "graph switch");
        sw = had && d->state == -1; } }
    if (first || sw) try_up(d, env_int("VELA_NPU_CONNECT_MS", 100));  // the only blocking call, once per device (fork context creation)
    std::lock_guard<std::mutex> l(d->mtx);
    a.status = d->state == 1 ? VELA_NPU_ACK_OK : d->state == -2 ? VELA_NPU_ACK_ERROR_TIMEOUT : VELA_NPU_ACK_NODAEMON;
    if (a.status == VELA_NPU_ACK_OK) zc_info(d, a);
  } else if (c->op == VELA_NPU_OP_WAIT) {
    std::lock_guard<std::mutex> l(d->mtx);
    int si = d->lastKick;
    if (si < 0 || d->slots[si].seq != c->seq || d->slots[si].gen != d->gen || d->state != 1) a.status = VELA_NPU_ACK_NODAEMON;   // nothing kicked for this seq
    else {
      CbRec& rec = d->recs[cb]; if (rec.jobs.empty() && rec.cons.empty()) d->nRec++;
      wait_and_copy_out(d, cb, si); rec.cons.push_back(Consumer{si, c->seq});
      d->lastKick = -1; slot_unref(d, si);                                    // the GPU order now holds the slot (E2 hold)
      a.slot = si; a.status = kPending; copySlot = si;
    }
    zc_info(d, a);
  } else if (c->op != VELA_NPU_OP_UPSCALE) a.status = VELA_NPU_ACK_BADARGS;
  else {
    std::lock_guard<std::mutex> l(d->mtx);
    const bool async = c->flags & VELA_NPU_F_ASYNC, zc = c->flags & VELA_NPU_F_ZEROCOPY; int si = -1;
    if (!(c->flags & VELA_NPU_F_TENSORS) || !img_ok(c->color) || !img_ok(c->output) || (c->motion.image && !img_ok(c->motion)) ||
        c->output.width != c->color.width || c->output.height != c->color.height) a.status = VELA_NPU_ACK_BADARGS;
    else if (d->state == -2 || !d->stBuf) a.status = d->state == -2 ? VELA_NPU_ACK_ERROR_TIMEOUT : VELA_NPU_ACK_NODAEMON;
    else if (d->state != 1) { a.status = VELA_NPU_ACK_NODAEMON; d->noDaemon++; }
    else if (zc && (c->pad0 != d->gen || !zc_ok(d))) a.status = VELA_NPU_ACK_BADARGS;   // stale buffer (reconnect): GPU network this frame
    else if (d->slow || (si = slot_take(d)) < 0) a.status = VELA_NPU_ACK_BUSY;   // slow execute running: GPU network, no kick
    else {
      Slot& s = d->slots[si]; CbRec& rec = d->recs[cb]; if (rec.jobs.empty() && rec.cons.empty()) d->nRec++;
      if (d->lastKick >= 0) { slot_unref(d, d->lastKick); d->lastKick = -1; }   // an older kick nobody waited for
      // copy-in: the fork's pack wrote the input image; release everything to the HOST domain; E1
      if (zc) mbar(d, cb, ALL, HOST, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);   // zero-copy: the pack wrote the dma-buf
      else {
      mbar(d, cb, ALL, TR, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      tensor_copy(d, cb, false, (VkImage)c->color.image, c->color.width, c->color.height, d->set.buf, d->off[0], d->bytes[0]);
      mbar(d, cb, TR | ALL, HOST, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
      }
      d->CmdSetEvent(cb, s.e1, ALL);
      s.refs = 1; s.prod = P_RECORDED; s.seq = c->seq; s.gen = d->gen;
      s.out0 = (VkImage)c->output.image; s.out1 = (VkImage)c->motion.image; s.w = c->output.width; s.h = c->output.height; s.zc = zc;
      rec.jobs.push_back(Job{si, c->seq, d->gen, 0, 0, async});
      if (zc && (c->flags & VELA_NPU_F_INSLOT) && d->set.offAlt) rec.jobs.back().in = c->reserved[32] & 1;   // input ping-pong slot
      if (zc && (c->flags & VELA_NPU_F_OUTSLOT) && d->set.offOut1) rec.jobs.back().out = c->reserved[33] & 1;   // output ping-pong slot
      a.slot = si;
      if (!async) { wait_and_copy_out(d, cb, si); a.status = kPending; copySlot = si; rec.cons.push_back(Consumer{si, c->seq}); }
      else { a.status = VELA_NPU_ACK_OK; d->lastKick = si; s.refs++; }        // "kicked"; hold until its OP_WAIT
    }
    zc_info(d, a);
  }
  if (mk) {
    d->CmdUpdateBuffer(cb, mk, off, sizeof a, &a);
    if (copySlot >= 0) {   // FINAL status + HTP us, from the worker's status slot, after the wait (GPU timeline)
      VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.buffer = mk; bb.offset = off; bb.size = sizeof a;
      d->CmdPipelineBarrier(cb, TR, TR, 0, 0, nullptr, 1, &bb, 0, nullptr);
      VkBufferCopy r[2] = {{uint64_t(copySlot) * 64, off + offsetof(VelaNpuAck, status), 4}, {uint64_t(copySlot) * 64 + 4, off + offsetof(VelaNpuAck, reserved) + 4, 4}};
      d->CmdCopyBuffer(cb, d->stBuf, mk, 2, r);
      mbar(d, cb, TR, ALL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }
  }
}

// ---------------------------------------------------------------- hooks
static VKAPI_ATTR void VKAPI_CALL vl_CmdVelaNpuUpscale(VkCommandBuffer cb, const VelaNpuCmd* c) {  // native route, no ack
  Dev* d = dev_of(cb); if (d && c && c->magic == VELA_NPU_MAGIC_CMD) record_marker(d, cb, c, VK_NULL_HANDLE, 0);
}
static VKAPI_ATTR void VKAPI_CALL vl_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void* data) {
  Dev* d = dev_of(cb);
  if (size != sizeof(VelaNpuCmd) || ((const VelaNpuCmd*)data)->magic != VELA_NPU_MAGIC_CMD) { d->CmdUpdateBuffer(cb, dst, off, size, data); return; }
  double t0 = now_ms();
  record_marker(d, cb, (const VelaNpuCmd*)data, dst, off);
  double dt = now_ms() - t0; if (((const VelaNpuCmd*)data)->op == VELA_NPU_OP_PROBE) return;
  std::lock_guard<std::mutex> l(d->mtx); d->markers++; d->sumRec += dt; d->maxRec = std::max(d->maxRec, dt);
}
static void drop_rec(Dev* d, VkCommandBuffer cb) {  // d->mtx held
  auto it = d->recs.find(cb); if (it == d->recs.end()) return;
  if (!it->second.submitted) for (Job& j : it->second.jobs) slot_unref(d, j.slot);
  d->recs.erase(it); d->nRec--;
}
static VKAPI_ATTR VkResult VKAPI_CALL vl_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
  Dev* d = dev_of(cb);
  note_begin(cb);   // remember the begin epoch
  if (d->nRec.load(std::memory_order_relaxed)) { std::lock_guard<std::mutex> l(d->mtx); drop_rec(d, cb); }
  return d->BeginCommandBuffer(cb, bi);
}
static VKAPI_ATTR void VKAPI_CALL vl_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {   // remembers the app's compute pipeline
  Dev* d = dev_of(cb);
  if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) { if (tl_bcb == cb) { tl_pcb = cb; tl_pep = tl_bep; tl_pipe = p; } else if (tl_pcb == cb) tl_pcb = VK_NULL_HANDLE; }
  d->CmdBindPipeline(cb, bp, p);
}
static VKAPI_ATTR VkResult VKAPI_CALL vl_AllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* cbs) {
  Dev* d = dev_of(dev); VkResult r = d->AllocateCommandBuffers(dev, ai, cbs);
  if (r == VK_SUCCESS) { std::lock_guard<std::mutex> l(d->mtx); for (uint32_t i = 0; i < ai->commandBufferCount; i++) d->cbPool[cbs[i]] = ai->commandPool; }
  return r;
}
static VKAPI_ATTR void VKAPI_CALL vl_FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
  Dev* d = dev_of(dev);
  { std::lock_guard<std::mutex> l(d->mtx); for (uint32_t i = 0; i < n; i++) if (cbs[i]) { d->cbPool.erase(cbs[i]); if (d->nRec) drop_rec(d, cbs[i]); } }
  d->FreeCommandBuffers(dev, pool, n, cbs);
}
static VKAPI_ATTR VkResult VKAPI_CALL vl_ResetCommandPool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags f) {
  Dev* d = dev_of(dev);
  if (d->nRec.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> l(d->mtx); std::vector<VkCommandBuffer> v;
    for (auto& r : d->recs) { auto p = d->cbPool.find(r.first); if (p != d->cbPool.end() && p->second == pool) v.push_back(r.first); }
    for (auto cb : v) drop_rec(d, cb);
  }
  return d->ResetCommandPool(dev, pool, f);
}
// Before the driver submit: a wait whose producer was never submitted (dropped / reused) is released now (status NODAEMON).
// After: queue the producer jobs for the worker.
template <class GetCb> static VkResult submit_common(Dev* d, uint32_t n, GetCb cbs, VkResult (*call)(void*), void* ctx) {
  if (!d->nRec.load(std::memory_order_relaxed)) return call(ctx);
  std::vector<std::pair<VkCommandBuffer, CbRec*>> mine; std::vector<int> forcedSlots;
  {
    std::lock_guard<std::mutex> l(d->mtx); std::vector<int> batchProd;
    for (uint32_t i = 0; i < n; i++) cbs(i, [&](VkCommandBuffer cb) {
      auto it = d->recs.find(cb); if (it == d->recs.end()) return;
      for (Job& j : it->second.jobs) { batchProd.push_back(j.slot); put_arm(d, j.slot); }   // x = 1 before the GPU can reach the wait
      for (Consumer& c : it->second.cons) {
        Slot& s = d->slots[c.slot]; bool inBatch = std::find(batchProd.begin(), batchProd.end(), c.slot) != batchProd.end();
        if (!inBatch && (s.seq != c.seq || s.prod == P_RECORDED || s.prod == P_DROPPED)) {
          put_status(d, c.slot, VELA_NPU_ACK_NODAEMON, 0); put_done(d, c.slot); d->SetEvent(d->dev, s.e2); d->forced++;
          if (!s.e2Set) { s.e2Set = true; s.e2After = ~0ull; s.refs++; forcedSlots.push_back(c.slot); }
        }
      }
      mine.push_back({cb, &it->second});
    });
  }
  VkResult r = call(ctx);
  std::lock_guard<std::mutex> l(d->mtx); double t = now_ms(); bool any = false;
  if (r == VK_ERROR_DEVICE_LOST && d->state != -2) { go_down(d, "device lost at submit"); d->state = -2; }
  for (auto& m : mine) {
    auto it = d->recs.find(m.first); if (it == d->recs.end()) continue;
    for (Job j : it->second.jobs)
      if (r == VK_SUCCESS) { j.tSubmit = t; j.qn = ++d->qCounter; d->slots[j.slot].refs++; d->slots[j.slot].prod = P_QUEUED;
        if (d->slow) { put_release(d, j.slot); j.released = true; }   // recorded before a slow execute began: GPU network
        d->q.push_back(j); any = true; }
    if (r == VK_SUCCESS && !it->second.submitted) { it->second.submitted = true; for (Job& j : it->second.jobs) slot_unref(d, j.slot); }
  }
  for (int i : forcedSlots) d->slots[i].e2After = d->qCounter;   // proof: E1 of a job from a later submit
  if (any) d->cv.notify_one();
  return r;
}
struct S1 { Dev* d; VkQueue q; uint32_t n; const VkSubmitInfo* si; VkFence f; };
struct S2 { Dev* d; VkQueue q; uint32_t n; const VkSubmitInfo2* si; VkFence f; PFN_vkQueueSubmit2 fn; };
static VKAPI_ATTR VkResult VKAPI_CALL vl_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
  Dev* d = dev_of(q); S1 c{d, q, n, si, f};
  return submit_common(d, n, [&](uint32_t i, auto fn) { for (uint32_t k = 0; k < si[i].commandBufferCount; k++) fn(si[i].pCommandBuffers[k]); },
                       [](void* p) { S1* s = (S1*)p; return s->d->QueueSubmit(s->q, s->n, s->si, s->f); }, &c);
}
static VkResult submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f, bool khr) {
  Dev* d = dev_of(q); S2 c{d, q, n, si, f, khr ? d->QueueSubmit2KHR : d->QueueSubmit2};
  return submit_common(d, n, [&](uint32_t i, auto fn) { for (uint32_t k = 0; k < si[i].commandBufferInfoCount; k++) fn(si[i].pCommandBufferInfos[k].commandBuffer); },
                       [](void* p) { S2* s = (S2*)p; return s->fn(s->q, s->n, s->si, s->f); }, &c);
}
static VKAPI_ATTR VkResult VKAPI_CALL vl_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) { return submit2(q, n, si, f, false); }
static VKAPI_ATTR VkResult VKAPI_CALL vl_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) { return submit2(q, n, si, f, true); }

static VKAPI_ATTR void VKAPI_CALL vl_DestroyDevice(VkDevice dev, const VkAllocationCallbacks* alloc) {
  Dev* d = dev_of(dev); if (!d) return;
  if (d->worker.joinable()) {
    { std::lock_guard<std::mutex> l(d->mtx); d->quit = true; } d->cv.notify_all(); d->worker.join();
  }
  if (d->on && d->stBuf) {
    for (int i = 0; i < NSLOT; i++) { put_done(d, i); if (d->slots[i].e2) d->SetEvent(dev, d->slots[i].e2); }  // release any GPU wait still pending
    line_l4(d); line_bw(d); line_slow(d);
    vlog("stats dev %p: markers %ld (record mean %.4f max %.3f ms) | jobs %ld ok %ld | E1 timeouts %ld, NPU errors %ld, no-daemon %ld, forced releases %ld, reconnects %ld | "
         "submit->E1 seen mean %.3f max %.3f ms | kick->done mean %.3f max %.3f ms", (void*)dev, d->markers, d->markers ? d->sumRec / d->markers : 0, d->maxRec,
         d->jobs, d->okJobs, d->e1Timeouts, d->npuErrors, d->noDaemon, d->forced, d->reconnects,
         d->okJobs ? d->sumE1 / d->okJobs : 0, d->maxE1, d->okJobs ? d->sumNpu / d->okJobs : 0, d->maxNpu);
  }
  if (d->DeviceWaitIdle) d->DeviceWaitIdle(dev);
  for (Slot& s : d->slots) { if (s.e1) d->DestroyEvent(dev, s.e1, nullptr); if (s.e2) d->DestroyEvent(dev, s.e2, nullptr); }
  bw_free(d);   // bounded wait resources
  set_free(d, d->set);
  for (auto& s : d->retired) set_free(d, s);
  if (d->stBuf) d->DestroyBuffer(dev, d->stBuf, nullptr);
  if (d->stMem) d->FreeMemory(dev, d->stMem, nullptr);
  PFN_vkDestroyDevice next = d->DestroyDevice;
  for (auto& e : g_dev) if (e.key.load() == key_of(dev)) { e.key.store(nullptr); e.d = nullptr; }
  delete d; next(dev, alloc);
}

// ---------------------------------------------------------------- instance / device plumbing
static VKAPI_ATTR VkResult VKAPI_CALL vl_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
  auto* li = (VkLayerInstanceCreateInfo*)ci->pNext;
  while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO)) li = (VkLayerInstanceCreateInfo*)li->pNext;
  if (!li) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr; li->u.pLayerInfo = li->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult r = create(ci, a, out); if (r != VK_SUCCESS) return r;
  { std::lock_guard<std::mutex> l(g_mtx); g_inst[key_of(*out)] = new Inst{gipa, *out}; }
  static std::atomic<bool> once{false};
  if (enabled() && !once.exchange(true)) vline("loaded v%u", VL_BUILD);
  return r;
}
static VKAPI_ATTR void VKAPI_CALL vl_DestroyInstance(VkInstance inst, const VkAllocationCallbacks* a) {
  Inst* in; { std::lock_guard<std::mutex> l(g_mtx); in = g_inst[key_of(inst)]; g_inst.erase(key_of(inst)); }
  if (!in) return;
  auto fn = (PFN_vkDestroyInstance)in->gipa(inst, "vkDestroyInstance"); delete in; fn(inst, a);
}
static bool bda_enabled(const VkDeviceCreateInfo* ci) {  // the app enabled bufferDeviceAddress (vkd3d-proton and fsr4vk do)
  for (auto* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext) {
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES && ((const VkPhysicalDeviceVulkan12Features*)p)->bufferDeviceAddress) return true;
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES && ((const VkPhysicalDeviceBufferDeviceAddressFeatures*)p)->bufferDeviceAddress) return true;
  }
  return false;
}
static VKAPI_ATTR VkResult VKAPI_CALL vl_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
  auto* li = (VkLayerDeviceCreateInfo*)ci->pNext;
  while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO)) li = (VkLayerDeviceCreateInfo*)li->pNext;
  if (!li) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr; li->u.pLayerInfo = li->u.pLayerInfo->pNext;
  Inst* in = nullptr; { std::lock_guard<std::mutex> l(g_mtx); auto it = g_inst.find(key_of(pd)); if (it != g_inst.end()) in = it->second; }
  if (!in) return VK_ERROR_INITIALIZATION_FAILED;
  const bool on = enabled();
  std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
  int have = 0;
  if (on) {  // add the dma-buf import extensions when the driver has them and the app did not enable them (vkd3d does not)
    auto enumExt = (PFN_vkEnumerateDeviceExtensionProperties)gipa(in->inst, "vkEnumerateDeviceExtensionProperties");
    uint32_t ne = 0; enumExt(pd, nullptr, &ne, nullptr); std::vector<VkExtensionProperties> ep(ne); enumExt(pd, nullptr, &ne, ep.data());
    const char* want[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    for (const char* w : want) {
      bool sup = false, en = false;
      for (auto& e : ep) sup |= !strcmp(e.extensionName, w);
      for (auto* e : exts) en |= !strcmp(e, w);
      if (sup && !en) exts.push_back(w);
      have += sup;
    }
  }
  VkDeviceCreateInfo dci = *ci; dci.enabledExtensionCount = uint32_t(exts.size()); dci.ppEnabledExtensionNames = exts.data();
  auto create = (PFN_vkCreateDevice)gipa(in->inst, "vkCreateDevice");
  VkResult r = create(pd, &dci, a, out); if (r != VK_SUCCESS) return r;
  Dev* d = new Dev; d->dev = *out; d->gdpa = gdpa; d->on = on; d->extOk = have == 2; d->bda = bda_enabled(ci);
  ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(in->inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &d->mp);
#define F(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
  F(DestroyDevice) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(AllocateMemory) F(FreeMemory)
  F(BindBufferMemory) F(MapMemory) F(GetMemoryFdPropertiesKHR) F(CreateEvent) F(DestroyEvent) F(SetEvent) F(ResetEvent)
  F(GetEventStatus) F(CmdSetEvent) F(CmdResetEvent) F(CmdWaitEvents) F(CmdPipelineBarrier) F(CmdUpdateBuffer) F(CmdCopyBuffer)
  F(CmdCopyImageToBuffer) F(CmdCopyBufferToImage)
  F(QueueSubmit) F(QueueSubmit2) F(QueueSubmit2KHR) F(BeginCommandBuffer) F(AllocateCommandBuffers) F(FreeCommandBuffers)
  F(ResetCommandPool) F(DeviceWaitIdle) F(GetBufferDeviceAddress)
  F(CreateShaderModule) F(DestroyShaderModule) F(CreatePipelineLayout) F(DestroyPipelineLayout) F(CreateComputePipelines) F(DestroyPipeline)
  F(CmdBindPipeline) F(CmdDispatchIndirect)
#undef F
  if (!d->GetMemoryFdPropertiesKHR) d->extOk = false;
  bool placed = false;
  { std::lock_guard<std::mutex> l(g_mtx); for (auto& e : g_dev) if (!e.key.load()) { e.d = d; e.key.store(key_of(*out), std::memory_order_release); placed = true; break; } }
  if (!placed) { d->on = false; vlog("more than 16 devices: device %p not tracked", (void*)*out); }  // lookup fails -> GDPA passes through
  if (on) vlog("device %p created (layer build %u): dma-buf import %s", (void*)*out, VL_BUILD, d->extOk ? "on" : "MISSING -> no NPU");
  return r;
}

VL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vl_GetDeviceProcAddr(VkDevice dev, const char* name) {
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vl_GetDeviceProcAddr;
  Dev* d = dev_of(dev); if (!d) return nullptr;
  if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)vl_DestroyDevice;
  if (d->on) {  // VELA_NPU=0 -> every other call goes straight to the driver (zero overhead)
#define H(n, f) if (!strcmp(name, n)) return (PFN_vkVoidFunction)(f);
    H(VELA_NPU_ENTRY_NAME, vl_CmdVelaNpuUpscale)  // private: answered before asking the next layer
    H("vkCmdUpdateBuffer", vl_CmdUpdateBuffer) H("vkQueueSubmit", vl_QueueSubmit) H("vkBeginCommandBuffer", vl_BeginCommandBuffer)
    H("vkCmdBindPipeline", vl_CmdBindPipeline)   // remembers the app's compute pipeline
    H("vkAllocateCommandBuffers", vl_AllocateCommandBuffers) H("vkFreeCommandBuffers", vl_FreeCommandBuffers) H("vkResetCommandPool", vl_ResetCommandPool)
    if (!strcmp(name, "vkQueueSubmit2")) return d->QueueSubmit2 ? (PFN_vkVoidFunction)vl_QueueSubmit2 : nullptr;
    if (!strcmp(name, "vkQueueSubmit2KHR")) return d->QueueSubmit2KHR ? (PFN_vkVoidFunction)vl_QueueSubmit2KHR : nullptr;
#undef H
  }
  return d->gdpa(dev, name);
}
VL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vl_GetInstanceProcAddr(VkInstance inst, const char* name) {
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vl_GetInstanceProcAddr;
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)vl_CreateInstance;
  if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)vl_DestroyInstance;
  if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)vl_CreateDevice;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vl_GetDeviceProcAddr;
  if (!inst) return nullptr;
  Inst* in; { std::lock_guard<std::mutex> l(g_mtx); auto it = g_inst.find(key_of(inst)); in = it == g_inst.end() ? nullptr : it->second; }
  return in ? in->gipa(inst, name) : nullptr;
}
VL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
  if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
  v->pfnGetInstanceProcAddr = vl_GetInstanceProcAddr; v->pfnGetDeviceProcAddr = vl_GetDeviceProcAddr; v->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
