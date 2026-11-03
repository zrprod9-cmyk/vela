/* Extensions of vela_npu_abi.h v1 (provider side).
 * All fit in v1's 256-byte payload/ack without moving any field, and a layer that does not know them ignores them safely:
 *  - VELA_NPU_F_TENSORS (flags): the provider runs prepass + pack / unpack + K1/K2 itself. The images are the HTP tensors as
 *    R32_UINT 1920x1080 (8,294,400 B, word j at pixel (j%1920, j/1920), GENERAL): color = graph input x, output = out0
 *    (p3u_o), motion = out1 (p1f_o), depth unused. The layer only moves bytes (copy or, later, alias) and handshakes.
 *    A layer without these extensions copies color -> dma-buf -> daemon -> output, which is exactly this contract for a 1-output graph.
 *  - VELA_NPU_F_ASYNC (flags): kick only. The layer records copy-in + SetEvent(E1) and returns; it acks OK = "kicked".
 *  - VELA_NPU_OP_WAIT (op): wait for the job of seq (E2 wait + copy-out); ack OK = "outputs are in the images".
 *  - VelaNpuAck.reserved[0..3] = caps (uint32 LE): VELA_NPU_CAP_ASYNC when the layer implements F_ASYNC + OP_WAIT,
 *    VELA_NPU_CAP_TENSORS when it implements F_TENSORS with 2 outputs. Layers without these extensions write 0: the provider then
 *    sends sync markers only (an async provider mode still works, just with the GPU waiting inside the kick). */
#ifndef VELA_NPU_EXT_H
#define VELA_NPU_EXT_H
#include "vela_npu_abi.h"
#define VELA_NPU_F_TENSORS 8u
#define VELA_NPU_F_ASYNC 16u
#define VELA_NPU_OP_WAIT 2u
#define VELA_NPU_CAP_ASYNC 1u
#define VELA_NPU_CAP_TENSORS 2u
/* Zero-copy: the layer exposes its imported fsr4npud dma-buf VkBuffer (same VkDevice, native handle).
 *  - ack.reserved[0..3] caps |= VELA_NPU_CAP_ZEROCOPY when the set is up with 3 tensors at 16-byte offsets; then
 *    reserved[8..15] = VkBuffer (uint64), [16..19] = gen, [20..23]/[24..27]/[28..31] = in/out0/out1 offsets in 16-byte units.
 *    Every ack (PROBE, UPSCALE, WAIT) carries the CURRENT set; the buffer stays alive until vkDestroyDevice.
 *  - UPSCALE flags |= VELA_NPU_F_ZEROCOPY, cmd.pad0 = gen: the provider's pack wrote the input into that buffer and its
 *    unpack reads the outputs from it; the layer only handshakes (no copies). Stale gen -> BADARGS (GPU network). */
#define VELA_NPU_F_ZEROCOPY 32u
#define VELA_NPU_CAP_ZEROCOPY 4u
/* Graph choice and the full FSR 4.1.1 network (1-output graph nf4_exact), layer build 5:
 *  - VELA_NPU_F_GRAPH (flags, PROBE/UPSCALE/WAIT): cmd.reserved = graph name ([a-z0-9_], NUL-terminated, <= 31 chars): the layer
 *    uses the daemon fsr4npud_<graph> (env VELA_NPUD_GRAPH_NAME overrides it); a PROBE for another daemon than the current set
 *    retires the set and connects that daemon. Without the flag: VELA_NPUD_NAME or fsr4npud (Lite, unchanged).
 *  - Zero-copy (CAP_ZEROCOPY) also for 2-tensor sets (in + one output; the out1 offset is 0).
 *  - VELA_NPU_CAP_BDA: the set's buffer was imported with SHADER_DEVICE_ADDRESS (2-tensor sets, when the app enabled
 *    bufferDeviceAddress); ack.reserved[32..39] = its VkDeviceAddress (the provider's pack/unpack use it directly). */
#define VELA_NPU_F_GRAPH 64u
#define VELA_NPU_CAP_BDA 8u
/* Ping-pong HTP input (layer build 7) for providers whose prepass writes the input itself.
 *  - VELA_NPU_CAP_PINGPONG: the set has a 2nd input slot; ack.reserved[40..43] = its offset in 16-byte units (same buffer, BDA base).
 *  - VELA_NPU_F_INSLOT (UPSCALE, zero-copy only): cmd.reserved[32] = input slot (0/1) of this kick. */
#define VELA_NPU_CAP_PINGPONG 16u
#define VELA_NPU_F_INSLOT 128u
/* Ping-pong HTP output (layer build 8) for 1-output graphs whose result the provider reads after the next kick
 * (nf4_k1h: K2 reads the k1 tensor of HTP(N-1) while HTP(N) runs).
 *  - VELA_NPU_CAP_OUTPINGPONG: the set has a 2nd output slot; ack.reserved[44..47] = its offset in 16-byte units (same buffer, BDA base).
 *  - VELA_NPU_F_OUTSLOT (UPSCALE, zero-copy only): cmd.reserved[33] = output slot (0/1) the HTP writes for this kick. */
#define VELA_NPU_CAP_OUTPINGPONG 32u
#define VELA_NPU_F_OUTSLOT 256u
#endif
