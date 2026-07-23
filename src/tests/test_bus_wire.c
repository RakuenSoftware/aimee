/* test_bus_wire.c: slice 1 of the event-bus feature tree.
 *
 * Two jobs:
 *
 *   1. Exercise the framing contract in bus_wire.h directly — round trip for
 *      every message pattern, and rejection of every malformed frame the
 *      decoder is responsible for catching.
 *
 *   2. Hold the encoder to the committed golden vectors in
 *      src/tests/fixtures/bus/wire_vectors.tsv. Those bytes, not this file and
 *      not the spec prose, are the cross-language authority: the Go reference
 *      client (slice 9) is held to the same table. Run with --regen to rewrite
 *      it after a deliberate wire change; a drift shows up as a failing diff.
 *
 * The vector table is deliberately dumb to parse — tab-separated
 * name/expect/hex/fields — so a client in any language can consume it without
 * a JSON dependency it would otherwise not need.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_wire.h"

/* Set by the build so the test is cwd-independent; overridable via BUS_VECTORS
 * for the --regen path and for the Go conformance run in slice 9. */
#ifndef BUS_VECTOR_DIR
#define BUS_VECTOR_DIR "fixtures/bus"
#endif
#define VECTOR_PATH BUS_VECTOR_DIR "/wire_vectors.tsv"

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
   static const char *h = "0123456789abcdef";
   for (size_t i = 0; i < n; i++)
   {
      out[2 * i] = h[in[i] >> 4];
      out[2 * i + 1] = h[in[i] & 0x0f];
   }
   out[2 * n] = '\0';
}

static int hex_decode(const char *in, uint8_t *out, size_t outsz, size_t *n)
{
   size_t len = strlen(in);
   if (len % 2 != 0 || len / 2 > outsz)
      return 0;
   for (size_t i = 0; i < len; i += 2)
   {
      int hi = -1, lo = -1;
      for (int k = 0; k < 16; k++)
      {
         if ("0123456789abcdef"[k] == in[i])
            hi = k;
         if ("0123456789abcdef"[k] == in[i + 1])
            lo = k;
      }
      if (hi < 0 || lo < 0)
         return 0;
      out[i / 2] = (uint8_t)((hi << 4) | lo);
   }
   *n = len / 2;
   return 1;
}

/* ------------------------------------------------------------------ */
/* the canonical vector set                                            */

typedef struct
{
   const char *name;
   bus_frame_t frame;   /* used when expect == BUS_WIRE_OK */
   const char *raw_hex; /* used when expect != BUS_WIRE_OK; NULL means mutate a good frame */
   bus_wire_result_t expect;
} vector_t;

static bus_frame_t base_frame(void)
{
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = BUS_KIND_MODULE_BASE;
   f.principal_ref = 0x11223344u;
   f.src_handle = 7;
   f.dst_handle = 0;
   f.logical_ts = 0x0102030405060708ull;
   return f;
}

static bus_frame_t notification_empty(void)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_NOTIFICATION;
   return f;
}

static bus_frame_t notification_inline(uint32_t len)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_INLINE;
   f.payload_len = len;
   f.payload_ref = BUS_WIRE_HDR_LEN; /* in-slot, just past the header */
   return f;
}

static bus_frame_t notification_arena(uint32_t len)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_ARENA;
   f.payload_len = len;
   f.payload_ref = 0x0000000000040000ull;
   return f;
}

static bus_frame_t request_frame(void)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_REQUEST | BUS_F_ARENA;
   f.correlation_id = 0xdeadbeefcafef00dull;
   f.payload_len = 4096;
   f.payload_ref = 0x0000000000080000ull;
   return f;
}

static bus_frame_t reply_frame(void)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_REPLY | BUS_F_INLINE;
   f.correlation_id = 0xdeadbeefcafef00dull;
   f.payload_len = 16;
   f.payload_ref = BUS_WIRE_HDR_LEN;
   f.seq = 4242;
   f.dst_handle = 7;
   return f;
}

static bus_frame_t cancel_frame(void)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_CANCEL;
   f.correlation_id = 0xdeadbeefcafef00dull;
   return f;
}

