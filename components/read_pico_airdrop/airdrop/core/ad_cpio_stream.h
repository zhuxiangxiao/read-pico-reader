/* ad_cpio_stream.h -- parse the cpio archive of an AirDrop transfer ONE CHUNK AT A TIME.
 *
 * WHY THIS EXISTS NEXT TO ad_cpio.h. That walker needs the whole archive in RAM, which
 * put a hard ceiling on every transfer: the decoded cpio had to fit in one PSRAM block
 * (measured: 5,373,940 B on the device that failed a 5.6 MB send). This parser eats the
 * archive as it decodes and hands each file's payload to a consumer as a stream, so the
 * only buffers in the path are the caller's decode chunk and this state object. The
 * ceiling becomes whatever the consumer's storage holds.
 *
 * The wire format is unchanged and the same two variants are accepted:
 *   odc  "070707"           76-byte octal header, no padding  (what Apple senders emit)
 *   newc "070701"/"070702"  110-byte hex header, 4-byte aligned name/data
 * The bounds checks are the ones ad_cpio.h proved (a remote device chooses every size in
 * this stream), restated for the incremental case: a name is capped rather than trusted,
 * a header that does not parse ends the walk instead of being skipped over, and no
 * arithmetic on remote sizes is used before it is bounds-checked.
 *
 * DELIVERY CONTRACT (the whole point of the file):
 *   - on_begin fires for every REGULAR entry, once its leading bytes identify the type
 *     (zero-length entries fire immediately). Directories are skipped silently; the
 *     TRAILER!!! record ends the archive and is not an entry.
 *   - Returning AD_CPIO_FILE_TAKE streams the payload through on_data in whatever chunks
 *     the caller feeds (zero copy: the pointers are into the caller's buffer) and then
 *     calls on_end EXACTLY ONCE -- complete=false if the archive died mid-payload.
 *   - Returning AD_CPIO_FILE_SKIP drops the payload silently (sidecars, filters): no
 *     on_data, no on_end. Returning AD_CPIO_FILE_FAIL fails the whole archive with
 *     AD_CPIO_STREAM_WRITE; clean up whatever on_begin opened before returning it.
 *
 * Dependency-free C, compiled unchanged into the firmware and into the host tests in
 * test/airdrop_cpio_stream/. Keep it that way: this is parser code fed by a stranger's
 * device, and host tests are where its bounds belong to be attacked.
 */
#ifndef AD_CPIO_STREAM_H
#define AD_CPIO_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ad_cpio.h" /* the octal/hex readers, the sniffer and the basename walk */

/* What feeding more bytes (or finishing) did. DONE/OK end a feed loop; BAD and WRITE are
   terminal -- feed nothing more. */
enum {
  AD_CPIO_STREAM_OK = 0, /* all bytes consumed; the archive is still open        */
  AD_CPIO_STREAM_DONE,   /* TRAILER!!! (or a clean boundary) ended the archive   */
  AD_CPIO_STREAM_BAD,    /* malformed or truncated archive                       */
  AD_CPIO_STREAM_WRITE,  /* a consumer callback failed; the archive is abandoned */
};

/* What on_begin wants done with the entry's payload. */
enum {
  AD_CPIO_FILE_TAKE = 0, /* stream it through on_data, then call on_end           */
  AD_CPIO_FILE_SKIP = 1, /* drop it silently (metadata sidecar, filtered type)    */
  AD_CPIO_FILE_FAIL = 2, /* cannot use it AND cannot continue: fail the archive   */
};

/* A name the caller may keep a pointer into only for the duration of its callback: the
   storage is inside the parser and is reused by the next entry. Both strings are
   NUL-terminated copies -- remote bytes are never handed over directly. */
struct AdCpioStreamFile {
  const char* name; /* basename, e.g. "kuro_icon.png"                             */
  const char* path; /* as the sender wrote it, e.g. "./kuro_icon.png"             */
  uint32_t size;    /* declared payload size in bytes                             */
  uint32_t index;   /* 0-based position among the files of THIS transfer          */
  int type;         /* AD_FILE_* -- sniffed from the leading bytes, never a name  */
};

