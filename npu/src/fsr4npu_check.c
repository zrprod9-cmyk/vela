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
 * fsr4npu-check: runs the fsr4npud graph a few times as an ordinary user and prints ms + output hash.
 * Usage: fsr4npu-check [--name fsr4npud] [--runs 20]. Exit 0 = OK (all runs succeeded, outputs identical in every run). */
#define _GNU_SOURCE
#include "fsr4npu.h"
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t fnv64(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
static void dsync(int fd, unsigned f) { struct dma_buf_sync s = { f }; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); }
static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
int main(int argc, char** argv) {
  const char* name = FSR4NPU_DEFAULT_NAME; int runs = 20;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
    else if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
    else { fprintf(stderr, "usage: %s [--name fsr4npud] [--runs 20]\n", argv[0]); return 2; } }
  if (runs < 2) runs = 2; if (runs > 1000) runs = 1000;
  fsr4npu* h = NULL; fsr4npu_info info; double t0 = now_ms();
  int st = fsr4npu_connect(name, 3000, &h, &info);
  if (st) { printf("FAIL connect @%s: %s (is the service running? systemctl status vela-npu)\n", name, fsr4npu_strerror(st)); return 1; }
  printf("daemon @%s: backend %s, graph %s, %u tensors (in %llu bytes", name, info.backend == B_QNN ? "qnn (NPU)" : "stub (no NPU)", info.graph, info.ntensors, (unsigned long long)info.bytes[0]);
  for (uint32_t k = 1; k < info.ntensors; k++) printf(", out%u %llu", k - 1, (unsigned long long)info.bytes[k]); printf(" bytes), connect %.1f ms\n", now_ms() - t0);
  int fd = -1; uint64_t total = 0, off[FSR4NPU_MAXT];
  if ((st = fsr4npu_alloc(h, 0, &fd, &total, off))) { printf("FAIL alloc: %s\n", fsr4npu_strerror(st)); return 1; }
  uint8_t* m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { printf("FAIL mmap shared buffer (%llu bytes)\n", (unsigned long long)total); return 1; }
  /* fixed input: bytes < 0x3b, so as fp16 every value is finite and |v| < 1; as uint8 a ramp. Same input every time -> same hash. */
  dsync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
  for (uint64_t i = 0; i < info.bytes[0]; i++) m[off[0] + i] = (uint8_t)((i * 7u + (i >> 12)) % 59u);
  dsync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
  double* ms = calloc(runs, sizeof *ms); double* ex = calloc(runs, sizeof *ex); uint64_t first = 0; int same = 1;
  for (int r = 0; r < runs; r++) {
    /* poison the outputs so a run that writes nothing cannot pass */
    dsync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    for (uint32_t k = 1; k < info.ntensors; k++) memset(m + off[k], 0xA5 ^ r, info.bytes[k]);
    dsync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    fsr4npu_stats s; memset(&s, 0, sizeof s); double a = now_ms();
    if ((st = fsr4npu_run(h, -1, 5000, &s))) { printf("FAIL run %d: %s\n", r, fsr4npu_strerror(st)); return 1; }
    ms[r] = now_ms() - a; ex[r] = s.exec_ns / 1e6;
    dsync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    uint64_t hh = 1469598103934665603ULL; for (uint32_t k = 1; k < info.ntensors; k++) hh ^= fnv64(m + off[k], info.bytes[k]) + k;
    dsync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    if (r == 0) first = hh; else if (hh != first) same = 0;
  }
  double ms0 = ms[0]; qsort(ms + 1, runs - 1, sizeof *ms, cmpd); qsort(ex + 1, runs - 1, sizeof *ex, cmpd);
  printf("first run %.2f ms; next %d runs: round trip p50 %.2f ms (max %.2f), NPU execute p50 %.2f ms\n", ms0, runs - 1, ms[runs / 2], ms[runs - 1], ex[runs / 2]);
  printf("input hash %016llx\n", (unsigned long long)fnv64(m + off[0], info.bytes[0]));
  printf("output hash %016llx (%s in all %d runs)\n", (unsigned long long)first, same ? "identical" : "DIFFERENT", runs);
  munmap(m, total); close(fd); fsr4npu_close(h);
  printf("%s\n", same ? "RESULT OK" : "RESULT FAIL outputs differ between runs");
  return same ? 0 : 1;
}