static bus_frame_t capability_absent_frame(void)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_REPLY;
   f.event_kind = BUS_KIND_CAPABILITY_ABSENT;
   f.correlation_id = 0xdeadbeefcafef00dull;
   f.seq = 99;
   f.dst_handle = 7;
   return f;
}

/* An overflow notice is an ordinary seq-stamped frame carrying the control
 * flag (D6) — not a side channel with its own shape. */
static bus_frame_t overflow_frame(void)
{
   bus_frame_t f = base_frame();
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_CONTROL | BUS_F_INLINE;
   f.event_kind = BUS_KIND_OVERFLOW;
   f.payload_len = 16;
   f.payload_ref = BUS_WIRE_HDR_LEN;
   f.seq = 5000;
   f.dst_handle = 7;
   return f;
}

/* D4's independence proof: two frames identical but for the placement flag, at
 * the provisional 192-byte inline budget. The frame does not encode the
 * threshold, so re-tuning inline_budget in slice 12 cannot invalidate these
 * bytes. */
static bus_frame_t budget_inline(void)
{
   return notification_inline(192);
}

static bus_frame_t budget_arena(void)
{
   bus_frame_t f = notification_arena(192);
   f.logical_ts = 0x0102030405060708ull;
   return f;
}

static const vector_t *vectors(size_t *count)
{
   static vector_t v[16];
   size_t n = 0;

   v[n++] = (vector_t){"notification.empty", notification_empty(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"notification.inline.32", notification_inline(32), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"notification.arena.65536", notification_arena(65536), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"request.arena", request_frame(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"reply.inline", reply_frame(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"cancel", cancel_frame(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"capability_absent", capability_absent_frame(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"control.overflow", overflow_frame(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"budget.inline.192", budget_inline(), NULL, BUS_WIRE_OK};
   v[n++] = (vector_t){"budget.arena.192", budget_arena(), NULL, BUS_WIRE_OK};

   *count = n;
   return v;
}

/* ------------------------------------------------------------------ */
/* direct contract tests                                               */

static void check_roundtrip(const char *what, bus_frame_t in)
{
   uint8_t buf[BUS_WIRE_HDR_LEN];
   size_t n = bus_wire_encode(&in, buf, sizeof buf);
   if (n != BUS_WIRE_HDR_LEN)
   {
      fprintf(stderr, "encode failed: %s\n", what);
      abort();
   }

   bus_frame_t out;
   bus_wire_result_t r = bus_wire_decode(buf, n, &out);
   if (r != BUS_WIRE_OK)
   {
      fprintf(stderr, "decode failed: %s -> %s\n", what, bus_wire_result_name(r));
      abort();
   }
   if (memcmp(&in, &out, sizeof in) != 0)
   {
      fprintf(stderr, "round trip changed the frame: %s\n", what);
      abort();
   }
}

static void test_roundtrips(void)
{
   size_t n = 0;
   const vector_t *v = vectors(&n);
   for (size_t i = 0; i < n; i++)
      if (v[i].expect == BUS_WIRE_OK)
         check_roundtrip(v[i].name, v[i].frame);

   /* Maximum payload is accepted; one byte more is not. The bound has to be
    * exercised from both sides or an off-by-one lands in the vectors. */
   check_roundtrip("payload.max", notification_arena(BUS_WIRE_MAX_PAYLOAD));
   printf("  roundtrips: %zu frames + payload bound\n", n);
}

/* Encode a frame that bus_wire_encode would refuse, so the decoder can be
 * tested against bytes it must reject. Writing them by hand is the only way to
 * reach the decoder's own guards. */
static void encode_unchecked(const bus_frame_t *f, uint8_t *out)
{
   bus_frame_t ok = notification_empty();
   size_t n = bus_wire_encode(&ok, out, BUS_WIRE_HDR_LEN);
   assert(n == BUS_WIRE_HDR_LEN);
   /* Overwrite the fields the caller wants malformed. Offsets mirror bus_wire.c. */
   out[4] = (uint8_t)(f->hdr_flags & 0xff);
   out[5] = (uint8_t)((f->hdr_flags >> 8) & 0xff);
   out[6] = (uint8_t)(f->wire_version & 0xff);
   out[7] = (uint8_t)((f->wire_version >> 8) & 0xff);
   for (int i = 0; i < 8; i++)
      out[16 + i] = (uint8_t)((f->correlation_id >> (8 * i)) & 0xff);
   for (int i = 0; i < 8; i++)
      out[40 + i] = (uint8_t)((f->payload_ref >> (8 * i)) & 0xff);
   for (int i = 0; i < 4; i++)
      out[48 + i] = (uint8_t)((f->payload_len >> (8 * i)) & 0xff);
}

static void expect_reject(const char *what, const uint8_t *buf, size_t n,
                          bus_wire_result_t want)
{
   bus_frame_t out;
   memset(&out, 0xa5, sizeof out);
   bus_frame_t before = out;
   bus_wire_result_t r = bus_wire_decode(buf, n, &out);
   if (r != want)
   {
      fprintf(stderr, "%s: expected %s, got %s\n", what, bus_wire_result_name(want),
              bus_wire_result_name(r));
      abort();
   }
   if (memcmp(&before, &out, sizeof out) != 0)
   {
      fprintf(stderr, "%s: decoder wrote to *out on failure\n", what);
      abort();
   }
}

static void test_rejections(void)
{
   uint8_t buf[BUS_WIRE_HDR_LEN];
   bus_frame_t good = notification_empty();
   assert(bus_wire_encode(&good, buf, sizeof buf) == BUS_WIRE_HDR_LEN);

   /* Truncation, at every length short of a full header. */
   for (size_t n = 0; n < BUS_WIRE_HDR_LEN; n++)
      expect_reject("short", buf, n, BUS_WIRE_ERR_SHORT);

   uint8_t bad[BUS_WIRE_HDR_LEN];

   memcpy(bad, buf, sizeof bad);
   bad[0] ^= 0xff;
   expect_reject("magic", bad, sizeof bad, BUS_WIRE_ERR_MAGIC);

   memcpy(bad, buf, sizeof bad);
   bad[60] = 1;
   expect_reject("reserved", bad, sizeof bad, BUS_WIRE_ERR_RESERVED);

   bus_frame_t f;

   f = notification_empty();
   f.wire_version = BUS_WIRE_VERSION + 1;
   encode_unchecked(&f, bad);
   expect_reject("version", bad, sizeof bad, BUS_WIRE_ERR_VERSION);

   f = notification_empty();
   f.hdr_flags = BUS_F_NOTIFICATION | 0x8000u;
   encode_unchecked(&f, bad);
   expect_reject("unknown flag bit", bad, sizeof bad, BUS_WIRE_ERR_FLAGS);

   f = notification_empty();
   f.hdr_flags = 0;
   encode_unchecked(&f, bad);
   expect_reject("no pattern", bad, sizeof bad, BUS_WIRE_ERR_FLAGS);

   f = notification_empty();
   f.hdr_flags = BUS_F_REQUEST | BUS_F_REPLY;
   f.correlation_id = 1;
   encode_unchecked(&f, bad);
   expect_reject("two patterns", bad, sizeof bad, BUS_WIRE_ERR_FLAGS);

   f = notification_empty();
   f.hdr_flags = BUS_F_NOTIFICATION;
   f.payload_len = 32;
   encode_unchecked(&f, bad);
   expect_reject("payload without placement", bad, sizeof bad, BUS_WIRE_ERR_FLAGS);

   f = notification_empty();
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_INLINE | BUS_F_ARENA;
   f.payload_len = 32;
   encode_unchecked(&f, bad);
   expect_reject("both placements", bad, sizeof bad, BUS_WIRE_ERR_FLAGS);

   f = notification_empty();
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_INLINE;
   f.payload_len = 0;
   encode_unchecked(&f, bad);
   expect_reject("placement without payload", bad, sizeof bad, BUS_WIRE_ERR_FLAGS);

   f = notification_empty();
   f.hdr_flags = BUS_F_NOTIFICATION;
   f.payload_ref = 4096;
   encode_unchecked(&f, bad);
   expect_reject("ref without payload", bad, sizeof bad, BUS_WIRE_ERR_PAYLOAD_LEN);

   f = notification_empty();
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_ARENA;
   f.payload_len = BUS_WIRE_MAX_PAYLOAD + 1;
   encode_unchecked(&f, bad);
   expect_reject("oversize payload", bad, sizeof bad, BUS_WIRE_ERR_PAYLOAD_LEN);

   f = notification_empty();
   f.correlation_id = 5;
   encode_unchecked(&f, bad);
   expect_reject("notification with correlation", bad, sizeof bad, BUS_WIRE_ERR_CORRELATION);

   f = notification_empty();
   f.hdr_flags = BUS_F_REQUEST;
   f.correlation_id = 0;
   encode_unchecked(&f, bad);
   expect_reject("request without correlation", bad, sizeof bad, BUS_WIRE_ERR_CORRELATION);

   /* The encoder must refuse everything the decoder refuses, so a caller
    * cannot construct a frame this process would then reject. */
   f = notification_empty();
   f.hdr_flags = BUS_F_REQUEST | BUS_F_REPLY;
   f.correlation_id = 1;
   assert(bus_wire_encode(&f, buf, sizeof buf) == 0);
   assert(bus_wire_encode(&good, buf, BUS_WIRE_HDR_LEN - 1) == 0);

   printf("  rejections: 14 malformed shapes + %d truncations\n", BUS_WIRE_HDR_LEN);
}

/* ------------------------------------------------------------------ */
/* golden vectors                                                      */

static void frame_fields(const bus_frame_t *f, char *out, size_t outsz)
{
   snprintf(out, outsz,
            "flags=0x%04x;ver=%u;kind=%u;principal=%u;corr=0x%016llx;seq=%llu;"
            "lts=0x%016llx;pref=0x%016llx;plen=%u;src=%u;dst=%u",
            f->hdr_flags, f->wire_version, f->event_kind, f->principal_ref,
            (unsigned long long)f->correlation_id, (unsigned long long)f->seq,
            (unsigned long long)f->logical_ts, (unsigned long long)f->payload_ref,
            f->payload_len, f->src_handle, f->dst_handle);
}

static void write_vectors(const char *path)
{
   FILE *fp = fopen(path, "w");
   if (!fp)
   {
      fprintf(stderr, "cannot write %s\n", path);
      exit(1);
   }
   fprintf(fp, "# event-bus wire vectors v%d — generated by test_bus_wire --regen.\n",
           BUS_WIRE_VERSION);
   fprintf(fp, "# These bytes are the cross-language conformance authority (D8).\n");
   fprintf(fp, "# name\texpect\thex\tfields\n");

   size_t n = 0;
   const vector_t *v = vectors(&n);
   for (size_t i = 0; i < n; i++)
   {
      uint8_t buf[BUS_WIRE_HDR_LEN];
      char hex[2 * BUS_WIRE_HDR_LEN + 1];
      char fields[512];
      size_t got = bus_wire_encode(&v[i].frame, buf, sizeof buf);
      assert(got == BUS_WIRE_HDR_LEN);
      hex_encode(buf, got, hex);
      frame_fields(&v[i].frame, fields, sizeof fields);
      fprintf(fp, "%s\t%s\t%s\t%s\n", v[i].name, bus_wire_result_name(v[i].expect), hex, fields);
   }
   fclose(fp);
   printf("wrote %zu vectors to %s\n", n, path);
}

static void verify_vectors(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
   {
      fprintf(stderr,
              "cannot open %s — run the test from src/tests, or --regen to create it\n",
              path);
      exit(1);
   }

   size_t want_n = 0;
   const vector_t *want = vectors(&want_n);
   size_t seen = 0;
   char line[2048];

   while (fgets(line, sizeof line, fp))
   {
      if (line[0] == '#' || line[0] == '\n')
         continue;
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      char *name = strtok(line, "\t");
      char *expect = strtok(NULL, "\t");
      char *hex = strtok(NULL, "\t");
      char *fields = strtok(NULL, "\t");
      if (!name || !expect || !hex || !fields)
      {
         fprintf(stderr, "malformed vector line %zu\n", seen + 1);
         exit(1);
      }
      if (seen >= want_n || strcmp(name, want[seen].name) != 0)
      {
         fprintf(stderr, "vector %zu: file has '%s', code expects '%s'\n", seen, name,
                 seen < want_n ? want[seen].name : "(no more)");
         exit(1);
      }

      /* The committed bytes must be exactly what the encoder produces now. */
      uint8_t enc[BUS_WIRE_HDR_LEN];
      char enc_hex[2 * BUS_WIRE_HDR_LEN + 1];
      size_t got = bus_wire_encode(&want[seen].frame, enc, sizeof enc);
      assert(got == BUS_WIRE_HDR_LEN);
      hex_encode(enc, got, enc_hex);
      if (strcmp(enc_hex, hex) != 0)
      {
         fprintf(stderr, "vector '%s' drifted:\n  committed %s\n  encoded   %s\n", name, hex,
                 enc_hex);
         exit(1);
      }

      /* And the committed bytes must decode back to the committed fields, so
       * the table is self-checking rather than just a copy of the encoder. */
      uint8_t raw[BUS_WIRE_HDR_LEN];
      size_t rawn = 0;
      if (!hex_decode(hex, raw, sizeof raw, &rawn) || rawn != BUS_WIRE_HDR_LEN)
      {
         fprintf(stderr, "vector '%s': bad hex\n", name);
         exit(1);
      }
      bus_frame_t dec;
      bus_wire_result_t r = bus_wire_decode(raw, rawn, &dec);
      if (strcmp(bus_wire_result_name(r), expect) != 0)
      {
         fprintf(stderr, "vector '%s': decode gave %s, committed %s\n", name,
                 bus_wire_result_name(r), expect);
         exit(1);
      }
      if (r == BUS_WIRE_OK)
      {
         char dec_fields[512];
         frame_fields(&dec, dec_fields, sizeof dec_fields);
         if (strcmp(dec_fields, fields) != 0)
         {
            fprintf(stderr, "vector '%s' fields drifted:\n  committed %s\n  decoded   %s\n",
                    name, fields, dec_fields);
            exit(1);
         }
      }
      seen++;
   }
   fclose(fp);

   if (seen != want_n)
   {
      fprintf(stderr, "vector count: file has %zu, code expects %zu\n", seen, want_n);
      exit(1);
   }
   printf("  vectors: %zu byte-exact against %s\n", seen, path);
}

/* D4: the frame carries the placement, never the threshold that chose it. Two
 * frames at the same payload_len differ only in the placement flag, so
 * re-tuning inline_budget in slice 12 cannot invalidate a committed vector. */
static void test_budget_independence(void)
{
   uint8_t a[BUS_WIRE_HDR_LEN], b[BUS_WIRE_HDR_LEN];
   bus_frame_t fi = budget_inline();
   bus_frame_t fa = budget_arena();
   assert(fi.payload_len == fa.payload_len);
   assert(bus_wire_encode(&fi, a, sizeof a) == BUS_WIRE_HDR_LEN);
   assert(bus_wire_encode(&fa, b, sizeof b) == BUS_WIRE_HDR_LEN);

   int differing = 0;
   for (size_t i = 0; i < sizeof a; i++)
      if (a[i] != b[i])
         differing++;

   /* Only hdr_flags (2 bytes, one differing) and payload_ref differ. Nothing
    * anywhere in the frame encodes the budget itself. */
   assert((a[4] ^ b[4]) == (BUS_F_INLINE ^ BUS_F_ARENA));
   assert(differing > 0);
   for (size_t i = 0; i < sizeof a; i++)
   {
      int is_flags = (i == 4 || i == 5);
      int is_ref = (i >= 40 && i < 48);
      if (!is_flags && !is_ref)
         assert(a[i] == b[i]);
   }
   printf("  budget independence: frame encodes placement, not threshold\n");
}

int main(int argc, char **argv)
{
   const char *path = getenv("BUS_VECTORS") ? getenv("BUS_VECTORS") : VECTOR_PATH;

   if (argc > 1 && strcmp(argv[1], "--regen") == 0)
   {
      write_vectors(path);
      return 0;
   }

   printf("test_bus_wire:\n");
   test_roundtrips();
   test_rejections();
   test_budget_independence();
   verify_vectors(path);
   printf("test_bus_wire: OK\n");
   return 0;
}
