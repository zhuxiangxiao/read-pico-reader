// AirDrop HTTP/1.1 stream layer: the chunked reader and everything that decides
// where a request boundary is.
//
// Dependency-free integer C over a single abstract byte source, so the SAME code
// compiles into the firmware and into a host test (tools/test-http.sh) that replays
// scripted byte streams with scripted arrival TIMES. The timing is the point: every
// bug this layer has produced was a timeout interacting with chunked framing, and
// none of them is reachable without modelling when bytes arrive.
//
// ---------------------------------------------------------------------------
// THE INVARIANT
// ---------------------------------------------------------------------------
// A read that consumes bytes and then stops for any reason OTHER than the parser's
// own delimiter -- a timeout, an EOF, a short read, a line with no terminator --
// has left the byte stream at an unknown offset. TLS provides no framing above it
// to resynchronise against, so the stream is finished: the only legal operation
// afterwards is close. That is `poisoned`, and it is enforced HERE, in the reader,
// not at the call sites -- the bug that motivated it was created by tuning a
// timeout at a call site, and a rule living at call sites is a rule the next
// timeout tweak can quietly reopen.
//
// What that bug was, for the record. sharingd sends /Ask with a ~129KB chunked body
// (a PKCS#7 Apple-ID validation record plus a FileIcon preview). Answering /Ask and
// then draining that body with a 400ms budget read ZERO bytes -- the body had not
// begun arriving, the link's RTT being 0.4-1.0s -- and the code then looped and read
// the body's first chunk header, "1F7BB", as an HTTP request line. It 404'd into the
// middle of somebody's body and closed. The transfer died with the badge showing
// "Receiving 0kB" while the Mac showed the send as complete.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "ad_dvzip.h"   /* record framing: dvzip_complete below judges with it */

#define HTTP_BUF 2048

// A byte source. read() returns >0 bytes read, 0 on TIMEOUT (nothing arrived within
// the caller's budget -- NOT a close), and <0 when the peer has closed. Conflating
// the middle case with the last one is what latched EOF on a deliberately short
// drain and killed the connection the next request was about to arrive on.
struct HttpRdr;
typedef int (*http_src_read_fn)(struct HttpRdr *r, unsigned char *buf, int len);

struct HttpRdr {
  void            *ctx;          // mbedtls_ssl_context* in the firmware; a script on the host
  http_src_read_fn src;
  uint32_t         timeout_ms;   // the caller's current patience, visible for tests/logs
  int              patience;     // consecutive SOURCE TIMEOUTS one read may wait through
                                 // (0 = give up on the first). A timeout is a pause, not
                                 // corruption -- see hr_read. Set per phase like timeout_ms.
  const unsigned char *pre;      // bytes already read past the headers (primed)
  int              pre_n, pre_p;
  unsigned char    buf[HTTP_BUF];
  int              n, p;
  bool             eof;          // the peer really closed: sticky, and correctly so
  bool             poisoned;     // see THE INVARIANT above: close is the only way out
  bool             timed_out;    // the last read expired: not a close
  uint32_t         consumed;     // body bytes actually consumed, so a log line can say
                                 // "gave up having read 0" at the moment it happens
  // 中文：可选的单调时限（R-11）。非 0 时每次向源要字节之前先检查；到期即 poison 并
  // 按“没有数据”返回。放在这里而不是两次读之间：对端每 <timeout 送 1 字节时，单次
  // 512 字节的读可以拖上几分钟，只在读之间检查时限根本不会触发。
  // English: optional monotonic deadline (R-11). When set, every source read checks it
  // first; once spent the stream is poisoned and the read reports "nothing available",
  // exactly like a timeout, so callers unwind through their existing paths. It lives
  // HERE rather than between reads because a peer sending one byte just before each
  // timeout turns a single 512-byte read into minutes -- a check between reads never
  // fires against that. deadline_ms = 0 disables it.
  uint32_t         deadline_ms;  // budget length / 预算毫秒数, 0 = off
  uint32_t         deadline_t0;  // start, in now_ms()'s domain / 起算时刻
  uint32_t (*now_ms)(void *ctx); // NULL = no clock / 没有时钟
  void            *now_ctx;
};