struct AdCpioStreamOps {
  void* ctx;
  int (*on_begin)(void* ctx, const struct AdCpioStreamFile* f);
  bool (*on_data)(void* ctx, const uint8_t* data, uint32_t len);
  void (*on_end)(void* ctx, bool complete);
};

#define AD_CPIO_NAME_MAX 256 /* incl. the stored NUL; a longer name is malformed   */
#define AD_CPIO_SNIFF_LEN 16 /* leading bytes held back to sniff the type          */

enum {
  AD_CPIO_PH_MAGIC = 0, /* collecting the 6-byte magic                             */
  AD_CPIO_PH_HDR,       /* collecting the rest of the 76/110-byte header           */
  AD_CPIO_PH_NAME,      /* collecting namesize bytes                               */
  AD_CPIO_PH_PAD1,      /* consuming the name's 4-byte alignment (newc only)       */
  AD_CPIO_PH_SNIFF,     /* holding back leading payload bytes before on_begin       */
  AD_CPIO_PH_DATA,      /* streaming (or skipping) the payload                     */
  AD_CPIO_PH_PAD2,      /* consuming the payload's 4-byte alignment (newc only)    */
  /* terminal states, in the same order as the AD_CPIO_STREAM_* verdicts minus one */
  AD_CPIO_PH_DONE,
  AD_CPIO_PH_BAD,
  AD_CPIO_PH_WRITE
};

struct AdCpioStream {
  struct AdCpioStreamOps ops;
  uint32_t pos; /* archive bytes consumed; drives the 4-byte alignment        */
  uint8_t phase;
  bool align4; /* newc (true) vs odc (false)                                 */
  bool take;   /* current entry streams through on_data                      */
  bool opened; /* on_begin returned TAKE; on_end is owed exactly once        */
  bool is_regular;
  uint32_t mode;
  uint32_t file_size;
  uint32_t data_left; /* payload bytes still owed for the current entry           */
  uint32_t pad_need;
  uint32_t file_index; /* regular entries begun so far                             */
  /* gather scratch: the header and the name are copied out of the stream because
     both can straddle a caller's chunk; the payload is never copied. */
  uint8_t hdr[112];
  uint32_t hdr_have, hdr_need;
  char name[AD_CPIO_NAME_MAX];
  uint32_t name_have, name_need;
  uint8_t sniff[AD_CPIO_SNIFF_LEN];
  uint32_t sniff_have, sniff_need;
};

static void ad_cpio_stream_init(struct AdCpioStream* s, struct AdCpioStreamOps ops) {
  memset(s, 0, sizeof(*s));
  s->ops = ops;
  s->phase = AD_CPIO_PH_MAGIC;
  s->hdr_need = 6;
}

static void ad_cpio_stream_next_entry(struct AdCpioStream* s) {
  s->phase = AD_CPIO_PH_MAGIC;
  s->hdr_need = 6;
  s->hdr_have = 0;
}

/* Close out the current entry when an archive failure cut it mid-payload: the consumer
   gets its rollback signal (on_end(false)) exactly when it was promised one. */
static void ad_cpio_stream_abort_entry(struct AdCpioStream* s) {
  if (s->opened) {
    s->opened = false;
    if (s->ops.on_end) s->ops.on_end(s->ops.ctx, false);
  }
}

/* The one failure funnel: pay out the rollback signal, park the state machine on a
   terminal state, and return the verdict that state maps to. */
static int ad_cpio_stream_fail(struct AdCpioStream* s, bool write_error) {
  ad_cpio_stream_abort_entry(s);
  s->phase = write_error ? AD_CPIO_PH_WRITE : AD_CPIO_PH_BAD;
  return write_error ? AD_CPIO_STREAM_WRITE : AD_CPIO_STREAM_BAD;
}

