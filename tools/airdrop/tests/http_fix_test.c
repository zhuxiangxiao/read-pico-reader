/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：R-04 DvZip 增量判定、R-11 HTTP 头部/drain 预算、R-16 bplist 偏移拒绝的本地主机回归。
 * English: Local host regression for R-04 incremental DvZip completeness, R-11 HTTP header/drain budgets and R-16 bplist offset rejection.
 */
/* Host regression for the protocol/DoS fixes on port_airdrop_feature:
 *   R-04  incremental DvZip completeness (O(n^2) -> linear)  [ad_http.h]
 *   R-11  header-section caps + bounded /Discover & 404 drains [ad_http.h, ad_serve.h]
 *   R-16  flat bplist writer refuses object offsets > 255     [ad_bplist.h]
 *
 * Compiled against the REAL headers, no IDF:
 *   cc -O2 -Wall -Wextra -Wno-unused-function -o build/review/http_fix_test \
 *      build/review/http_fix_test.c && ./build/review/http_fix_test
 *
 * The byte source is scripted in content AND virtual time, exactly like the upstream
 * ESP32Drop host tests this port came from: every bug in this layer was a timeout
 * interacting with framing, and none is reachable without modelling when bytes arrive.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../../../components/read_pico_airdrop/airdrop/core/ad_http.h"
#include "../../../components/read_pico_airdrop/airdrop/core/ad_serve.h"
#include "../../../components/read_pico_airdrop/airdrop/core/ad_bplist.h"

static int g_fails = 0, g_ran = 0;
static void check(const char *name, bool ok, const char *detail) {
  g_ran++;
  printf("  [%s] %-62s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) g_fails++;
}

static bool has_bytes(const char *hay, int hn, const char *needle, int nn) {
  if (hn < nn) return false;
  for (int i = 0; i + nn <= hn; i++) if (memcmp(hay + i, needle, (size_t)nn) == 0) return true;
  return false;
}

/* ======================= scripted source (content + time) ======================= */
#define MAXSEG 256
typedef struct { uint32_t at_ms; const unsigned char *p; int n; } Seg;
typedef struct {
  Seg seg[MAXSEG]; int nseg, cur, off;
  uint32_t now_ms;
  bool close_at_end;
} Script;

static void script_add(Script *s, uint32_t at, const void *p, int n) {
  if (s->nseg >= MAXSEG) { fprintf(stderr, "MAXSEG overflow\n"); exit(2); }
  s->seg[s->nseg].at_ms = at; s->seg[s->nseg].p = (const unsigned char *)p;
  s->seg[s->nseg].n = n; s->nseg++;
}

static int script_read(struct HttpRdr *r, unsigned char *buf, int len) {
  Script *s = (Script *)r->ctx;
  uint32_t deadline = s->now_ms + r->timeout_ms;
  for (;;) {
    if (s->cur >= s->nseg) {
      if (s->close_at_end) return -1;
      s->now_ms = deadline; return 0;              /* silence until the timeout */
    }
    Seg *g = &s->seg[s->cur];
    if (g->at_ms > deadline) { s->now_ms = deadline; return 0; }
    if (g->at_ms > s->now_ms) s->now_ms = g->at_ms;
    int avail = g->n - s->off;
    int take = (avail < len) ? avail : len;
    memcpy(buf, g->p + s->off, take);
    s->off += take;
    if (s->off >= g->n) { s->cur++; s->off = 0; }
    return take;
  }
}

/* ======================= hostile source: endless trickle ======================= */
/* One byte per read, each read costs gap_ms of virtual time. The head script is
   delivered first (a request line / headers / a huge chunk-size line), then `tail`
   repeats forever (chunk framing or a bare endless line/body). */
typedef struct {
  uint32_t now_ms, gap_ms;
  const unsigned char *head; int hn, hoff;
  const unsigned char *tail; int tn, toff;
  bool close_after_head;
} Host;
static int host_read(struct HttpRdr *r, unsigned char *buf, int len) {
  (void)len;
  Host *h = (Host *)r->ctx;
  if (h->hoff < h->hn) { buf[0] = h->head[h->hoff++]; return 1; }  /* head: no delay */
  if (h->close_after_head || h->tn <= 0) return -1;
  h->now_ms += h->gap_ms;                                         /* body: trickles */
  buf[0] = h->tail[h->toff];
  h->toff = (h->toff + 1) % h->tn;
  return 1;
}

/* ======================= sinks ======================= */
static bool sink_grow(struct HttpSink *sk, uint32_t need) {
  uint32_t nc = sk->cap ? sk->cap : 1024;
  while (nc < need) nc <<= 1;
  unsigned char *np = (unsigned char *)realloc(sk->buf, nc);
  if (!np) return false;
  sk->buf = np; sk->cap = nc;
  return true;
}
static void sink_init(struct HttpSink *sk, uint32_t max) {
  memset(sk, 0, sizeof(*sk)); sk->max = max; sk->grow = sink_grow;
}

/* Consuming sink, mirroring the firmware's streaming decoder: the progress hook drains
   complete records out of buf and compacts the residue to the front. */
static int g_cons_recs = 0;
static uint32_t g_cons_pay = 0;
static bool cons_emit(void *ctx, const struct AdDvzipRec *rec, const uint8_t *payload) {
  (void)ctx; (void)payload;
  g_cons_recs++; g_cons_pay += rec->len;
  return true;
}
static void cons_progress(struct HttpSink *sk) {
  int end;
  uint32_t c = ad_dvzip_drain(sk->buf, sk->len, false, cons_emit, NULL, &end);
  if (c) { memmove(sk->buf, sk->buf + c, sk->len - c); sk->len -= c; }
}

