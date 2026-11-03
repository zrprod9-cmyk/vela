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
 * fsr4npu wire protocol v1. AF_UNIX abstract SOCK_SEQPACKET, one fixed-size message per packet,
 * file descriptors travel as SCM_RIGHTS on the same packet. Little-endian, same host only.
 *
 *  client                                   fsr4npud
 *  HELLO                         ------->   (checks SO_PEERCRED uid)
 *                                <-------   INFO   ntensors, bytes[] per tensor (t[0] = input, t[1..] = outputs), graph name
 *  SETUP  fds=[buf fds.., kick efd, done efd]   t[i] = {fd_idx, off, bytes}; flags F_EFD when the last 2 fds are eventfds
 *     or ALLOC fds=[kick efd, done efd]  daemon allocates ONE buffer (in @0, outs at NB multiples), returns its fd
 *                                <-------   SETUP_OK (ALLOC: fds=[buffer fd], t[] = layout, total) | ERROR status
 *  per frame, either
 *    RUN seq [fds=[sync_file]] F_SYNCFD  ->  daemon waits the sync_file (GPU done), QnnGraph_execute
 *                                <-------   DONE seq status v[0]=exec ns v[1]=fence-wait ns v[2]=fnv(in) v[3]=fnv(out0) (stub only)
 *    or write(kick efd, 1)        ------->   execute; write(done efd, 1) on success, 2 + (-status) on failure
 *  BYE                            ------->   daemon frees mem handles / buffers and closes; client death = same cleanup */
#ifndef FSR4NPU_PROTO_H
#define FSR4NPU_PROTO_H
#include <stdint.h>

#define FSR4NPU_MAGIC   0x3450534Eu   /* "NSP4" */
#define FSR4NPU_VERSION 1u
#define FSR4NPU_MAXT    4             /* 1 input + up to 3 outputs */
#define FSR4NPU_MAXFD   6             /* up to 4 buffer fds + kick + done */
#define FSR4NPU_ALIGN   64u           /* tensor offsets must be multiples of this */
#define FSR4NPU_DEFAULT_NAME "fsr4npud"

enum fsr4npu_type { M_HELLO = 1, M_INFO, M_SETUP, M_ALLOC, M_SETUP_OK, M_RUN, M_DONE, M_ERROR, M_BYE };
enum fsr4npu_flags { F_EFD = 1u, F_SYNCFD = 2u, F_REGDMA = 4u /* qnn: remote_register_dma_handle the client fds first */,
                     F_VERIFY = 8u /* stub RUN: return fnv of in/out0 (slow: CPU reads of uncached dma-buf memory, ~100 ms per 8.3 MB) */ };
/* Ping-pong input: ALLOC flag; SETUP_OK v[4] = byte offset of input slot 1 (0 = not granted). Kick value KICK_IN1 or
 * RUN F_PINGPONG + v[0] = 1 runs the graph on input slot 1. Old daemons ignore the flag (v[4] = 0). */
enum { F_PINGPONG = 16u };
/* Ping-pong output: ALLOC flag; SETUP_OK v[5] = byte offset of output slot 1 (the outputs again, 4 KiB-rounded, graph order;
 * 0 = not granted). Kick bit 33 (FSR4NPU_KICK_OUT1) or RUN F_PINGPONG + v[0] bit 1 writes the outputs to slot 1. Old daemons: v[5] = 0. */
enum { F_PINGPONG_OUT = 32u };
#define FSR4NPU_KICK_OUT1 ((1ull << 33) | 1ull)
#define FSR4NPU_KICK_IN1 ((1ull << 32) | 1ull)
enum fsr4npu_status {
  S_OK = 0, E_PROTO = -1, E_SIZE = -2, E_FD = -3, E_STATE = -4, E_EXEC = -5, E_BUSY = -6, E_FENCE = -7,
  E_PERM = -8, E_ALLOC = -9, E_DISCONNECTED = -10, E_TIMEOUT = -11, E_SYS = -12, E_REGISTER = -13
};
enum fsr4npu_backend { B_STUB = 0, B_QNN = 1 };

struct fsr4npu_tdesc { uint32_t fd_idx, pad; uint64_t off, bytes; };
struct fsr4npu_msg {
  uint32_t magic, type, version, flags;
  uint64_t seq;
  int32_t status; uint32_t ntensors;
  struct fsr4npu_tdesc t[FSR4NPU_MAXT];
  uint64_t total;
  uint64_t v[6];
  char text[64];
};
#endif
