// SPDX-License-Identifier: GPL-3.0-or-later WITH AdditionRef-Vela-Qualcomm-QNN-linking-exception
// Copyright (C) 2026 the Vela author
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program (the file LICENSE at the top
// of the Vela repository). If not, see <https://www.gnu.org/licenses/>.
//
// Additional permission under GNU GPL version 3 section 7
//
// If you modify this Program, or any covered work, by linking or combining it with the Qualcomm AI Engine Direct (QNN)
// runtime libraries or the Qualcomm AI Engine Direct SDK header files (or a modified version of them), containing parts
// covered by the terms of the Qualcomm AI Stack License, the licensors of this Program grant you additional permission to
// convey the resulting work. This includes loading those libraries at run time (for example with dlopen()).
//
// fsr4npud: native aarch64 NPU helper daemon for the FSR 4 networks (Hexagon HTP). Runs as vela-npu.service (systemd Type=notify).
// One process serves every graph, so all of them share one cDSP fastrpc session (the cDSP has only 7). It listens on one abstract
// AF_UNIX SOCK_SEQPACKET socket per graph (protocol: fsr4npu_proto.h). Clients pass dma-buf fds (or ask the daemon to allocate one
// shared buffer), the daemon registers them with QNN as QNN_HTP_MEM_SHARED_BUFFER (the zero-copy path) and runs QnnGraph_execute per
// frame request, one at a time.
// Graph life cycle: every socket exists from the start. HELLO and ALLOC answer from the sizes in the graph file (no context needed), so
// connecting never waits for a load. The first client starts QnnContext_createFromBinary on the loader thread; the client's mem handles
// are registered and its kick is read when the load ends, so until then it sees a slow execute and its frames run on the GPU network. A
// graph without a client stays loaded for --graph-idle-ms, at most --max-loaded stay loaded (least recently used goes first), and a
// graph with a client is never unloaded. A bad or missing file fails only its own socket and is tried again after --graph-retry-ms.
// Features: --alloc auto (rpcmem on the kernel system dma-heap, else a Vulkan-exported buffer); a CDSP power vote held only
// while a client is connected; a soft and a hard execute timeout (a slow execute drops that client, only a stuck one restarts
// the daemon); ping-pong input and output slots (ALLOC F_PINGPONG / F_PINGPONG_OUT; kick bit 32 / 33, RUN v[0] bit 0 / 1);
// clean SIGTERM; --allow-gid; a listener health check.
// --backend stub: no QNN, no fastrpc; copies the input tensor into every output (exercises the full IPC/fence/dma-buf path
// without root). A stub graph file is one line: stub BYTES NOUT [EXEC_DELAY_US] [LOAD_MS]. Allocation in stub mode uses the daemon's
// own Turnip device (exportable dma-buf), in qnn mode rpcmem; --alloc memfd is for IPC tests on a machine without dma-bufs.
// Usage: fsr4npud --graphs a,b,c --graph-dir DIR [--socket-prefix fsr4npud_] [--max-loaded 3] [--graph-idle-ms 120000] [--graph-retry-ms 10000]
//                 [--load-hard-timeout-ms 20000]            (or one graph, loaded at the start: --name N --ctx lite_x.bin)
//                 [--backend stub|qnn] [--alloc auto|rpcmem|vk|memfd] [--exec-timeout-ms 2000]
//                 [--exec-hard-timeout-ms 10000] [--power-grace-ms 2000] [--stub-bytes 8294400] [--stub-nout 1]
//                 [--stub-delay-us 0] [--stub-load-ms 0] [--max-clients 4] [--allow-uid U]... [--exit-after-clients K] [--max-seconds S] [--qnn-log L]
#define _GNU_SOURCE 1
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "QnnInterface.h"
#include "System/QnnSystemInterface.h"
#include "HTP/QnnHtpDevice.h"
#include "HTP/QnnHtpPerfInfrastructure.h"
#include "HTP/QnnHtpMem.h"
extern "C" {
#include "fsr4npu_proto.h"
}
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
using namespace std;

static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double T0;
static void logf(const char* fmt, ...) { va_list a; va_start(a, fmt); fprintf(stderr, "[%9.3f] ", (now_ms() - T0) / 1e3); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a); }
static uint64_t fnv64(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
static void sdNotify(const char* msg) {   // systemd Type=notify, no libsystemd dependency
  const char* e = getenv("NOTIFY_SOCKET"); if (!e || !*e || (e[0] != '/' && e[0] != '@')) return;
  int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (s < 0) return;
  sockaddr_un a{}; a.sun_family = AF_UNIX; size_t n = strlen(e); if (n >= sizeof a.sun_path) { close(s); return; }
  memcpy(a.sun_path, e, n); if (a.sun_path[0] == '@') a.sun_path[0] = 0;
  if (sendto(s, msg, strlen(msg), MSG_NOSIGNAL, (sockaddr*)&a, (socklen_t)(offsetof(sockaddr_un, sun_path) + n)) < 0) {}
  close(s); }
static volatile sig_atomic_t gStop = 0;
static void onSig(int) { gStop = 1; }

// ---------------- options
static string optName = FSR4NPU_DEFAULT_NAME, optBackend = "stub", optCtx;
static uint64_t optStubBytes = 8294400; static int optStubNout = 1, optStubDelayUs = 0, optStubLoadMs = 0, optMaxClients = 4, optExitAfter = 0, optMaxSec = 0, optQnnLog = 0;
static vector<uid_t> allowUid; static vector<gid_t> allowGid;
// 3 loaded graphs: a game uses the Performance and Ultra Performance graphs of one output size and may pass through a second size.
static string optGraphs, optGraphDir, optSocketPrefix = "fsr4npud_"; static int optMaxLoaded = 3, optGraphIdleMs = 120000, optGraphRetryMs = 10000, optLoadHardMs = 20000;
// --alloc vk|rpcmem. Without /dev/dma_heap, rpcmem is FASTRPC_IOCTL_ALLOC_DMA_BUFF: the dma-buf's release
// (fastrpc_release -> fastrpc_buf_free) dereferences buf->fl->cctx, and buf->fl is freed with the fastrpc session -> a client
// or GPU import outliving the daemon's session is a use-after-free (seen as a kernel Oops). vk = a Turnip-exported
// dma-buf (msm GEM), mapped into the DSP by QNN's SHARED_BUFFER registration: its lifetime no longer depends on fastrpc.
// Default auto = rpcmem when /dev/dma_heap/system is openable (libcdsprpc then allocates from the kernel system dma-heap;
// in testing: deterministic output, 0 of 119 reruns differed, no Oops in 10 teardowns), verified by the dma-buf exporter name; else vk.
static string optAlloc = "auto"; static bool gAllocVk = false, gAllocReady = true, gMemfd = false; static string gAllocName = "?";
// Hung-HTP watchdog. A QnnGraph_execute blocked in the fastrpc ioctl cannot be cancelled in-process (a graph can wedge the
// DSP session), so on timeout the daemon tells the in-flight client (M_ERROR E_TIMEOUT / done eventfd 2+11), logs, and
// _exits 75; process exit closes the fastrpc fd so the kernel tears the DSP session down, and systemd (Restart=on-failure)
// starts a fresh daemon + QNN context. Clients see E_TIMEOUT/E_DISCONNECTED and fall back.
// Two stages. Soft (--exec-timeout-ms): the in-flight client is told E_TIMEOUT at once (falls back to the GPU); if the
// execute then returns, only that client is dropped (its late result can no longer be matched) and the daemon keeps its
// DSP session (no teardown, no restart). Hard (--exec-hard-timeout-ms): the execute never returned -> _exit 75.
// 3 soft timeouts within 120 s = a sick session -> ordered exit 75 (fresh session via systemd).
static int optExecTimeoutMs = 2000, optExecHardMs = 10000, gExitCode = 0;
static std::atomic<uint64_t> gExecId{0}, gExecTimedOutId{0}; static double gSoftT[3] = {-1e18, -1e18, -1e18}; static int gSoftN = 0;
// Power vote: top corner (0xA0) + DSP sleep disabled only while >= 1 client is connected; released --power-grace-ms
// after the last client left (a layer reconnect on swapchain re-creation does not flap the vote).
static int optPowerGraceMs = 2000; static bool gVoted = false; static double gIdleSince = 0; static uint32_t gPcid = 0;
static QnnHtpDevice_PerfInfrastructure_t* gPerf = nullptr;
static std::atomic<double> gExecStart{0}; static std::atomic<int> gExecSock{-1}, gExecDone{-1}; static std::atomic<uint64_t> gExecSeq{0};

// ---------------- graphs (both backends)
struct Graph {
  string sock, path, name = "stub"; int ls = -1; bool keep = false;   // keep: --name mode, loaded at the start and never unloaded
  enum State { IDLE, LOADING, LOADED, UNLOADING, FAILED } st = IDLE; bool infoReady = false, reload = false, evictRetried = false; double retryAt = 0, lastUse = 0;
  uint32_t nt = 0; uint64_t bytes[FSR4NPU_MAXT] = {}; int stubDelayUs = -1; int pins = 0;   // pins: connected clients
  // valid while LOADED; the loader thread owns them during LOADING and UNLOADING
  Qnn_GraphHandle_t G = nullptr; Qnn_ContextHandle_t CTX = nullptr; QnnSystemContext_Handle_t sc = nullptr; vector<uint8_t> bin; Qnn_Tensor_t T[FSR4NPU_MAXT];
};
static vector<Graph*> graphs; static bool isQnn = false;

// ---------------- QNN state
// The QNN documents do not say that QnnContext_createFromBinary or QnnContext_free may run beside QnnGraph_execute (only QnnBackend_create is called
// thread-safe), so every call into the backend holds gQnnMu. A frame that meets a load waits for it and goes to the GPU network by the slow-execute rule.
static QNN_INTERFACE_VER_TYPE Q{}; static QNN_SYSTEM_INTERFACE_VER_TYPE S{}; static Qnn_BackendHandle_t BE = nullptr; static Qnn_DeviceHandle_t DV = nullptr; static std::mutex gQnnMu;
typedef void (*RInit)(); typedef void (*RDeinit)(); typedef void* (*RAlloc)(int, uint32_t, int); typedef int (*RFd)(void*); typedef void (*RFree)(void*);
typedef int (*RegDma)(int, uint32_t);
static RAlloc rAlloc = nullptr; static RFd rToFd = nullptr; static RFree rFree = nullptr; static RDeinit rDeinit = nullptr; static RegDma rRegDma = nullptr;

static int dtypeBytes(Qnn_DataType_t d) { unsigned x = (unsigned)d & 0xff; int bits = ((x >> 4) & 0xf) * 10 + (x & 0xf); return bits >= 8 ? bits / 8 : 1; }
static uint64_t tensorBytes(const Qnn_Tensor_t& t) { uint64_t n = dtypeBytes(t.v1.dataType); for (uint32_t k = 0; k < t.v1.rank; k++) n *= t.v1.dimensions[k]; return n; }

#define QK(x) do { Qnn_ErrorHandle_t e_ = (x); if (e_ != QNN_SUCCESS) { logf("FAIL %s = 0x%lx", #x, (unsigned long)e_); return false; } } while (0)
static bool qnnOpen() {   // backend, device, power id, rpcmem: the fastrpc session of this process; no graph
  double t0 = now_ms();
  void* hs = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_LOCAL); void* hb = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_LOCAL); void* hr = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_GLOBAL);
  if (!hs || !hb || !hr) { logf("FAIL dlopen %s", dlerror()); return false; }
  typedef Qnn_ErrorHandle_t (*SysProv)(const QnnSystemInterface_t***, uint32_t*); typedef Qnn_ErrorHandle_t (*Prov)(const QnnInterface_t***, uint32_t*);
  const QnnSystemInterface_t** sp; const QnnInterface_t** ip; uint32_t ns = 0, ni = 0;
  QK(((SysProv)dlsym(hs, "QnnSystemInterface_getProviders"))(&sp, &ns)); QK(((Prov)dlsym(hb, "QnnInterface_getProviders"))(&ip, &ni));
  S = sp[0]->QNN_SYSTEM_INTERFACE_VER_NAME; Q = ip[0]->QNN_INTERFACE_VER_NAME;
  Qnn_LogHandle_t lgh = nullptr;
  if (optQnnLog > 0) Q.logCreate([](const char* fmt, QnnLog_Level_t lv, uint64_t, va_list a) { fprintf(stderr, "QNNLOG%d ", (int)lv); vfprintf(stderr, fmt, a); size_t L = strlen(fmt); if (!L || fmt[L - 1] != '\n') fputc('\n', stderr); }, (QnnLog_Level_t)optQnnLog, &lgh);
  QK(Q.backendCreate(lgh, nullptr, &BE)); Q.deviceCreate(nullptr, nullptr, &DV);
  { QnnDevice_Infrastructure_t inf = nullptr;   // create the power config id only; the vote follows the clients (powerVote)
    if (Q.deviceGetInfrastructure(&inf) == QNN_SUCCESS && inf) { gPerf = &((QnnHtpDevice_Infrastructure_t*)inf)->perfInfra; QK(gPerf->createPowerConfigId(0, 0, &gPcid));
      logf("power: config id %u, no vote until a client connects", gPcid); } else logf("power: no HTP perf infrastructure, no votes"); }
  ((RInit)dlsym(hr, "rpcmem_init"))(); rAlloc = (RAlloc)dlsym(hr, "rpcmem_alloc"); rToFd = (RFd)dlsym(hr, "rpcmem_to_fd"); rFree = (RFree)dlsym(hr, "rpcmem_free");
  rDeinit = (RDeinit)dlsym(hr, "rpcmem_deinit"); rRegDma = (RegDma)dlsym(hr, "remote_register_dma_handle");
  logf("backend open %.1f ms; rpcmem alloc %p to_fd %p free %p remote_register_dma_handle %p", now_ms() - t0, (void*)rAlloc, (void*)rToFd, (void*)rFree, (void*)rRegDma);
  return rAlloc && rToFd;
}
// What a graph file says, with no context and no DSP. sc stays alive with T[]: their dimensions point into it.
struct Parsed { vector<uint8_t> bin; QnnSystemContext_Handle_t sc = nullptr; Qnn_Tensor_t T[FSR4NPU_MAXT]; uint32_t nt = 0; uint64_t bytes[FSR4NPU_MAXT] = {}; string name; int stubDelayUs = -1, stubLoadMs = 0; };
static bool parseStub(const Graph* g, Parsed& o) {
  unsigned long long b = optStubBytes; int nout = optStubNout; o.stubLoadMs = optStubLoadMs; o.name = g->name;
  if (!g->path.empty()) { FILE* f = fopen(g->path.c_str(), "r"); if (!f) { logf("FAIL cannot open graph file %s", g->path.c_str()); return false; }
    int n = fscanf(f, "stub %llu %d %d %d", &b, &nout, &o.stubDelayUs, &o.stubLoadMs); fclose(f); if (n < 2) { logf("FAIL %s is not a stub graph file", g->path.c_str()); return false; } }
  if (nout < 1 || nout > FSR4NPU_MAXT - 1) return false;
  o.nt = 1 + nout; for (uint32_t k = 0; k < o.nt; k++) o.bytes[k] = b; return true; }