/* archive_done veto: the hook below can be told to answer true/false. */
static int g_archive_done = -1;   /* -1 = NULL (old behaviour), 0 = not done, 1 = done */
static bool archive_done_cb(struct HttpSink *sk) { (void)sk; return g_archive_done > 0; }

/* ======================= DvZip body builders ======================= */
static int dvzip_rec(unsigned char *out, int payload) {
  out[0] = (unsigned char)(payload >> 24); out[1] = (unsigned char)(payload >> 16);
  out[2] = (unsigned char)(payload >> 8);  out[3] = (unsigned char)payload;
  out[4] = 0x78; out[5] = 0x9c;                        /* zlib header */
  for (int i = 6; i < 4 + payload; i++) out[i] = (unsigned char)(i & 0xff);
  return 4 + payload;
}
static int chunk_wrap(unsigned char *out, const unsigned char *p, int n) {
  int k = sprintf((char *)out, "%X\r\n", n);
  memcpy(out + k, p, n); k += n;
  out[k++] = '\r'; out[k++] = '\n';
  return k;
}

/* The attack shape from R-04: first header 80 00 00 00 (stored, length 0 -> the port's
   rx_body_shape() calls this RAW), then a run of [00 00 00 01][78] pseudo-records. It
   never parses as "not DvZip" (each deflated record starts with the zlib CMF byte), so
   the old probe walked the whole chain after every chunk. */
static size_t build_attack_payload(unsigned char *out, size_t body_bytes) {
  size_t pl = 0;
  out[pl++] = 0x80; out[pl++] = 0; out[pl++] = 0; out[pl++] = 0;
  while (pl + 5 <= body_bytes) {
    out[pl++] = 0; out[pl++] = 0; out[pl++] = 0; out[pl++] = 1;
    out[pl++] = 0x78;
  }
  return pl;
}
/* Chunk the payload with one HTTP chunk per byte: the worst case for the old probe. */
static size_t wrap_per_byte(unsigned char *out, const unsigned char *p, size_t n) {
  size_t w = 0;
  for (size_t i = 0; i < n; i++) w += (size_t)sprintf((char *)out + w, "1\r\n%c\r\n", p[i]);
  memcpy(out + w, "0\r\n\r\n", 5); w += 5;
  return w;
}

/* ======================= old dvzip_complete (reference) ======================= */
/* The pre-fix implementation, kept verbatim so "the verdicts must not change" is a
   comparison, not an opinion. */
static int ref_dvzip_complete(const unsigned char *in, uint32_t len) {
  if (len < 4) return 0;
  struct AdDvzipIter it; struct AdDvzipRec rec;
  ad_dvzip_begin(&it, in, len);
  while (ad_dvzip_next(&it, &rec)) {
    if (!rec.stored && in[rec.off] != 0x78) return 0;
  }
  if (it.end == AD_DVZIP_END_RECORD) return 2;
  return (it.end == AD_DVZIP_END_EXACT) ? 1 : 0;
}

/* plain (void*) clock for the direct drain tests */
static uint32_t hcx_now(void *ctx) { return ((Host *)ctx)->now_ms; }

/* ======================= harness for the serve layer ======================= */
typedef struct {
  uint32_t *now_ms;                 /* points at Script.now_ms or Host.now_ms */
  char out[16384]; int outn;
  char logs[64][192]; int nlog;
  uint32_t disc_ms, ask_ms, disc_ok, ask_ok, desync;
  int ask_sent, uploads, up_chunked, up_clen, up_rc;
} Harness;

static int hz_write(struct HttpServe *sv, const unsigned char *p, int n) {
  Harness *h = (Harness *)sv->io;
  if (h->outn + n < (int)sizeof(h->out)) { memcpy(h->out + h->outn, p, n); h->outn += n; }
  return n;
}
static uint32_t hz_now(struct HttpServe *sv) {
  Harness *h = (Harness *)sv->io;
  return *h->now_ms;
}
static void hz_log(struct HttpServe *sv, const char *line) {
  Harness *h = (Harness *)sv->io;
  if (h->nlog < 64) { snprintf(h->logs[h->nlog], 192, "%s", line); h->nlog++; }
}
static void hz_set_timeout(struct HttpRdr *r, uint32_t ms) { r->timeout_ms = ms; }
static void hz_on_ask_sent(struct HttpServe *sv) { ((Harness *)sv->io)->ask_sent++; }

static void hz_upload(struct HttpServe *sv, bool chunked, int clen) {
  Harness *h = (Harness *)sv->io;
  h->uploads++; h->up_chunked = chunked; h->up_clen = clen;
  if (chunked) {
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    h->up_rc = http_recv_chunked(sv->r, &sk, 10000, 100, sv->set_timeout);
    free(sk.buf);
  } else if (clen > 0) {
    hr_skip(sv->r, (uint32_t)clen);
    h->up_rc = sv->r->poisoned ? HTTP_RECV_POISONED : HTTP_RECV_OK_LAST_CHUNK;
  } else {
    h->up_rc = HTTP_RECV_OK_LAST_CHUNK;
  }
  const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  sv->write(sv, (const unsigned char *)ok, (int)strlen(ok));
}