static inline void http_rdr_init(struct HttpRdr *r, void *ctx, http_src_read_fn src) {
  memset(r, 0, sizeof(*r));
  r->ctx = ctx; r->src = src; r->timeout_ms = 3000;
}

// 中文：设置/关闭当前阶段的单调时限。ms=0 或没有时钟即关闭。
// English: arm/clear the monotonic deadline for the current phase; 0 or no clock = off.
static inline void http_deadline_set(struct HttpRdr *r, uint32_t ms,
                                     uint32_t (*now_ms)(void *ctx), void *ctx) {
  r->deadline_ms = (ms && now_ms) ? ms : 0;
  r->now_ms = now_ms; r->now_ctx = ctx;
  r->deadline_t0 = r->deadline_ms ? now_ms(ctx) : 0;
}
static inline void http_deadline_off(struct HttpRdr *r) { r->deadline_ms = 0; }

static int hr_fill(struct HttpRdr *r) {
  if (r->p < r->n) return r->n - r->p;
  r->p = r->n = 0;
  if (r->pre_p < r->pre_n) {                       // primed bytes first
    int take = r->pre_n - r->pre_p;
    if (take > HTTP_BUF) take = HTTP_BUF;
    memcpy(r->buf, r->pre + r->pre_p, take); r->pre_p += take; r->n = take; return r->n;
  }
  if (r->eof) return 0;
  // 中文：时限在“每次向源要字节”处生效，见 deadline_ms 的说明。
  // English: the deadline is enforced at every source read; see deadline_ms above.
  if (r->deadline_ms && (uint32_t)(r->now_ms(r->now_ctx) - r->deadline_t0) >= r->deadline_ms) {
    r->poisoned = true; r->timed_out = true; return 0;
  }
  int ret = r->src(r, r->buf, HTTP_BUF);
  if (ret == 0) { r->timed_out = true; return 0; }   // timeout: NOT a close
  if (ret < 0)  { r->eof = true; return 0; }
  r->timed_out = false;
  r->n = ret; return r->n;
}

static int hr_read(struct HttpRdr *r, unsigned char *dst, int len) {
  int got = 0;
  int strikes = r->patience;
  while (got < len) {
    if (hr_fill(r) <= 0) {
      /* A source TIMEOUT is a pause, not corruption: `len` bytes are owed and the
         framing is intact, so waiting is safe and a stalled sender resumes. Field
         evidence: a multi-item send paused ~15 s mid-body (sender packing the next
         item, or a TCP retransmit burst on the lossy AWDL link) and the old code
         condemned the whole transfer at the first timeout as a framing break.
         `patience` bounds the wait; a real close (eof) still ends the read. */
      if (r->eof || strikes-- <= 0) break;
      continue;
    }
    int take = r->n - r->p; if (take > len - got) take = len - got;
    memcpy(dst + got, r->buf + r->p, take); r->p += take; got += take;
  }
  r->consumed += (uint32_t)got;
  if (got > 0 && got < len) r->poisoned = true;     // stopped mid-request
  return got;
}

