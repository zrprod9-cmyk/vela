/* NPU layer test app (native aarch64, no Proton, no game). Plays the fork (contract: vela_npu_abi.h + vela_npu_ext.h)
 * against the real VK_LAYER_VELA_npu + fsr4npud stub (stub copies input -> every output):
 *   tensor images tin/out0/out1 R32_UINT WxH in GENERAL; per frame: clear out0/out1 to 0, [async: OP_WAIT(seq f-1) into
 *   marker word k=1], clear tin to colour(f) (the "pack"), UPSCALE|F_TENSORS[|F_ASYNC] (k=0), readback out0/out1.
 *   exact: final ack.status 0 -> out0 == out1 == colour(f).  async: the WAIT ack's final status 0 -> == colour(f-1).
 *   Wrong = final status 0 but other bytes (must be 0).  Frames with a non-zero final status are fallbacks (GPU path).
 * Usage: wl_app exact|async|plain <frames> [W H]     plain = no marker, tin copied to out0/out1 by the app (baseline).
 * Env: WL_INFLIGHT 1|2 (2), WL_FRAME_MS (pacing), WL_SUBMIT2=1, WL_DEV2=1 (second VkDevice + PROBE), WL_EXPECT_NONPU=1. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "vela_npu_abi.h"
#include "vela_npu_ext.h"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(3); } } while (0)
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkInstance inst;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  printf("FAIL no memory type\n"); exit(3);
}
static VkBuffer mkbuf(VkDevice dev, VkDeviceSize sz, VkBufferUsageFlags u, int host, void** map, VkDeviceMemory* mem) {
  VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = sz; bc.usage = u; VkBuffer b; CK(vkCreateBuffer(dev, &bc, NULL, &b));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
  VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size;
  ma.memoryTypeIndex = mtype(mr.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CK(vkAllocateMemory(dev, &ma, NULL, mem)); CK(vkBindBufferMemory(dev, b, *mem, 0));
  if (map) CK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
  return b;
}
static VkDevice mkdev(int sync2) {
  float pr = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = 0; q.queueCount = 1; q.pQueuePriorities = &pr;
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.synchronization2 = VK_TRUE;
  VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &q; if (sync2) dc.pNext = &f13;
  VkDevice d; CK(vkCreateDevice(pd, &dc, NULL, &d)); return d;   /* NO external-memory extensions, like vkd3d */
}
static long rss_kb(void) { FILE* f = fopen("/proc/self/statm", "r"); long a = 0, b = 0; if (f) { if (fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; fclose(f); } return b * (sysconf(_SC_PAGESIZE) / 1024); }
static int nfds(void) { int n = 0; DIR* d = opendir("/proc/self/fd"); struct dirent* e; if (!d) return -1; while ((e = readdir(d))) n += e->d_name[0] != '.'; closedir(d); return n; }
static uint32_t colour(uint64_t seq) { uint32_t h = (uint32_t)(seq * 2654435761u) ^ 0x5A5A1234u; h ^= h >> 15; return h | 1u; }
static VelaNpuImage nimg(VkImage im, uint32_t w, uint32_t h) { VelaNpuImage r; memset(&r, 0, sizeof r); r.image = (uint64_t)im; r.format = VK_FORMAT_R32_UINT; r.layout = VK_IMAGE_LAYOUT_GENERAL; r.width = w; r.height = h; r.aspect = VK_IMAGE_ASPECT_COLOR_BIT; return r; }

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "exact"; int frames = argc > 2 ? atoi(argv[2]) : 600;
  uint32_t W = argc > 4 ? (uint32_t)atoi(argv[3]) : 1920, H = argc > 4 ? (uint32_t)atoi(argv[4]) : 1080; VkDeviceSize NB = (VkDeviceSize)W * H * 4;
  const int plain = !strcmp(mode, "plain"), async = !strcmp(mode, "async");
  const int inflight = getenv("WL_INFLIGHT") ? atoi(getenv("WL_INFLIGHT")) : 2, sync2 = getenv("WL_SUBMIT2") != NULL;
  const double pace = getenv("WL_FRAME_MS") ? atof(getenv("WL_FRAME_MS")) : 0;
  VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &ai; CK(vkCreateInstance(&ic, NULL, &inst));
  uint32_t np = 8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst, &np, pds)); pd = pds[0];
  for (uint32_t i = 0; i < np; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) { pd = pds[i]; break; } }
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  VkDevice dev = mkdev(sync2); VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
  PFN_vkQueueSubmit2 qs2 = sync2 ? (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(dev, "vkQueueSubmit2") : NULL;
  VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool; CK(vkCreateCommandPool(dev, &pc, NULL, &pool));
  enum { R = 2 };
  VkCommandBuffer cb[R]; VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = R; CK(vkAllocateCommandBuffers(dev, &ca, cb));
  VkFence fence[R]; VkBuffer mk[R], down[R]; VkDeviceMemory mkm[R], downm[R]; void *mkp[R], *downp[R];
  for (int r = 0; r < R; r++) {
    VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; CK(vkCreateFence(dev, &fc, NULL, &fence[r]));
    mk[r] = mkbuf(dev, 512, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &mkp[r], &mkm[r]);   /* word k=0 @0, k=1 @256 */
    down[r] = mkbuf(dev, 2 * NB, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &downp[r], &downm[r]);
  }
  VkImage img[3]; VkDeviceMemory imgm[3];
  for (int i = 0; i < 3; i++) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R32_UINT; ii.extent = (VkExtent3D){W, H, 1};
    ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    CK(vkCreateImage(dev, &ii, NULL, &img[i])); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img[i], &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size; ma.memoryTypeIndex = mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkAllocateMemory(dev, &ma, NULL, &imgm[i])); CK(vkBindImageMemory(dev, img[i], imgm[i], 0));
  }
  VkImage tin = img[0], o0 = img[1], o1 = img[2];
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkBufferImageCopy rg = {0}; rg.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageExtent = (VkExtent3D){W, H, 1};
#define MBAR(c) do { VkMemoryBarrier m_ = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT}; \
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m_, 0, NULL, 0, NULL); } while (0)
  VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  VelaNpuCmd c; memset(&c, 0, sizeof c); c.magic = VELA_NPU_MAGIC_CMD; c.version = VELA_NPU_VERSION; c.op = VELA_NPU_OP_PROBE;
  CK(vkBeginCommandBuffer(cb[0], &bi));
  for (int i = 0; i < 3; i++) { VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img[i]; b.subresourceRange = sr;
    vkCmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b); }
  memset(mkp[0], 0, 512); vkCmdUpdateBuffer(cb[0], mk[0], 0, sizeof c, &c);
  vkCmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
  CK(vkEndCommandBuffer(cb[0])); { VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb[0];
    CK(vkQueueSubmit(q, 1, &si, fence[0])); CK(vkWaitForFences(dev, 1, &fence[0], VK_TRUE, 3000000000ull)); vkResetFences(dev, 1, &fence[0]);
    const VelaNpuAck* a = (const VelaNpuAck*)mkp[0]; uint32_t caps; memcpy(&caps, a->reserved, 4);
    printf("probe: marker %s status %u layerBuild %u caps %u\n", a->magic == VELA_NPU_MAGIC_ACK ? "ACK" : a->magic == VELA_NPU_MAGIC_CMD ? "CMD (no layer)" : "none", a->status, a->layerBuild, caps); }
  VkDevice dev2 = VK_NULL_HANDLE;
  if (getenv("WL_DEV2")) {  /* a second VkDevice in the same process, its own PROBE (own daemon client) */
    dev2 = mkdev(0); VkQueue q2; vkGetDeviceQueue(dev2, 0, 0, &q2); VkCommandPool p2; CK(vkCreateCommandPool(dev2, &pc, NULL, &p2));
    VkCommandBuffer c2; VkCommandBufferAllocateInfo a2 = ca; a2.commandPool = p2; a2.commandBufferCount = 1; CK(vkAllocateCommandBuffers(dev2, &a2, &c2));
    VkDeviceMemory m2; void* p2m; VkBuffer b2 = mkbuf(dev2, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &p2m, &m2);
    CK(vkBeginCommandBuffer(c2, &bi)); vkCmdUpdateBuffer(c2, b2, 0, sizeof c, &c); vkCmdPipelineBarrier(c2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL); CK(vkEndCommandBuffer(c2));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &c2; CK(vkQueueSubmit(q2, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q2));
    const VelaNpuAck* a = (const VelaNpuAck*)p2m; printf("device 2: probe ack %s status %u\n", a->magic == VELA_NPU_MAGIC_ACK ? "ACK" : "none", a->status);
    vkDestroyBuffer(dev2, b2, NULL); vkFreeMemory(dev2, m2, NULL); vkDestroyCommandPool(dev2, p2, NULL);
  }
  long ok = 0, wrong = 0, fbs[8] = {0}, hangs = 0, rss100 = 0; int fd100 = 0;
  double sRec = 0, mRec = 0, sSub = 0, mSub = 0, sFen = 0, mFen = 0, sHtp = 0, tSubAt[R] = {0}, tStart = now_ms(); int pendingF[R] = {-1, -1};
  for (int f = 0; f <= frames + R; f++) {
    int r = f % R, pr = inflight == 1 ? (f + R - 1) % R : r;   /* serial: collect the frame just submitted */
    if (pendingF[pr] >= 0) {
      int rr = pr, pf = pendingF[rr];
      VkResult wr = vkWaitForFences(dev, 1, &fence[rr], VK_TRUE, 2000000000ull);
      if (wr != VK_SUCCESS) { hangs++; printf("FAIL frame %d: fence %d (GPU hang?)\n", pf, wr); break; }
      double fen = now_ms() - tSubAt[rr]; sFen += fen; if (fen > mFen) mFen = fen; vkResetFences(dev, 1, &fence[rr]);
      const VelaNpuAck* a = (const VelaNpuAck*)((char*)mkp[rr] + (async ? 256 : 0));   /* the ack that carries the final status */
      const uint32_t* px = (const uint32_t*)downp[rr]; int expectSeq = async ? pf - 1 : pf;
      if (async && pf == 0) { /* no WAIT in frame 0 */ }
      else if (plain || (a->magic == VELA_NPU_MAGIC_ACK && a->status == 0)) {
        uint32_t want = colour(plain ? (uint64_t)pf : (uint64_t)expectSeq), htp; memcpy(&htp, a->reserved + 4, 4); int eq = 1;
        for (VkDeviceSize i = 0; i < NB / 4; i += 61) eq &= px[i] == want && px[NB / 4 + i] == want;
        eq &= px[NB / 4 - 1] == want && px[NB / 2 - 1] == want;
        if (!plain) { eq &= a->seq == (uint64_t)expectSeq; sHtp += htp / 1000.0; }
        ok += eq; wrong += !eq;
        if (!eq && wrong < 5) printf("WRONG frame %d: ack seq %llu out0[0] %08x out1[0] %08x want %08x\n", pf, (unsigned long long)a->seq, px[0], px[NB / 4], want);
      } else fbs[a->magic == VELA_NPU_MAGIC_ACK ? (a->status < 6 ? a->status : 6) : 7]++;
      pendingF[rr] = -1;
    }
    if (f == 100) { rss100 = rss_kb(); fd100 = nfds(); }
    if (f >= frames) continue;
    if (pace > 0) { double next = tStart + f * pace; while (now_ms() < next) { struct timespec ts = {0, 200000}; nanosleep(&ts, NULL); } }
    VkCommandBuffer b = cb[r]; CK(vkResetCommandBuffer(b, 0)); CK(vkBeginCommandBuffer(b, &bi)); memset(mkp[r], 0, 512);
    VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(b, o0, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr); vkCmdClearColorImage(b, o1, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr); MBAR(b);
    double rec = 0;
    if (async && f > 0) {  /* POST(f-1): wait for the previous kick, outputs land in o0/o1 */
      memset(&c, 0, sizeof c); c.magic = VELA_NPU_MAGIC_CMD; c.version = VELA_NPU_VERSION; c.op = VELA_NPU_OP_WAIT; c.seq = (uint64_t)(f - 1);
      double t0 = now_ms(); vkCmdUpdateBuffer(b, mk[r], 256, sizeof c, &c); rec += now_ms() - t0;
      MBAR(b); rg.bufferOffset = 0; vkCmdCopyImageToBuffer(b, o0, VK_IMAGE_LAYOUT_GENERAL, down[r], 1, &rg); rg.bufferOffset = NB; vkCmdCopyImageToBuffer(b, o1, VK_IMAGE_LAYOUT_GENERAL, down[r], 1, &rg); MBAR(b);
    }
    VkClearColorValue cc; memset(&cc, 0, sizeof cc); cc.uint32[0] = colour(f); vkCmdClearColorImage(b, tin, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &sr); MBAR(b);   /* "pack" */
    if (plain) {
      VkImageCopy ic2 = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
      vkCmdCopyImage(b, tin, VK_IMAGE_LAYOUT_GENERAL, o0, VK_IMAGE_LAYOUT_GENERAL, 1, &ic2); vkCmdCopyImage(b, tin, VK_IMAGE_LAYOUT_GENERAL, o1, VK_IMAGE_LAYOUT_GENERAL, 1, &ic2);
    } else {
      memset(&c, 0, sizeof c); c.magic = VELA_NPU_MAGIC_CMD; c.version = VELA_NPU_VERSION; c.op = VELA_NPU_OP_UPSCALE; c.seq = (uint64_t)f;
      c.flags = VELA_NPU_F_TENSORS | (async ? VELA_NPU_F_ASYNC : 0); c.color = nimg(tin, W, H); c.output = nimg(o0, W, H); c.motion = nimg(o1, W, H);
      double t0 = now_ms(); vkCmdUpdateBuffer(b, mk[r], 0, sizeof c, &c); rec += now_ms() - t0;
    }
    sRec += rec; if (rec > mRec) mRec = rec;
    if (!async) { MBAR(b); rg.bufferOffset = 0; vkCmdCopyImageToBuffer(b, o0, VK_IMAGE_LAYOUT_GENERAL, down[r], 1, &rg); rg.bufferOffset = NB; vkCmdCopyImageToBuffer(b, o1, VK_IMAGE_LAYOUT_GENERAL, down[r], 1, &rg); }
    vkCmdPipelineBarrier(b, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(b));
    double t0 = now_ms();
    if (qs2) { VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; ci.commandBuffer = b;
               VkSubmitInfo2 s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; s2.commandBufferInfoCount = 1; s2.pCommandBufferInfos = &ci; CK(qs2(q, 1, &s2, fence[r])); }
    else { VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &b; CK(vkQueueSubmit(q, 1, &si, fence[r])); }
    double ds = now_ms() - t0; sSub += ds; if (ds > mSub) mSub = ds; tSubAt[r] = t0; pendingF[r] = f;
  }
  long rssEnd = rss_kb(); int fdEnd = nfds(); double total = now_ms() - tStart;
  long fb = 0; for (int k = 0; k < 8; k++) fb += fbs[k];
  printf("RESULT mode %s %ux%u frames %d: ok %ld, wrong %ld, fallback %ld (NODAEMON %ld BADARGS %ld BUSY %ld status5 %ld other %ld noack %ld), hangs %ld, HTP(ack) mean %.3f ms, %.0f ms\n",
         mode, W, H, frames, ok, wrong, fb, fbs[1], fbs[2], fbs[3], fbs[5], fbs[4] + fbs[6], fbs[7], hangs, ok ? sHtp / ok : 0, total);
  printf("TIMING mode %s inflight %d: marker record mean %.4f max %.3f ms | vkQueueSubmit mean %.4f max %.3f ms | submit->fence mean %.3f max %.3f ms\n",
         mode, inflight, sRec / frames, mRec, sSub / frames, mSub, sFen / frames, mFen);
  printf("RSS frame100 %ld kB end %ld kB (delta %ld kB), fds %d -> %d\n", rss100, rssEnd, rssEnd - rss100, fd100, fdEnd);
  vkDeviceWaitIdle(dev);
  if (dev2) vkDestroyDevice(dev2, NULL);
  for (int r = 0; r < R; r++) { vkDestroyFence(dev, fence[r], NULL); vkDestroyBuffer(dev, mk[r], NULL); vkFreeMemory(dev, mkm[r], NULL); vkDestroyBuffer(dev, down[r], NULL); vkFreeMemory(dev, downm[r], NULL); }
  for (int i = 0; i < 3; i++) { vkDestroyImage(dev, img[i], NULL); vkFreeMemory(dev, imgm[i], NULL); }
  vkDestroyCommandPool(dev, pool, NULL); vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
  int pass = wrong == 0 && hangs == 0 && (plain || ok > 0 || getenv("WL_EXPECT_NONPU"));
  printf("TEST %s %s\n", mode, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
