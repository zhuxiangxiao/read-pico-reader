/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：R-12 整 AW 修正后 gauge 直方图旋转方向的本地主机回归。
 * English: Local host regression for the R-12 histogram rotation sign after a whole-AW correction.
 */
/* 宿主机回归：awdl_gauge.h 的整 AW 修正方向（评审 R-12）。/
 * Host regression for the whole-AW correction sign in awdl_gauge.h (review R-12).
 *
 * 直接编译真实固件头文件，不复制算法。/ Compiles the real firmware header, no algorithm copy.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -Wno-unused-function \
 *      -I components/read_pico_airdrop/awdl/core -o build/review/awdl_gauge_fix_test \
 *      build/review/awdl_gauge_fix_test.c && build/review/awdl_gauge_fix_test
 *
 * 修复前的预期：+1 AW 修正后主峰停在 +2×修正量（+32768 us）。修复后停在 0。/
 * Before the fix the peak lands at +2x the correction (+32768 us) after a +1 AW fix; after
 * the fix it lands at 0.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "awdl_gauge.h"

static int fails = 0, ran = 0;

static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* ga_eval 自己的取峰规则：严格大于，所以平顶取最低 bin。/ The very rule ga_eval uses. */
static int peak_bin(const ga_t *g) {
  int pos = 0;
  uint16_t peak = 0;
  for (int i = 0; i < GA_NB; i++) if (g->hist[i] > peak) { peak = g->hist[i]; pos = i; }
  return pos;
}
/* 峰换算成 ga_eval 眼里的修正量（相对当前 C）。/ The peak as the correction ga_eval sees. */
static int32_t peak_sv(const ga_t *g) {
  return ga_wrap((int32_t)((uint32_t)peak_bin(g) * GA_BIN));
}

/* 造出“恰好要求 aw_off 个 AW 修正”的锁定状态：直方图平顶 + 两个一致同意的发送者。/
   Build the locked state that asks for exactly aw_off AW: a histogram plateau plus two
   senders that agree on it. */
static void seed_aw_request(ga_t *g, int aw_off, uint32_t rx) {
  ga_reset(g);
  g->started = 1; g->locked = 1;
  g->t0 = g->t_dec = g->t_eval = g->c_t = g->t_lock = rx;
  int b = ga_bin(aw_off * (int32_t)GA_AW);
  for (int i = 0; i < GA_NV; i++) g->hist[(b + i) & (GA_NB - 1)] = 40;
  for (int i = 0; i < 2; i++) {
    g->src[i].used = 1;
    g->src[i].av_n = (int8_t)aw_off;
    g->src[i].av_c = GA_AWV_MIN;
    g->src[i].rx_last = rx;                       /* 活跃 / active */
    g->src[i].av_t = rx;                          /* 新鲜 / fresh */
  }
}

static void test_rotation_direction(void) {
  ga_t g;
  ga_reset(&g);
  g.hist[100] = 7; g.hist[777] = 3;
  ga_rotate(&g, GA_AW);                            /* 正数 = 样本搬到更大的 bin / to higher bins */
  check("ga_rotate(+1 AW) moves a feature UP by 16 bins",
        g.hist[116] == 7 && g.hist[793] == 3 && g.hist[100] == 0, "hist[100]->116, hist[777]->793");
  ga_reset(&g);
  g.hist[100] = 7;
  ga_rotate(&g, -GA_AW);
  check("ga_rotate(-1 AW) moves a feature DOWN by 16 bins",
        g.hist[84] == 7 && g.hist[100] == 0, "hist[100]->84");
  /* 跨 0 的环绕。/ Wrapping through bin 0. */
  ga_reset(&g);
  g.hist[4] = 5;
  ga_rotate(&g, -GA_AW);
  check("...wrapping through bin 0", g.hist[1012] == 5 && g.hist[4] == 0, "hist[4]->1012");
}

static void test_correction_lands_at_zero(void) {
  const int32_t offs[2] = { +1, -1 };
  for (int k = 0; k < 2; k++) {
    int aw_off = offs[k];
    ga_t g;
    const uint32_t rx = 1000000;
    seed_aw_request(&g, aw_off, rx);

    int32_t sv_before = peak_sv(&g);
    uint32_t c_before = g.c_off;
    ga_eval(&g, rx);                               /* 走真实的 AW 修正路径 / real fix path */
    int32_t sv_after = peak_sv(&g);
    uint32_t c_after = g.c_off;

    char d[160];
    snprintf(d, sizeof d, "%+d AW: peak %d us -> %d us, c_off %u -> %u, n_awfix=%u",
             aw_off, (int)sv_before, (int)sv_after, (unsigned)c_before, (unsigned)c_after,
             (unsigned)g.n_awfix);
    check("the evidence asked for exactly the requested AW offset", sv_before == aw_off * (int32_t)GA_AW, d);
    check("after the correction the peak lands at 0, not 2x the offset", sv_after == 0, d);
    check("c_off moved by exactly the requested offset",
          c_after == (((uint32_t)((int32_t)c_before + aw_off * (int32_t)GA_AW)) & GA_MASK)
              && g.n_awfix == 1, d);
  }
}

static void test_defect_counterfactual(void) {
  /* 同一份直方图按缺陷写法（+mv）旋转：主峰停在 +2 AW，这正是评审复现的 +31744/+32768us。/
     The same histogram rotated the defective way (+mv): the peak stays at +2 AW, which is the
     +31744/+32768 us the review reproduced. */
  ga_t g;
  seed_aw_request(&g, +1, 1000000);
  ga_shift(&g, GA_AW);
  ga_rotate(&g, GA_AW);                            /* 缺陷：方向反了 / the defect */
  int32_t sv = peak_sv(&g);
  char d[96];
  snprintf(d, sizeof d, "peak %d us (2x the +16384 us correction)", (int)sv);
  check("counterfactual: +mv leaves the peak at twice the correction", sv == 2 * (int32_t)GA_AW, d);
}

static void test_aligned_state_is_untouched(void) {
  /* 已经对齐（峰在 0）且没人提议整 AW 修正时，不得发生修正。/ An aligned gauge with no AW
     proposal must not be corrected. */
  ga_t g;
  seed_aw_request(&g, 0, 1000000);
  uint32_t c = g.c_off;
  int32_t sv_before = peak_sv(&g);
  ga_eval(&g, 1000000);
  check("an aligned gauge is not moved by ga_eval",
        g.n_awfix == 0 && g.c_off == c && peak_sv(&g) == sv_before, "n_awfix stays 0");
}

int main(void) {
  printf("== awdl_gauge.h AW-fix sign regression (R-12) ==\n");
  test_rotation_direction();
  test_correction_lands_at_zero();
  test_defect_counterfactual();
  test_aligned_state_is_untouched();
  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
