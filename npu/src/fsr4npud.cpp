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
// fsr4npud: native aarch64 NPU helper daemon for the FSR 4 networks (Hexagon HTP). Runs as vela-npu@.service (systemd Type=notify).
// Serves the protocol of fsr4npu_proto.h on an abstract AF_UNIX SOCK_SEQPACKET socket. Clients pass dma-buf fds (or ask the
// daemon to allocate one shared buffer), the daemon registers them with QNN as QNN_HTP_MEM_SHARED_BUFFER (the zero-copy path)
// and runs QnnGraph_execute per frame request.
// Features: --alloc auto (rpcmem on the kernel system dma-heap, else a Vulkan-exported buffer); a CDSP power vote held only
// while a client is connected; a soft and a hard execute timeout (a slow execute drops that client, only a stuck one restarts
// the daemon); ping-pong input and output slots (ALLOC F_PINGPONG / F_PINGPONG_OUT; kick bit 32 / 33, RUN v[0] bit 0 / 1);
// clean SIGTERM; --allow-gid; a listener health check.
// --backend stub: no QNN, no fastrpc; copies the input tensor into every output (exercises the full IPC/fence/dma-buf path
// without root). Allocation in stub mode uses the daemon's own Turnip device (exportable dma-buf), in qnn mode rpcmem.
// Usage: fsr4npud [--name N] [--backend stub|qnn] [--ctx lite_x.bin] [--alloc auto|rpcmem|vk] [--exec-timeout-ms 2000]
//                 [--exec-hard-timeout-ms 10000] [--power-grace-ms 2000] [--stub-bytes 8294400] [--stub-nout 1]
//                 [--stub-delay-us 0] [--max-clients 4] [--allow-uid U]... [--exit-after-clients K] [--max-seconds S] [--qnn-log L]
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
static uint64_t optStubBytes = 8294400; static int optStubNout = 1, optStubDelayUs = 0, optMaxClients = 4, optExitAfter = 0, optMaxSec = 0, optQnnLog = 0;
static vector<uid_t> allowUid; static vector<gid_t> allowGid;
// --alloc vk|rpcmem. Without /dev/dma_heap, rpcmem is FASTRPC_IOCTL_ALLOC_DMA_BUFF: the dma-buf's release
// (fastrpc_release -> fastrpc_buf_free) dereferences buf->fl->cctx, and buf->fl is freed with the fastrpc session -> a client
// or GPU import outliving the daemon's session is a use-after-free (seen as a kernel Oops). vk = a Turnip-exported
// dma-buf (msm GEM), mapped into the DSP by QNN's SHARED_BUFFER registration: its lifetime no longer depends on fastrpc.
// Default auto = rpcmem when /dev/dma_heap/system is openable (libcdsprpc then allocates from the kernel system dma-heap;
// in testing: deterministic output, 0 of 119 reruns differed, no Oops in 10 teardowns), verified by the dma-buf exporter name; else vk.
static string optAlloc = "auto"; static bool gAllocVk = false; static string gAllocName = "?";
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

// ---------------- graph description (both backends)
static uint32_t gNt = 0; static uint64_t gBytes[FSR4NPU_MAXT]; static string gGraph = "stub";
static bool isQnn = false;

// ---------------- QNN state
static QNN_INTERFACE_VER_TYPE Q{}; static Qnn_GraphHandle_t G = nullptr; static Qnn_ContextHandle_t CTX = nullptr; static Qnn_BackendHandle_t BE = nullptr;
static Qnn_Tensor_t gT[FSR4NPU_MAXT];
typedef void (*RInit)(); typedef void (*RDeinit)(); typedef void* (*RAlloc)(int, uint32_t, int); typedef int (*RFd)(void*); typedef void (*RFree)(void*);
typedef int (*RegDma)(int, uint32_t);
static RAlloc rAlloc = nullptr; static RFd rToFd = nullptr; static RFree rFree = nullptr; static RDeinit rDeinit = nullptr; static RegDma rRegDma = nullptr;

static int dtypeBytes(Qnn_DataType_t d) { unsigned x = (unsigned)d & 0xff; int bits = ((x >> 4) & 0xf) * 10 + (x & 0xf); return bits >= 8 ? bits / 8 : 1; }
static uint64_t tensorBytes(const Qnn_Tensor_t& t) { uint64_t n = dtypeBytes(t.v1.dataType); for (uint32_t k = 0; k < t.v1.rank; k++) n *= t.v1.dimensions[k]; return n; }

