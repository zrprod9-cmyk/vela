// Per-frame constants straight from the game's FSR 2 constant buffer on the GPU.
//
// RE4 builds cbFSR2 on the GPU (it is neither CPU-mapped nor filled by a buffer copy we could follow), so the
// values that change every frame -- jitter, pre-exposure -- cannot be read on the CPU in time. Instead, inside our
// interop command buffer and before our passes, we copy those few bytes from the game's buffer (its VkBuffer from
// vkd3d interop) over the same fields of this frame's slot in our own uniform buffer. The fields that do not change
// (sizes, MV scale, FOV) are filled on the CPU once, from a one-time GPU readback. Zero CPU latency, no stall.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

struct GpuPatch
{
    uint64_t srcOff; // byte offset inside `src` (already includes the resource's offset within the VkBuffer)
    uint32_t dstOff; // byte offset inside our per-frame uniform slot
    uint32_t size;
};

struct GpuPatches
{
    VkBuffer src = VK_NULL_HANDLE;
    std::vector<GpuPatch> list;
};

// vkCmdCopyBuffer through vkGetDeviceProcAddr (the toolchain's vulkan-1 import library lists only what the fork
// uses); the upscalers call GpuPatchInit(device) in their Init.
inline PFN_vkCmdCopyBuffer& GpuPatchCopyFn()
{
    static PFN_vkCmdCopyBuffer f = nullptr;
    return f;
}
inline void GpuPatchInit(VkDevice dev)
{
    if (!GpuPatchCopyFn()) GpuPatchCopyFn() = (PFN_vkCmdCopyBuffer) vkGetDeviceProcAddr(dev, "vkCmdCopyBuffer");
}

// Record the copies into `dst` at `dstBase` and make them visible to the shaders that read the uniform buffer.
inline void RecordGpuPatches(VkCommandBuffer cb, const GpuPatches& p, VkBuffer dst, uint64_t dstBase)
{
    if (p.src == VK_NULL_HANDLE || p.list.empty() || !GpuPatchCopyFn()) return;
    std::vector<VkBufferCopy> r;
    for (auto& q : p.list) r.push_back({q.srcOff, dstBase + q.dstOff, q.size});
    VkMemoryBarrier pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    pre.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0, nullptr,
                         0, nullptr);
    GpuPatchCopyFn()(cb, p.src, dst, (uint32_t) r.size(), r.data());
    VkMemoryBarrier post{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &post, 0, nullptr, 0, nullptr);
}
