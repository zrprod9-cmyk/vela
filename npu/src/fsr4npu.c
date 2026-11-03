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
 * fsr4npu client library implementation (see fsr4npu.h, fsr4npu_proto.h). */
#define _GNU_SOURCE
#include "fsr4npu.h"
static __thread uint64_t tl_out1;   /* v[5] of the last ALLOC on this thread */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct fsr4npu {
  int sock, kick, done; char name[96]; uint64_t seq;
  int alloc_mode, use_efd; uint32_t nt, xflags, verify; fsr4npu_tensor t[FSR4NPU_MAXT]; /* t[].fd = our dup, for reconnect */
};
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

const char* fsr4npu_strerror(int s) {
  switch (s) { case S_OK: return "ok"; case E_PROTO: return "protocol"; case E_SIZE: return "wrong size/offset";
    case E_FD: return "bad fd (not a dma-buf?)"; case E_STATE: return "wrong state"; case E_EXEC: return "execute failed";
    case E_BUSY: return "daemon busy (max clients)"; case E_FENCE: return "sync_file wait failed"; case E_PERM: return "permission (uid)";
    case E_ALLOC: return "alloc failed"; case E_DISCONNECTED: return "disconnected"; case E_TIMEOUT: return "timeout";
    case E_SYS: return "system error"; case E_REGISTER: return "QNN memRegister failed"; default: return "unknown"; }
}
int fsr4npu_raw_send(fsr4npu* h, const struct fsr4npu_msg* m, const int* fds, int nfd) {
  struct iovec io = {(void*)m, sizeof *m}; char cm[CMSG_SPACE(sizeof(int) * FSR4NPU_MAXFD)]; memset(cm, 0, sizeof cm);
  struct msghdr mh; memset(&mh, 0, sizeof mh); mh.msg_iov = &io; mh.msg_iovlen = 1;
  if (nfd > 0) { mh.msg_control = cm; mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfd); struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * nfd); memcpy(CMSG_DATA(c), fds, sizeof(int) * nfd); }
  ssize_t r = sendmsg(h->sock, &mh, MSG_NOSIGNAL);
  return r == (ssize_t)sizeof *m ? 0 : E_DISCONNECTED;
}
int fsr4npu_raw_recv(fsr4npu* h, struct fsr4npu_msg* m, int* fds, int* nfd, int timeout_ms) {
  struct pollfd p = {h->sock, POLLIN, 0}; int pr = poll(&p, 1, timeout_ms);
  if (pr == 0) return E_TIMEOUT; if (pr < 0) return E_SYS;
  struct iovec io = {m, sizeof *m}; char cm[CMSG_SPACE(sizeof(int) * FSR4NPU_MAXFD)];
  struct msghdr mh; memset(&mh, 0, sizeof mh); mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = cm; mh.msg_controllen = sizeof cm;
  ssize_t r = recvmsg(h->sock, &mh, MSG_CMSG_CLOEXEC); int n = 0;
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < k; i++) { int fd; memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd); if (fds && n < FSR4NPU_MAXFD) fds[n++] = fd; else close(fd); } }
  if (nfd) *nfd = n; else for (int i = 0; i < n; i++) close(fds[i]);
  if (r != (ssize_t)sizeof *m || m->magic != FSR4NPU_MAGIC) { for (int i = 0; i < n && fds; i++) close(fds[i]); if (nfd) *nfd = 0; return r <= 0 ? E_DISCONNECTED : E_PROTO; }
  return 0;
}
static void msg_init(struct fsr4npu_msg* m, uint32_t type) { memset(m, 0, sizeof *m); m->magic = FSR4NPU_MAGIC; m->version = FSR4NPU_VERSION; m->type = type; }

