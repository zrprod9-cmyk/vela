/* The ABI between the Vela upscaler (OptiScaler fork, ARM64EC PE inside the game) and the native aarch64 Vulkan
 * layer that drives the Hexagon NPU through fsr4npud. Two documented-Vulkan entry routes, same payload:
 *  1. native apps / native games: vkGetDeviceProcAddr(dev, VELA_NPU_ENTRY_NAME) -> PFN_vkCmdVelaNpuUpscaleVELA
 *     (the Vulkan loader hands unknown device names to the top layer's vkGetDeviceProcAddr);
 *  2. PE code under Proton: winevulkan returns NULL for unknown names ("Unsupported device function"), so the fork
 *     records vkCmdUpdateBuffer(cb, markerBuffer, 0, sizeof(VelaNpuCmd), &cmd). winevulkan thunks vkCmdUpdateBuffer
 *     as core 1.0 (pData passed through untouched, VkImage/VkBuffer handles are not wrapped), the layer below sees the
 *     payload, replaces the write with its own work and writes a VelaNpuAck into the same buffer instead.
 *     Without the layer the driver just writes the payload: the fork reads magic == CMD back and stays on its GPU path.
 * All handles are the native (host) handles; the fork gets them from vkd3d-proton's GetVulkanResourceInfo1. */
#ifndef VELA_NPU_ABI_H
#define VELA_NPU_ABI_H
#include <stdint.h>
#include <vulkan/vulkan.h>

#define VELA_NPU_MAGIC_CMD 0x31435056414C4556ull /* "VELAVPC1" */
#define VELA_NPU_MAGIC_ACK 0x314B4356414C4556ull /* "VELAVCK1" */
#define VELA_NPU_VERSION 1u
#define VELA_NPU_ENTRY_NAME "vkCmdVelaNpuUpscaleVELA"

enum { VELA_NPU_OP_PROBE = 0, VELA_NPU_OP_UPSCALE = 1 };
enum { VELA_NPU_F_RESET = 1u, VELA_NPU_F_HDR = 2u, VELA_NPU_F_DEPTH_INVERTED = 4u };
enum { VELA_NPU_ACK_OK = 0, VELA_NPU_ACK_NODAEMON = 1, VELA_NPU_ACK_BADARGS = 2, VELA_NPU_ACK_BUSY = 3 };

typedef struct VelaNpuImage { uint64_t image; uint32_t format, layout, width, height, aspect, pad; } VelaNpuImage; /* 32 B */

typedef struct VelaNpuCmd {                       /* 256 B, a multiple of 4 (vkCmdUpdateBuffer rule) */
  uint64_t magic; uint32_t version, op;
  uint64_t seq; uint32_t flags, pad0;
  VelaNpuImage color, depth, motion, output;      /* layouts = what the image is in at the call and must be in after */
  float jitter[2], mvScale[2];
  float preExposure, frameTimeMs, cameraNear, cameraFar, fovV, pad1;
  uint8_t reserved[256 - 32 - 4 * 32 - 40];
} VelaNpuCmd;

typedef struct VelaNpuAck {                       /* written by the layer (GPU timeline) in place of the payload */
  uint64_t magic; uint32_t version, status;
  uint64_t seq; uint32_t layerBuild, slot;
  uint8_t reserved[256 - 32];
} VelaNpuAck;

#ifdef __cplusplus
static_assert(sizeof(VelaNpuCmd) == 256, "VelaNpuCmd size");
static_assert(sizeof(VelaNpuAck) == 256, "VelaNpuAck size");
#endif
typedef void (VKAPI_PTR* PFN_vkCmdVelaNpuUpscaleVELA)(VkCommandBuffer cb, const VelaNpuCmd* cmd);
#endif