static void harness_setup(Harness *h, struct HttpServe *sv, struct HttpRdr *r,
                          void *read_ctx, http_src_read_fn read, uint32_t *now_ms) {
  memset(h, 0, sizeof(*h));
  h->now_ms = now_ms;
  memset(sv, 0, sizeof(*sv));
  sv->r = r; sv->io = h;
  sv->write = hz_write; sv->now = hz_now; sv->log = hz_log; sv->set_timeout = hz_set_timeout;
  static const unsigned char DISCBODY[] = "DISCBODY";
  static const unsigned char ASKBODY[]  = "ASKBODY";
  sv->disc_body = DISCBODY; sv->disc_len = (int)sizeof(DISCBODY) - 1;
  sv->ask_body  = ASKBODY;  sv->ask_len  = (int)sizeof(ASKBODY) - 1;
  sv->disc_ms = &h->disc_ms; sv->ask_ms = &h->ask_ms;
  sv->disc_ok = &h->disc_ok; sv->ask_ok = &h->ask_ok; sv->desync = &h->desync;
  sv->on_ask_sent = hz_on_ask_sent; sv->upload = hz_upload;
  sv->enter_ms = *now_ms;
  http_rdr_init(r, read_ctx, read);
}

static bool hz_has_log(Harness *h, const char *needle) {
  for (int i = 0; i < h->nlog; i++) if (strstr(h->logs[i], needle)) return true;
  return false;
}
static bool hz_answered(Harness *h) { return h->outn > 0; }