// Returns the line length, 0 for a blank line, -1 when nothing was available.
// A line with bytes but no terminator poisons the stream: the old version returned
// it as though it were complete, which made a timeout in the middle of a header or
// a chunk-size line indistinguishable from a real one. Timeouts within `patience`
// are waited through, exactly as in hr_read.
// 中文：超出行缓冲的行同样按 framing 错误 poisoning，而不是静默截断——被截断的前缀
// 可能解析成 chunk-size=0（假的 last-chunk），把半截请求体当成协议终点。
// English: a line longer than the buffer poisons for the same reason instead of
// being silently truncated: a truncated prefix can parse as a chunk-size of 0 -- a
// bogus last-chunk -- and turn half a body into a framed terminator. Every line this
// layer reads is bounded by HTTP framing (a chunk-size line is a few hex digits, a
// header is small); no measured sender gets near the cap.
static int hr_line(struct HttpRdr *r, char *out, int outsz) {
  int i = 0;
  int strikes = r->patience;
  for (;;) {
    if (hr_fill(r) <= 0) {
      if (!r->eof && strikes-- > 0) continue;
      if (i) { r->poisoned = true; out[i] = 0; } return i ? i : -1;
    }
    char c = (char)r->buf[r->p++];
    if (c == '\n') {
      if (i && out[i - 1] == '\r') i--;
      out[i] = 0;
      return i;
    }
    if (i < outsz - 1) {
      out[i++] = c;
    } else {
      // 中文：越过行缓冲立即按 framing 错误终止，不再等换行：超长行本身就是错误，
      // 而“无限长且永不换行”的行只靠读超时是结束不了的。
      // English: overflow ends the line at once as a framing error instead of waiting
      // for a terminator: an overlong line is already wrong, and a line that is
      // infinite AND never terminated would otherwise only end on a read timeout.
      r->poisoned = true; out[outsz - 1] = 0; return outsz - 1;
    }
  }
}

static void hr_skip(struct HttpRdr *r, uint32_t len) {
  unsigned char tmp[512];
  while (len > 0) {
    int t = (len < 512) ? (int)len : 512;
    int g = hr_read(r, tmp, t);
    if (g <= 0) break;
    len -= (uint32_t)g;
  }
}

// The per-chunk CRLF: two bytes that say for free whether we are still aligned with
// the sender's framing. Discarding them threw that away.
static bool hr_crlf(struct HttpRdr *r) {
  unsigned char c[2];
  if (hr_read(r, c, 2) != 2 || c[0] != '\r' || c[1] != '\n') { r->poisoned = true; return false; }
  return true;
}

// ---------------------------------------------------------------------------
// Body drain: BOUNDED (R-11)
// ---------------------------------------------------------------------------
// 中文：/Discover 与 404 路径都会丢弃一个自己从不使用的请求体。两者原先没有总字节/总
// 时长上限：对端只要在每次读超时之前送来 1 个字节，就能把唯一的 TLS 任务无限期占住
// （同步的 /Ask drain 早就有 30 s 上限，这里向它看齐）。字节数或单调时限任一用尽即
// poison——请求体已不在自己的分隔符上，之后只能关闭连接，不能继续解析或作答。
// English: /Discover and the 404 path both discard a request body they never use, and
// neither had a total byte or wall-time budget: a peer sending one byte just before each
// read timeout held the single TLS task for ever (the synchronous /Ask drain has had a
// 30 s cap all along; these now follow it). Exhausting either budget poisons -- the body
// is no longer at its delimiter, so the only legal next move is close.
struct HttpDrainBudget {
  uint32_t max_bytes;             ///< 0 = 无字节上限 / no byte cap
  uint32_t max_ms;                ///< 0 = 无时限上限 / no time cap
  uint32_t (*now_ms)(void *ctx);  ///< NULL = 只按字节限 / no clock: byte cap only
  void     *now_ctx;
};

// True once a budget is spent, and poisons: the caller may not read on from here.
// 中文：字节上限用 ">"——正好读满 max_bytes 的完整 body 仍然算读成功，只有第
// max_bytes+1 个字节才越界（否则边界值上的正常请求会被误判为超预算）。
// English: the byte cap uses ">" so a body that ends exactly on max_bytes still counts
// as drained; only the byte PAST it is over budget (a ">=" here would fail a request
// whose legitimate size happens to land on the cap).
static bool http_drain_over(struct HttpRdr *r, const struct HttpDrainBudget *b,
                            uint32_t start, uint32_t got) {
  if (!b) return false;
  if (b->max_bytes && got > b->max_bytes) { r->poisoned = true; return true; }
  if (b->max_ms && b->now_ms && (uint32_t)(b->now_ms(b->now_ctx) - start) >= b->max_ms) {
    r->poisoned = true; return true;
  }
  return false;
}