static int do_connect(fsr4npu* h, int timeout_ms, fsr4npu_info* info) {
  uint64_t t0 = now_ns();
  for (;;) {
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); if (s < 0) return E_SYS;
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; size_t L = strlen(h->name);
    memcpy(a.sun_path + 1, h->name, L); /* abstract: leading NUL */
    if (connect(s, (struct sockaddr*)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + L)) == 0) { h->sock = s; break; }
    close(s);
    if ((int)((now_ns() - t0) / 1000000ull) >= timeout_ms) return E_DISCONNECTED;
    usleep(20000);
  }
  struct fsr4npu_msg m; msg_init(&m, M_HELLO); int r = fsr4npu_raw_send(h, &m, NULL, 0); if (r) return r;
  r = fsr4npu_raw_recv(h, &m, NULL, NULL, 2000); if (r) return r;
  if (m.type == M_ERROR) return m.status ? m.status : E_PROTO;
  if (m.type != M_INFO) return E_PROTO;
  if (info) { memset(info, 0, sizeof *info); info->ntensors = m.ntensors; info->backend = (uint32_t)m.v[0];
    for (uint32_t k = 0; k < m.ntensors && k < FSR4NPU_MAXT; k++) info->bytes[k] = m.t[k].bytes; memcpy(info->graph, m.text, sizeof info->graph); info->graph[63] = 0; }
  return 0;
}
int fsr4npu_connect(const char* name, int timeout_ms, fsr4npu** out, fsr4npu_info* info) {
  fsr4npu* h = (fsr4npu*)calloc(1, sizeof *h); if (!h) return E_SYS;
  h->sock = h->kick = h->done = -1; for (int k = 0; k < FSR4NPU_MAXT; k++) h->t[k].fd = -1;
  snprintf(h->name, sizeof h->name, "%s", name && *name ? name : FSR4NPU_DEFAULT_NAME);
  int r = do_connect(h, timeout_ms, info); if (r) { if (h->sock >= 0) close(h->sock); free(h); return r; }
  *out = h; return 0;
}
static int mk_efd(fsr4npu* h) {
  if (h->kick < 0) h->kick = eventfd(0, EFD_CLOEXEC); if (h->done < 0) h->done = eventfd(0, EFD_CLOEXEC);
  return h->kick >= 0 && h->done >= 0 ? 0 : E_SYS;
}
static int send_setup(fsr4npu* h) {
  struct fsr4npu_msg m; msg_init(&m, M_SETUP); int fds[FSR4NPU_MAXFD], nfd = 0; m.ntensors = h->nt; m.flags = h->xflags;
  for (uint32_t k = 0; k < h->nt; k++) { int idx = -1; for (int j = 0; j < nfd; j++) if (fds[j] == h->t[k].fd) idx = j;
    if (idx < 0) { idx = nfd; fds[nfd++] = h->t[k].fd; } m.t[k].fd_idx = (uint32_t)idx; m.t[k].off = h->t[k].off; }
  if (h->use_efd) { if (mk_efd(h)) return E_SYS; m.flags |= F_EFD; fds[nfd++] = h->kick; fds[nfd++] = h->done; }
  int r = fsr4npu_raw_send(h, &m, fds, nfd); if (r) return r;
  r = fsr4npu_raw_recv(h, &m, NULL, NULL, 5000); if (r) return r;
  return m.type == M_SETUP_OK ? 0 : (m.status ? m.status : E_PROTO);
}
int fsr4npu_setup(fsr4npu* h, uint32_t nt, const fsr4npu_tensor* t, int use_efd) { return fsr4npu_setup_flags(h, nt, t, use_efd, 0); }
int fsr4npu_setup_flags(fsr4npu* h, uint32_t nt, const fsr4npu_tensor* t, int use_efd, uint32_t extra_flags) {
  h->xflags = extra_flags & F_REGDMA;
  if (nt == 0 || nt > FSR4NPU_MAXT) return E_SIZE;
  for (uint32_t k = 0; k < h->nt; k++) { int dupd = 0; for (uint32_t j = 0; j < k; j++) dupd |= h->t[j].fd == h->t[k].fd; if (!dupd && h->t[k].fd >= 0) close(h->t[k].fd); }
  h->nt = nt; h->use_efd = use_efd; h->alloc_mode = 0;
  for (uint32_t k = 0; k < nt; k++) { h->t[k].off = t[k].off; h->t[k].fd = -1;
    for (uint32_t j = 0; j < k; j++) if (t[j].fd == t[k].fd) h->t[k].fd = h->t[j].fd;
    if (h->t[k].fd < 0) h->t[k].fd = fcntl(t[k].fd, F_DUPFD_CLOEXEC, 3); }
  int r = S_OK; for (uint32_t k = 0; k < nt; k++) if (h->t[k].fd < 0) r = E_FD;   /* caller passed a closed/invalid fd */
  if (r == S_OK) r = send_setup(h);
  if (r != S_OK) {   /* keep no half state: a later fsr4npu_reconnect must not replay a rejected SETUP */
    for (uint32_t k = 0; k < nt; k++) { int dupd = 0; for (uint32_t j = 0; j < k; j++) dupd |= h->t[j].fd == h->t[k].fd; if (!dupd && h->t[k].fd >= 0) close(h->t[k].fd); h->t[k].fd = -1; }
    h->nt = 0; }
  return r;
}
static int alloc_impl(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT], uint32_t xf, uint64_t* in1) {
  struct fsr4npu_msg m; msg_init(&m, M_ALLOC); m.flags |= xf; int fds[2], nfd = 0; h->use_efd = use_efd; h->alloc_mode = 1;
  if (use_efd) { if (mk_efd(h)) return E_SYS; m.flags |= F_EFD; fds[0] = h->kick; fds[1] = h->done; nfd = 2; }
  int r = fsr4npu_raw_send(h, &m, fds, nfd); if (r) return r;
  int rf[FSR4NPU_MAXFD], rn = 0; r = fsr4npu_raw_recv(h, &m, rf, &rn, 5000); if (r) return r;
  if (m.type != M_SETUP_OK) { for (int i = 0; i < rn; i++) close(rf[i]); return m.status ? m.status : E_PROTO; }
  if (rn < 1) return E_PROTO; for (int i = 1; i < rn; i++) close(rf[i]);
  *fd = rf[0]; if (total) *total = m.total; if (in1) *in1 = (xf & F_PINGPONG) ? m.v[4] : 0; tl_out1 = (xf & F_PINGPONG_OUT) ? m.v[5] : 0; h->nt = m.ntensors;
  for (uint32_t k = 0; k < FSR4NPU_MAXT; k++) if (off) off[k] = k < m.ntensors ? m.t[k].off : 0;
  return 0;
}
int fsr4npu_run(fsr4npu* h, int sync_fd, int timeout_ms, fsr4npu_stats* st) {
  uint64_t t0 = now_ns();
  if (h->use_efd && sync_fd < 0) { int r = fsr4npu_kick(h); if (r) return r; r = fsr4npu_wait(h, timeout_ms); if (st) { memset(st, 0, sizeof *st); st->rtt_ns = now_ns() - t0; } return r; }
  struct fsr4npu_msg m; msg_init(&m, M_RUN); m.seq = ++h->seq; int fds[1] = {sync_fd};
  if (sync_fd >= 0) m.flags |= F_SYNCFD; if (h->verify) m.flags |= F_VERIFY;
  int r = fsr4npu_raw_send(h, &m, fds, sync_fd >= 0 ? 1 : 0); if (r) return r;
  r = fsr4npu_raw_recv(h, &m, NULL, NULL, timeout_ms); if (r) return r;
  if (m.type != M_DONE && m.type != M_ERROR) return E_PROTO;
  if (m.type == M_DONE && m.seq != h->seq) return E_PROTO;
  if (st) { st->exec_ns = m.v[0]; st->fence_ns = m.v[1]; st->fnv_in = m.v[2]; st->fnv_out = m.v[3]; st->rtt_ns = now_ns() - t0; }
  return m.type == M_ERROR ? (m.status ? m.status : E_PROTO) : m.status;
}
int fsr4npu_alloc(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT]) { return alloc_impl(h, use_efd, fd, total, off, 0, NULL); }
int fsr4npu_alloc_pp(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT], uint64_t* in1) { return alloc_impl(h, use_efd, fd, total, off, F_PINGPONG, in1); }
int fsr4npu_kick_slot(fsr4npu* h, int slot) { if (h->kick < 0) return E_STATE; uint64_t v = slot ? FSR4NPU_KICK_IN1 : 1; return write(h->kick, &v, 8) == 8 ? 0 : E_SYS; }
int fsr4npu_alloc_pp2(fsr4npu* h, int use_efd, int* fd, uint64_t* total, uint64_t off[FSR4NPU_MAXT], uint64_t* in1, uint64_t* out1) {
  tl_out1 = 0; int r = alloc_impl(h, use_efd, fd, total, off, F_PINGPONG | F_PINGPONG_OUT, in1); if (out1) *out1 = r ? 0 : tl_out1; return r; }