static bool parseGraph(const Graph* g, Parsed& o) {
  if (!isQnn) return parseStub(g, o);
  FILE* f = fopen(g->path.c_str(), "rb"); if (!f) { logf("FAIL cannot open context %s", g->path.c_str()); return false; }
  fseek(f, 0, SEEK_END); size_t bsz = ftell(f); fseek(f, 0, SEEK_SET); o.bin.resize(bsz);
  if (fread(o.bin.data(), 1, bsz, f) != bsz) { fclose(f); logf("FAIL cannot read context %s", g->path.c_str()); return false; } fclose(f);
  QK(S.systemContextCreate(&o.sc)); const QnnSystemContext_BinaryInfo_t* bi = nullptr; Qnn_ContextBinarySize_t bis = 0;
  QK(S.systemContextGetBinaryInfo(o.sc, o.bin.data(), bsz, &bi, &bis));
  const QnnSystemContext_GraphInfo_t* gi = bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1 ? bi->contextBinaryInfoV1.graphs : bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2 ? bi->contextBinaryInfoV2.graphs : bi->contextBinaryInfoV3.graphs;
  const char* gname; const Qnn_Tensor_t *gin, *gout; uint32_t nOut;
  if (gi->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) { gname = gi->graphInfoV1.graphName; gin = gi->graphInfoV1.graphInputs; gout = gi->graphInfoV1.graphOutputs; nOut = gi->graphInfoV1.numGraphOutputs; }
  else if (gi->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) { gname = gi->graphInfoV2.graphName; gin = gi->graphInfoV2.graphInputs; gout = gi->graphInfoV2.graphOutputs; nOut = gi->graphInfoV2.numGraphOutputs; }
  else { gname = gi->graphInfoV3.graphName; gin = gi->graphInfoV3.graphInputs; gout = gi->graphInfoV3.graphOutputs; nOut = gi->graphInfoV3.numGraphOutputs; }
  if (nOut < 1 || nOut > FSR4NPU_MAXT - 1) { logf("FAIL graph has %u outputs", nOut); return false; }
  o.T[0] = gin[0];
  if (nOut == 2) { int iu = strcmp(gout[0].v1.name, "p3u_o") == 0 ? 0 : 1; o.T[1] = gout[iu]; o.T[2] = gout[1 - iu]; }   // with two outputs, the one named p3u_o goes first
  else for (uint32_t k = 0; k < nOut; k++) o.T[1 + k] = gout[k];
  o.nt = 1 + nOut; o.name = gname; for (uint32_t k = 0; k < o.nt; k++) o.bytes[k] = tensorBytes(o.T[k]);
  return true;
}
static void parsedFree(Parsed& o) { if (o.sc) S.systemContextFree(o.sc); o.sc = nullptr; vector<uint8_t>().swap(o.bin); }

static void powerVote(bool on, const char* why) {
  if (!gPerf || on == gVoted) return;
  std::lock_guard<std::mutex> ql(gQnnMu);
  QnnHtpPerfInfrastructure_PowerConfig_t d; memset(&d, 0, sizeof d); d.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3; auto& v = d.dcvsV3Config;
  v.contextId = gPcid; v.setDcvsEnable = 1; v.setSleepLatency = 1; v.setSleepDisable = 1; v.setBusParams = 1; v.setCoreParams = 1;
  if (on) { v.dcvsEnable = 0; v.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_PERFORMANCE_MODE; v.sleepLatency = 40; v.sleepDisable = 1;   // maximum-performance settings
    v.busVoltageCornerMin = v.busVoltageCornerTarget = v.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
    v.coreVoltageCornerMin = v.coreVoltageCornerTarget = v.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER; }
  else { v.dcvsEnable = 1; v.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_POWER_SAVER_MODE; v.sleepLatency = 1000; v.sleepDisable = 0;   // no corner vote: DSP DCVS decides
    v.busVoltageCornerMin = v.busVoltageCornerTarget = v.busVoltageCornerMax = DCVS_VOLTAGE_CORNER_DISABLE;
    v.coreVoltageCornerMin = v.coreVoltageCornerTarget = v.coreVoltageCornerMax = DCVS_VOLTAGE_CORNER_DISABLE; }
  const QnnHtpPerfInfrastructure_PowerConfig_t* c1[] = {&d, nullptr}; Qnn_ErrorHandle_t e = gPerf->setPowerConfig(gPcid, c1);
  if (e != QNN_SUCCESS && !on) {   // release refused: drop the whole vote client instead
    Qnn_ErrorHandle_t e2 = gPerf->destroyPowerConfigId(gPcid); uint32_t np = 0; Qnn_ErrorHandle_t e3 = gPerf->createPowerConfigId(0, 0, &np); if (e3 == QNN_SUCCESS) gPcid = np;
    logf("power_vote release rc 0x%lx -> destroyPowerConfigId rc 0x%lx, new id rc 0x%lx", (unsigned long)e, (unsigned long)e2, (unsigned long)e3); e = e2; }
  if (e == QNN_SUCCESS) gVoted = on;
  logf("power_vote %s (%s) rc 0x%lx", on ? "ON top corner 0xA0, DCVS off, sleep disabled" : "OFF released (DCVS on, power saver, sleep allowed)", why, (unsigned long)e); }
static string dmabufExporter(int fd) {   // /proc/self/fdinfo exp_name of a dma-buf: system = kernel dma-heap, else the fastrpc exporter
  char p[64]; snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd); FILE* f = fopen(p, "r"); if (!f) return ""; char l[256]; string r;
  while (fgets(l, sizeof l, f)) if (!strncmp(l, "exp_name:", 9)) { char* q = l + 9; while (*q == ' ' || *q == '\t') q++; r = q; while (!r.empty() && (r.back() == '\n' || r.back() == ' ')) r.pop_back(); break; }
  fclose(f); return r; }

// ---------------- stub-mode allocator: the daemon's own Turnip device, exportable dma-buf (no root)
struct VkA { PFN_vkGetInstanceProcAddr gipa = nullptr; VkInstance inst = 0; VkPhysicalDevice pd = 0; VkDevice dev = 0; VkPhysicalDeviceMemoryProperties mp{};
  PFN_vkCreateBuffer createBuffer; PFN_vkDestroyBuffer destroyBuffer; PFN_vkGetBufferMemoryRequirements req; PFN_vkAllocateMemory alloc; PFN_vkFreeMemory freeMem; PFN_vkGetMemoryFdKHR getFd; bool ok = false; };