/* ======================= R-04: equivalence ======================= */
static uint32_t g_rng = 0x12345678u;
static uint32_t rng(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static void test_verdict_equivalence(void) {
  printf("== R-04 verdict equivalence: dvzip_complete / dvzip_probe vs the old walk ==\n");
  /* A battery of every terminus the iterator can produce, plus the attack shape. */
  static unsigned char bodies[8][4096];
  int nb = 0, lens[8];
  /* 1. two deflated records, then the zero end record */
  { int o = 0; o += dvzip_rec(bodies[nb] + o, 32); o += dvzip_rec(bodies[nb] + o, 40);
    memset(bodies[nb] + o, 0, 4); o += 4; lens[nb++] = o; }
  /* 2. records ending exactly on a boundary (no end record) */
  { int o = 0; o += dvzip_rec(bodies[nb] + o, 32); o += dvzip_rec(bodies[nb] + o, 40);
    lens[nb++] = o; }
  /* 3. stored record (bit 31) */
  { int o = 0; uint32_t n = 24;
    bodies[nb][o++] = (unsigned char)(0x80 | (n >> 24)); bodies[nb][o++] = (unsigned char)(n >> 16);
    bodies[nb][o++] = (unsigned char)(n >> 8); bodies[nb][o++] = (unsigned char)n;
    for (uint32_t i = 0; i < n; i++) bodies[nb][o++] = (unsigned char)(i * 7);
    bodies[nb][o++] = 0; bodies[nb][o++] = 0; bodies[nb][o++] = 0; bodies[nb][o++] = 0;
    lens[nb++] = o; }
  /* 4. truncated mid-payload (CUT) */
  { int o = 0; o += dvzip_rec(bodies[nb] + o, 64); lens[nb++] = o - 10; }
  /* 5. strand: 1-3 bytes after a record */
  { int o = 0; o += dvzip_rec(bodies[nb] + o, 20); bodies[nb][o++] = 0xAA; bodies[nb][o++] = 0xBB;
    lens[nb++] = o; }
  /* 6. deflated record whose payload is not zlib (permanent 0) */
  { int o = 0; o += dvzip_rec(bodies[nb] + o, 16); bodies[nb][4] = 0x11; lens[nb++] = o; }
  /* 7. the R-04 attack shape */
  { lens[nb] = (int)build_attack_payload(bodies[nb], 2048); nb++; }
  /* 8. empty body */
  { lens[nb++] = 0; }

  int bad = 0;
  for (int b = 0; b < nb; b++) {
    struct DvZipProbe pb; dvzip_probe_reset(&pb);
    for (int len = 0; len <= lens[b]; len++) {
      int ref = ref_dvzip_complete(bodies[b], (uint32_t)len);
      int one = dvzip_complete(bodies[b], (uint32_t)len);
      int inc = dvzip_probe(&pb, bodies[b], (uint32_t)len);
      if (one != ref || inc != ref) {
        if (!bad++) printf("    mismatch body=%d len=%d ref=%d one=%d inc=%d\n", b, len, ref, one, inc);
      }
    }
  }
  /* 300 random bodies, every prefix: a random body mostly lands on CUT/strand, but the
     point is that the resumed walk agrees with a from-scratch one at EVERY prefix. */
  static unsigned char rb[512];
  for (int t = 0; t < 300; t++) {
    int n = (int)(rng() % 256);
    for (int i = 0; i < n; i++) rb[i] = (unsigned char)rng();
    if (t & 1) { dvzip_rec(rb, 12); }                      /* half start as a plain record */
    struct DvZipProbe pb; dvzip_probe_reset(&pb);
    for (int len = 0; len <= n; len++) {
      int ref = ref_dvzip_complete(rb, (uint32_t)len);
      if (dvzip_complete(rb, (uint32_t)len) != ref || dvzip_probe(&pb, rb, (uint32_t)len) != ref) bad++;
    }
  }
  check("all prefixes of 8 crafted + 300 random bodies match the old verdicts", bad == 0, NULL);
}

/* ======================= R-04: the crafted RAW body is linear now ======================= */
static double now_real_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double time_attack(size_t body_bytes, int *rc_out, uint32_t *len_out) {
  static unsigned char payload[8u << 20];
  size_t pl = build_attack_payload(payload, body_bytes);
  size_t wire_cap = pl * 6 + 64;
  unsigned char *wire = (unsigned char *)malloc(wire_cap);
  size_t wn = wrap_per_byte(wire, payload, pl);

  Script s; memset(&s, 0, sizeof(s));
  script_add(&s, 0, wire, (int)wn);
  s.close_at_end = true;
  struct HttpRdr r; http_rdr_init(&r, &s, script_read);
  struct HttpSink sk; sink_init(&sk, 8u << 20);
  sk.buf = (unsigned char *)malloc(4096); sk.cap = 4096;

  double t0 = now_real_ms();
  int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
  double t1 = now_real_ms();
  *rc_out = rc; *len_out = sk.len;
  free(wire); free(sk.buf);
  return t1 - t0;
}

static void test_attack_linear(void) {
  printf("== R-04 crafted chunked /Upload body: cost is linear, not quadratic ==\n");
  int rc1, rc2, rc3; uint32_t l1, l2, l3;
  double t64  = time_attack(64u << 10,  &rc1, &l1);
  double t256 = time_attack(256u << 10, &rc2, &l2);
  double t1m  = time_attack(1u << 20,  &rc3, &l3);
  char d[256];
  snprintf(d, sizeof d, "64K=%.1fms 256K=%.1fms 1M=%.1fms rcs=%d/%d/%d lens=%u/%u/%u (before fix: 64K=1726ms)",
           t64, t256, t1m, rc1, rc2, rc3, l1, l2, l3);
  /* Absolute bounds, generous for a loaded CI box: the pre-fix code needed ~27s for
     256K and ~7 minutes for 1M, so these thresholds cannot hide a quadratic walk. */
  uint32_t expect = 4u + 5u * (((1u << 20) - 4u) / 5u);
  bool ok = rc1 == HTTP_RECV_OK_LAST_CHUNK && rc2 == HTTP_RECV_OK_LAST_CHUNK &&
            rc3 == HTTP_RECV_OK_LAST_CHUNK && l1 == 4u + 5u * (((64u << 10) - 4u) / 5u) &&
            l2 == 4u + 5u * (((256u << 10) - 4u) / 5u) && l3 == expect;
  check("64K/256K/1M crafted bodies complete on the last-chunk marker (rc=3)", ok, d);
  check("64K body under 60 ms, 256K under 200 ms, 1M under 800 ms",
        t64 < 60.0 && t256 < 200.0 && t1m < 800.0, d);
  check("growth is linear (256K/64K < 6x, 1M/64K < 20x)",
        t256 < t64 * 6 + 5 && t1m < t64 * 20 + 10, d);
}

/* ======================= valid DvZip paths must still work ======================= */
static void test_valid_paths(void) {
  printf("== valid DvZip / RAW paths through http_recv_chunked ==\n");
  static unsigned char body[4096], wire[16384];
  int bn = 0;
  bn += dvzip_rec(body + bn, 32);
  bn += dvzip_rec(body + bn, 40);
  static const unsigned char endrec[4] = {0, 0, 0, 0};

  /* explicit end record, whole body in ONE chunk: structural certainty, no wait */
  {
    int w = 0;
    w += chunk_wrap(wire + w, body, bn);
    w += chunk_wrap(wire + w, endrec, 4);
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 3000, NULL);
    char d[128]; snprintf(d, sizeof d, "rc=%d len=%u virtual_ms=%u", rc, sk.len, s.now_ms);
    check("end record in one chunk -> HTTP_RECV_OK_END_RECORD, no waiting",
          rc == HTTP_RECV_OK_END_RECORD && s.now_ms == 0 && sk.len == (uint32_t)(bn + 4), d);
    free(sk.buf);
  }
  /* same bytes split into 1-byte HTTP chunks: the incremental probe must still get there */
  {
    static unsigned char wire2[32768];
    size_t w = 0;
    for (int i = 0; i < bn; i++) w += (size_t)sprintf((char *)wire2 + w, "1\r\n%c\r\n", body[i]);
    for (int i = 0; i < 4; i++) w += (size_t)sprintf((char *)wire2 + w, "1\r\n%c\r\n", endrec[i]);
    memcpy(wire2 + w, "0\r\n\r\n", 5); w += 5;
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire2, (int)w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 3000, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d virtual_ms=%u", rc, s.now_ms);
    check("end record split into 1-byte chunks -> still rc=2, no wait",
          rc == HTTP_RECV_OK_END_RECORD && s.now_ms == 0, d);
    free(sk.buf);
  }
  /* boundary + silence: settle probe still reports "boundary-aligned" (rc=1) */
  {
    int w = chunk_wrap(wire, body, bn);
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = false;   /* silence, not a close */
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d virtual_ms=%u", rc, s.now_ms);
    check("boundary + silence -> HTTP_RECV_OK_SILENCE after settle_ms", rc == HTTP_RECV_OK_SILENCE, d);
    free(sk.buf);
  }
  /* boundary + silence, but archive_done says the payload is NOT finished: silence is a
     pause, the body read waits it out and then the end record lands -> rc=2 */
  {
    int w = 0;
    w += chunk_wrap(wire + w, body, bn);
    int base = w;
    w += chunk_wrap(wire + w, endrec, 4);
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, base);            /* first record only, then a pause */
    script_add(&s, 500, wire + base, w - base); /* end record arrives 500 ms later */
    s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    g_archive_done = 0;                       /* veto: not structurally complete yet */
    sk.archive_done = archive_done_cb;
    int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
    g_archive_done = -1;
    char d[96]; snprintf(d, sizeof d, "rc=%d virtual_ms=%u", rc, s.now_ms);
    check("archive_done veto turns a boundary pause into a wait, then rc=2",
          rc == HTTP_RECV_OK_END_RECORD, d);
    free(sk.buf);
  }
  /* truncated body: header claims more than ever arrives, then the peer closes */
  {
    int w = 0;
    static unsigned char big[256];
    int biglen = dvzip_rec(big, 200);
    w += chunk_wrap(wire + w, big, biglen - 60);    /* 60 bytes missing */
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d", rc);
    check("truncated body ends TRUNCATED/POISONED, never OK", rc == HTTP_RECV_TRUNCATED || rc == HTTP_RECV_POISONED, d);
    free(sk.buf);
  }
  /* chunk size over the sink max: overflow, no partial success */
  {
    int w = sprintf((char *)wire, "FFFFFFFF\r\n");
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
    check("chunk size beyond max -> HTTP_RECV_OVERFLOW", rc == HTTP_RECV_OVERFLOW, NULL);
    free(sk.buf);
  }
  /* an overlong chunk-size line must poison, NOT read as a valid last-chunk */
  {
    static const char longsizeline[] = "00000000000000000000000000000000000000000000000010\r\n";
    int w = (int)sizeof(longsizeline) - 1;
    memcpy(wire, longsizeline, (size_t)w);
    memcpy(wire + w, "0\r\n\r\n", 5); w += 5;
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d", rc);
    check("truncated huge chunk-size line poisons, is not a last-chunk", rc == HTTP_RECV_POISONED, d);
    free(sk.buf);
  }
  /* consuming (streaming) sink: the hold-back rule plus the incremental probe */
  {
    static unsigned char recs[3][256];
    int rn[3], w = 0;
    for (int i = 0; i < 3; i++) rn[i] = dvzip_rec(recs[i], 32);
    for (int i = 0; i < 3; i++) w += chunk_wrap(wire + w, recs[i], rn[i]);
    w += chunk_wrap(wire + w, endrec, 4);
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    sk.buf = (unsigned char *)malloc(4096); sk.cap = 4096;
    sk.progress = cons_progress;
    g_cons_recs = 0; g_cons_pay = 0;
    int rc = http_recv_chunked(&r, &sk, 10000, 3000, NULL);
    char d[160];
    snprintf(d, sizeof d, "rc=%d recs=%d residue=%u", rc, g_cons_recs, sk.len);
    check("consuming sink: end record still recognised, residue is the end record",
          rc == HTTP_RECV_OK_END_RECORD && g_cons_recs == 3 && sk.len == 4 &&
          sk.buf[0] == 0 && sk.buf[1] == 0 && sk.buf[2] == 0 && sk.buf[3] == 0, d);
    check("consuming sink: every record payload was seen exactly once",
          g_cons_pay == (uint32_t)(3 * 32), NULL);
    free(sk.buf);
  }
  /* the RAW (gzip-like) shape still ends on the HTTP last-chunk marker */
  {
    static const unsigned char gz[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    int w = chunk_wrap(wire, gz, (int)sizeof(gz));
    memcpy(wire + w, "0\r\n\r\n", 5); w += 5;      /* the HTTP last-chunk marker */
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    struct HttpRdr r; http_rdr_init(&r, &s, script_read);
    struct HttpSink sk; sink_init(&sk, 1u << 20);
    int rc = http_recv_chunked(&r, &sk, 10000, 100, NULL);
    char dd[96]; snprintf(dd, sizeof dd, "rc=%d len=%u w=%d poisoned=%d", rc, sk.len, w, r.poisoned);
    check("gzip-shaped RAW body -> HTTP_RECV_OK_LAST_CHUNK", rc == HTTP_RECV_OK_LAST_CHUNK, dd);
    free(sk.buf);
  }
}