// Read and discard `len` bytes, stopping when the budget trips. b == NULL keeps the
// original unbounded behaviour (hr_skip), which is what http_drain_body() below is.
static bool http_drain_skip(struct HttpRdr *r, uint32_t len,
                            const struct HttpDrainBudget *b, uint32_t start, uint32_t *got) {
  if (!b) { hr_skip(r, len); return !r->poisoned; }
  unsigned char tmp[512];
  while (len > 0) {
    if (http_drain_over(r, b, start, *got)) return false;
    uint32_t t = (len < (uint32_t)sizeof(tmp)) ? len : (uint32_t)sizeof(tmp);
    int g = hr_read(r, tmp, (int)t);
    if (g <= 0 || r->poisoned) return false;
    *got += (uint32_t)g; len -= (uint32_t)g;
  }
  return !http_drain_over(r, b, start, *got);
}

// Read and discard a request body under a budget. Returns true only if it ended at its
// own delimiter; anything else has poisoned the stream. The time budget is armed on the
// reader too (deadline_ms), so a trickled body cannot stretch one read past the budget.
static bool http_drain_body_bounded(struct HttpRdr *r, bool chunked, int clen,
                                    const struct HttpDrainBudget *b) {
  uint32_t start = (b && b->now_ms) ? b->now_ms(b->now_ctx) : 0;
  uint32_t got = 0;
  bool ok = true;
  if (b && b->max_ms && b->now_ms) http_deadline_set(r, b->max_ms, b->now_ms, b->now_ctx);
  if (chunked) {
    char ln[40];
    for (;;) {
      if (http_drain_over(r, b, start, got)) { ok = false; break; }
      if (hr_line(r, ln, sizeof(ln)) < 0) { ok = false; break; }
      if (r->poisoned) { ok = false; break; }
      uint32_t csz = (uint32_t)strtoul(ln, 0, 16);
      if (csz == 0) {                              // last chunk, then trailers
        for (;;) {
          int t = hr_line(r, ln, sizeof(ln));
          if (t <= 0) { ok = (t == 0 && !r->poisoned); break; }
          if (r->poisoned || http_drain_over(r, b, start, got)) { ok = false; break; }
        }
        break;
      }
      if (!http_drain_skip(r, csz, b, start, &got)) { ok = false; break; }
      if (!hr_crlf(r)) { ok = false; break; }
    }
  } else if (clen > 0) {
    ok = http_drain_skip(r, (uint32_t)clen, b, start, &got);
  }
  http_deadline_off(r);
  return ok;
}

// Unbounded form: the pre-R-11 behaviour, and the host-test baseline for the drain
// contract. Every /Discover and 404 call site uses the bounded form instead.
static bool http_drain_body(struct HttpRdr *r, bool chunked, int clen) {
  return http_drain_body_bounded(r, chunked, clen, NULL);
}

// A request line is by definition "<VERB> <PATH> HTTP/x.y". Anything else means we
// are not at a request boundary -- we are reading the middle of a body -- and every
// byte after it is garbage. Detected here so it can never be answered into.
static bool http_reqline_ok(const char *s) {
  if (strncmp(s, "POST /", 6) != 0 && strncmp(s, "GET /", 5) != 0) return false;
  return strstr(s, " HTTP/1.") != 0;
}