static VkA VA;
static bool vkInit() {
  if (VA.ok) return true;
  void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL); if (!h) { logf("vk: dlopen libvulkan.so.1 failed"); return false; }
  VA.gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
  auto createInst = (PFN_vkCreateInstance)VA.gipa(nullptr, "vkCreateInstance");
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1; ai.pApplicationName = "fsr4npud";
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &ai; if (createInst(&ic, nullptr, &VA.inst) != VK_SUCCESS) return false;
  auto enumPd = (PFN_vkEnumeratePhysicalDevices)VA.gipa(VA.inst, "vkEnumeratePhysicalDevices"); uint32_t n = 1; VkPhysicalDevice pds[1];
  if (enumPd(VA.inst, &n, pds) < 0 || n < 1) return false; VA.pd = pds[0];
  ((PFN_vkGetPhysicalDeviceMemoryProperties)VA.gipa(VA.inst, "vkGetPhysicalDeviceMemoryProperties"))(VA.pd, &VA.mp);
  const char* ext[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"}; float pr = 1.0f;
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &pr;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &q; dc.enabledExtensionCount = 2; dc.ppEnabledExtensionNames = ext;
  if (((PFN_vkCreateDevice)VA.gipa(VA.inst, "vkCreateDevice"))(VA.pd, &dc, nullptr, &VA.dev) != VK_SUCCESS) return false;
  auto gdpa = (PFN_vkGetDeviceProcAddr)VA.gipa(VA.inst, "vkGetDeviceProcAddr");
  VA.createBuffer = (PFN_vkCreateBuffer)gdpa(VA.dev, "vkCreateBuffer"); VA.destroyBuffer = (PFN_vkDestroyBuffer)gdpa(VA.dev, "vkDestroyBuffer");
  VA.req = (PFN_vkGetBufferMemoryRequirements)gdpa(VA.dev, "vkGetBufferMemoryRequirements"); VA.alloc = (PFN_vkAllocateMemory)gdpa(VA.dev, "vkAllocateMemory");
  VA.freeMem = (PFN_vkFreeMemory)gdpa(VA.dev, "vkFreeMemory"); VA.getFd = (PFN_vkGetMemoryFdKHR)gdpa(VA.dev, "vkGetMemoryFdKHR");
  VA.ok = VA.getFd != nullptr; logf("vk allocator ready: %s", VA.ok ? "yes" : "no"); return VA.ok;
}
static int vkAllocFd(uint64_t size, VkDeviceMemory* memOut) {
  if (!vkInit()) return -1;
  VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO}; eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.pNext = &eb; bc.size = size; bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer b; if (VA.createBuffer(VA.dev, &bc, nullptr, &b) != VK_SUCCESS) return -1;
  VkMemoryRequirements mr; VA.req(VA.dev, b, &mr); uint32_t mt = 99;
  for (uint32_t i = 0; i < VA.mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (VA.mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
  VA.destroyBuffer(VA.dev, b, nullptr); if (mt == 99) return -1;
  VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.pNext = &ex; ma.allocationSize = mr.size; ma.memoryTypeIndex = mt;
  VkDeviceMemory m; if (VA.alloc(VA.dev, &ma, nullptr, &m) != VK_SUCCESS) return -1;
  VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR}; gi.memory = m; gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; int fd = -1;
  if (VA.getFd(VA.dev, &gi, &fd) != VK_SUCCESS) { VA.freeMem(VA.dev, m, nullptr); return -1; }
  *memOut = m; return fd;
}

static bool cleanFd(int fd, uint64_t n) {   // zero the buffer, then clean+invalidate it to the point of coherency once, before any device reads it
  void* m = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (m == MAP_FAILED) { logf("cleanFd mmap: %s", strerror(errno)); return false; }
  dma_buf_sync s0{DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW}; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s0); memset(m, 0, n);
  for (uintptr_t x = (uintptr_t)m & ~63ul, e = (uintptr_t)m + n; x < e; x += 64) asm volatile("dc civac, %0" ::"r"(x) : "memory");
  asm volatile("dsb sy" ::: "memory"); dma_buf_sync s1{DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW}; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s1); munmap(m, n); return true; }
// qnn: which allocator? rpcmem is decided at the start. The Vulkan path needs a context for its self-test: it is decided at the first load (allocVkTest), and ALLOC waits until then.
static void allocDecide() {
  gAllocVk = false; string mode = optAlloc;
  if (mode == "memfd") { gAllocName = "rpcmem/test"; logf("allocator: memfd test mode, no dma-buf check"); return; }
  if (mode == "auto") { int h = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (h >= 0) { close(h); mode = "rpcmem"; logf("allocator auto: /dev/dma_heap/system is openable -> rpcmem"); }
    else { logf("allocator auto: /dev/dma_heap/system not openable (%s) -> vk", strerror(errno)); mode = "vk"; } }
  if (mode == "rpcmem") {   // without the heap libcdsprpc silently falls back to FASTRPC_IOCTL_ALLOC_DMA_BUFF (the teardown Oops described at optAlloc): check
    void* b = rAlloc(25, 1, 65536); int fd = b ? rToFd(b) : -1; string ex = fd >= 0 ? dmabufExporter(fd) : string("alloc failed"); if (b) rFree(b);
    if (ex == "system") { gAllocName = "rpcmem/heap"; logf("allocator: rpcmem on the kernel system dma-heap (exporter %s)", ex.c_str()); return; }
    if (optAlloc == "rpcmem") { gAllocName = "rpcmem/fastrpc"; logf("allocator: rpcmem FORCED, exporter '%s' is NOT the system dma-heap (teardown can Oops on 7.2.6)", ex.c_str()); return; }
    logf("allocator auto: rpcmem exporter '%s' is not the system dma-heap -> vk", ex.c_str()); }
  gAllocReady = false; gAllocName = "vk?"; }
static void allocVkTest(Graph* g) {   // can QNN register a Turnip dma-buf as SHARED_BUFFER? decided once
  gAllocReady = true;
  uint64_t n = (g->bytes[0] + 4095) & ~4095ull; VkDeviceMemory vm = 0; int fd = vkAllocFd(n, &vm);
  if (fd < 0) { gAllocName = "rpcmem/fallback"; logf("allocator: vk export failed, using rpcmem (fastrpc-lifetime buffers)"); return; }
  Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
  md.memShape = {g->T[0].v1.rank, g->T[0].v1.dimensions, nullptr}; md.dataType = g->T[0].v1.dataType;
  hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = n; hd.sharedBufferConfig.fd = fd; hd.sharedBufferConfig.offset = 0; md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd;
  Qnn_MemHandle_t mh = nullptr; Qnn_ErrorHandle_t e; { std::lock_guard<std::mutex> ql(gQnnMu); e = Q.memRegister(g->CTX, &md, 1, &mh); if (e == QNN_SUCCESS) Q.memDeRegister(&mh, 1); }
  close(fd); VA.freeMem(VA.dev, vm, nullptr); gAllocVk = e == QNN_SUCCESS; gAllocName = gAllocVk ? "vk" : "rpcmem/fallback";
  logf("allocator: %s (vk SHARED_BUFFER self-test rc 0x%lx)", gAllocVk ? "vk dma-buf" : "rpcmem fallback", (unsigned long)e); }

// ---------------- clients
struct Client {
  int sock = -1, kick = -1, done = -1; uid_t uid = 0; pid_t pid = 0; int id = 0; Graph* g = nullptr; bool registered = false, quiet = false; uint32_t setupFlags = 0;
  int fds[FSR4NPU_MAXT]; uint64_t fdSize[FSR4NPU_MAXT]; void* map[FSR4NPU_MAXT]; int nfd = 0;
  bool ready = false; uint32_t nt = 0; fsr4npu_tdesc t[FSR4NPU_MAXT];
  Qnn_MemHandle_t mh[FSR4NPU_MAXT]; Qnn_Tensor_t qt[FSR4NPU_MAXT]; bool regDma[FSR4NPU_MAXT];
  bool alt = false; fsr4npu_tdesc tAlt{}; Qnn_MemHandle_t mhAlt = nullptr; Qnn_Tensor_t qtAlt{};   // ping-pong input slot 1 (F_PINGPONG)
  bool altOut = false; uint64_t offOut1 = 0; Qnn_MemHandle_t mhOut1[FSR4NPU_MAXT]{}; Qnn_Tensor_t qtOut1[FSR4NPU_MAXT]{};   // output slot 1 (F_PINGPONG_OUT)
  void* rpcBase = nullptr; VkDeviceMemory vkMem = 0;
  void* stMap = nullptr; uint64_t stLen = 0; vector<double> vInv; long stale = 0;   // cached read-only view of the buffer (input invalidation / diagnostics)
  long frames = 0, errors = 0; vector<double> vExec, vFence, vSvc; double tConn = 0; bool timedOut = false;
};
static vector<Client*> clients; static int clientSeq = 0, clientsDone = 0; static bool gExiting = false;
static void trimLoaded();

static string fdLink(int fd) { char p[64], l[160] = {0}; snprintf(p, sizeof p, "/proc/self/fd/%d", fd); if (readlink(p, l, sizeof l - 1) < 0) return ""; return l; }
static void stat3(const char* k, vector<double> v, string& out) { if (v.empty()) return; sort(v.begin(), v.end()); char b[160];
  snprintf(b, sizeof b, " %s p50 %.3f p90 %.3f max %.3f ms", k, v[v.size() / 2], v[v.size() * 9 / 10], v.back()); out += b; }