/* ======================= valid paths stay linear too ======================= */
static void test_valid_linear(void) {
  printf("== valid DvZip streaming path: tiny chunks stay linear ==\n");
  static unsigned char payload[512 * 1024], wire[4 * 1024 * 1024];
  size_t pl = 0, recs = 0;
  /* 64 KiB of legitimate DvZip records (4-byte header + zlib-CMF payload) */
  while (pl + 256 < (512u << 10) && pl < (64u << 10)) { pl += (size_t)dvzip_rec(payload + pl, 252); recs++; }
  memset(payload + pl, 0, 4); pl += 4;
  size_t w = 0;
  for (size_t i = 0; i < pl; i++) w += (size_t)sprintf((char *)wire + w, "1\r\n%c\r\n", payload[i]);
  memcpy(wire + w, "0\r\n\r\n", 5); w += 5;

  Script s; memset(&s, 0, sizeof(s));
  script_add(&s, 0, wire, (int)w); s.close_at_end = true;
  struct HttpRdr r; http_rdr_init(&r, &s, script_read);
  struct HttpSink sk; sink_init(&sk, 1u << 20);
  double t0 = now_real_ms();
  int rc = http_recv_chunked(&r, &sk, 10000, 3000, NULL);
  double t1 = now_real_ms();
  char d[192];
  snprintf(d, sizeof d, "records=%zu payload=%zu wire=%zu rc=%d %.1fms", recs, pl, w, rc, t1 - t0);
  check("64K valid DvZip body in 1-byte chunks: rc=2 and linear", rc == HTTP_RECV_OK_END_RECORD && (t1 - t0) < 400.0, d);
  free(sk.buf);

  /* the same records through the consuming (streaming) sink */
  {
    struct HttpRdr r2; http_rdr_init(&r2, &s, script_read);
    s.cur = 0; s.off = 0; s.now_ms = 0;
    struct HttpSink sk2; sink_init(&sk2, 1u << 20);
    sk2.buf = (unsigned char *)malloc(4096); sk2.cap = 4096;
    sk2.progress = cons_progress;
    g_cons_recs = 0;
    double t2 = now_real_ms();
    int rc2 = http_recv_chunked(&r2, &sk2, 10000, 3000, NULL);
    double t3 = now_real_ms();
    snprintf(d, sizeof d, "records=%d residue=%u rc=%d %.1fms", g_cons_recs, sk2.len, rc2, t3 - t2);
    check("consuming sink drains the same body without a rescan blow-up",
          rc2 == HTTP_RECV_OK_END_RECORD && g_cons_recs == (int)recs && (t3 - t2) < 400.0, d);
    free(sk2.buf);
  }
}