static int ad_cpio_stream_verdict(const struct AdCpioStream* s) {
  switch (s->phase) {
    case AD_CPIO_PH_DONE:
      return AD_CPIO_STREAM_DONE;
    case AD_CPIO_PH_BAD:
      return AD_CPIO_STREAM_BAD;
    case AD_CPIO_PH_WRITE:
      return AD_CPIO_STREAM_WRITE;
    default:
      return AD_CPIO_STREAM_OK;
  }
}

/* Parse one gathered header (magic + body already in s->hdr). Sets up name/data state
   for the next phases. */
static int ad_cpio_stream_header(struct AdCpioStream* s) {
  uint32_t namesize = 0, filesize = 0, mode = 0;
  bool align4;
  if (memcmp(s->hdr, "070707", 6) == 0) { /* odc: what Apple senders emit */
    align4 = false;
    if (!ad_cpio_oct(s->hdr + 18, 6, &mode) || !ad_cpio_oct(s->hdr + 59, 6, &namesize) ||
        !ad_cpio_oct(s->hdr + 65, 11, &filesize)) {
      return ad_cpio_stream_fail(s, false);
    }
  } else if (memcmp(s->hdr, "070701", 6) == 0 || memcmp(s->hdr, "070702", 6) == 0) {
    align4 = true;
    if (!ad_cpio_hex(s->hdr + 14, 8, &mode) || !ad_cpio_hex(s->hdr + 54, 8, &filesize) ||
        !ad_cpio_hex(s->hdr + 94, 8, &namesize)) {
      return ad_cpio_stream_fail(s, false);
    }
  } else {
    return ad_cpio_stream_fail(s, false);
  }

  /* The stored size includes the trailing NUL; zero or over the parser's own buffer is
     malformed. The cap is a HARD bound: a longer name is refused rather than truncated,
     because a truncated basename writes a file under the wrong name. */
  if (namesize == 0 || namesize > AD_CPIO_NAME_MAX) {
    return ad_cpio_stream_fail(s, false);
  }

  s->align4 = align4;
  s->mode = mode;
  s->file_size = filesize;
  s->data_left = filesize;
  s->name_need = namesize;
  s->name_have = 0;
  s->phase = AD_CPIO_PH_NAME;
  return AD_CPIO_STREAM_OK;
}

/* Sniff window closed (or empty): fire on_begin and start the payload. */
static int ad_cpio_stream_open_file(struct AdCpioStream* s) {
  struct AdCpioStreamFile f;
  uint32_t name_len = s->name_need ? s->name_need - 1 : 0;
  uint32_t base_len = 0;
  const char* base = ad_cpio_basename(s->name, name_len, &base_len);
  (void)base_len;

  f.name = base;
  f.path = s->name;
  f.size = s->file_size;
  f.index = s->file_index;
  f.type = ad_cpio_sniff(s->sniff, s->sniff_have);

  int want = s->ops.on_begin ? s->ops.on_begin(s->ops.ctx, &f) : AD_CPIO_FILE_SKIP;
  if (want == AD_CPIO_FILE_FAIL) return ad_cpio_stream_fail(s, true);
  if (want != AD_CPIO_FILE_TAKE) {
    /* SKIP: no on_data, no on_end -- nothing was opened. The held-back bytes are
       dropped with the rest of the payload by the DATA state. */
    s->take = false;
    s->data_left -= s->sniff_have;
  } else {
    s->take = true;
    s->opened = true;
    s->file_index++;
    /* Deliver the held-back sniff bytes first so the payload arrives in order. */
    if (s->sniff_have) {
      bool ok = s->ops.on_data ? s->ops.on_data(s->ops.ctx, s->sniff, s->sniff_have) : true;
      s->data_left -= s->sniff_have;
      if (!ok) return ad_cpio_stream_fail(s, true);
    }
    if (s->data_left == 0) {
      s->opened = false;
      if (s->ops.on_end) s->ops.on_end(s->ops.ctx, true);
    }
  }
  if (s->data_left) {
    s->phase = AD_CPIO_PH_DATA;
  } else {
    s->pad_need = (s->align4 && (s->pos & 3u)) ? (4u - (s->pos & 3u)) : 0u;
    if (s->pad_need) {
      s->phase = AD_CPIO_PH_PAD2;
    } else {
      ad_cpio_stream_next_entry(s);
    }
  }
  return AD_CPIO_STREAM_OK;
}