// ---------------------------------------------------------------------------
// DvZip completion
// ---------------------------------------------------------------------------
// The /Upload body is a run of [BE32 length][that many bytes of zlib] records. An
// explicit zero-length record is structural certainty; running out exactly on a
// record boundary is only a MAYBE, because a compressor that flushes per record
// makes every intermediate boundary look like the end.
//   2 = certain, 1 = boundary-aligned (needs corroborating silence), 0 = incomplete.
// Walks with ad_dvzip.h's iterator rather than by hand. The hand-rolled walk this
// replaces was a second copy of the framing knowledge, and a second copy learns
// slowly: it had to be taught about stored records separately from the iterator
// that already knew (before that, one stored record made this return 0 for ever,
// so such a body could only ever end on the HTTP last-chunk marker or a timeout --
// measured, 6 of the 18 records in a 2.35 MB photo are stored).
//
// 中文：判定改成可续走的（R-04）。原先每收到一个 chunk 都从 body 起点重走整条记录链，
// 一个首 4 字节为 80 00 00 00（stored 且长度 0 → RAW）、随后全是 5 字节伪记录的
// chunked /Upload 体，会让接收成本变成 O(n²)：宿主实测 8K/16K/32K/64K = 26/104/406/
// 1654 ms，每翻倍 4 倍；6 MB 上限下单个 TLS 任务可被占住数分钟。
// English: the judgement is resumable (R-04). It used to re-walk the record chain from
// the start of the body after every chunk, so a chunked /Upload whose first four bytes
// are 80 00 00 00 (stored, length 0 -> RAW) followed by five-byte pseudo-records made
// reception quadratic -- measured on the host at 26/104/406/1654 ms for 8K/16K/32K/64K,
// 4x per doubling, i.e. minutes of the single TLS task inside the 6 MB RAW ceiling.
// Now the walk remembers the next unjudged record boundary: every body byte is visited
// at most once, and only the tail record (the one still arriving) is re-read. A 1MB
// body whose records arrive one byte per chunk parses once, not once per chunk.
struct DvZipProbe {
  uint32_t pos;   // 下一条未判定记录的边界 / offset of the next unjudged record
  bool     bad;   // 粘性：见到非 DvZip 记录后永久为 0 / sticky "not DvZip at all"
};

static inline void dvzip_probe_reset(struct DvZipProbe *pb) { pb->pos = 0; pb->bad = false; }

// Same 0/1/2 verdict as dvzip_complete(), but resumable. `in`/`len` is the CURRENT
// residue; for a consuming sink the caller shifts `pos` by the bytes the progress hook
// removed from the front, so the cursor stays relative to buf[0].
static int dvzip_probe(struct DvZipProbe *pb, const unsigned char *in, uint32_t len) {
  if (pb->bad) return 0;
  // 中文：len<4 时旧实现直接返回 0（不能因恰好落在起点而报“边界对齐”）。
  if (len < 4) { pb->pos = 0; return 0; }
  if (pb->pos > len) pb->pos = 0;       // 缓冲区收缩比游标更靠前：只能重走（防御）
  struct AdDvzipIter it; struct AdDvzipRec rec;
  ad_dvzip_begin(&it, in, len);
  it.pos = pb->pos;                     // resume at the last record boundary
  while (ad_dvzip_next(&it, &rec)) {
    /* A deflated record's payload must open with a zlib header; a stored one must not
       be asked to. Anything else means these bytes are not DvZip at all. */
    if (!rec.stored && in[rec.off] != 0x78) { pb->bad = true; return 0; }
  }
  pb->pos = it.pos;                     // EXACT: len; CUT/STRAND: the unfinished header
  if (it.end == AD_DVZIP_END_RECORD) return 2;
  return (it.end == AD_DVZIP_END_EXACT) ? 1 : 0;
}

// One-shot form, kept for the existing callers and the upstream host tests: same
// verdict, walk from the first byte.
static int dvzip_complete(const unsigned char *in, uint32_t len) {
  struct DvZipProbe pb; dvzip_probe_reset(&pb);
  return dvzip_probe(&pb, in, len);
}

