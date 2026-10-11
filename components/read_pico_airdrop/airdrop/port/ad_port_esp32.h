/* ad_port_esp32.h -- the AirDrop layer's entry points on the ESP32 backend.
 *
 * Everything here runs over a link the AWDL layer owns. There is no configuration for
 * that link and none is offered: AirDrop is not a protocol you can point somewhere else.
 *
 * THE CALLBACK RULE. ad_poll() is the only thing that runs user code, and it runs it on
 * the CALLER's task. Nothing in this header can put your code on the TLS task or the
 * frame task, which is why there is no "onReceive fires from wherever" option -- the
 * invariants those tasks carry are not ones a library can ask a user to honour.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../core/ad_status.h"
#include "../core/ad_dnssd.h"   /* AIRDROP_PORT */
#include "../core/ad_cpio.h"    /* AD_FILE_* and the archive walker */


/* One received file, delivered AS A STREAM. A transfer is a cpio archive and may hold
   several; you get each one, in order. The library does not choose between them, and in
   particular it does not filter by type -- `type` is sniffed from the leading bytes as a
   convenience, and AD_FILE_OTHER is delivered exactly like the rest. (The code this
   replaces returned the FIRST JPEG or PNG and discarded the archive, so a PDF, a text
   file, or the second of two photos vanished and the transfer was reported as "no
   displayable image".)

   WHY A STREAM AND NOT A BUFFER. The whole-file version handed over one PSRAM block
   holding the entire decoded archive, which put a hard ceiling on every transfer -- 5.4 MB
   measured, and a 5.6 MB send died on it. The bytes now arrive in bounded chunks while
   the transfer runs, so the only limit left is what your sink can store.

   The name/path pointers live inside the receiver and are valid only for the duration of
   the callback: copy what you need. */
struct AdFile {
  const char *name;  /* basename, NUL-terminated, e.g. "kuro_icon.png"           */
  const char *path;  /* as the sender wrote it, e.g. "./kuro_icon.png"           */
  uint32_t    size;  /* DECLARED payload size; what arrives is exactly this much */
  uint32_t    index; /* 0-based position among the files in THIS transfer        */
  int         type;  /* AD_FILE_* -- from the bytes, never from the name         */
};

/* THE DELIVERY CONTRACT. Three hooks -- begin, data, end -- called on the ad_poll() task
   -- the same task that called ad_poll(), never the TLS or frame tasks -- so a sink may
   touch the SD card, the display, or anything else the sketch owns.

   on_begin:  fired once a file's leading bytes identify it. Return AD_FILE_TAKE to
              receive its payload, AD_FILE_SKIP to drop it silently (metadata sidecars,
              filters -- no on_data, no on_end), or AD_FILE_FAIL when the file cannot be
              taken AND the transfer cannot continue (no space, no card). FAIL fails the
              whole transfer and is answered to the sender as 507; clean up whatever
              on_begin opened before returning it.
   on_data:   a payload chunk; the data pointer is into the receiver's buffer, valid only
              for this call. Return false to fail the transfer (a write error -- the
              receiver answers 507 and rolls the file back through on_end(false)).
   on_end:    exactly once per taken file. complete=false means the file was cut (or its
              write failed): delete whatever half-written thing you have. complete=true
              means the declared size arrived and the file is whole. */
enum { AD_FILE_TAKE = 0, AD_FILE_SKIP = 1, AD_FILE_FAIL = 2 };

struct AdFileOps {
  void *ctx;
  int  (*on_begin)(void *ctx, const struct AdFile *f);
  bool (*on_data)(void *ctx, const uint8_t *data, uint32_t len);
  void (*on_end)(void *ctx, bool complete);
};

void ad_on_file(struct AdFileOps ops);

/* Starts the TLS listener. AWDL must already be up: this binds to the link-local address
 * the AWDL netif owns.
 *
 * `name` is what appears on the sender's share sheet. It is UTF-8, so "リビングのバッジ"
 * and "badge 🐱" work -- the plist carries non-ASCII as UTF-16BE. It is measured, not
 * assumed: the tile reads ReceiverComputerName out of the /Discover response, established
 * by giving three candidate fields three different values and looking.
 *
 * Returns 0; -1 if the listener task could not be created; -2 if the name could not be
 * encoded (malformed UTF-8, or longer than the format's one-byte length). A name that
 * cannot be encoded is refused rather than truncated: it is something a user typed. */