static void unreg(Client* c) {
  bool anyH = c->mhAlt != nullptr; for (uint32_t k = 0; k < FSR4NPU_MAXT; k++) anyH = anyH || c->mh[k] || c->mhOut1[k];
  std::unique_lock<std::mutex> ql(gQnnMu, std::defer_lock); if (isQnn && anyH) ql.lock();   // a client that never reached its context has nothing to deregister and does not wait for a load
  c->registered = false;
  for (uint32_t k = 0; k < FSR4NPU_MAXT; k++) {
    if (isQnn && c->mh[k]) { Qnn_ErrorHandle_t e = Q.memDeRegister(&c->mh[k], 1); if (e != QNN_SUCCESS) logf("client %d memDeRegister %u rc 0x%lx", c->id, k, (unsigned long)e); c->mh[k] = nullptr; }
  }
  if (isQnn && c->mhAlt) { Qnn_ErrorHandle_t e = Q.memDeRegister(&c->mhAlt, 1); if (e != QNN_SUCCESS) logf("client %d memDeRegister in1 rc 0x%lx", c->id, (unsigned long)e); c->mhAlt = nullptr; }
  for (uint32_t k = 0; k < FSR4NPU_MAXT; k++) if (isQnn && c->mhOut1[k]) { Qnn_ErrorHandle_t e = Q.memDeRegister(&c->mhOut1[k], 1); if (e != QNN_SUCCESS) logf("client %d memDeRegister out1 %u rc 0x%lx", c->id, k, (unsigned long)e); c->mhOut1[k] = nullptr; }
  c->alt = false; c->altOut = false;
  if (ql.owns_lock()) ql.unlock();
  if (c->stMap) { munmap(c->stMap, c->stLen); c->stMap = nullptr; c->stLen = 0; }
  for (int i = 0; i < c->nfd; i++) { if (c->map[i]) munmap(c->map[i], c->fdSize[i]); c->map[i] = nullptr; if (c->fds[i] >= 0 && !(i == 0 && c->rpcBase)) close(c->fds[i]); c->fds[i] = -1; }
  c->nfd = 0;
  if (c->rpcBase && rFree) { rFree(c->rpcBase); c->rpcBase = nullptr; }
  if (c->vkMem && VA.ok) { VA.freeMem(VA.dev, c->vkMem, nullptr); c->vkMem = 0; }
  if (c->kick >= 0) close(c->kick); if (c->done >= 0) close(c->done); c->kick = c->done = -1;
  c->ready = false;
}
static void dropClient(Client* c, const char* why) {
  string s; stat3("exec", c->vExec, s); stat3("fence_wait", c->vFence, s); stat3("service", c->vSvc, s); stat3("inval", c->vInv, s); if (c->stale) { char b[48]; snprintf(b, sizeof b, " stale_frames %ld", c->stale); s += b; }
  if (!c->quiet) logf("client %d of @%s (pid %d uid %u) gone: %s; frames %ld errors %ld over %.1f s;%s", c->id, c->g->sock.c_str(), (int)c->pid, (unsigned)c->uid, why, c->frames, c->errors, (now_ms() - c->tConn) / 1e3, s.c_str());
  Graph* g = c->g; unreg(c); if (c->sock >= 0) close(c->sock); clients.erase(find(clients.begin(), clients.end(), c)); delete c; clientsDone++;
  g->pins--; g->lastUse = now_ms();   // after unreg: every mem handle of a context is gone before the context can be freed
  if (clients.empty()) gIdleSince = now_ms();
  if (!gExiting) trimLoaded();
}
static void msgInit(fsr4npu_msg& m, uint32_t type) { memset(&m, 0, sizeof m); m.magic = FSR4NPU_MAGIC; m.version = FSR4NPU_VERSION; m.type = type; }
static bool sendMsg(Client* c, const fsr4npu_msg& m, const int* fds = nullptr, int nfd = 0) {
  iovec io{(void*)&m, sizeof m}; char cm[CMSG_SPACE(sizeof(int) * FSR4NPU_MAXFD)]; memset(cm, 0, sizeof cm); msghdr mh{}; mh.msg_iov = &io; mh.msg_iovlen = 1;
  if (nfd > 0) { mh.msg_control = cm; mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfd); cmsghdr* h = CMSG_FIRSTHDR(&mh); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int) * nfd); memcpy(CMSG_DATA(h), fds, sizeof(int) * nfd); }
  return sendmsg(c->sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof m;
}
static bool sendErr(Client* c, int st, uint64_t seq, const char* text) { fsr4npu_msg m; msgInit(m, M_ERROR); m.status = st; m.seq = seq; snprintf(m.text, sizeof m.text, "%s", text); if (!c->quiet) logf("client %d error %d: %s", c->id, st, text); c->errors++; return sendMsg(c, m); }

static int registerTensors(Client* c, uint32_t flags) {
  std::unique_lock<std::mutex> ql(gQnnMu, std::defer_lock); if (isQnn) ql.lock();
  for (uint32_t k = 0; k < c->nt; k++) {
    int i = (int)c->t[k].fd_idx;
    if (!isQnn) { if (!c->map[i]) { c->map[i] = mmap(nullptr, c->fdSize[i], PROT_READ | PROT_WRITE, MAP_SHARED, c->fds[i], 0); if (c->map[i] == MAP_FAILED) { c->map[i] = nullptr; logf("client %d mmap fd %d failed: %s", c->id, c->fds[i], strerror(errno)); return E_FD; } } continue; }
    if ((flags & F_REGDMA) && !c->regDma[i]) { int r = rRegDma ? rRegDma(c->fds[i], (uint32_t)c->fdSize[i]) : -1; logf("client %d remote_register_dma_handle fd %d size %llu rc %d", c->id, c->fds[i], (unsigned long long)c->fdSize[i], r); c->regDma[i] = true; }
    Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
    c->qt[k] = c->g->T[k]; md.memShape = {c->g->T[k].v1.rank, c->g->T[k].v1.dimensions, nullptr}; md.dataType = c->g->T[k].v1.dataType;
    hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = c->fdSize[i]; hd.sharedBufferConfig.fd = c->fds[i]; hd.sharedBufferConfig.offset = c->t[k].off;
    md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd; Qnn_MemHandle_t mh = nullptr;
    Qnn_ErrorHandle_t e = Q.memRegister(c->g->CTX, &md, 1, &mh);
    logf("client %d memRegister tensor %u SHARED_BUFFER fd %d size %llu offset %llu rc 0x%lx", c->id, k, c->fds[i], (unsigned long long)c->fdSize[i], (unsigned long long)c->t[k].off, (unsigned long)e);
    if (e != QNN_SUCCESS) return E_REGISTER;
    c->mh[k] = mh; c->qt[k].v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; c->qt[k].v1.memHandle = mh;
  }
  if (c->alt && isQnn) {   // input slot 1 = graph input tensor 0 at tAlt.off of the same buffer
    int i = (int)c->tAlt.fd_idx; Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
    c->qtAlt = c->g->T[0]; md.memShape = {c->g->T[0].v1.rank, c->g->T[0].v1.dimensions, nullptr}; md.dataType = c->g->T[0].v1.dataType;
    hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = c->fdSize[i]; hd.sharedBufferConfig.fd = c->fds[i]; hd.sharedBufferConfig.offset = c->tAlt.off;
    md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd; Qnn_MemHandle_t mh = nullptr; Qnn_ErrorHandle_t e = Q.memRegister(c->g->CTX, &md, 1, &mh);
    logf("client %d memRegister input slot 1 SHARED_BUFFER fd %d offset %llu rc 0x%lx", c->id, c->fds[i], (unsigned long long)c->tAlt.off, (unsigned long)e);
    if (e != QNN_SUCCESS) return E_REGISTER;
    c->mhAlt = mh; c->qtAlt.v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; c->qtAlt.v1.memHandle = mh;
  }
  if (c->altOut && isQnn) for (uint32_t k = 1; k < c->nt; k++) {   // output slot 1 = the graph outputs at offOut1 (+ offsets relative to output 0)
    int i = (int)c->t[k].fd_idx; Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
    c->qtOut1[k] = c->g->T[k]; md.memShape = {c->g->T[k].v1.rank, c->g->T[k].v1.dimensions, nullptr}; md.dataType = c->g->T[k].v1.dataType;
    hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = c->fdSize[i]; hd.sharedBufferConfig.fd = c->fds[i]; hd.sharedBufferConfig.offset = c->offOut1 + (c->t[k].off - c->t[1].off);
    md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd; Qnn_MemHandle_t mh = nullptr; Qnn_ErrorHandle_t e = Q.memRegister(c->g->CTX, &md, 1, &mh);
    logf("client %d memRegister output %u slot 1 SHARED_BUFFER fd %d offset %llu rc 0x%lx", c->id, k, c->fds[i], (unsigned long long)hd.sharedBufferConfig.offset, (unsigned long)e);
    if (e != QNN_SUCCESS) return E_REGISTER;
    c->mhOut1[k] = mh; c->qtOut1[k].v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; c->qtOut1[k].v1.memHandle = mh;
  }
  return S_OK;
}
// Validate + adopt fds/tensors. bufFds: buffer fds owned by now by this function (closed on failure).
static int adopt(Client* c, const fsr4npu_msg& m, int* fds, int nbuf, int kick, int done) {
  int st = S_OK; char why[96] = "";
  if (m.ntensors != c->g->nt) { st = E_SIZE; snprintf(why, sizeof why, "ntensors %u, graph has %u", m.ntensors, c->g->nt); }
  for (int i = 0; i < nbuf && st == S_OK; i++) {
    string l = fdLink(fds[i]); if (!gMemfd && l.find("dmabuf") == string::npos) { st = E_FD; snprintf(why, sizeof why, "fd %d is not a dma-buf (%s)", i, l.c_str()); break; }
    off_t sz = lseek(fds[i], 0, SEEK_END); if (sz <= 0) { st = E_FD; snprintf(why, sizeof why, "fd %d size unknown", i); break; } c->fdSize[i] = (uint64_t)sz; }
  for (uint32_t k = 0; k < m.ntensors && st == S_OK; k++) {
    const fsr4npu_tdesc& t = m.t[k];
    if ((int)t.fd_idx >= nbuf) { st = E_PROTO; snprintf(why, sizeof why, "tensor %u fd_idx %u >= %d fds", k, t.fd_idx, nbuf); break; }
    if (t.bytes && t.bytes != c->g->bytes[k]) { st = E_SIZE; snprintf(why, sizeof why, "tensor %u bytes %llu != graph %llu", k, (unsigned long long)t.bytes, (unsigned long long)c->g->bytes[k]); break; }
    if (t.off % FSR4NPU_ALIGN) { st = E_SIZE; snprintf(why, sizeof why, "tensor %u offset %llu not %u-aligned", k, (unsigned long long)t.off, FSR4NPU_ALIGN); break; }
    if (t.off + c->g->bytes[k] > c->fdSize[t.fd_idx]) { st = E_SIZE; snprintf(why, sizeof why, "tensor %u off %llu + %llu > buffer %llu", k, (unsigned long long)t.off, (unsigned long long)c->g->bytes[k], (unsigned long long)c->fdSize[t.fd_idx]); break; }
    for (uint32_t j = 0; j < k; j++) if (m.t[j].fd_idx == t.fd_idx && t.off < m.t[j].off + c->g->bytes[j] && m.t[j].off < t.off + c->g->bytes[k]) { st = E_SIZE; snprintf(why, sizeof why, "tensors %u and %u overlap", j, k); }
  }
  if (st == S_OK && kick >= 0 && (fdLink(kick).find("eventfd") == string::npos || fdLink(done).find("eventfd") == string::npos)) { st = E_FD; snprintf(why, sizeof why, "kick/done are not eventfds"); }
  if (st != S_OK) { for (int i = 0; i < nbuf; i++) close(fds[i]); if (kick >= 0) close(kick); if (done >= 0) close(done); sendErr(c, st, 0, why); return st; }
  unreg(c);   // re-setup replaces the previous registration
  c->nfd = nbuf; for (int i = 0; i < nbuf; i++) { c->fds[i] = fds[i]; c->map[i] = nullptr; c->regDma[i] = false; }
  c->nt = m.ntensors; for (uint32_t k = 0; k < c->nt; k++) { c->t[k] = m.t[k]; c->t[k].bytes = c->g->bytes[k]; c->mh[k] = nullptr; }
  c->kick = kick; c->done = done;
  return S_OK;
}