#define QK(x) do { Qnn_ErrorHandle_t e_ = (x); if (e_ != QNN_SUCCESS) { logf("FAIL %s = 0x%lx", #x, (unsigned long)e_); return false; } } while (0)
static bool qnnInit() {
  FILE* f = fopen(optCtx.c_str(), "rb"); if (!f) { logf("FAIL cannot open context %s", optCtx.c_str()); return false; }
  fseek(f, 0, SEEK_END); size_t bsz = ftell(f); fseek(f, 0, SEEK_SET); static vector<uint8_t> bin; bin.resize(bsz);
  if (fread(bin.data(), 1, bsz, f) != bsz) { fclose(f); return false; } fclose(f);
  void* hs = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_LOCAL); void* hb = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_LOCAL); void* hr = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_GLOBAL);
  if (!hs || !hb || !hr) { logf("FAIL dlopen %s", dlerror()); return false; }
  typedef Qnn_ErrorHandle_t (*SysProv)(const QnnSystemInterface_t***, uint32_t*); typedef Qnn_ErrorHandle_t (*Prov)(const QnnInterface_t***, uint32_t*);
  const QnnSystemInterface_t** sp; const QnnInterface_t** ip; uint32_t ns = 0, ni = 0;
  QK(((SysProv)dlsym(hs, "QnnSystemInterface_getProviders"))(&sp, &ns)); QK(((Prov)dlsym(hb, "QnnInterface_getProviders"))(&ip, &ni));
  auto S = sp[0]->QNN_SYSTEM_INTERFACE_VER_NAME; Q = ip[0]->QNN_INTERFACE_VER_NAME;
  QnnSystemContext_Handle_t sc; QK(S.systemContextCreate(&sc)); const QnnSystemContext_BinaryInfo_t* bi = nullptr; Qnn_ContextBinarySize_t bis = 0;
  QK(S.systemContextGetBinaryInfo(sc, bin.data(), bsz, &bi, &bis));   // sc stays alive: gT[] dimensions point into it
  const QnnSystemContext_GraphInfo_t* gi = bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1 ? bi->contextBinaryInfoV1.graphs : bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2 ? bi->contextBinaryInfoV2.graphs : bi->contextBinaryInfoV3.graphs;
  const char* gname; const Qnn_Tensor_t *gin, *gout; uint32_t nOut;
  if (gi->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) { gname = gi->graphInfoV1.graphName; gin = gi->graphInfoV1.graphInputs; gout = gi->graphInfoV1.graphOutputs; nOut = gi->graphInfoV1.numGraphOutputs; }
  else if (gi->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) { gname = gi->graphInfoV2.graphName; gin = gi->graphInfoV2.graphInputs; gout = gi->graphInfoV2.graphOutputs; nOut = gi->graphInfoV2.numGraphOutputs; }
  else { gname = gi->graphInfoV3.graphName; gin = gi->graphInfoV3.graphInputs; gout = gi->graphInfoV3.graphOutputs; nOut = gi->graphInfoV3.numGraphOutputs; }
  if (nOut < 1 || nOut > FSR4NPU_MAXT - 1) { logf("FAIL graph has %u outputs", nOut); return false; }
  gT[0] = gin[0];
  if (nOut == 2) { int iu = strcmp(gout[0].v1.name, "p3u_o") == 0 ? 0 : 1; gT[1] = gout[iu]; gT[2] = gout[1 - iu]; }   // with two outputs, the one named p3u_o goes first
  else for (uint32_t k = 0; k < nOut; k++) gT[1 + k] = gout[k];
  gNt = 1 + nOut; gGraph = gname; for (uint32_t k = 0; k < gNt; k++) gBytes[k] = tensorBytes(gT[k]);
  Qnn_LogHandle_t lgh = nullptr;
  if (optQnnLog > 0) Q.logCreate([](const char* fmt, QnnLog_Level_t lv, uint64_t, va_list a) { fprintf(stderr, "QNNLOG%d ", (int)lv); vfprintf(stderr, fmt, a); size_t L = strlen(fmt); if (!L || fmt[L - 1] != '\n') fputc('\n', stderr); }, (QnnLog_Level_t)optQnnLog, &lgh);
  QK(Q.backendCreate(lgh, nullptr, &BE)); Qnn_DeviceHandle_t dv = nullptr; Q.deviceCreate(nullptr, nullptr, &dv);
  { QnnDevice_Infrastructure_t inf = nullptr;   // create the power config id only; the vote follows the clients (powerVote)
    if (Q.deviceGetInfrastructure(&inf) == QNN_SUCCESS && inf) { gPerf = &((QnnHtpDevice_Infrastructure_t*)inf)->perfInfra; QK(gPerf->createPowerConfigId(0, 0, &gPcid));
      logf("power: config id %u, no vote until a client connects", gPcid); } else logf("power: no HTP perf infrastructure, no votes"); }
  double t0 = now_ms(); QK(Q.contextCreateFromBinary(BE, dv, nullptr, bin.data(), bsz, &CTX, nullptr)); QK(Q.graphRetrieve(CTX, gname, &G));
  logf("context loaded %.1f ms", now_ms() - t0);
  ((RInit)dlsym(hr, "rpcmem_init"))(); rAlloc = (RAlloc)dlsym(hr, "rpcmem_alloc"); rToFd = (RFd)dlsym(hr, "rpcmem_to_fd"); rFree = (RFree)dlsym(hr, "rpcmem_free");
  rDeinit = (RDeinit)dlsym(hr, "rpcmem_deinit"); rRegDma = (RegDma)dlsym(hr, "remote_register_dma_handle");
  logf("rpcmem alloc %p to_fd %p free %p remote_register_dma_handle %p", (void*)rAlloc, (void*)rToFd, (void*)rFree, (void*)rRegDma);
  return rAlloc && rToFd;
}