/* ======================= R-11: dispatch-layer budgets ======================= */
static void test_serve_budgets(void) {
  printf("== R-11 dispatch budgets (header section + /Discover & 404 drains) ==\n");
  struct HttpRdr r; struct HttpServe sv; Harness h;

  /* /Discover with an ENDLESS chunked body trickled at 300 ms/byte: the 200 is sent
     first (it does not depend on the body) and the drain must stop on its budget. */
  {
    static const unsigned char head[] =
      "POST /Discover HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    static const unsigned char tail[] = "1\r\nx\r\n";   /* an endless valid chunk stream */
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.head = head; ho.hn = (int)sizeof(head) - 1;
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1;
    ho.gap_ms = 300;
    harness_setup(&h, &sv, &r, &ho, host_read, &ho.now_ms);
    serve_http_run(&sv);
    char d[192];
    snprintf(d, sizeof d, "virtual_ms=%u disc_ok=%lu desync=%lu out=%d",
             ho.now_ms, (unsigned long)h.disc_ok, (unsigned long)h.desync, h.outn);
    bool ok = ho.now_ms < 6000 && h.disc_ok == 1 &&
              has_bytes(h.out, h.outn, "200 OK", 6);
    check("/Discover: 200 sent, endless trickled body drain bounded", ok, d);
  }
  /* /Discover with ONE huge chunk-size line and an endless slow payload: without a
     source-level deadline a single 512-byte read would take minutes. */
  {
    static const unsigned char head[] =
      "POST /Discover HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1000000\r\n";
    static const unsigned char tail[] = "xxxxxxxx";
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.head = head; ho.hn = (int)sizeof(head) - 1;
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1;
    ho.gap_ms = 300;
    harness_setup(&h, &sv, &r, &ho, host_read, &ho.now_ms);
    serve_http_run(&sv);
    char d[160];
    snprintf(d, sizeof d, "virtual_ms=%u disc_ok=%lu", ho.now_ms, (unsigned long)h.disc_ok);
    check("/Discover: huge chunk + slow payload bounded by the monotonic deadline",
          ho.now_ms < 6000 && h.disc_ok == 1, d);
  }
  /* 404 path with an endless trickled body: close instead of answering a half-read
     request. */
  {
    static const unsigned char head[] =
      "POST /Nope HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    static const unsigned char tail[] = "1\r\nx\r\n";
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.head = head; ho.hn = (int)sizeof(head) - 1;
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1;
    ho.gap_ms = 300;
    harness_setup(&h, &sv, &r, &ho, host_read, &ho.now_ms);
    serve_http_run(&sv);
    char d[192];
    snprintf(d, sizeof d, "virtual_ms=%u out=%d desync=%lu log=%d",
             ho.now_ms, h.outn, (unsigned long)h.desync, (int)hz_has_log(&h, "http_drain_stop"));
    check("404: endless trickled body -> bounded close, no half-read response",
          ho.now_ms < 12000 && h.outn == 0 && h.desync > 0, d);
  }
  /* /Discover with a huge body that arrives INSTANTLY: the byte cap must stop it even
     though the clock never advances (this pins the cap independently of the deadline). */
  {
    static unsigned char wire[4 + (1u << 20) + 128];
    int w = (int)snprintf((char *)wire, sizeof(wire),
                          "POST /Discover HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n%X\r\n", 1u << 20);
    memset(wire + w, 'x', 1u << 20); w += 1u << 20;
    memcpy(wire + w, "\r\n", 2); w += 2;            /* no terminating chunk on purpose */
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = false;
    harness_setup(&h, &sv, &r, &s, script_read, &s.now_ms);
    serve_http_run(&sv);
    char d[160];
    snprintf(d, sizeof d, "virtual_ms=%u disc_ok=%lu has200=%d", s.now_ms,
             (unsigned long)h.disc_ok, (int)has_bytes(h.out, h.outn, "200 OK", 6));
    check("/Discover: 1MB instant body stopped by the byte cap, 200 already sent",
          h.disc_ok == 1 && has_bytes(h.out, h.outn, "200 OK", 6) && s.now_ms < 4000, d);
  }

  /* header-section line-count cap: an endless header flood closes, no response */
  {
    static const unsigned char head[] = "POST /Discover HTTP/1.1\r\n";
    static const unsigned char tail[] = "X-A: b\r\n";
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.head = head; ho.hn = (int)sizeof(head) - 1;
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1;
    ho.gap_ms = 0;                                    /* test the count, not the clock */
    harness_setup(&h, &sv, &r, &ho, host_read, &ho.now_ms);
    serve_http_run(&sv);
    char d[160];
    snprintf(d, sizeof d, "out=%d desync=%lu budget=%d", h.outn, (unsigned long)h.desync,
             (int)hz_has_log(&h, "http_hdr_budget"));
    check("endless header lines -> http_hdr_budget close, no response",
          h.outn == 0 && h.desync > 0 && hz_has_log(&h, "http_hdr_budget"), d);
  }
  /* a single header line longer than the buffer: framing error, not a silent trim */
  {
    static const unsigned char head[] = "POST /Discover HTTP/1.1\r\n";
    static const unsigned char tail[] = "AAAAAAAA";  /* never a newline */
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.head = head; ho.hn = (int)sizeof(head) - 1;
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1;
    ho.gap_ms = 0;
    harness_setup(&h, &sv, &r, &ho, host_read, &ho.now_ms);
    serve_http_run(&sv);
    char d[128];
    snprintf(d, sizeof d, "out=%d desync=%lu", h.outn, (unsigned long)h.desync);
    check("endless overlong header line -> framing-error close, no response",
          h.outn == 0 && h.desync > 0, d);
  }
  /* unterminated request line trickled byte by byte: bounded by the line buffer */
  {
    static const unsigned char head[] = "POST /Dis";
    static const unsigned char tail[] = "AAAA";
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.head = head; ho.hn = (int)sizeof(head) - 1;
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1;
    ho.gap_ms = 0;
    harness_setup(&h, &sv, &r, &ho, host_read, &ho.now_ms);
    serve_http_run(&sv);
    char d[128];
    snprintf(d, sizeof d, "out=%d desync=%lu", h.outn, (unsigned long)h.desync);
    check("endless unterminated request line -> bounded close, no response",
          h.outn == 0 && h.desync > 0, d);
  }

  /* positive controls: the ordinary exchanges are unchanged */
  {
    static const unsigned char wire[] =
      "POST /Discover HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nABCD\r\n0\r\n\r\n";
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, (int)sizeof(wire) - 1); s.close_at_end = true;
    harness_setup(&h, &sv, &r, &s, script_read, &s.now_ms);
    serve_http_run(&sv);
    check("valid /Discover still answered 200 and counted",
          h.disc_ok == 1 && has_bytes(h.out, h.outn, "200 OK", 6), NULL);
  }
  {
    static const unsigned char wire[] =
      "POST /Nope HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, (int)sizeof(wire) - 1); s.close_at_end = true;
    harness_setup(&h, &sv, &r, &s, script_read, &s.now_ms);
    serve_http_run(&sv);
    check("unknown path with a complete body still answered 404",
          h.outn > 0 && has_bytes(h.out, h.outn, "404 Not Found", 13) && h.desync == 0,
          NULL);
  }
  {
    static unsigned char body[512], wire[2048];
    int bn = dvzip_rec(body, 32);
    int w = sprintf((char *)wire, "POST /Upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    w += chunk_wrap(wire + w, body, bn);
    static const unsigned char endrec[4] = {0, 0, 0, 0};
    w += chunk_wrap(wire + w, endrec, 4);
    Script s; memset(&s, 0, sizeof(s));
    script_add(&s, 0, wire, w); s.close_at_end = true;
    harness_setup(&h, &sv, &r, &s, script_read, &s.now_ms);
    serve_http_run(&sv);
    char d[128];
    snprintf(d, sizeof d, "uploads=%d chunked=%d rc=%d", h.uploads, h.up_chunked, h.up_rc);
    check("/Upload still routes to the body pipeline (valid body -> rc=2)",
          h.uploads == 1 && h.up_chunked == 1 && h.up_rc == HTTP_RECV_OK_END_RECORD, d);
  }
}