static int dtHash = -1, dtRerun = 0, dtSync = 0;
static uint64_t xh(const void* p, size_t n) { const uint64_t* q = (const uint64_t*)p; uint64_t h = 0x9E3779B97F4A7C15ull; for (size_t i = 0; i < n / 8; i++) { h ^= q[i]; h *= 0x100000001B3ull; h ^= h >> 29; } return h; }
static void dsync(int fd, uint64_t f) { dma_buf_sync s{f}; if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) < 0) logf("dma-buf sync 0x%llx failed: %s", (unsigned long long)f, strerror(errno)); }
static void dtHashOut(Client* c, uint64_t* h) { const uint8_t* b = (const uint8_t*)c->map[0]; int fd = c->fds[0]; dsync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  for (uint32_t k = 0; k < c->nt; k++) h[k] = xh(b + c->t[k].off, c->g->bytes[k]); dsync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ); }
// CPU-cache invalidation of the HTP input. The cDSP compute context banks are dma-coherent (device tree 'dma-coherent'), the GPU
// (Adreno) is not: the GPU writes the input to memory, the HTP's coherent read snoops the CPU caches, and a line the CPU
// holds from before the GPU's write (speculative fill through the kernel linear map; the pages are cacheable system-heap
// pages) wins over memory. dc civac by VA on our read-only mapping acts on the physical line, so every such copy is gone
// before QnnGraph_execute; lines filled after it come from memory (the GPU has finished writing). The CPU never writes the
// buffer after cleanFd, so civac never writes back data.
static int gStInv = -1, gStDiag = 0; static vector<uint8_t> gStA;
static void civac(const uint8_t* p, uint64_t n) { for (uintptr_t x = (uintptr_t)p & ~63ul, e = (uintptr_t)p + n; x < e; x += 64) asm volatile("dc civac, %0" ::"r"(x) : "memory"); asm volatile("dsb sy" ::: "memory"); }
static int execute(Client* c, double* exMs, int slot = 0) {
  const Qnn_Tensor_t* qin = ((slot & 1) && c->alt) ? &c->qtAlt : &c->qt[0];   // ping-pong input (slot bit 0)
  const bool o1 = (slot & 2) && c->altOut; Qnn_Tensor_t* qout = o1 ? &c->qtOut1[1] : &c->qt[1];   // ping-pong output (slot bit 1)
  if (dtHash < 0) { dtHash = getenv("FSR4NPUD_HASH_OUTPUT") ? atoi(getenv("FSR4NPUD_HASH_OUTPUT")) : 0; dtRerun = getenv("FSR4NPUD_RERUN") ? atoi(getenv("FSR4NPUD_RERUN")) : 0;
    dtSync = getenv("FSR4NPUD_DMABUF_SYNC") ? atoi(getenv("FSR4NPUD_DMABUF_SYNC")) : 0; logf("debug options: output hash %d, rerun %d, dma-buf sync %d", dtHash, dtRerun, dtSync); }
  if (isQnn && dtHash && !c->map[0] && c->nfd == 1) { void* m = mmap(nullptr, c->fdSize[0], PROT_READ, MAP_SHARED, c->fds[0], 0); c->map[0] = m == MAP_FAILED ? nullptr : m; if (!c->map[0]) logf("mmap of the shared buffer failed: %s", strerror(errno)); }
  uint64_t hA[FSR4NPU_MAXT] = {}, hB[FSR4NPU_MAXT] = {}, hC[FSR4NPU_MAXT] = {}; bool dh = isQnn && dtHash && c->map[0];
  if (isQnn && (dtSync & 1)) for (int i = 0; i < c->nfd; i++) { dsync(c->fds[i], DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW); dsync(c->fds[i], DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW); }
  { static int dly = getenv("FSR4NPUD_DELAY_US") ? atoi(getenv("FSR4NPUD_DELAY_US")) : 0; if (isQnn && dly > 0) usleep(dly); }
  if (dh) dtHashOut(c, hA);
  if (gStInv < 0) { gStInv = getenv("FSR4NPUD_INPUT_INVALIDATE") ? atoi(getenv("FSR4NPUD_INPUT_INVALIDATE")) : 1; gStDiag = getenv("FSR4NPUD_INPUT_DIAG") ? atoi(getenv("FSR4NPUD_INPUT_DIAG")) : 0; logf("input cache invalidation %d, diagnostics %d", gStInv, gStDiag); }
  const uint64_t inOff = ((slot & 1) && c->alt) ? c->tAlt.off : c->t[0].off; const uint8_t* inP = (isQnn && c->stMap) ? (const uint8_t*)c->stMap + inOff : nullptr;
  if (inP && gStDiag) { gStA.resize(c->g->bytes[0]); memcpy(gStA.data(), inP, c->g->bytes[0]); }   // diagnostics: the coherent view = what the HTP reads (INV=0)
  if (inP && gStInv) { double i0 = now_ms(); civac(inP, c->g->bytes[0]); c->vInv.push_back(now_ms() - i0); }
  std::unique_lock<std::mutex> ql(gQnnMu, std::defer_lock); if (isQnn) ql.lock();   // before the clock: the timeouts count the execute, not a load beside it
  double t0 = now_ms(); int st = S_OK;
  uint64_t myId = ++gExecId; gExecSock = c->sock; gExecDone = c->done; gExecStart = t0;
  if (isQnn) { Qnn_ErrorHandle_t e = Q.graphExecute(c->g->G, qin, 1, qout, c->nt - 1, nullptr, nullptr); if (e != QNN_SUCCESS) { logf("client %d graphExecute rc 0x%lx", c->id, (unsigned long)e); st = E_EXEC; }
    if (dtSync & 2) for (int i = 0; i < c->nfd; i++) { dsync(c->fds[i], DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW); dsync(c->fds[i], DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW); }
    if (dh) { dtHashOut(c, hB);
      { uint64_t h1[FSR4NPU_MAXT], h5[FSR4NPU_MAXT]; usleep(1000); dtHashOut(c, h1); usleep(4000); dtHashOut(c, h5);
        bool l1 = h1[1] != hB[1] || h1[2] != hB[2], l5 = h5[1] != h1[1] || h5[2] != h1[2]; if (l1 || l5) logf("late output change: client %d frame %ld late1 %d late5 %d", c->id, c->frames, (int)l1, (int)l5); }
      if (dtRerun) { Q.graphExecute(c->g->G, qin, 1, &c->qt[1], c->nt - 1, nullptr, nullptr); dtHashOut(c, hC); }
      logf("output hash: client %d frame %ld in %016llx in_after %016llx o0 %016llx o1 %016llx%s r_o0 %016llx r_o1 %016llx ex %.3f", c->id, c->frames, (unsigned long long)hA[0], (unsigned long long)hB[0],
           (unsigned long long)hB[1], (unsigned long long)hB[2], dtRerun ? ((hC[1] != hB[1] || hC[2] != hB[2]) ? " RERUN_DIFF" : " rerun_same") : "", (unsigned long long)hC[1], (unsigned long long)hC[2], now_ms() - t0); } }
  else {
    for (int i = 0; i < c->nfd; i++) { dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW}; ioctl(c->fds[i], DMA_BUF_IOCTL_SYNC, &s); }
    const uint8_t* in = (const uint8_t*)c->map[c->t[0].fd_idx] + (((slot & 1) && c->alt) ? c->tAlt.off : c->t[0].off);
    for (uint32_t k = 1; k < c->nt; k++) memcpy((uint8_t*)c->map[c->t[k].fd_idx] + (o1 ? c->offOut1 + (c->t[k].off - c->t[1].off) : c->t[k].off), in, min(c->g->bytes[0], c->g->bytes[k]));
    for (int i = 0; i < c->nfd; i++) { dma_buf_sync s{DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW}; ioctl(c->fds[i], DMA_BUF_IOCTL_SYNC, &s); }
    { const int dl = c->g->stubDelayUs >= 0 ? c->g->stubDelayUs : optStubDelayUs; if (dl > 0) { double e = now_ms() + dl / 1000.0; while (now_ms() < e) {} } }
  }
  gExecStart = 0; *exMs = now_ms() - t0; if (ql.owns_lock()) ql.unlock();
  if (inP && gStDiag) {   // diagnostics: memory (after civac) vs the view before execute, per 64-byte line
    civac(inP, c->g->bytes[0]); long nl = 0; uint64_t first[4] = {}; const uint8_t* A = gStA.data();
    for (uint64_t o = 0; o < c->g->bytes[0]; o += 64) if (memcmp(A + o, inP + o, std::min<uint64_t>(64, c->g->bytes[0] - o))) { if (nl < 4) first[nl] = o; nl++; }
    const uint64_t hv = xh(A, c->g->bytes[0]), hf = xh(inP, c->g->bytes[0]); civac(inP, c->g->bytes[0]);
    const uint64_t oOff = (slot & 2) && c->altOut ? c->offOut1 : c->t[1].off; const uint8_t* oP = (const uint8_t*)c->stMap + oOff; const uint64_t ho = xh(oP, c->g->bytes[1]); civac(oP, c->g->bytes[1]);
    logf("input view: client %d frame %ld slot %d in_view %016llx in_mem %016llx out %016llx%s", c->id, c->frames, slot, (unsigned long long)hv, (unsigned long long)hf, (unsigned long long)ho, nl ? " STALE" : "");
    if (nl) { c->stale++; logf("stale input: client %d frame %ld slot %d: %ld input lines of 64 B differ (view vs memory), first at +%llu +%llu +%llu +%llu (pixel %llu)", c->id, c->frames, slot, nl,
      (unsigned long long)first[0], (unsigned long long)first[1], (unsigned long long)first[2], (unsigned long long)first[3], (unsigned long long)(first[0] / 16)); } }
  if (gExecTimedOutId == myId) { c->timedOut = true; st = E_TIMEOUT; double tn = now_ms(); gSoftT[gSoftN++ % 3] = tn;
    logf("client %d execute returned after %.0f ms (> %d ms soft timeout): dropping this client, session kept", c->id, *exMs, optExecTimeoutMs);
    if (tn - gSoftT[0] < 120000 && tn - gSoftT[1] < 120000 && tn - gSoftT[2] < 120000) { logf("FAIL 3 slow executes within 120 s: ordered exit 75 for a fresh DSP session"); gExitCode = 75; gStop = 1; } }
  return st;
}

static void onRun(Client* c, const fsr4npu_msg& m, int* fds, int nfd, double tRecv) {
  fsr4npu_msg r; msgInit(r, M_DONE); r.seq = m.seq; double fenceMs = 0;
  if (!c->ready) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_STATE, m.seq, "RUN before SETUP/ALLOC"); return; }
  if ((m.flags & F_SYNCFD) && nfd >= 1) {
    double f0 = now_ms(); pollfd p{fds[0], POLLIN, 0}; int pr = poll(&p, 1, 1000); fenceMs = now_ms() - f0;
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (pr != 1) { sendErr(c, E_FENCE, m.seq, pr == 0 ? "sync_file not signalled within 1000 ms" : "sync_file poll error"); return; }
  } else for (int i = 0; i < nfd; i++) close(fds[i]);
  gExecSeq = m.seq; double ex = 0; int st = execute(c, &ex, (m.flags & F_PINGPONG) ? int(m.v[0] & 3) : 0); if (c->timedOut) { dropClient(c, "execute exceeded the soft timeout (client was told E_TIMEOUT)"); return; }
  r.status = st; r.v[0] = (uint64_t)(ex * 1e6); r.v[1] = (uint64_t)(fenceMs * 1e6);
  if (!isQnn && st == S_OK && (m.flags & F_VERIFY)) { r.v[2] = fnv64((const uint8_t*)c->map[c->t[0].fd_idx] + c->t[0].off, c->g->bytes[0]); r.v[3] = fnv64((const uint8_t*)c->map[c->t[1].fd_idx] + c->t[1].off, c->g->bytes[1]); }
  if (st) c->errors++; else { c->frames++; c->vExec.push_back(ex); c->vFence.push_back(fenceMs); }
  c->vSvc.push_back(now_ms() - tRecv);
  if (!sendMsg(c, r)) logf("client %d DONE send failed (client gone?)", c->id);
}