static void powerVote(bool on, const char* why) {
  if (!gPerf || on == gVoted) return;
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
static void allocSelfTest() {   // qnn: can QNN register a Turnip dma-buf as SHARED_BUFFER? decided once at start
  gAllocVk = false; string mode = optAlloc;
  if (mode == "auto") { int h = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (h >= 0) { close(h); mode = "rpcmem"; logf("allocator auto: /dev/dma_heap/system is openable -> rpcmem"); }
    else { logf("allocator auto: /dev/dma_heap/system not openable (%s) -> vk", strerror(errno)); mode = "vk"; } }
  if (mode == "rpcmem") {   // without the heap libcdsprpc silently falls back to FASTRPC_IOCTL_ALLOC_DMA_BUFF (the teardown Oops described at optAlloc): check
    void* b = rAlloc(25, 1, 65536); int fd = b ? rToFd(b) : -1; string ex = fd >= 0 ? dmabufExporter(fd) : string("alloc failed"); if (b) rFree(b);
    if (ex == "system") { gAllocName = "rpcmem/heap"; logf("allocator: rpcmem on the kernel system dma-heap (exporter %s)", ex.c_str()); return; }
    if (optAlloc == "rpcmem") { gAllocName = "rpcmem/fastrpc"; logf("allocator: rpcmem FORCED, exporter '%s' is NOT the system dma-heap (teardown can Oops on 7.2.6)", ex.c_str()); return; }
    logf("allocator auto: rpcmem exporter '%s' is not the system dma-heap -> vk", ex.c_str()); }
  uint64_t n = (gBytes[0] + 4095) & ~4095ull; VkDeviceMemory vm = 0; int fd = vkAllocFd(n, &vm);
  if (fd < 0) { gAllocName = "rpcmem/fallback"; logf("allocator: vk export failed, using rpcmem (fastrpc-lifetime buffers)"); return; }
  Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
  md.memShape = {gT[0].v1.rank, gT[0].v1.dimensions, nullptr}; md.dataType = gT[0].v1.dataType;
  hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = n; hd.sharedBufferConfig.fd = fd; hd.sharedBufferConfig.offset = 0; md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd;
  Qnn_MemHandle_t mh = nullptr; Qnn_ErrorHandle_t e = Q.memRegister(CTX, &md, 1, &mh); if (e == QNN_SUCCESS) Q.memDeRegister(&mh, 1);
  close(fd); VA.freeMem(VA.dev, vm, nullptr); gAllocVk = e == QNN_SUCCESS; gAllocName = gAllocVk ? "vk" : "rpcmem/fallback";
  logf("allocator: %s (vk SHARED_BUFFER self-test rc 0x%lx)", gAllocVk ? "vk dma-buf" : "rpcmem fallback", (unsigned long)e); }

// ---------------- clients
struct Client {
  int sock = -1, kick = -1, done = -1; uid_t uid = 0; pid_t pid = 0; int id = 0;
  int fds[FSR4NPU_MAXT]; uint64_t fdSize[FSR4NPU_MAXT]; void* map[FSR4NPU_MAXT]; int nfd = 0;
  bool ready = false; uint32_t nt = 0; fsr4npu_tdesc t[FSR4NPU_MAXT];
  Qnn_MemHandle_t mh[FSR4NPU_MAXT]; Qnn_Tensor_t qt[FSR4NPU_MAXT]; bool regDma[FSR4NPU_MAXT];
  bool alt = false; fsr4npu_tdesc tAlt{}; Qnn_MemHandle_t mhAlt = nullptr; Qnn_Tensor_t qtAlt{};   // ping-pong input slot 1 (F_PINGPONG)
  bool altOut = false; uint64_t offOut1 = 0; Qnn_MemHandle_t mhOut1[FSR4NPU_MAXT]{}; Qnn_Tensor_t qtOut1[FSR4NPU_MAXT]{};   // output slot 1 (F_PINGPONG_OUT)
  void* rpcBase = nullptr; VkDeviceMemory vkMem = 0;
  void* stMap = nullptr; uint64_t stLen = 0; vector<double> vInv; long stale = 0;   // cached read-only view of the buffer (input invalidation / diagnostics)
  long frames = 0, errors = 0; vector<double> vExec, vFence, vSvc; double tConn = 0; bool timedOut = false;
};
static vector<Client*> clients; static int clientSeq = 0, clientsDone = 0;

static string fdLink(int fd) { char p[64], l[160] = {0}; snprintf(p, sizeof p, "/proc/self/fd/%d", fd); if (readlink(p, l, sizeof l - 1) < 0) return ""; return l; }
static void stat3(const char* k, vector<double> v, string& out) { if (v.empty()) return; sort(v.begin(), v.end()); char b[160];
  snprintf(b, sizeof b, " %s p50 %.3f p90 %.3f max %.3f ms", k, v[v.size() / 2], v[v.size() * 9 / 10], v.back()); out += b; }

static void unreg(Client* c) {
  for (uint32_t k = 0; k < FSR4NPU_MAXT; k++) {
    if (isQnn && c->mh[k]) { Qnn_ErrorHandle_t e = Q.memDeRegister(&c->mh[k], 1); if (e != QNN_SUCCESS) logf("client %d memDeRegister %u rc 0x%lx", c->id, k, (unsigned long)e); c->mh[k] = nullptr; }
  }
  if (isQnn && c->mhAlt) { Qnn_ErrorHandle_t e = Q.memDeRegister(&c->mhAlt, 1); if (e != QNN_SUCCESS) logf("client %d memDeRegister in1 rc 0x%lx", c->id, (unsigned long)e); c->mhAlt = nullptr; }
  for (uint32_t k = 0; k < FSR4NPU_MAXT; k++) if (isQnn && c->mhOut1[k]) { Qnn_ErrorHandle_t e = Q.memDeRegister(&c->mhOut1[k], 1); if (e != QNN_SUCCESS) logf("client %d memDeRegister out1 %u rc 0x%lx", c->id, k, (unsigned long)e); c->mhOut1[k] = nullptr; }
  c->alt = false; c->altOut = false;
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
  logf("client %d (pid %d uid %u) gone: %s; frames %ld errors %ld over %.1f s;%s", c->id, (int)c->pid, (unsigned)c->uid, why, c->frames, c->errors, (now_ms() - c->tConn) / 1e3, s.c_str());
  unreg(c); if (c->sock >= 0) close(c->sock); clients.erase(find(clients.begin(), clients.end(), c)); delete c; clientsDone++;
  if (clients.empty()) gIdleSince = now_ms();
}
static void msgInit(fsr4npu_msg& m, uint32_t type) { memset(&m, 0, sizeof m); m.magic = FSR4NPU_MAGIC; m.version = FSR4NPU_VERSION; m.type = type; }
static bool sendMsg(Client* c, const fsr4npu_msg& m, const int* fds = nullptr, int nfd = 0) {
  iovec io{(void*)&m, sizeof m}; char cm[CMSG_SPACE(sizeof(int) * FSR4NPU_MAXFD)]; memset(cm, 0, sizeof cm); msghdr mh{}; mh.msg_iov = &io; mh.msg_iovlen = 1;
  if (nfd > 0) { mh.msg_control = cm; mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfd); cmsghdr* h = CMSG_FIRSTHDR(&mh); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int) * nfd); memcpy(CMSG_DATA(h), fds, sizeof(int) * nfd); }
  return sendmsg(c->sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof m;
}
static bool sendErr(Client* c, int st, uint64_t seq, const char* text) { fsr4npu_msg m; msgInit(m, M_ERROR); m.status = st; m.seq = seq; snprintf(m.text, sizeof m.text, "%s", text); logf("client %d error %d: %s", c->id, st, text); c->errors++; return sendMsg(c, m); }