// Growable sink for the received body. `grow` must make room for `need` total bytes
// and return false if it cannot; the firmware backs it with PSRAM, the host with
// realloc.
struct HttpSink {
  unsigned char *buf;
  uint32_t cap, len, max;
  void *ctx;
  bool (*grow)(struct HttpSink *s, uint32_t need);
  /* Called after every successful append, if set. Optional and may be NULL.
   *
   * It exists because "receiving" was a state with no evidence behind it. The byte
   * counter a caller publishes was assigned once, AFTER the whole body had been read, so
   * a 73-second transfer displayed "0 KB" for 73 seconds and a transfer that never
   * started displayed "Receiving" for ever. Progress that is only reported at the end is
   * not progress; it is a result.
   *
   * A progress hook MAY CONSUME. It is allowed to remove a decoded prefix of buf
   * (memmove the tail down, reduce len), which is how the streaming DvZip sink decodes
   * a body WHILE it arrives instead of after the response has already promised success.
   * http_recv_chunked re-reads buf/len/max after every call and must keep doing so:
   * caching len across the hook would break the consuming sink silently, so the contract
   * is pinned by a host test (tools/http_test.c, "consuming sink"). A consuming hook in
   * turn owes three things, all supplied by ad_dvzip_drain's hold-back rule: remove only
   * whole records, never remove a terminating zero record, and never leave the buffer
   * empty -- dvzip_complete below judges the residue, and its verdict equals its verdict
   * on the whole body only under those three. */
  void (*progress)(struct HttpSink *s);
  /* Optional "is the payload structurally complete?" veto for the settle probe below.
   *
   * WHY IT EXISTS. Silence at a record boundary is NOT proof the body ended: a sender
   * that pauses between items, or a link stall that lands exactly on a record boundary
   * (a 15 s one was measured mid-transfer), leaves a boundary-aligned residue and goes
   * quiet -- and the settle probe would report success on an archive that is missing
   * its tail. The archive layer knows better (a cpio that has not reached TRAILER!!! is
   * not finished), so it gets to veto: silence then means "pause", and the normal body
   * read waits it out. NULL keeps the old behaviour (silence ends the body). */
  bool (*archive_done)(struct HttpSink *s);
};

enum {
  HTTP_RECV_OK_END_RECORD = 2,  // finished on an explicit DvZip end record
  HTTP_RECV_OK_SILENCE    = 1,  // boundary-aligned and the sender then went quiet
  HTTP_RECV_OK_LAST_CHUNK = 3,  // finished on the HTTP last-chunk marker
  HTTP_RECV_TRUNCATED     = 0,  // ran out: what we have may be incomplete
  HTTP_RECV_OVERFLOW      = -1,
  HTTP_RECV_POISONED      = -2,
};

// How many consecutive source timeouts one body read waits through (each lasts `body_ms`,
// so 6 x 10 s = tolerate a 60 s stall). Field evidence: a multi-item send paused ~15 s
// mid-body and resumed on its own; the transfer died only because the reader gave up at
// the first timeout. A sender that is gone for good still fails within a minute, and the
// settle probe below deliberately opts out (it uses silence as its signal).
#define HTTP_BODY_STALL_STRIKES 6