static void handleMsg(Client* c) {
  double tRecv = now_ms(); fsr4npu_msg m; iovec io{&m, sizeof m}; char cm[CMSG_SPACE(sizeof(int) * FSR4NPU_MAXFD)];
  msghdr mh{}; mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = cm; mh.msg_controllen = sizeof cm;
  ssize_t n = recvmsg(c->sock, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  int fds[FSR4NPU_MAXFD]; int nfd = 0;
  for (cmsghdr* h = CMSG_FIRSTHDR(&mh); h; h = CMSG_NXTHDR(&mh, h)) if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
    int k = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int)); for (int i = 0; i < k; i++) { int fd; memcpy(&fd, CMSG_DATA(h) + i * sizeof(int), sizeof fd); if (nfd < FSR4NPU_MAXFD) fds[nfd++] = fd; else close(fd); } }
  if (n == 0 || (n < 0 && errno != EAGAIN)) { for (int i = 0; i < nfd; i++) close(fds[i]); dropClient(c, n == 0 ? "hung up" : strerror(errno)); return; }
  if (n < 0) return;
  if (n != (ssize_t)sizeof m || m.magic != FSR4NPU_MAGIC || m.version != FSR4NPU_VERSION || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
    for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "bad packet (size/magic/version/truncated)"); dropClient(c, "protocol error"); return; }
  if (c->g->st == Graph::FAILED && (m.type == M_HELLO || m.type == M_SETUP || m.type == M_ALLOC)) {   // a bad graph answers at once: its client goes on without the NPU, the other sockets are not touched
    for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_SYS, m.seq, "graph file missing or bad"); dropClient(c, "graph not available"); clientsDone--; return; }
  switch (m.type) {
  case M_HELLO: { fsr4npu_msg r; msgInit(r, M_INFO); r.ntensors = c->g->nt; r.v[0] = isQnn ? B_QNN : B_STUB; for (uint32_t k = 0; k < c->g->nt; k++) r.t[k].bytes = c->g->bytes[k];
    snprintf(r.text, sizeof r.text, "%s", c->g->name.c_str()); for (int i = 0; i < nfd; i++) close(fds[i]); sendMsg(c, r); break; }
  case M_SETUP: {
    int kick = -1, done = -1, nbuf = nfd;
    if (m.flags & F_EFD) { if (nfd < 3) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "F_EFD needs buffer fds + 2 eventfds"); break; } kick = fds[nfd - 2]; done = fds[nfd - 1]; nbuf = nfd - 2; }
    if (nbuf < 1) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "SETUP without buffer fds"); break; }
    if (adopt(c, m, fds, nbuf, kick, done) != S_OK) break;
    c->setupFlags = m.flags; const bool now = c->g->st == Graph::LOADED;   // not loaded yet: registered when the load ends
    int st = now ? registerTensors(c, m.flags) : S_OK; if (st) { unreg(c); sendErr(c, st, 0, "register failed"); break; }
    c->registered = now; c->ready = true; fsr4npu_msg r; msgInit(r, M_SETUP_OK); r.ntensors = c->nt; for (uint32_t k = 0; k < c->nt; k++) r.t[k] = c->t[k]; sendMsg(c, r);
    logf("client %d SETUP ok: %d buffer fd(s), %u tensors, eventfd %s", c->id, nbuf, c->nt, kick >= 0 ? "yes" : "no"); break; }
  case M_ALLOC: {
    int kick = -1, done = -1; if (m.flags & F_EFD) { if (nfd != 2) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "ALLOC F_EFD needs 2 eventfds"); break; } kick = fds[0]; done = fds[1]; }
    else for (int i = 0; i < nfd; i++) close(fds[i]);
    uint64_t off[FSR4NPU_MAXT], tot = 0; for (uint32_t k = 0; k < c->g->nt; k++) { off[k] = tot; tot += (c->g->bytes[k] + 4095) & ~4095ull; }
    uint64_t offAlt = 0; if (m.flags & F_PINGPONG) { offAlt = tot; tot += (c->g->bytes[0] + 4095) & ~4095ull; }   // input slot 1 after the outputs
    uint64_t offOut1 = 0; if (m.flags & F_PINGPONG_OUT) { offOut1 = tot; for (uint32_t k = 1; k < c->g->nt; k++) tot += (c->g->bytes[k] + 4095) & ~4095ull; }   // output slot 1 after input slot 1
    int bfd = -1; void* base = nullptr; VkDeviceMemory vm = 0;
    if (isQnn && !gAllocVk) { base = rAlloc(25, 1, (int)tot); bfd = base ? rToFd(base) : -1; }
    else if (gMemfd) { bfd = memfd_create("fsr4npu", MFD_CLOEXEC); if (bfd >= 0 && ftruncate(bfd, (off_t)tot) != 0) { close(bfd); bfd = -1; } }
    else bfd = vkAllocFd(tot, &vm);
    if (isQnn && bfd >= 0 && !(getenv("FSR4NPUD_NO_CLEAN") && atoi(getenv("FSR4NPUD_NO_CLEAN")))) { double c0 = now_ms(); cleanFd(bfd, tot); logf("client %d buffer zeroed + cleaned to PoC (%.2f ms)", c->id, now_ms() - c0); }
    if (bfd < 0) { if (kick >= 0) { close(kick); close(done); } sendErr(c, E_ALLOC, 0, isQnn ? "rpcmem_alloc failed" : "vk export alloc failed"); break; }
    // qnn: register rpcmem's own fd (rpcmem_free closes it). stub: the exported fd is ours.
    int own = bfd;
    fsr4npu_msg am; msgInit(am, M_ALLOC); am.ntensors = c->g->nt; for (uint32_t k = 0; k < c->g->nt; k++) { am.t[k].fd_idx = 0; am.t[k].off = off[k]; am.t[k].bytes = c->g->bytes[k]; }
    int one[1] = {own};
    if (base) one[0] = fcntl(bfd, F_DUPFD_CLOEXEC, 3);   // adopt() closes on failure: hand it a dup to validate, swap in bfd after
    if (adopt(c, am, one, 1, kick, done) != S_OK) { if (base) rFree(base); if (vm) VA.freeMem(VA.dev, vm, nullptr); break; }
    if (base) { close(c->fds[0]); c->fds[0] = bfd; }
    if (offAlt) { c->alt = true; c->tAlt.fd_idx = 0; c->tAlt.off = offAlt; c->tAlt.bytes = c->g->bytes[0]; }
    if (offOut1) { c->altOut = true; c->offOut1 = offOut1; }
    c->rpcBase = base; c->vkMem = vm;
    if (isQnn && !base && getenv("FSR4NPUD_REGISTER_ATTR")) { typedef int (*RegAttr)(int, uint32_t, uint32_t); static RegAttr ra = (RegAttr)dlsym(RTLD_DEFAULT, "remote_register_dma_handle_attr");
      uint32_t at = (uint32_t)atoi(getenv("FSR4NPUD_REGISTER_ATTR")); int r = ra ? ra(c->fds[0], (uint32_t)tot, at) : -999; logf("remote_register_dma_handle_attr fd %d len %llu attr %u rc %d", c->fds[0], (unsigned long long)tot, at, r); }
    c->setupFlags = 0; const bool now = c->g->st == Graph::LOADED;   // not loaded yet: registered when the load ends; the client has its buffer now
    int st = now ? registerTensors(c, 0) : S_OK; if (st) { unreg(c); sendErr(c, st, 0, "register failed"); break; }
    c->registered = now;
    if (isQnn) { void* m = mmap(nullptr, tot, PROT_READ, MAP_SHARED, c->fds[0], 0); if (m != MAP_FAILED) { c->stMap = m; c->stLen = tot; } else logf("client %d: mmap of the input view failed: %s (no input invalidation)", c->id, strerror(errno)); }
    c->ready = true; fsr4npu_msg r; msgInit(r, M_SETUP_OK); r.ntensors = c->nt; r.total = tot; for (uint32_t k = 0; k < c->nt; k++) r.t[k] = c->t[k]; r.v[4] = c->alt ? c->tAlt.off : 0; r.v[5] = c->altOut ? c->offOut1 : 0;
    int sfd[1] = {c->fds[0]}; sendMsg(c, r, sfd, 1);
    logf("client %d ALLOC ok: %s buffer %llu bytes, %u tensors, eventfd %s%s", c->id, base ? "rpcmem" : gMemfd ? "memfd" : "vk", (unsigned long long)tot, c->nt, kick >= 0 ? "yes" : "no", now ? "" : ", context still loading"); break; }
  case M_RUN: onRun(c, m, fds, nfd, tRecv); break;
  case M_BYE: for (int i = 0; i < nfd; i++) close(fds[i]); dropClient(c, "bye"); break;
  default: for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, m.seq, "unknown message type"); break;
  }
}
static void onKick(Client* c) {
  uint64_t v; if (read(c->kick, &v, 8) != 8) return; double tRecv = now_ms();
  uint64_t out; if (!c->ready) out = 2 + (uint64_t)(-E_STATE);
  else { double ex = 0; int st = execute(c, &ex, int((v >> 32) & 3)); if (c->timedOut) { dropClient(c, "execute exceeded the soft timeout (client was told E_TIMEOUT)"); return; }
    out = st ? 2 + (uint64_t)(-st) : 1; if (st) c->errors++; else { c->frames++; c->vExec.push_back(ex); } }
  c->vSvc.push_back(now_ms() - tRecv);
  if (write(c->done, &out, 8) != 8) logf("client %d done eventfd write failed", c->id);
}

// ---------------- loader thread: file reads and context create/free, never on the frame path. The main thread owns the graph states; a job and its result pass through a locked queue.
enum JobType { J_INFO, J_LOAD, J_UNLOAD };
struct Job { JobType type; Graph* g; };
struct Result { JobType type = J_INFO; Graph* g = nullptr; bool ok = false, ctxFail = false; double ms = 0; Parsed p; Qnn_ContextHandle_t ctx = nullptr; Qnn_GraphHandle_t gh = nullptr; };
static std::mutex gJobMu; static std::condition_variable gJobCv; static deque<Job> gJobs; static deque<Result*> gResults; static bool gJobQuit = false; static int gWakeFd = -1;
static std::atomic<double> gLoadStart{0};
static void post(JobType t, Graph* g) { std::lock_guard<std::mutex> l(gJobMu); gJobs.push_back({t, g}); gJobCv.notify_one(); }
static bool contextLoad(Result* r) {
  Graph* g = r->g; if (!parseGraph(g, r->p)) return false;
  if (!isQnn) { if (r->p.stubLoadMs > 0) usleep(r->p.stubLoadMs * 1000); return true; }
  std::lock_guard<std::mutex> ql(gQnnMu); gLoadStart = now_ms();   // the hung-load clock starts with the call into the DSP
  Qnn_ErrorHandle_t e = Q.contextCreateFromBinary(BE, DV, nullptr, r->p.bin.data(), r->p.bin.size(), &r->ctx, nullptr);
  if (e != QNN_SUCCESS) { logf("graph %s: contextCreateFromBinary rc 0x%lx", g->sock.c_str(), (unsigned long)e); gLoadStart = 0; r->ctx = nullptr; r->ctxFail = true; return false; }
  e = Q.graphRetrieve(r->ctx, r->p.name.c_str(), &r->gh);
  if (e != QNN_SUCCESS) { logf("graph %s: graphRetrieve rc 0x%lx", g->sock.c_str(), (unsigned long)e); Q.contextFree(r->ctx, nullptr); r->ctx = nullptr; gLoadStart = 0; return false; }
  gLoadStart = 0; return true; }