static int registerTensors(Client* c, uint32_t flags) {
  for (uint32_t k = 0; k < c->nt; k++) {
    int i = (int)c->t[k].fd_idx;
    if (!isQnn) { if (!c->map[i]) { c->map[i] = mmap(nullptr, c->fdSize[i], PROT_READ | PROT_WRITE, MAP_SHARED, c->fds[i], 0); if (c->map[i] == MAP_FAILED) { c->map[i] = nullptr; logf("client %d mmap fd %d failed: %s", c->id, c->fds[i], strerror(errno)); return E_FD; } } continue; }
    if ((flags & F_REGDMA) && !c->regDma[i]) { int r = rRegDma ? rRegDma(c->fds[i], (uint32_t)c->fdSize[i]) : -1; logf("client %d remote_register_dma_handle fd %d size %llu rc %d", c->id, c->fds[i], (unsigned long long)c->fdSize[i], r); c->regDma[i] = true; }
    Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
    c->qt[k] = gT[k]; md.memShape = {gT[k].v1.rank, gT[k].v1.dimensions, nullptr}; md.dataType = gT[k].v1.dataType;
    hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = c->fdSize[i]; hd.sharedBufferConfig.fd = c->fds[i]; hd.sharedBufferConfig.offset = c->t[k].off;
    md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd; Qnn_MemHandle_t mh = nullptr;
    Qnn_ErrorHandle_t e = Q.memRegister(CTX, &md, 1, &mh);
    logf("client %d memRegister tensor %u SHARED_BUFFER fd %d size %llu offset %llu rc 0x%lx", c->id, k, c->fds[i], (unsigned long long)c->fdSize[i], (unsigned long long)c->t[k].off, (unsigned long)e);
    if (e != QNN_SUCCESS) return E_REGISTER;
    c->mh[k] = mh; c->qt[k].v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; c->qt[k].v1.memHandle = mh;
  }
  if (c->alt && isQnn) {   // input slot 1 = graph input tensor 0 at tAlt.off of the same buffer
    int i = (int)c->tAlt.fd_idx; Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
    c->qtAlt = gT[0]; md.memShape = {gT[0].v1.rank, gT[0].v1.dimensions, nullptr}; md.dataType = gT[0].v1.dataType;
    hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = c->fdSize[i]; hd.sharedBufferConfig.fd = c->fds[i]; hd.sharedBufferConfig.offset = c->tAlt.off;
    md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd; Qnn_MemHandle_t mh = nullptr; Qnn_ErrorHandle_t e = Q.memRegister(CTX, &md, 1, &mh);
    logf("client %d memRegister input slot 1 SHARED_BUFFER fd %d offset %llu rc 0x%lx", c->id, c->fds[i], (unsigned long long)c->tAlt.off, (unsigned long)e);
    if (e != QNN_SUCCESS) return E_REGISTER;
    c->mhAlt = mh; c->qtAlt.v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; c->qtAlt.v1.memHandle = mh;
  }
  if (c->altOut && isQnn) for (uint32_t k = 1; k < c->nt; k++) {   // output slot 1 = the graph outputs at offOut1 (+ offsets relative to output 0)
    int i = (int)c->t[k].fd_idx; Qnn_MemDescriptor_t md = QNN_MEM_DESCRIPTOR_INIT; QnnMemHtp_Descriptor_t hd; memset(&hd, 0, sizeof hd);
    c->qtOut1[k] = gT[k]; md.memShape = {gT[k].v1.rank, gT[k].v1.dimensions, nullptr}; md.dataType = gT[k].v1.dataType;
    hd.type = QNN_HTP_MEM_SHARED_BUFFER; hd.size = c->fdSize[i]; hd.sharedBufferConfig.fd = c->fds[i]; hd.sharedBufferConfig.offset = c->offOut1 + (c->t[k].off - c->t[1].off);
    md.memType = QNN_MEM_TYPE_CUSTOM; md.customInfo = &hd; Qnn_MemHandle_t mh = nullptr; Qnn_ErrorHandle_t e = Q.memRegister(CTX, &md, 1, &mh);
    logf("client %d memRegister output %u slot 1 SHARED_BUFFER fd %d offset %llu rc 0x%lx", c->id, k, c->fds[i], (unsigned long long)hd.sharedBufferConfig.offset, (unsigned long)e);
    if (e != QNN_SUCCESS) return E_REGISTER;
    c->mhOut1[k] = mh; c->qtOut1[k].v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; c->qtOut1[k].v1.memHandle = mh;
  }
  return S_OK;
}
// Validate + adopt fds/tensors. bufFds: buffer fds owned by now by this function (closed on failure).
static int adopt(Client* c, const fsr4npu_msg& m, int* fds, int nbuf, int kick, int done) {
  int st = S_OK; char why[96] = "";
  if (m.ntensors != gNt) { st = E_SIZE; snprintf(why, sizeof why, "ntensors %u, graph has %u", m.ntensors, gNt); }
  for (int i = 0; i < nbuf && st == S_OK; i++) {
    string l = fdLink(fds[i]); if (l.find("dmabuf") == string::npos) { st = E_FD; snprintf(why, sizeof why, "fd %d is not a dma-buf (%s)", i, l.c_str()); break; }
    off_t sz = lseek(fds[i], 0, SEEK_END); if (sz <= 0) { st = E_FD; snprintf(why, sizeof why, "fd %d size unknown", i); break; } c->fdSize[i] = (uint64_t)sz; }
  for (uint32_t k = 0; k < m.ntensors && st == S_OK; k++) {
    const fsr4npu_tdesc& t = m.t[k];
    if ((int)t.fd_idx >= nbuf) { st = E_PROTO; snprintf(why, sizeof why, "tensor %u fd_idx %u >= %d fds", k, t.fd_idx, nbuf); break; }
    if (t.bytes && t.bytes != gBytes[k]) { st = E_SIZE; snprintf(why, sizeof why, "tensor %u bytes %llu != graph %llu", k, (unsigned long long)t.bytes, (unsigned long long)gBytes[k]); break; }
    if (t.off % FSR4NPU_ALIGN) { st = E_SIZE; snprintf(why, sizeof why, "tensor %u offset %llu not %u-aligned", k, (unsigned long long)t.off, FSR4NPU_ALIGN); break; }
    if (t.off + gBytes[k] > c->fdSize[t.fd_idx]) { st = E_SIZE; snprintf(why, sizeof why, "tensor %u off %llu + %llu > buffer %llu", k, (unsigned long long)t.off, (unsigned long long)gBytes[k], (unsigned long long)c->fdSize[t.fd_idx]); break; }
    for (uint32_t j = 0; j < k; j++) if (m.t[j].fd_idx == t.fd_idx && t.off < m.t[j].off + gBytes[j] && m.t[j].off < t.off + gBytes[k]) { st = E_SIZE; snprintf(why, sizeof why, "tensors %u and %u overlap", j, k); }
  }
  if (st == S_OK && kick >= 0 && (fdLink(kick).find("eventfd") == string::npos || fdLink(done).find("eventfd") == string::npos)) { st = E_FD; snprintf(why, sizeof why, "kick/done are not eventfds"); }
  if (st != S_OK) { for (int i = 0; i < nbuf; i++) close(fds[i]); if (kick >= 0) close(kick); if (done >= 0) close(done); sendErr(c, st, 0, why); return st; }
  unreg(c);   // re-setup replaces the previous registration
  c->nfd = nbuf; for (int i = 0; i < nbuf; i++) { c->fds[i] = fds[i]; c->map[i] = nullptr; c->regDma[i] = false; }
  c->nt = m.ntensors; for (uint32_t k = 0; k < c->nt; k++) { c->t[k] = m.t[k]; c->t[k].bytes = gBytes[k]; c->mh[k] = nullptr; }
  c->kick = kick; c->done = done;
  return S_OK;
}