/* ======================= R-11 direct: the bounded drain contract ======================= */
/* A chunked body of exactly `n` payload bytes, then the terminating chunk. */
static int make_chunked_body(unsigned char *out, int n) {
  int w = sprintf((char *)out, "%X\r\n", n);
  memset(out + w, 'x', (size_t)n); w += n;
  out[w++] = '\r'; out[w++] = '\n';
  memcpy(out + w, "0\r\n\r\n", 5); w += 5;
  return w;
}

static bool drain_bounded_script(Script *s, const struct HttpDrainBudget *bd, bool *poisoned) {
  struct HttpRdr r; http_rdr_init(&r, s, script_read);
  s->close_at_end = true;
  bool ok = http_drain_body_bounded(&r, true, 0, bd);
  if (poisoned) *poisoned = r.poisoned;
  return ok;
}

static void test_drain_budget_direct(void) {
  printf("== R-11 direct: bounded drain byte/time edge cases ==\n");
  static unsigned char wire[8192];
  int w = make_chunked_body(wire, 64);
  bool poisoned = false;

  /* exactly on the byte cap is still a clean drain (the ">" rule) */
  {
    Script s; memset(&s, 0, sizeof(s)); script_add(&s, 0, wire, w);
    struct HttpDrainBudget bd = { 64, 5000, NULL, NULL };
    bool ok = drain_bounded_script(&s, &bd, &poisoned);
    char d[96]; snprintf(d, sizeof d, "drain=%d poisoned=%d", ok, poisoned);
    check("body of exactly max_bytes drains cleanly", ok && !poisoned, d);
  }
  /* one byte over the cap: poisoned, reported as failure */
  {
    Script s; memset(&s, 0, sizeof(s)); script_add(&s, 0, wire, w);
    struct HttpDrainBudget bd = { 63, 5000, NULL, NULL };
    bool ok = drain_bounded_script(&s, &bd, &poisoned);
    char d[96]; snprintf(d, sizeof d, "drain=%d poisoned=%d", ok, poisoned);
    check("body one byte past max_bytes is refused and poisoned", !ok && poisoned, d);
  }
  /* trickled endless body: the monotonic deadline (armed on the reader) stops it */
  {
    static const unsigned char tail[] = "1\r\nx\r\n";
    Host ho; memset(&ho, 0, sizeof(ho));
    ho.tail = tail; ho.tn = (int)sizeof(tail) - 1; ho.gap_ms = 300;
    struct HttpRdr r; http_rdr_init(&r, &ho, host_read);
    struct HttpDrainBudget bd = { 1024 * 1024, 1500, hcx_now, &ho };
    bool ok = http_drain_body_bounded(&r, true, 0, &bd);
    char d[128]; snprintf(d, sizeof d, "drain=%d virtual_ms=%u poisoned=%d",
                          ok, ho.now_ms, r.poisoned);
    check("endless trickle stops on the time budget, not on a read boundary", !ok && ho.now_ms < 3000, d);
  }
}