// Receive a chunked body into `sink`. `settle_ms` is how long the sender must stay
// quiet before a boundary-aligned DvZip stream is accepted as complete. Set it from
// the link, not from optimism: measured stalls here are 3004ms and 4104ms, and
// concluding "complete" during one returns success on a truncated body whose DvZip
// prefix still decompresses -- a corrupt image reported as a successful transfer.
// `set_timeout` lets the caller retune the source's patience per phase.
static int http_recv_chunked(struct HttpRdr *r, struct HttpSink *sink,
                             uint32_t body_ms, uint32_t settle_ms,
                             void (*set_timeout)(struct HttpRdr *, uint32_t)) {
  char ln[40];
  bool have_hdr = false;      // a chunk header already read during a settle window
  // 中文：增量判定游标（R-04）；只在本次 body 读取内有效。
  struct DvZipProbe probe; dvzip_probe_reset(&probe);
  for (;;) {
    // ONE place that reads a chunk header, ONE that appends, ONE that judges
    // completeness. The first version had a second append inside the settle branch
    // and did not re-judge after it, so a DvZip end record arriving as the chunk
    // right after a boundary-aligned probe was consumed and never recognised -- the
    // receiver then waited out the body timeout on a body it already had complete.
    // The host tests caught that in code already flashed to the device.
    if (!have_hdr) {
      if (set_timeout) set_timeout(r, body_ms);
      r->patience = HTTP_BODY_STALL_STRIKES;
      if (hr_line(r, ln, sizeof(ln)) < 0) return HTTP_RECV_TRUNCATED;
      if (r->poisoned) return HTTP_RECV_POISONED;
    }
    have_hdr = false;

    uint32_t csz = (uint32_t)strtoul(ln, 0, 16);
    if (csz == 0) return HTTP_RECV_OK_LAST_CHUNK;
    /* CHECK THE CHUNK SIZE BEFORE ADDING IT TO ANYTHING.
     *
     * csz comes from a hex line the sender wrote, so it can be anything. Written as
     * "sink->len + csz > sink->max" the addition WRAPS: a header of FFFC0000 makes the sum
     * small, the max guard passes, grow is skipped, and hr_read is called with (int)csz
     * negative. No memory is written -- hr_read does nothing with a negative length -- but
     * the framing desynchronises and the transfer dies, which is a denial of service a
     * stranger's device can trigger at will. Subtracting instead of adding cannot wrap,
     * because the len > max case is tested first. AND IT HAS TO BE: len <= max does NOT
     * always hold. A consuming progress hook signals "stop receiving" by setting max to 0
     * while the residue is still there, and with a bare subtraction 0 - len underflows to
     * four billion, the guard passes, and the abort never happens -- so the fix for one
     * unsigned bug quietly created another. Both were caught by reading, then pinned by
     * tests. */
    if (sink->len > sink->max || csz > sink->max - sink->len) return HTTP_RECV_OVERFLOW;
    if (sink->len > sink->cap || csz > sink->cap - sink->len) {
      if (!sink->grow(sink, sink->len + csz)) return HTTP_RECV_OVERFLOW;
    }
    if (set_timeout) set_timeout(r, body_ms);
    r->patience = HTTP_BODY_STALL_STRIKES;
    uint32_t len_before = sink->len;
    int got = hr_read(r, sink->buf + sink->len, (int)csz);
    if (got > 0) {
      sink->len += (uint32_t)got;
      if (sink->progress) sink->progress(sink);
      /* 中文：消费型 hook 可以 memmove 掉已解码的前缀；游标坐标随之左移，否则下一次
         判定会落在错位的字节上。hook 按契约只删前缀，所以减一次就够。
         English: a consuming hook may remove a decoded prefix with a memmove. The probe
         cursor is relative to buf[0], so it shifts by exactly what was removed; the hook
         contract only ever removes a prefix, so one subtraction is enough. */
      if (sink->len < len_before + (uint32_t)got) {
        uint32_t removed = len_before + (uint32_t)got - sink->len;
        probe.pos = (removed <= probe.pos) ? probe.pos - removed : 0;
      }
    }
    if (got < (int)csz) return r->poisoned ? HTTP_RECV_POISONED : HTTP_RECV_TRUNCATED;
    if (!hr_crlf(r)) return HTTP_RECV_POISONED;

    int dvz = dvzip_probe(&probe, sink->buf, sink->len);
    if (dvz == 2) return HTTP_RECV_OK_END_RECORD;      // structural certainty
    if (dvz != 1) continue;                            // definitely more to come

    // Boundary-aligned: only the sender's silence can distinguish "done" from
    // "flushed a record". If something does arrive, keep its header and process it
    // through the normal path above rather than duplicating that path here.
    // patience = 0: this probe USES the timeout as its signal, so it must not wait
    // one out. But the signal is only trustworthy once the payload is structurally
    // complete (archive_done); otherwise silence is just a pause and the body read
    // above waits it out within its patience.
    if (set_timeout) set_timeout(r, settle_ms);
    r->patience = 0;
    int lr = hr_line(r, ln, sizeof(ln));
    if (lr < 0) {
      if (!sink->archive_done || sink->archive_done(sink)) return HTTP_RECV_OK_SILENCE;
      continue;                       // a pause at a boundary, not the end of the body
    }
    if (r->poisoned) return HTTP_RECV_POISONED;
    have_hdr = true;
  }
}