static int dtHash = -1, dtRerun = 0, dtSync = 0;
static uint64_t xh(const void* p, size_t n) { const uint64_t* q = (const uint64_t*)p; uint64_t h = 0x9E3779B97F4A7C15ull; for (size_t i = 0; i < n / 8; i++) { h ^= q[i]; h *= 0x100000001B3ull; h ^= h >> 29; } return h; }
static void dsync(int fd, uint64_t f) { dma_buf_sync s{f}; if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) < 0) logf("dma-buf sync 0x%llx failed: %s", (unsigned long long)f, strerror(errno)); }
static void dtHashOut(Client* c, uint64_t* h) { const uint8_t* b = (const uint8_t*)c->map[0]; int fd = c->fds[0]; dsync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  for (uint32_t k = 0; k < c->nt; k++) h[k] = xh(b + c->t[k].off, gBytes[k]); dsync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ); }
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
  if (inP && gStDiag) { gStA.resize(gBytes[0]); memcpy(gStA.data(), inP, gBytes[0]); }   // diagnostics: the coherent view = what the HTP reads (INV=0)
  if (inP && gStInv) { double i0 = now_ms(); civac(inP, gBytes[0]); c->vInv.push_back(now_ms() - i0); }
  double t0 = now_ms(); int st = S_OK;
  uint64_t myId = ++gExecId; gExecSock = c->sock; gExecDone = c->done; gExecStart = t0;
  if (isQnn) { Qnn_ErrorHandle_t e = Q.graphExecute(G, qin, 1, qout, c->nt - 1, nullptr, nullptr); if (e != QNN_SUCCESS) { logf("client %d graphExecute rc 0x%lx", c->id, (unsigned long)e); st = E_EXEC; }
    if (dtSync & 2) for (int i = 0; i < c->nfd; i++) { dsync(c->fds[i], DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW); dsync(c->fds[i], DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW); }
    if (dh) { dtHashOut(c, hB);
      { uint64_t h1[FSR4NPU_MAXT], h5[FSR4NPU_MAXT]; usleep(1000); dtHashOut(c, h1); usleep(4000); dtHashOut(c, h5);
        bool l1 = h1[1] != hB[1] || h1[2] != hB[2], l5 = h5[1] != h1[1] || h5[2] != h1[2]; if (l1 || l5) logf("late output change: client %d frame %ld late1 %d late5 %d", c->id, c->frames, (int)l1, (int)l5); }
      if (dtRerun) { Q.graphExecute(G, qin, 1, &c->qt[1], c->nt - 1, nullptr, nullptr); dtHashOut(c, hC); }
      logf("output hash: client %d frame %ld in %016llx in_after %016llx o0 %016llx o1 %016llx%s r_o0 %016llx r_o1 %016llx ex %.3f", c->id, c->frames, (unsigned long long)hA[0], (unsigned long long)hB[0],
           (unsigned long long)hB[1], (unsigned long long)hB[2], dtRerun ? ((hC[1] != hB[1] || hC[2] != hB[2]) ? " RERUN_DIFF" : " rerun_same") : "", (unsigned long long)hC[1], (unsigned long long)hC[2], now_ms() - t0); } }
  else {
    for (int i = 0; i < c->nfd; i++) { dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW}; ioctl(c->fds[i], DMA_BUF_IOCTL_SYNC, &s); }
    const uint8_t* in = (const uint8_t*)c->map[c->t[0].fd_idx] + (((slot & 1) && c->alt) ? c->tAlt.off : c->t[0].off);
    for (uint32_t k = 1; k < c->nt; k++) memcpy((uint8_t*)c->map[c->t[k].fd_idx] + (o1 ? c->offOut1 + (c->t[k].off - c->t[1].off) : c->t[k].off), in, min(gBytes[0], gBytes[k]));
    for (int i = 0; i < c->nfd; i++) { dma_buf_sync s{DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW}; ioctl(c->fds[i], DMA_BUF_IOCTL_SYNC, &s); }
    if (optStubDelayUs > 0) { double e = now_ms() + optStubDelayUs / 1000.0; while (now_ms() < e) {} }
  }
  gExecStart = 0; *exMs = now_ms() - t0;
  if (inP && gStDiag) {   // diagnostics: memory (after civac) vs the view before execute, per 64-byte line
    civac(inP, gBytes[0]); long nl = 0; uint64_t first[4] = {}; const uint8_t* A = gStA.data();
    for (uint64_t o = 0; o < gBytes[0]; o += 64) if (memcmp(A + o, inP + o, std::min<uint64_t>(64, gBytes[0] - o))) { if (nl < 4) first[nl] = o; nl++; }
    const uint64_t hv = xh(A, gBytes[0]), hf = xh(inP, gBytes[0]); civac(inP, gBytes[0]);
    const uint64_t oOff = (slot & 2) && c->altOut ? c->offOut1 : c->t[1].off; const uint8_t* oP = (const uint8_t*)c->stMap + oOff; const uint64_t ho = xh(oP, gBytes[1]); civac(oP, gBytes[1]);
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
  if (!isQnn && st == S_OK && (m.flags & F_VERIFY)) { r.v[2] = fnv64((const uint8_t*)c->map[c->t[0].fd_idx] + c->t[0].off, gBytes[0]); r.v[3] = fnv64((const uint8_t*)c->map[c->t[1].fd_idx] + c->t[1].off, gBytes[1]); }
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
  switch (m.type) {
  case M_HELLO: { fsr4npu_msg r; msgInit(r, M_INFO); r.ntensors = gNt; r.v[0] = isQnn ? B_QNN : B_STUB; for (uint32_t k = 0; k < gNt; k++) r.t[k].bytes = gBytes[k];
    snprintf(r.text, sizeof r.text, "%s", gGraph.c_str()); for (int i = 0; i < nfd; i++) close(fds[i]); sendMsg(c, r); break; }
  case M_SETUP: {
    int kick = -1, done = -1, nbuf = nfd;
    if (m.flags & F_EFD) { if (nfd < 3) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "F_EFD needs buffer fds + 2 eventfds"); break; } kick = fds[nfd - 2]; done = fds[nfd - 1]; nbuf = nfd - 2; }
    if (nbuf < 1) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "SETUP without buffer fds"); break; }
    if (adopt(c, m, fds, nbuf, kick, done) != S_OK) break;
    int st = registerTensors(c, m.flags); if (st) { unreg(c); sendErr(c, st, 0, "register failed"); break; }
    c->ready = true; fsr4npu_msg r; msgInit(r, M_SETUP_OK); r.ntensors = c->nt; for (uint32_t k = 0; k < c->nt; k++) r.t[k] = c->t[k]; sendMsg(c, r);
    logf("client %d SETUP ok: %d buffer fd(s), %u tensors, eventfd %s", c->id, nbuf, c->nt, kick >= 0 ? "yes" : "no"); break; }
  case M_ALLOC: {
    int kick = -1, done = -1; if (m.flags & F_EFD) { if (nfd != 2) { for (int i = 0; i < nfd; i++) close(fds[i]); sendErr(c, E_PROTO, 0, "ALLOC F_EFD needs 2 eventfds"); break; } kick = fds[0]; done = fds[1]; }
    else for (int i = 0; i < nfd; i++) close(fds[i]);
    uint64_t off[FSR4NPU_MAXT], tot = 0; for (uint32_t k = 0; k < gNt; k++) { off[k] = tot; tot += (gBytes[k] + 4095) & ~4095ull; }
    uint64_t offAlt = 0; if (m.flags & F_PINGPONG) { offAlt = tot; tot += (gBytes[0] + 4095) & ~4095ull; }   // input slot 1 after the outputs
    uint64_t offOut1 = 0; if (m.flags & F_PINGPONG_OUT) { offOut1 = tot; for (uint32_t k = 1; k < gNt; k++) tot += (gBytes[k] + 4095) & ~4095ull; }   // output slot 1 after input slot 1
    int bfd = -1; void* base = nullptr; VkDeviceMemory vm = 0;
    if (isQnn && !gAllocVk) { base = rAlloc(25, 1, (int)tot); bfd = base ? rToFd(base) : -1; }
    else bfd = vkAllocFd(tot, &vm);
    if (isQnn && bfd >= 0 && !(getenv("FSR4NPUD_NO_CLEAN") && atoi(getenv("FSR4NPUD_NO_CLEAN")))) { double c0 = now_ms(); cleanFd(bfd, tot); logf("client %d buffer zeroed + cleaned to PoC (%.2f ms)", c->id, now_ms() - c0); }
    if (bfd < 0) { if (kick >= 0) { close(kick); close(done); } sendErr(c, E_ALLOC, 0, isQnn ? "rpcmem_alloc failed" : "vk export alloc failed"); break; }
    // qnn: register rpcmem's own fd (rpcmem_free closes it). stub: the exported fd is ours.
    int own = bfd;
    fsr4npu_msg am; msgInit(am, M_ALLOC); am.ntensors = gNt; for (uint32_t k = 0; k < gNt; k++) { am.t[k].fd_idx = 0; am.t[k].off = off[k]; am.t[k].bytes = gBytes[k]; }
    int one[1] = {own};
    if (base) one[0] = fcntl(bfd, F_DUPFD_CLOEXEC, 3);   // adopt() closes on failure: hand it a dup to validate, swap in bfd after
    if (adopt(c, am, one, 1, kick, done) != S_OK) { if (base) rFree(base); if (vm) VA.freeMem(VA.dev, vm, nullptr); break; }
    if (base) { close(c->fds[0]); c->fds[0] = bfd; }
    if (offAlt) { c->alt = true; c->tAlt.fd_idx = 0; c->tAlt.off = offAlt; c->tAlt.bytes = gBytes[0]; }
    if (offOut1) { c->altOut = true; c->offOut1 = offOut1; }
    c->rpcBase = base; c->vkMem = vm;
    if (isQnn && !base && getenv("FSR4NPUD_REGISTER_ATTR")) { typedef int (*RegAttr)(int, uint32_t, uint32_t); static RegAttr ra = (RegAttr)dlsym(RTLD_DEFAULT, "remote_register_dma_handle_attr");
      uint32_t at = (uint32_t)atoi(getenv("FSR4NPUD_REGISTER_ATTR")); int r = ra ? ra(c->fds[0], (uint32_t)tot, at) : -999; logf("remote_register_dma_handle_attr fd %d len %llu attr %u rc %d", c->fds[0], (unsigned long long)tot, at, r); }
    int st = registerTensors(c, 0); if (st) { unreg(c); sendErr(c, st, 0, "register failed"); break; }
    if (isQnn) { void* m = mmap(nullptr, tot, PROT_READ, MAP_SHARED, c->fds[0], 0); if (m != MAP_FAILED) { c->stMap = m; c->stLen = tot; } else logf("client %d: mmap of the input view failed: %s (no input invalidation)", c->id, strerror(errno)); }
    c->ready = true; fsr4npu_msg r; msgInit(r, M_SETUP_OK); r.ntensors = c->nt; r.total = tot; for (uint32_t k = 0; k < c->nt; k++) r.t[k] = c->t[k]; r.v[4] = c->alt ? c->tAlt.off : 0; r.v[5] = c->altOut ? c->offOut1 : 0;
    int sfd[1] = {c->fds[0]}; sendMsg(c, r, sfd, 1);
    logf("client %d ALLOC ok: %s buffer %llu bytes, %u tensors, eventfd %s", c->id, base ? "rpcmem" : "vk", (unsigned long long)tot, c->nt, kick >= 0 ? "yes" : "no"); break; }
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

