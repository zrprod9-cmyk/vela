/* SPDX-License-Identifier: GPL-3.0-or-later WITH AdditionRef-Vela-Qualcomm-QNN-linking-exception
 * Copyright (C) 2026 the Vela author
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program (the file LICENSE at the top
 * of the Vela repository). If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7
 *
 * If you modify this Program, or any covered work, by linking or combining it with the Qualcomm AI Engine Direct (QNN)
 * runtime libraries or the Qualcomm AI Engine Direct SDK header files (or a modified version of them), containing parts
 * covered by the terms of the Qualcomm AI Stack License, the licensors of this Program grant you additional permission to
 * convey the resulting work. This includes loading those libraries at run time (for example with dlopen()).
 *
 * fsr4npu client library (C, aarch64 Linux). Talks to fsr4npud over the abstract AF_UNIX socket. Not thread-safe per handle. */
#ifndef FSR4NPU_H
#define FSR4NPU_H
#include <stdint.h>
#include "fsr4npu_proto.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fsr4npu fsr4npu;
typedef struct { uint32_t ntensors, backend; uint64_t bytes[FSR4NPU_MAXT]; char graph[64]; } fsr4npu_info;
typedef struct { int fd; uint64_t off; } fsr4npu_tensor;          /* tensor k lives in dma-buf fd at byte offset off */
typedef struct { uint64_t exec_ns, fence_ns, fnv_in, fnv_out, rtt_ns; } fsr4npu_stats;

/* Connect (retries for timeout_ms, so a daemon that is still starting or restarting is found). 0 or negative status. */
int  fsr4npu_connect(const char* name, int timeout_ms, fsr4npu** h, fsr4npu_info* info);
/* Register client-owned dma-bufs (several tensors may share one fd at different offsets). The fds are dup'd (kept for reconnect). */
int  fsr4npu_setup(fsr4npu* h, uint32_t nt, const fsr4npu_tensor* t, int use_efd);
int  fsr4npu_setup_flags(fsr4npu* h, uint32_t nt, const fsr4npu_tensor* t, int use_efd, uint32_t extra_flags);
/* Let the daemon allocate one shared buffer (QNN rpcmem in qnn mode). *fd is the caller's; off[k] = tensor k offset. */
int  fsr4npu_alloc(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT]);
/* ALLOC with a 2nd input slot (ping-pong); *in1 = its byte offset in the same buffer, 0 = daemon without ping-pong. */
int  fsr4npu_alloc_pp(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT], uint64_t* in1);
/* eventfd kick that runs the graph on input slot 0 or 1 (slot 1 needs alloc_pp with *in1 != 0). */
int  fsr4npu_kick_slot(fsr4npu* h, int slot);
/* ALLOC with input slot 1 and output slot 1; *out1 = byte offset of output slot 1 (0 = daemon without it). */
int  fsr4npu_alloc_pp2(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT], uint64_t* in1, uint64_t* out1);
/* eventfd kick on input slot in_slot and output slot out_slot (0/1 each). */
int  fsr4npu_kick_slots(fsr4npu* h, int in_slot, int out_slot);
/* One frame, blocking. sync_fd >= 0: a sync_file the daemon waits on before executing (not closed here). */
int  fsr4npu_run(fsr4npu* h, int sync_fd, int timeout_ms, fsr4npu_stats* st);
/* eventfd fast path (setup/alloc with use_efd=1): kick, then wait for done. */
int  fsr4npu_kick(fsr4npu* h);
int  fsr4npu_wait(fsr4npu* h, int timeout_ms);
/* After E_DISCONNECTED: reconnect and replay the same SETUP (not possible after ALLOC: returns E_STATE, redo alloc). */
int  fsr4npu_reconnect(fsr4npu* h, int timeout_ms);
/* stub backend: ask the daemon for fnv(in), fnv(out0) on each RUN (costly, test only). */
void fsr4npu_set_verify(fsr4npu* h, int on);
void fsr4npu_close(fsr4npu* h);
const char* fsr4npu_strerror(int status);
/* low-level, for tests: send a raw message with fds, receive one. */
int  fsr4npu_raw_send(fsr4npu* h, const struct fsr4npu_msg* m, const int* fds, int nfd);
int  fsr4npu_raw_recv(fsr4npu* h, struct fsr4npu_msg* m, int* fds, int* nfd, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
