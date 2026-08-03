/* bus_capture.c: capture stream writer, reader, and observational replay (D10). */
#include <stdlib.h>
#include <string.h>

#include <aimee/core/event_bus/bus_capture.h>
#include <aimee/core/event_bus/bus_wire.h>

/* ---- CRC-32C (Castagnoli), the record checksum named by D10 ---- */

static uint32_t crc32c(const uint8_t *p, size_t n)
{
   static uint32_t table[256];
   static int built = 0;
   if (!built)
   {
      for (uint32_t i = 0; i < 256; i++)
      {
         uint32_t c = i;
         for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82f63b78u ^ (c >> 1)) : (c >> 1);
         table[i] = c;
      }
      built = 1;
   }
   uint32_t crc = 0xffffffffu;
   for (size_t i = 0; i < n; i++)
      crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
   return crc ^ 0xffffffffu;
}

/* Record layout (D10):
 *   0   4  record_len (total incl this field and the trailing CRC)
 *   4   2  record_type
 *   6   2  record_flags
 *   8   BUS_WIRE_HDR_LEN  frame header, verbatim
 *   ...  payload_len      materialized payload
 *   end-4  4  crc32c over every preceding byte
 */
#define REC_PREFIX 8u
#define REC_MIN    (REC_PREFIX + BUS_WIRE_HDR_LEN + 4u)

static void put_u16(uint8_t *p, uint16_t v)
{
   p[0] = v & 0xff;
   p[1] = (v >> 8) & 0xff;
}
static void put_u32(uint8_t *p, uint32_t v)
{
   for (int i = 0; i < 4; i++)
      p[i] = (v >> (8 * i)) & 0xff;
}
static void put_u64(uint8_t *p, uint64_t v)
{
   for (int i = 0; i < 8; i++)
      p[i] = (v >> (8 * i)) & 0xff;
}
static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int reserve(bus_capture_t *c, size_t extra)
{
   if (c->broken)
      return 0;
   if (c->len + extra <= c->cap)
      return 1;
   size_t ncap = c->cap ? c->cap * 2 : 4096;
   while (ncap < c->len + extra)
      ncap *= 2;
   uint8_t *nb = realloc(c->buf, ncap);
   if (!nb)
   {
      c->broken = 1;
      return 0;
   }
   c->buf = nb;
   c->cap = ncap;
   return 1;
}

void bus_capture_init(bus_capture_t *c, uint32_t spec_version, uint32_t layout_version,
                      uint64_t host_epoch)
{
   memset(c, 0, sizeof *c);
   c->spec_version = spec_version;
   c->layout_version = layout_version;
   c->host_epoch = host_epoch;
}

void bus_capture_free(bus_capture_t *c)
{
   free(c->buf);
   c->buf = NULL;
   c->cap = c->len = 0;
}

static void write_header(bus_capture_t *c)
{
   if (!reserve(c, BUS_CAPTURE_HEADER_LEN))
      return;
   uint8_t *h = c->buf;
   memset(h, 0, BUS_CAPTURE_HEADER_LEN);
   memcpy(h + 0, BUS_CAPTURE_MAGIC, 8);
   put_u16(h + 8, BUS_CAPTURE_FORMAT_VERSION);
   put_u16(h + 10, BUS_CAPTURE_HEADER_LEN);
   put_u32(h + 12, 0); /* flags */
   put_u32(h + 16, c->spec_version);
   put_u32(h + 20, c->layout_version);
   put_u64(h + 24, c->host_epoch);
   put_u64(h + 32, c->first_seq);
   put_u64(h + 40, 0); /* created_unix_nanos: informational, left 0 (no wall clock here) */
   /* 48..64 reserved, zero. */
   c->len = BUS_CAPTURE_HEADER_LEN;
   c->header_written = 1;
}

static bus_capture_record_type_t type_for_kind(uint32_t kind)
{
   switch (kind)
   {
   case BUS_KIND_OVERFLOW:
      return BUS_CAP_OVERFLOW;
   case BUS_KIND_PRODUCER_REAPED:
      return BUS_CAP_PRODUCER_REAPED;
   case BUS_KIND_EPOCH_CHANGE:
      return BUS_CAP_EPOCH_CHANGE;
   default:
      return BUS_CAP_EVENT;
   }
}

void bus_capture_tap(void *ctx, const bus_frame_t *frame, const uint8_t *payload,
                     uint32_t payload_len)
{
   bus_capture_t *c = ctx;
   if (c->broken)
      return;

   if (!c->have_first)
   {
      c->first_seq = frame->seq;
      c->have_first = 1;
   }
   if (!c->header_written)
      write_header(c);

   /* Whatever the tap was handed is materialized into the record: an inline
    * payload, or the arena span the host resolved for us pre-routing (bus_route's
    * pump reads a producer-held lease once and passes the bytes here). Either way
    * the record carries the payload verbatim, so replay reads it from the record
    * blob and never needs the (long-gone) lease. An event handed no bytes records
    * with payload_len 0 and the flag clear. */
   uint32_t plen = (payload && payload_len > 0) ? payload_len : 0;
   uint32_t record_len = REC_PREFIX + BUS_WIRE_HDR_LEN + plen + 4;
   if (!reserve(c, record_len))
      return;

   uint8_t *r = c->buf + c->len;
   put_u32(r + 0, record_len);
   put_u16(r + 4, (uint16_t)type_for_kind(frame->event_kind));
   put_u16(r + 6, plen ? BUS_CAP_F_MATERIALIZED : 0);
   /* The frame header, verbatim (the exact bytes bus_wire produces). */
   if (bus_wire_encode(frame, r + REC_PREFIX, BUS_WIRE_HDR_LEN) != BUS_WIRE_HDR_LEN)
   {
      /* A frame the encoder refuses should never reach the tap; abandon rather
       * than write a corrupt record. */
      c->broken = 1;
      return;
   }
   if (plen)
      memcpy(r + REC_PREFIX + BUS_WIRE_HDR_LEN, payload, plen);
   uint32_t crc = crc32c(r, record_len - 4);
   put_u32(r + record_len - 4, crc);

   c->len += record_len;
   c->last_seq = frame->seq;
}