int main(int argc, char** argv) {
  T0 = now_ms(); setvbuf(stderr, nullptr, _IOLBF, 0);
  for (int i = 1; i < argc; i++) { string a = argv[i]; auto nx = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); } return argv[++i]; };
    if (a == "--name") optName = nx(); else if (a == "--backend") optBackend = nx(); else if (a == "--ctx") optCtx = nx();
    else if (a == "--stub-bytes") optStubBytes = strtoull(nx(), 0, 10); else if (a == "--stub-nout") optStubNout = atoi(nx()); else if (a == "--stub-delay-us") optStubDelayUs = atoi(nx());
    else if (a == "--max-clients") optMaxClients = atoi(nx()); else if (a == "--allow-uid") allowUid.push_back((uid_t)atoi(nx())); else if (a == "--allow-gid") allowGid.push_back((gid_t)atoi(nx())); else if (a == "--exec-timeout-ms") optExecTimeoutMs = atoi(nx());
    else if (a == "--exit-after-clients") optExitAfter = atoi(nx()); else if (a == "--max-seconds") optMaxSec = atoi(nx()); else if (a == "--qnn-log") optQnnLog = atoi(nx()); else if (a == "--alloc") optAlloc = nx();
    else if (a == "--exec-hard-timeout-ms") optExecHardMs = atoi(nx()); else if (a == "--power-grace-ms") optPowerGraceMs = atoi(nx());
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; } }
  if (optAlloc != "auto" && optAlloc != "rpcmem" && optAlloc != "vk") { fprintf(stderr, "--alloc must be auto, rpcmem or vk\n"); return 2; }
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
    for (;;) { usleep(50000); uint64_t id = gExecId; double s0 = gExecStart; if (s0 <= 0 || id != gExecId) continue; double el = now_ms() - s0;
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
  if (getenv("NOTIFY_SOCKET")) sdNotify(isQnn ? "STATUS=loading QNN context" : "STATUS=stub backend");
  if (isQnn) { if (qnnInit()) allocSelfTest(); else { logf("FAIL qnn init (is vela-npu-listener.service running? journalctl -u vela-npu-listener)"); sdNotify("STATUS=FAIL qnn init"); return 3; } }
  else if (optBackend == "stub") { if (optStubNout < 1 || optStubNout > FSR4NPU_MAXT - 1) return 2; gNt = 1 + optStubNout; for (uint32_t k = 0; k < gNt; k++) gBytes[k] = optStubBytes; }
  else { fprintf(stderr, "backend must be stub or qnn\n"); return 2; }
  int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); sockaddr_un a{}; a.sun_family = AF_UNIX; memcpy(a.sun_path + 1, optName.data(), optName.size());
  if (bind(ls, (sockaddr*)&a, (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + optName.size())) < 0) { logf("FAIL bind @%s: %s (another fsr4npud running?)", optName.c_str(), strerror(errno)); return 4; }
  listen(ls, 8);
  string bs; for (uint32_t k = 0; k < gNt; k++) bs += (k ? "," : "") + to_string(gBytes[k]);
  logf("fsr4npud READY name=@%s backend=%s graph=%s tensors=%u bytes=%s alloc=%s pid=%d uid=%u", optName.c_str(), optBackend.c_str(), gGraph.c_str(), gNt, bs.c_str(), isQnn ? gAllocName.c_str() : "vk", (int)getpid(), (unsigned)getuid());
  { string st = "READY=1\nSTATUS=serving @" + optName + " graph " + gGraph + " alloc " + (isQnn ? gAllocName : string("vk")); sdNotify(st.c_str()); }
  while (!gStop) {
    vector<pollfd> p; p.push_back({ls, POLLIN, 0}); vector<pair<Client*, int>> who;
    for (Client* c : clients) { p.push_back({c->sock, POLLIN, 0}); who.push_back({c, 0}); if (c->ready && c->kick >= 0) { p.push_back({c->kick, POLLIN, 0}); who.push_back({c, 1}); } }
    int to = 1000; if (gVoted && clients.empty()) to = max(0, min(1000, (int)(gIdleSince + optPowerGraceMs - now_ms()) + 1));
    int pr = poll(p.data(), p.size(), to); if (pr < 0) { if (errno == EINTR) continue; logf("poll: %s", strerror(errno)); gExitCode = 1; break; }
    if (gVoted && clients.empty() && now_ms() - gIdleSince >= optPowerGraceMs) powerVote(false, "no client");
    if (p[0].revents & POLLIN) {
      int s = accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
      if (s >= 0) { ucred cr{}; socklen_t cl = sizeof cr; getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl);
        Client* c = new Client(); c->sock = s; c->uid = cr.uid; c->pid = cr.pid; c->id = ++clientSeq; c->tConn = now_ms();
        for (int k = 0; k < FSR4NPU_MAXT; k++) { c->fds[k] = -1; c->map[k] = nullptr; c->mh[k] = nullptr; c->regDma[k] = false; }
        clients.push_back(c);
        bool okUid = find(allowUid.begin(), allowUid.end(), cr.uid) != allowUid.end() || cr.uid == 0 || find(allowGid.begin(), allowGid.end(), cr.gid) != allowGid.end();
        logf("client %d connected pid %d uid %u%s", c->id, (int)cr.pid, (unsigned)cr.uid, okUid ? "" : " (uid not allowed)");
        if (!okUid) { sendErr(c, E_PERM, 0, "uid not allowed"); dropClient(c, "uid not allowed"); clientsDone--; }
        else if ((int)clients.size() > optMaxClients) { sendErr(c, E_BUSY, 0, "too many clients"); dropClient(c, "busy"); clientsDone--; }
        else if (isQnn && !gVoted) { char w[48]; snprintf(w, sizeof w, "client %d connected", c->id); powerVote(true, w); } }
    }
    vector<Client*> alive(clients);
    for (size_t i = 1; i < p.size(); i++) { Client* c = who[i - 1].first; if (find(clients.begin(), clients.end(), c) == clients.end()) continue;
      if (who[i - 1].second == 1) { if (p[i].revents & POLLIN) onKick(c); continue; }
      if (p[i].revents & POLLIN) handleMsg(c); else if (p[i].revents & (POLLHUP | POLLERR | POLLNVAL)) dropClient(c, "socket closed (client died?)"); }
    if (optExitAfter > 0 && clientsDone >= optExitAfter) { logf("served %d clients, exiting", clientsDone); break; }
  }
  if (gStop) logf("stop signal"); sdNotify("STOPPING=1");
  while (!clients.empty()) dropClient(clients.front(), "daemon exit");
  close(ls);
  // release order: every client's memDeRegister -> munmap -> close fds -> free buffer (dropClient above), then the QNN
  // context and backend (closes the fastrpc session), then rpcmem, then our Vulkan device last (vk buffers are already freed).
  if (isQnn) { powerVote(false, "daemon exit"); if (gPerf) gPerf->destroyPowerConfigId(gPcid); if (CTX) Q.contextFree(CTX, nullptr); if (BE) Q.backendFree(BE); if (rDeinit) rDeinit(); }
  if (VA.ok) { ((PFN_vkDestroyDevice)VA.gipa(VA.inst, "vkDestroyDevice"))(VA.dev, nullptr); ((PFN_vkDestroyInstance)VA.gipa(VA.inst, "vkDestroyInstance"))(VA.inst, nullptr); }
  logf("fsr4npud exit %d", gExitCode);
  return gExitCode;
}