/* ======================= R-16: flat bplist offset width ======================= */
static void test_bplist_offsets(void) {
  printf("== R-16 flat bplist writer: refuse object offsets > 255 ==\n");
  static uint8_t out[1024];
  static char name[64];

  /* the port's real /Discover reply with the longest name AD_NAME_MAX allows (63 B) */
  memset(name, 'A', 63); name[63] = 0;
  {
    static const char MEDIA[] = "{\"Version\":1}";
    struct AdBplistKV disc[] = {
      { "ReceiverComputerName",      AD_BP_STR,  name,  0, 0 },
      { "ReceiverMediaCapabilities", AD_BP_DATA, MEDIA, (uint32_t)(sizeof(MEDIA) - 1), 0 },
      { "ReceiverModelName",         AD_BP_STR,  "M5Stack", 0, 0 },
    };
    uint32_t n = ad_bplist_dict(out, sizeof(out), disc, 3);
    char d[96]; snprintf(d, sizeof d, "len=%u", n);
    check("real /Discover reply with a 63-byte ASCII name still builds", n > 0 &&
          memcmp(out, "bplist00", 8) == 0, d);
  }
  /* worst UTF-16 case: 63 code units -> 126 payload bytes, the largest this field can
     carry (U+00A2, two UTF-8 bytes per code unit) */
  {
    static char cjk[256];
    int n = 0;
    for (int i = 0; i < 63; i++) { cjk[n++] = (char)0xC2; cjk[n++] = (char)0xA2; }
    struct AdBplistKV ask[] = {
      { "ReceiverComputerName", AD_BP_STR, cjk, (uint32_t)n, 0 },          /* 63 code units */
      { "ReceiverModelName",    AD_BP_STR, "M5Stack", 0, 0 },
    };
    uint32_t len = ad_bplist_dict(out, sizeof(out), ask, 2);
    char d[96]; snprintf(d, sizeof d, "len=%u", len);
    check("flat writer accepts a 63-code-unit UTF-16 name (offsets still < 256)", len > 0, d);
  }
  /* an object offset past 255 would have been (uint8_t)-truncated: must now refuse */
  {
    static char big[300];
    memset(big, 'x', sizeof(big));
    struct AdBplistKV kv[] = {
      { "a", AD_BP_DATA, big, (uint32_t)sizeof(big), 0 },   /* pushes the next value past 255 */
      { "z", AD_BP_INT,  NULL, 0, 0 },
    };
    kv[1].num = 77;
    uint32_t n = ad_bplist_dict(out, sizeof(out), kv, 2);
    char d[96]; snprintf(d, sizeof d, "returned=%u (old writer emitted a truncated table)", n);
    check("object offset > 255 refused instead of silently truncated", n == 0, d);
  }
  /* nothing changes for the ordinary small dictionary */
  {
    struct AdBplistKV ask[] = {
      { "ReceiverComputerName", AD_BP_STR, "M5 Badge", 0, 0 },
      { "ReceiverModelName",    AD_BP_STR, "M5Stack", 0, 0 },
    };
    uint32_t n = ad_bplist_dict(out, sizeof(out), ask, 2);
    check("ordinary /Ask reply still builds", n > 0 && memcmp(out, "bplist00", 8) == 0, NULL);
  }
}

int main(void) {
  printf("== airDrop protocol/DoS host regression (real headers, no IDF) ==\n");
  test_verdict_equivalence();
  test_attack_linear();
  test_valid_paths();
  test_serve_budgets();
  test_drain_budget_direct();
  test_valid_linear();
  test_bplist_offsets();
  printf("\n%d checks, %d failed\n", g_ran, g_fails);
  return g_fails ? 1 : 0;
}