/* ---- reading ---- */

const char *bus_capture_status_name(bus_capture_status_t s)
{
   switch (s)
   {
   case BUS_CAPTURE_COMPLETE:
      return "COMPLETE";
   case BUS_CAPTURE_OPEN:
      return "OPEN";
   case BUS_CAPTURE_TRUNCATED:
      return "TRUNCATED";
   case BUS_CAPTURE_CORRUPT:
      return "CORRUPT";
   default:
      return "UNKNOWN";
   }
}

static bus_capture_report_t done(bus_capture_status_t st, uint64_t recs, uint64_t last, size_t off,
                                 int rule)
{
   bus_capture_report_t r = {
       .status = st, .records = recs, .last_good_seq = last, .offending_off = off, .rule = rule};
   return r;
}

bus_capture_report_t bus_capture_read(const uint8_t *buf, size_t len, bus_capture_cb cb, void *ctx)
{
   const uint32_t rec_max = REC_PREFIX + BUS_WIRE_HDR_LEN + BUS_WIRE_MAX_PAYLOAD + 4;

   /* Rule 0: the file header. An unknown format_version is refused outright. */
   if (len < BUS_CAPTURE_HEADER_LEN || memcmp(buf, BUS_CAPTURE_MAGIC, 8) != 0)
      return done(BUS_CAPTURE_CORRUPT, 0, 0, 0, 0);
   if (get_u16(buf + 8) != BUS_CAPTURE_FORMAT_VERSION)
      return done(BUS_CAPTURE_CORRUPT, 0, 0, 0, 0);

   size_t p = get_u16(buf + 10); /* header_len; a later version may extend it */
   if (p < BUS_CAPTURE_HEADER_LEN || p > len)
      p = BUS_CAPTURE_HEADER_LEN;

   uint64_t records = 0, last_seq = 0, prev_seq = 0;
   int have_prev = 0, prev_was_epoch = 0;

   for (;;)
   {
      size_t remaining = len - p;
      if (remaining == 0)
         /* Rule 1: clean end. complete iff the last record was epoch_change. */
         return done(prev_was_epoch ? BUS_CAPTURE_COMPLETE : BUS_CAPTURE_OPEN, records, last_seq,
                     len, 1);
      if (remaining < 8)
         return done(BUS_CAPTURE_TRUNCATED, records, last_seq, p, 2); /* no length prefix */

      uint32_t record_len = get_u32(buf + p);
      if (record_len < REC_MIN)
         return done(BUS_CAPTURE_CORRUPT, records, last_seq, p, 3); /* malformed prefix */
      if (record_len > rec_max)
         return done(remaining <= rec_max ? BUS_CAPTURE_TRUNCATED : BUS_CAPTURE_CORRUPT, records,
                     last_seq, p, 3);
      if (remaining < record_len)
         return done(BUS_CAPTURE_TRUNCATED, records, last_seq, p, 4);

      uint32_t stored_crc = get_u32(buf + p + record_len - 4);
      if (crc32c(buf + p, record_len - 4) != stored_crc)
         /* Rule 5: a bad CRC at EOF is a torn final write; anywhere else is damage. */
         return done(remaining == record_len ? BUS_CAPTURE_TRUNCATED : BUS_CAPTURE_CORRUPT, records,
                     last_seq, p, 5);

      /* Rule 6: structural contradictions no interrupted write can produce. */
      uint16_t rtype = get_u16(buf + p + 4);
      uint16_t rflags = get_u16(buf + p + 6);
      bus_frame_t f;
      if (bus_wire_decode(buf + p + REC_PREFIX, BUS_WIRE_HDR_LEN, &f) != BUS_WIRE_OK)
         return done(BUS_CAPTURE_CORRUPT, records, last_seq, p, 6);
      uint32_t plen = record_len - REC_PREFIX - BUS_WIRE_HDR_LEN - 4;
      if (rflags & ~BUS_CAP_F_MATERIALIZED)
         return done(BUS_CAPTURE_CORRUPT, records, last_seq, p, 6);
      if (prev_was_epoch)
         return done(BUS_CAPTURE_CORRUPT, records, last_seq, p, 6); /* record after epoch_change */
      if (have_prev && f.seq != prev_seq + 1)
         return done(BUS_CAPTURE_CORRUPT, records, last_seq, p, 6); /* seq gap */

      if (cb)
      {
         bus_capture_event_t ev;
         memset(&ev, 0, sizeof ev);
         ev.type = (bus_capture_record_type_t)rtype;
         ev.frame = f;
         ev.payload_len = plen;
         ev.payload = plen ? (buf + p + REC_PREFIX + BUS_WIRE_HDR_LEN) : NULL;
         cb(ctx, &ev);
      }

      records++;
      last_seq = f.seq;
      prev_seq = f.seq;
      have_prev = 1;
      prev_was_epoch = (rtype == BUS_CAP_EPOCH_CHANGE);
      p += record_len;
   }
}