/* Name gathered: TRAILER check, delivery decision, alignment. */
static int ad_cpio_stream_name_done(struct AdCpioStream* s) {
  uint32_t name_len = s->name_need ? s->name_need - 1 : 0; /* the stored size keeps the NUL */
  s->name[name_len] = 0;

  /* TRAILER!!! ends the archive and is not an entry. Same test as ad_cpio_next's. */
  if (s->name_need >= 11 && memcmp(s->name, "TRAILER!!!", 10) == 0) {
    s->phase = AD_CPIO_PH_DONE;
    return AD_CPIO_STREAM_DONE;
  }

  s->is_regular = ad_cpio_is_regular(s->mode);
  s->take = false;
  s->opened = false;
  s->sniff_have = 0;
  /* Hold back up to AD_CPIO_SNIFF_LEN leading payload bytes so on_begin can carry the
     sniffed type; a zero-length entry (or a directory) has nothing to sniff. */
  s->sniff_need =
      (s->is_regular && s->file_size) ? ((s->file_size < AD_CPIO_SNIFF_LEN) ? s->file_size : AD_CPIO_SNIFF_LEN) : 0;

  s->pad_need = (s->align4 && (s->pos & 3u)) ? (4u - (s->pos & 3u)) : 0u;
  if (s->pad_need) {
    s->phase = AD_CPIO_PH_PAD1;
    return AD_CPIO_STREAM_OK;
  }
  if (!s->is_regular) {
    s->phase = AD_CPIO_PH_DATA; /* directories: payload skipped, no callbacks */
    return AD_CPIO_STREAM_OK;
  }
  if (s->sniff_need == 0) return ad_cpio_stream_open_file(s); /* zero-length file */
  s->phase = AD_CPIO_PH_SNIFF;
  return AD_CPIO_STREAM_OK;
}

/* The incremental entry point. Consumes as many of `len` bytes as the state machine can
   and reports how many through *consumed (0 when a terminal state was already reached).
   Returns OK, DONE, BAD or WRITE; BAD/WRITE are terminal. */