static void loaderMain() {
  for (;;) { Job j; { std::unique_lock<std::mutex> l(gJobMu); gJobCv.wait(l, [] { return gJobQuit || !gJobs.empty(); }); if (gJobQuit) return; j = gJobs.front(); gJobs.pop_front(); }
    Result* r = new Result(); r->type = j.type; r->g = j.g; double t0 = now_ms();
    if (j.type == J_INFO) { r->ok = parseGraph(j.g, r->p); if (isQnn) parsedFree(r->p); }
    else if (j.type == J_LOAD) { r->ok = contextLoad(r); if (!r->ok && isQnn) parsedFree(r->p); }
    else { if (isQnn) { { std::lock_guard<std::mutex> ql(gQnnMu); gLoadStart = now_ms(); Qnn_ErrorHandle_t e = Q.contextFree(j.g->CTX, nullptr); gLoadStart = 0; if (e != QNN_SUCCESS) logf("graph %s: contextFree rc 0x%lx", j.g->sock.c_str(), (unsigned long)e); }
        if (j.g->sc) S.systemContextFree(j.g->sc); j.g->sc = nullptr; vector<uint8_t>().swap(j.g->bin); } r->ok = true; }
    r->ms = now_ms() - t0; { std::lock_guard<std::mutex> l(gJobMu); gResults.push_back(r); } uint64_t v = 1; if (write(gWakeFd, &v, 8) != 8) {} } }

static string loadedList() { string s; for (Graph* g : graphs) if (g->st == Graph::LOADED) s += (s.empty() ? "" : " ") + g->sock; return s.empty() ? "none" : s; }
static void statusLine() { string st = "STATUS=serving " + to_string(graphs.size()) + " socket(s), alloc " + (isQnn ? gAllocName : string(gMemfd ? "memfd" : "vk")) + ", loaded: " + loadedList(); sdNotify(st.c_str()); }
static int loadedCount() { int n = 0; for (Graph* g : graphs) if (g->st == Graph::LOADED || g->st == Graph::LOADING) n++; return n; }
static Graph* lruIdle() { Graph* b = nullptr; for (Graph* g : graphs) if (g->st == Graph::LOADED && g->pins == 0 && !g->keep && (!b || g->lastUse < b->lastUse)) b = g; return b; }
static void startUnload(Graph* g, const char* why) { logf("graph %s: unloading (%s)", g->sock.c_str(), why); g->st = Graph::UNLOADING; post(J_UNLOAD, g); }
static void startLoad(Graph* g) {
  while (loadedCount() + 1 > optMaxLoaded) { Graph* v = lruIdle(); if (!v) { logf("graph %s: every loaded graph has a client, going over --max-loaded %d", g->sock.c_str(), optMaxLoaded); break; } startUnload(v, "least recently used"); }
  g->st = Graph::LOADING; post(J_LOAD, g); }
static void trimLoaded() { while (loadedCount() > optMaxLoaded) { Graph* v = lruIdle(); if (!v) break; startUnload(v, "least recently used"); } }
static void graphWanted(Graph* g) {   // a client connected
  g->lastUse = now_ms();
  if (g->st == Graph::IDLE) startLoad(g);
  else if (g->st == Graph::UNLOADING) g->reload = true;
  else if (g->st == Graph::FAILED && now_ms() >= g->retryAt) startLoad(g); }
static void infoApply(Graph* g, const Parsed& p) { g->nt = p.nt; memcpy(g->bytes, p.bytes, sizeof g->bytes); g->name = p.name; g->stubDelayUs = p.stubDelayUs; g->infoReady = true; }
static void loadedApply(Graph* g, Result* r) { infoApply(g, r->p); g->CTX = r->ctx; g->G = r->gh; g->sc = r->p.sc; r->p.sc = nullptr; g->bin.swap(r->p.bin); memcpy(g->T, r->p.T, sizeof g->T); g->st = Graph::LOADED; g->evictRetried = false; g->lastUse = now_ms(); }
static void graphFailed(Graph* g) {
  g->st = Graph::FAILED; g->retryAt = now_ms() + optGraphRetryMs; g->evictRetried = false;
  vector<Client*> mine; for (Client* c : clients) if (c->g == g && c->ready) mine.push_back(c);   // these were told SETUP_OK; a client before ALLOC gets the error as its answer
  for (Client* c : mine) { sendErr(c, E_SYS, 0, "graph could not be loaded"); dropClient(c, "graph load failed"); clientsDone--; } }
static void graphLoaded(Graph* g) {
  if (!gAllocReady) allocVkTest(g);
  vector<Client*> mine; for (Client* c : clients) if (c->g == g && c->ready && !c->registered) mine.push_back(c);
  for (Client* c : mine) { int st = registerTensors(c, c->setupFlags); if (st) { unreg(c); sendErr(c, st, 0, "register failed"); dropClient(c, "register failed"); clientsDone--; } else c->registered = true; } }
static void resultsApply() {
  for (;;) { Result* r; { std::lock_guard<std::mutex> l(gJobMu); if (gResults.empty()) return; r = gResults.front(); gResults.pop_front(); } Graph* g = r->g;
    if (r->type == J_INFO) { if (r->ok) { if (!g->infoReady) infoApply(g, r->p); }
      else if (g->st == Graph::IDLE) { logf("graph %s: file %s missing or bad, the other graphs are not affected", g->sock.c_str(), g->path.c_str()); g->st = Graph::FAILED; g->retryAt = now_ms() + optGraphRetryMs; } }
    else if (r->type == J_LOAD) {
      if (r->ok) { loadedApply(g, r); logf("graph %s (%s): context loaded %.1f ms; loaded now: %s", g->sock.c_str(), g->name.c_str(), r->ms, loadedList().c_str()); graphLoaded(g); statusLine(); }
      else { bool freed = false; if (r->ctxFail && !g->evictRetried) while (Graph* v = lruIdle()) { startUnload(v, "a load failed, freeing memory"); freed = true; }   // idle graphs may hold the memory: free them, then try once more
        if (freed) { g->evictRetried = true; post(J_LOAD, g); } else { logf("graph %s: load failed, next try after %d ms; the other graphs are not affected", g->sock.c_str(), optGraphRetryMs); graphFailed(g); } } }
    else { g->st = Graph::IDLE; g->CTX = nullptr; g->G = nullptr; logf("graph %s: unloaded %.1f ms; loaded now: %s", g->sock.c_str(), r->ms, loadedList().c_str()); statusLine();
      const bool again = g->reload || g->pins > 0; g->reload = false; if (again && !gExiting) startLoad(g); }
    delete r; } }