/* `max_receive` is a POLICY cap on the DECODED archive this device will accept, in bytes.
   Pass 0 to decline one: the transfer is then bounded by whatever the file sink can store
   (the decoded bytes are streamed to it as they arrive -- see AdFile above -- so PSRAM is
   no longer the ceiling it was). A transfer past this cap is refused with HTTP 413 to the
   sender, so the Mac says the transfer failed instead of appearing to succeed. That is a
   policy limit and it is reported as one; running out of storage below the limit is a
   different fact and answers 507. ad_stats_read() publishes the effective ceiling.

   The shapes that cannot be streamed (a gzip or bare-cpio body) still accumulate in RAM
   and keep the old ceiling whatever this says: about 7.1 MB of decoded archive on an
   ESP32-S3R8 with this firmware, once the residue buffer and RX_OUT_HEADROOM are taken
   out of the largest free PSRAM block (measured base: 7,864,308 B). A sketch that wants
   RAM left over for a framebuffer or a log should pass a cap rather than rely on that. */
int  ad_begin(const char *name, uint32_t max_receive);
void ad_end(void);

/* Fires pending callbacks on the calling task, and drains decoded archive bytes to the
   file sink as they arrive -- a long file is written to storage inside this call, so call
   it often while a transfer is running. Cheap when there is nothing to do. Blocking
   inside a callback costs nothing on the air: the transmit cadence is owned by the AWDL
   layer's own task, and the receive side stalls the sender through TCP flow control when
   the sink falls behind. */
void ad_poll(void);

/* True once ad_begin() has claimed this device for receiving. The sender refuses to start
 * on a device that has, and vice versa: the two cannot share the memory. */
bool ad_role_is_receiver(void);

void ad_stats_read(struct AdStatus *out);

/* The SMALLEST free stack the TLS listener task has ever had, in bytes, or 0 before it
 * runs. The task is created with 32,768; a real transfer peaks at 17,348 bytes used, so
 * roughly 15 KB of that is headroom. The number matters because 32,768 CONTIGUOUS bytes of
 * internal RAM is the largest single allocation this library makes, out of the same heap
 * mbedTLS is competing for -- an ad_begin() that returns -1 failed to find that block, not
 * to find enough stack. Whether the reserve can be cut is then a measurement rather than an
 * argument.
 *
 * That measurement is taken only under -DAD_DIAG_DETAIL: sampling it costs a
 * uxTaskGetStackHighWaterMark() on every pass of a 100 ms loop, for ever, to re-derive a
 * figure that is already known. Without the define this returns 0, the same as it does
 * before the task has run. */
uint32_t ad_listen_stack_min_free(void);

/* --- diagnostics ----------------------------------------------------------------
 *
 * The library never writes to Serial. It stages one line at a time into a small ring and
 * you decide where they go -- which is the only arrangement that works, because these
 * lines are produced on the TLS task and a user-supplied sink would run there too. There
 * is no setLogSink() for that reason.
 *
 * Copies ONE whole line (NUL-terminated, keeping its own trailing newline) into dst and
 * returns its length, or 0 when the ring is empty. Call it in a loop:
 *
 *     char line[200];
 *     while (ad_diag_read_line(line, sizeof(line))) Serial.print(line);
 *
 * Lines are dropped rather than blocking a protocol task when the ring is full;
 * ad_diag_dropped() is the count, and a non-zero value means you are draining too slowly,
 * not that anything is wrong on the air. */
size_t   ad_diag_read_line(char *dst, size_t cap);
uint32_t ad_diag_dropped(void);

#ifdef AIRDROP_BODY_KEEP
/* --- raw-body probe (NOT part of the library's interface) ------------------------
 *
 * Only under -DAIRDROP_BODY_KEEP, which tools/build-bodykeep.sh sets. In that build the
 * /Upload body is NOT parsed: it is held so the sketch can write it out verbatim, because
 * a captured stream answers every question about the format at once and offline, while a
 * device-side instrument answers one question per physical AirDrop.
 *
 * ad_body_peek() returns the length and lends the pointer -- the library still owns the
 * memory. Call ad_body_release() when finished with it; nothing else frees it, so a
 * caller that forgets holds 2.35 MB of PSRAM and drops the next transfer. */
uint32_t ad_body_peek(const uint8_t **p);
void     ad_body_release(void);
#endif