static int ad_cpio_stream_feed(struct AdCpioStream* s, const uint8_t* data, uint32_t len, uint32_t* consumed) {
  uint32_t used = 0;

  while (used < len) {
    switch (s->phase) {
      case AD_CPIO_PH_MAGIC:
      case AD_CPIO_PH_HDR: {
        uint32_t want = s->hdr_need - s->hdr_have;
        uint32_t take = (want < len - used) ? want : (len - used);
        memcpy(s->hdr + s->hdr_have, data + used, take);
        s->hdr_have += take;
        used += take;
        s->pos += take;
        if (s->hdr_have < s->hdr_need) break; /* header straddles the chunk */
        if (s->phase == AD_CPIO_PH_MAGIC) {
          /* Magic gathered: it names the header size, so only now can the rest be asked
             for. A header that is not one of these three is not a header; stopping here
             is what keeps the walk from parsing payload bytes as sizes. */
          if (memcmp(s->hdr, "070707", 6) == 0) {
            s->hdr_need = 76;
          } else if (memcmp(s->hdr, "070701", 6) == 0 || memcmp(s->hdr, "070702", 6) == 0) {
            s->hdr_need = 110;
          } else {
            ad_cpio_stream_fail(s, false);
            goto out;
          }
          s->phase = AD_CPIO_PH_HDR;
          break;
        }
        if (ad_cpio_stream_header(s) != AD_CPIO_STREAM_OK) goto out;
        break;
      }
      case AD_CPIO_PH_NAME: {
        uint32_t want = s->name_need - s->name_have;
        uint32_t take = (want < len - used) ? want : (len - used);
        memcpy(s->name + s->name_have, data + used, take);
        s->name_have += take;
        used += take;
        s->pos += take;
        if (s->name_have < s->name_need) break;
        if (ad_cpio_stream_name_done(s) != AD_CPIO_STREAM_OK) goto out;
        break;
      }
      case AD_CPIO_PH_PAD1:
      case AD_CPIO_PH_PAD2: {
        uint32_t take = (s->pad_need < len - used) ? s->pad_need : (len - used);
        used += take;
        s->pos += take;
        s->pad_need -= take;
        if (s->pad_need) break;
        if (s->phase == AD_CPIO_PH_PAD1) {
          s->phase = s->is_regular ? AD_CPIO_PH_SNIFF : AD_CPIO_PH_DATA;
          if (s->is_regular && s->sniff_need == 0) {
            if (ad_cpio_stream_open_file(s) != AD_CPIO_STREAM_OK) goto out;
          }
        } else {
          ad_cpio_stream_next_entry(s);
        }
        break;
      }
      case AD_CPIO_PH_SNIFF: {
        uint32_t want = s->sniff_need - s->sniff_have;
        uint32_t take = (want < len - used) ? want : (len - used);
        memcpy(s->sniff + s->sniff_have, data + used, take);
        s->sniff_have += take;
        used += take;
        s->pos += take;
        if (s->sniff_have < s->sniff_need) break;
        if (ad_cpio_stream_open_file(s) != AD_CPIO_STREAM_OK) goto out;
        break;
      }
      case AD_CPIO_PH_DATA: {
        uint32_t take = (s->data_left < len - used) ? s->data_left : (len - used);
        if (s->take && take) {
          bool ok = s->ops.on_data ? s->ops.on_data(s->ops.ctx, data + used, take) : true;
          if (!ok) {
            used += take;
            s->pos += take;
            s->data_left -= take;
            ad_cpio_stream_fail(s, true);
            goto out;
          }
        }
        used += take;
        s->pos += take;
        s->data_left -= take;
        if (s->data_left) break;
        if (s->opened) {
          s->opened = false;
          if (s->ops.on_end) s->ops.on_end(s->ops.ctx, true);
        }
        s->pad_need = (s->align4 && (s->pos & 3u)) ? (4u - (s->pos & 3u)) : 0u;
        if (s->pad_need) {
          s->phase = AD_CPIO_PH_PAD2;
        } else {
          ad_cpio_stream_next_entry(s);
        }
        break;
      }
      default:
        goto out; /* terminal states: DONE / BAD / WRITE */
    }
  }

out:
  if (consumed) *consumed = used;
  return ad_cpio_stream_verdict(s);
}

/* No more bytes will arrive. DONE when the walk is at a clean entry boundary (with or
   without a TRAILER!!!), BAD when an entry was cut mid-payload -- the same "that was all
   of it" vs "that was as much as survived" line ad_cpio_next draws with it->truncated. */
static int ad_cpio_stream_finish(struct AdCpioStream* s) {
  if (s->phase == AD_CPIO_PH_DONE) return AD_CPIO_STREAM_DONE;
  if (s->phase == AD_CPIO_PH_BAD || s->phase == AD_CPIO_PH_WRITE) {
    return ad_cpio_stream_verdict(s);
  }
  if (s->phase == AD_CPIO_PH_MAGIC && s->hdr_have == 0) {
    s->phase = AD_CPIO_PH_DONE;
    return AD_CPIO_STREAM_DONE;
  }
  return ad_cpio_stream_fail(s, false);
}

#endif /* AD_CPIO_STREAM_H */
