/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：R-13 mif_ceiling 跨周期 run 计数的本地主机回归（全部 65536 个掩码对拍）。
 * English: Local host regression for R-13 wrap-around run counting in mif_ceiling (all 65536 masks).
 */
/* 宿主机回归：awdl_status_mif_ceiling() 的跨周期 run 计数（评审 R-13）。/
 * Host regression for the wrap-around run count in awdl_status_mif_ceiling() (review R-13).
 *
 * 对全部 65536 个 chanseq 掩码、多个 MIF 周期，与一个独立参考实现逐一对拍；并保留评审
 * 记录的旧实现，证明这个对拍能区分缺陷。/ Compares every one of the 65536 masks, for several
 * MIF periods, against an independent reference, and keeps the old implementation from the
 * review to show the comparison discriminates the defect.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -Wno-unused-function \
 *      -I components/read_pico_airdrop/awdl/core -o build/review/awdl_status_ceiling_test \
 *      build/review/awdl_status_ceiling_test.c && build/review/awdl_status_ceiling_test
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "awdl_status.h"

static int fails = 0, ran = 0;

static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* 独立参考：按位找每个环形 run 的起点，run 只量一次。wrap 的 run 由 (k-1)&15 判定。/
   Independent reference: find every circular run's start by bit, measure each run once. A
   wrapping run is found by testing (k-1)&15. */
static float reference_ceiling(uint16_t mask, uint32_t period) {
  if (!mask || !period) return 0.0f;
  int total = 0;
  if (mask == 0xFFFFu) {            /* 全 ch6：没有 run 起点，一整个不闭合的 run。/
                                       Full ch6: no run start, one never-closing run. */
    while ((int64_t)total * period < AWC_US) total++;
    return (float)total * 1000000.0f / (float)AWC_US;
  }
  for (int k = 0; k < AWDL_SLOTS; k++) {
    if (!(mask & (uint16_t)(1u << k))) continue;
    if (mask & (uint16_t)(1u << ((k + AWDL_SLOTS - 1) & (AWDL_SLOTS - 1)))) continue;
    int run = 1;
    while (run < AWDL_SLOTS && (mask & (uint16_t)(1u << ((k + run) & (AWDL_SLOTS - 1))))) run++;
    int64_t usable;
    if (run == AWDL_SLOTS) usable = AWC_US;                    /* 不闭合 / never closes */
    else usable = (int64_t)run * AWDL_SLOT_US - AWDL_WIN_LEAD_US - AWDL_WIN_GUARD_US;
    int n = 0;
    while ((int64_t)n * period < usable) n++;
    total += n;
  }
  return (float)total * 1000000.0f / (float)AWC_US;
}

/* 评审 R-13 记录的原实现（缺陷保留，仅用于证明对拍有效）。/
   The original implementation from review R-13, kept only to show the comparison bites. */
static float buggy_ceiling(uint16_t mask, uint32_t period) {
  if (!mask || !period) return 0.0f;
  struct AwdlWindow w; awdl_window_init(&w, mask);
  int total = 0;
  int64_t p = 0;
  for (int guard = 0; guard < AWDL_SLOTS && p < AWC_US; guard++) {
    int64_t open_rel, len;
    if (!awdl_window_run_at(&w, p, &open_rel, &len)) break;
    if (p + open_rel >= AWC_US) break;
    int n = 0;
    while ((int64_t)n * period < len) n++;
    total += n;
    p = p + open_rel + len + 1;
  }
  return (float)total * 1000000.0f / (float)AWC_US;
}

static int feq(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
  printf("== awdl_status_mif_ceiling() wrap-run regression (R-13) ==\n");

  /* 1. 上游 status_test.c 钉住的已知值，修复不能改变它们。/ Known values pinned by the
     upstream status_test.c; the fix must not change them. */
  {
    struct { uint16_t mask; float want; } C[] = {
      { 0x0100, 3.8147f }, { 0x0300, 7.6294f }, { 0x3b00, 20.0272f }, { 0x0000, 0.0f },
    };
    for (unsigned i = 0; i < 4; i++) {
      float got = awdl_status_mif_ceiling(C[i].mask, 15000);
      char d[96];
      snprintf(d, sizeof d, "0x%04x -> %.4f/s (want %.4f)", C[i].mask, got, C[i].want);
      check("known non-wrapping masks keep their upstream value",
            fabsf(got - C[i].want) < 0.01f, d);
    }
    check("a zero MIF period has no ceiling",
          awdl_status_mif_ceiling(0x0100, 0) == 0.0f, NULL);
  }

  /* 2. 缺陷用例 0x8001：slots 15 和 0 是一个跨周期接缝的 run，每个周期 8 个 instant。/
     The defect's mask: slots 15 and 0 are ONE run across the cycle seam, 8 instants a cycle. */
  {
    float got = awdl_status_mif_ceiling(0x8001, 15000);
    float ref = reference_ceiling(0x8001, 15000);
    char d[128];
    snprintf(d, sizeof d, "fixed %.4f/s, reference %.4f/s, old %.4f/s", got, ref,
             buggy_ceiling(0x8001, 15000));
    check("0x8001 counts the wrapping run once (7.6294/s, not 15.2588/s)",
          fabsf(got - 7.6294f) < 0.01f && feq(got, ref), d);
    check("...and the old implementation really did double count",
          fabsf(buggy_ceiling(0x8001, 15000) - 2 * ref) < 1e-3f, d);
  }

  /* 3. 全 ch6 掩码：一整个不闭合的 run。/ Full ch6 mask: one run that never closes. */
  {
    float got = awdl_status_mif_ceiling(0xFFFF, 15000);
    float ref = reference_ceiling(0xFFFF, 15000);
    char d[96];
    snprintf(d, sizeof d, "0xffff -> %.4f/s (reference %.4f/s)", got, ref);
    check("full ch6 mask is one never-closing run", feq(got, ref) && got > 0, d);
  }

  /* 4. 穷举：全部 65536 个掩码 × 4 个 MIF 周期。/ Exhaustive: all 65536 masks x 4 periods. */
  {
    static const uint32_t periods[] = { 5000, 15000, 20480, 60000 };
    int bad = 0, bad_buggy = 0, checked = 0;
    uint16_t first_bad = 0, first_bad_buggy = 0;
    for (unsigned pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++) {
      for (uint32_t m = 0; m <= 0xFFFFu; m++) {
        uint16_t mask = (uint16_t)m;
        float got = awdl_status_mif_ceiling(mask, periods[pi]);
        float ref = reference_ceiling(mask, periods[pi]);
        checked++;
        if (!feq(got, ref)) { if (!bad) first_bad = mask; bad++; }
        if (!feq(buggy_ceiling(mask, periods[pi]), ref)) {
          if (!bad_buggy) first_bad_buggy = mask;
          bad_buggy++;
        }
      }
    }
    char d[192];
    snprintf(d, sizeof d, "%d cases, %d mismatch (first 0x%04x); old impl %d mismatch (first 0x%04x)",
             checked, bad, first_bad, bad_buggy, first_bad_buggy);
    check("fixed ceiling matches the reference for every mask and period", bad == 0, d);
    check("the old implementation mismatches exactly the 16384 masks with bit0+bit15 set",
          bad_buggy == 4 * 16384, d);
  }

  /* 5. 输入边界。/ Input boundaries. */
  check("mask 0 => 0", awdl_status_mif_ceiling(0, 15000) == 0.0f, NULL);
  check("period 0 => 0", awdl_status_mif_ceiling(0x8001, 0) == 0.0f, NULL);

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