int fsr4npu_kick_slots(fsr4npu* h, int in_slot, int out_slot) { if (h->kick < 0) return E_STATE;
  uint64_t v = 1ull | (in_slot ? (1ull << 32) : 0) | (out_slot ? (1ull << 33) : 0); return write(h->kick, &v, 8) == 8 ? 0 : E_SYS; }
int fsr4npu_kick(fsr4npu* h) { if (h->kick < 0) return E_STATE; uint64_t one = 1; return write(h->kick, &one, 8) == 8 ? 0 : E_SYS; }
int fsr4npu_wait(fsr4npu* h, int timeout_ms) {
  if (h->done < 0) return E_STATE;
  struct pollfd p[2] = {{h->done, POLLIN, 0}, {h->sock, POLLIN, 0}};
  int pr = poll(p, 2, timeout_ms); if (pr == 0) return E_TIMEOUT; if (pr < 0) return E_SYS;
  if (p[0].revents & POLLIN) { uint64_t v = 0; if (read(h->done, &v, 8) != 8) return E_SYS; return v == 1 ? 0 : (v >= 2 ? -(int)(v - 2) : E_PROTO); }
  struct fsr4npu_msg m; int r = fsr4npu_raw_recv(h, &m, NULL, NULL, 0); return r ? r : (m.status ? m.status : E_PROTO); /* daemon spoke or hung up */
}
int fsr4npu_reconnect(fsr4npu* h, int timeout_ms) {
  if (h->sock >= 0) { close(h->sock); h->sock = -1; }
  if (h->kick >= 0) { close(h->kick); h->kick = -1; } if (h->done >= 0) { close(h->done); h->done = -1; }
  int r = do_connect(h, timeout_ms, NULL); if (r) return r;
  if (h->alloc_mode) return E_STATE;
  return h->nt ? send_setup(h) : 0;
}
void fsr4npu_set_verify(fsr4npu* h, int on) { h->verify = on ? 1u : 0u; }
void fsr4npu_close(fsr4npu* h) {
  if (!h) return;
  if (h->sock >= 0) { struct fsr4npu_msg m; msg_init(&m, M_BYE); fsr4npu_raw_send(h, &m, NULL, 0); close(h->sock); }
  if (h->kick >= 0) close(h->kick); if (h->done >= 0) close(h->done);
  for (uint32_t k = 0; k < h->nt; k++) { int dupd = 0; for (uint32_t j = 0; j < k; j++) dupd |= h->t[j].fd == h->t[k].fd; if (!dupd && h->t[k].fd >= 0) close(h->t[k].fd); }
  free(h);
}