int main(int argc, char** argv) {
  T0 = now_ms(); setvbuf(stderr, nullptr, _IOLBF, 0);
  for (int i = 1; i < argc; i++) { string a = argv[i]; auto nx = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); } return argv[++i]; };
    if (a == "--name") optName = nx(); else if (a == "--backend") optBackend = nx(); else if (a == "--ctx") optCtx = nx();
    else if (a == "--stub-bytes") optStubBytes = strtoull(nx(), 0, 10); else if (a == "--stub-nout") optStubNout = atoi(nx()); else if (a == "--stub-delay-us") optStubDelayUs = atoi(nx()); else if (a == "--stub-load-ms") optStubLoadMs = atoi(nx());
    else if (a == "--graphs") optGraphs = nx(); else if (a == "--graph-dir") optGraphDir = nx(); else if (a == "--socket-prefix") optSocketPrefix = nx(); else if (a == "--max-loaded") optMaxLoaded = atoi(nx());
    else if (a == "--graph-idle-ms") optGraphIdleMs = atoi(nx()); else if (a == "--graph-retry-ms") optGraphRetryMs = atoi(nx()); else if (a == "--load-hard-timeout-ms") optLoadHardMs = atoi(nx());
    else if (a == "--max-clients") optMaxClients = atoi(nx()); else if (a == "--allow-uid") allowUid.push_back((uid_t)atoi(nx())); else if (a == "--allow-gid") allowGid.push_back((gid_t)atoi(nx())); else if (a == "--exec-timeout-ms") optExecTimeoutMs = atoi(nx());
    else if (a == "--exit-after-clients") optExitAfter = atoi(nx()); else if (a == "--max-seconds") optMaxSec = atoi(nx()); else if (a == "--qnn-log") optQnnLog = atoi(nx()); else if (a == "--alloc") optAlloc = nx();
    else if (a == "--exec-hard-timeout-ms") optExecHardMs = atoi(nx()); else if (a == "--power-grace-ms") optPowerGraceMs = atoi(nx());
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; } }
  if (optAlloc != "auto" && optAlloc != "rpcmem" && optAlloc != "vk" && optAlloc != "memfd") { fprintf(stderr, "--alloc must be auto, rpcmem, vk or memfd\n"); return 2; }
  if (!optGraphs.empty() && (optGraphDir.empty() || !optCtx.empty())) { fprintf(stderr, "--graphs needs --graph-dir and excludes --ctx\n"); return 2; }
  if (optMaxLoaded < 1) { fprintf(stderr, "--max-loaded must be at least 1\n"); return 2; }
  gMemfd = optAlloc == "memfd";
  if (optExecHardMs < optExecTimeoutMs) optExecHardMs = optExecTimeoutMs;
  allowUid.push_back(getuid()); if (getenv("SUDO_UID")) allowUid.push_back((uid_t)atoi(getenv("SUDO_UID")));
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = onSig; sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr); sigaction(SIGALRM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);
  { struct sigaction cs; memset(&cs, 0, sizeof cs); cs.sa_flags = SA_SIGINFO | SA_RESETHAND;   // crash backtrace: print it on a fatal signal, then exit 139
    cs.sa_sigaction = [](int sg, siginfo_t* si, void*) { char b[128]; int n = snprintf(b, sizeof b, "CRASH fsr4npud signal %d fault addr %p backtrace:\n", sg, si->si_addr); if (write(2, b, n) < 0) {}
      void* bt[48]; int m = backtrace(bt, 48); backtrace_symbols_fd(bt, m, 2); _exit(139); };
    sigaction(SIGSEGV, &cs, nullptr); sigaction(SIGBUS, &cs, nullptr); sigaction(SIGABRT, &cs, nullptr); }
  if (optMaxSec > 0) alarm((unsigned)optMaxSec);
  if (optExecTimeoutMs > 0) std::thread([] {
    uint64_t fired = 0;
    for (;;) { usleep(50000);
      { double l0 = gLoadStart; if (l0 > 0 && now_ms() - l0 >= optLoadHardMs) { logf("FAIL a context load or free hung for > %d ms (HTP/DSP session wedged): exiting 75 so systemd restarts a fresh session", optLoadHardMs); sdNotify("STATUS=FAIL context load hung, restarting"); _exit(75); } }
      uint64_t id = gExecId; double s0 = gExecStart; if (s0 <= 0 || id != gExecId) continue; double el = now_ms() - s0;
      if (el >= optExecTimeoutMs && fired != id) { fired = id; gExecTimedOutId = id;
        logf("WARN execute running > %d ms: client told E_TIMEOUT (falls back); restart only if still stuck at %d ms", optExecTimeoutMs, optExecHardMs);
        fsr4npu_msg m; memset(&m, 0, sizeof m); m.magic = FSR4NPU_MAGIC; m.version = FSR4NPU_VERSION; m.type = M_ERROR; m.status = E_TIMEOUT; m.seq = gExecSeq;
        snprintf(m.text, sizeof m.text, "NPU execute slower than %d ms", optExecTimeoutMs);
        int so = gExecSock, dn = gExecDone; if (so >= 0 && send(so, &m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {}
        if (dn >= 0) { uint64_t v = 2 + (uint64_t)(-E_TIMEOUT); if (write(dn, &v, 8) < 0) {} }
        sdNotify("STATUS=WARN execute slow"); }
      if (el >= optExecHardMs) { logf("FAIL execute hung for > %d ms (HTP/DSP session wedged): exiting 75 so systemd restarts a fresh session", optExecHardMs);
        sdNotify("STATUS=FAIL execute hung, restarting"); _exit(75); } } }).detach();
  isQnn = optBackend == "qnn";
  if (!isQnn && optBackend != "stub") { fprintf(stderr, "backend must be stub or qnn\n"); return 2; }
  if (!optGraphs.empty()) { size_t a = 0; while (a <= optGraphs.size()) { size_t b = optGraphs.find(',', a); if (b == string::npos) b = optGraphs.size(); string n = optGraphs.substr(a, b - a); a = b + 1; if (n.empty()) continue;
      Graph* g = new Graph(); g->sock = optSocketPrefix + n; g->path = optGraphDir + "/" + n + ".bin"; g->name = n; graphs.push_back(g); }
    if (graphs.empty()) { fprintf(stderr, "--graphs is empty\n"); return 2; } }
  else { Graph* g = new Graph(); g->sock = optName; g->path = optCtx; g->keep = true; graphs.push_back(g); }
  const bool oneGraph = graphs[0]->keep;
  if (getenv("NOTIFY_SOCKET")) sdNotify(isQnn ? "STATUS=opening the QNN backend" : "STATUS=stub backend");
  if (isQnn) { if (qnnOpen()) allocDecide(); else { logf("FAIL qnn init (is vela-npu-listener.service running? journalctl -u vela-npu-listener)"); sdNotify("STATUS=FAIL qnn init"); return 3; } }
  gWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); if (gWakeFd < 0) { logf("FAIL eventfd: %s", strerror(errno)); return 1; }
  if (oneGraph) {   // one graph: loaded before READY, a failure ends the process, as always
    Graph* g = graphs[0]; Result r; r.g = g; double t0 = now_ms(); if (!contextLoad(&r)) { logf("FAIL qnn init (is vela-npu-listener.service running? journalctl -u vela-npu-listener)"); sdNotify("STATUS=FAIL qnn init"); return 3; }
    loadedApply(g, &r); logf("context loaded %.1f ms", now_ms() - t0); if (!gAllocReady) allocVkTest(g); }
  int bound = 0;
  for (Graph* g : graphs) { int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); sockaddr_un a{}; a.sun_family = AF_UNIX;
    if (g->sock.size() >= sizeof a.sun_path - 1) { logf("FAIL socket name too long: %s", g->sock.c_str()); return 2; } memcpy(a.sun_path + 1, g->sock.data(), g->sock.size());
    if (bind(ls, (sockaddr*)&a, (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + g->sock.size())) < 0) { logf("%s bind @%s: %s (another fsr4npud running?)", oneGraph ? "FAIL" : "SKIP", g->sock.c_str(), strerror(errno)); close(ls); continue; }
    listen(ls, 8); g->ls = ls; bound++; }
  if (!bound) { logf("FAIL no socket could be bound"); return 4; }
  { string bs; if (oneGraph) for (uint32_t k = 0; k < graphs[0]->nt; k++) bs += (k ? "," : "") + to_string(graphs[0]->bytes[k]);
    logf("fsr4npud READY name=@%s sockets=%d backend=%s graph=%s tensors=%u bytes=%s max-loaded=%d alloc=%s pid=%d uid=%u", graphs[0]->sock.c_str(), bound, optBackend.c_str(), oneGraph ? graphs[0]->name.c_str() : "on demand", graphs[0]->nt, bs.c_str(), optMaxLoaded, isQnn ? gAllocName.c_str() : gMemfd ? "memfd" : "vk", (int)getpid(), (unsigned)getuid()); }
  sdNotify("READY=1"); statusLine();
  std::thread loader(loaderMain);
  if (!oneGraph) for (Graph* g : graphs) if (g->ls >= 0) post(J_INFO, g);   // the sizes for HELLO and ALLOC, before the first client asks; a missing file shows in the log now
  while (!gStop) {
    vector<pollfd> p; vector<Graph*> lg; for (Graph* g : graphs) if (g->ls >= 0) { p.push_back({g->ls, POLLIN, 0}); lg.push_back(g); }
    const size_t nl = lg.size(); p.push_back({gWakeFd, POLLIN, 0}); vector<pair<Client*, int>> who;
    for (Client* c : clients) { Graph* g = c->g;
      // A client waits by not being read: until the sizes and the allocator are known, and after SETUP_OK until its handles are registered. Its kick stays in the eventfd. A hang-up is still seen.
      const bool wait = g->st != Graph::FAILED && (!g->infoReady || (isQnn && !gAllocReady) || (c->ready && !c->registered));
      p.push_back({c->sock, short(wait ? 0 : POLLIN), 0}); who.push_back({c, 0}); if (c->ready && c->registered && c->kick >= 0) { p.push_back({c->kick, POLLIN, 0}); who.push_back({c, 1}); } }
    int to = 1000; if (gVoted && clients.empty()) to = max(0, min(1000, (int)(gIdleSince + optPowerGraceMs - now_ms()) + 1));
    int pr = poll(p.data(), p.size(), to); if (pr < 0) { if (errno == EINTR) continue; logf("poll: %s", strerror(errno)); gExitCode = 1; break; }
    if (gVoted && clients.empty() && now_ms() - gIdleSince >= optPowerGraceMs) powerVote(false, "no client");
    for (size_t li = 0; li < nl; li++) if (p[li].revents & POLLIN) { Graph* g = lg[li];
      int s = accept4(g->ls, nullptr, nullptr, SOCK_CLOEXEC);
      if (s >= 0) { ucred cr{}; socklen_t cl = sizeof cr; getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl);
        Client* c = new Client(); c->g = g; g->pins++; c->quiet = g->st == Graph::FAILED && now_ms() < g->retryAt; c->sock = s; c->uid = cr.uid; c->pid = cr.pid; c->id = ++clientSeq; c->tConn = now_ms();
        for (int k = 0; k < FSR4NPU_MAXT; k++) { c->fds[k] = -1; c->map[k] = nullptr; c->mh[k] = nullptr; c->regDma[k] = false; }
        clients.push_back(c);
        bool okUid = find(allowUid.begin(), allowUid.end(), cr.uid) != allowUid.end() || cr.uid == 0 || find(allowGid.begin(), allowGid.end(), cr.gid) != allowGid.end();
        if (!c->quiet) logf("client %d connected to @%s pid %d uid %u%s", c->id, g->sock.c_str(), (int)cr.pid, (unsigned)cr.uid, okUid ? "" : " (uid not allowed)");
        if (!okUid) { sendErr(c, E_PERM, 0, "uid not allowed"); dropClient(c, "uid not allowed"); clientsDone--; }
        else if (g->pins > optMaxClients) { sendErr(c, E_BUSY, 0, "too many clients"); dropClient(c, "busy"); clientsDone--; }
        else if (!c->quiet) { if (isQnn && !gVoted) { char w[48]; snprintf(w, sizeof w, "client %d connected", c->id); powerVote(true, w); } graphWanted(g); } }   // the vote first: the load runs at the top corner
    }
    for (size_t i = nl + 1; i < p.size(); i++) { Client* c = who[i - nl - 1].first; if (find(clients.begin(), clients.end(), c) == clients.end()) continue;
      if (who[i - nl - 1].second == 1) { if (p[i].revents & POLLIN) onKick(c); continue; }
      if (p[i].revents & POLLIN) handleMsg(c); else if (p[i].revents & (POLLHUP | POLLERR | POLLNVAL)) dropClient(c, "socket closed (client died?)"); }
    if (p[nl].revents & POLLIN) { uint64_t v; if (read(gWakeFd, &v, 8) != 8) {} resultsApply(); }
    { double tn = now_ms(); for (Graph* g : graphs) if (g->st == Graph::LOADED && g->pins == 0 && !g->keep && tn - g->lastUse >= optGraphIdleMs) startUnload(g, "idle"); }
    if (optExitAfter > 0 && clientsDone >= optExitAfter) { logf("served %d clients, exiting", clientsDone); break; }
  }
  if (gStop) logf("stop signal"); sdNotify("STOPPING=1");
  gExiting = true;
  { std::lock_guard<std::mutex> l(gJobMu); gJobQuit = true; gJobCv.notify_all(); } loader.join(); resultsApply();   // the job in hand ends first; queued jobs are dropped
  while (!clients.empty()) dropClient(clients.front(), "daemon exit");
  for (Graph* g : graphs) if (g->ls >= 0) close(g->ls);
  // release order: every client's memDeRegister -> munmap -> close fds -> free buffer (dropClient above), then the QNN
  // context and backend (closes the fastrpc session), then rpcmem, then our Vulkan device last (vk buffers are already freed).
  if (isQnn) { powerVote(false, "daemon exit"); if (gPerf) gPerf->destroyPowerConfigId(gPcid); for (Graph* g : graphs) if (g->CTX) { Q.contextFree(g->CTX, nullptr); g->CTX = nullptr; if (g->sc) S.systemContextFree(g->sc); g->sc = nullptr; } if (BE) Q.backendFree(BE); if (rDeinit) rDeinit(); }
  if (VA.ok) { ((PFN_vkDestroyDevice)VA.gipa(VA.inst, "vkDestroyDevice"))(VA.dev, nullptr); ((PFN_vkDestroyInstance)VA.gipa(VA.inst, "vkDestroyInstance"))(VA.inst, nullptr); }
  logf("fsr4npud exit %d", gExitCode);
  return gExitCode;
}
