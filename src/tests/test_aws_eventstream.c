/* test_aws_eventstream.c: P6b AWS eventstream framing decoder tests (pure/offline).
 *
 * Memory-safety-critical: this decoder parses attacker-influenced binary length
 * fields, so the headline gates are (d) the bounds/ERROR matrix, (h) the
 * rolling-buffer resume path, and (g) the deterministic FUZZ SWEEP that asserts
 * aws_es_decode ALWAYS returns exactly one of OK/NEED_MORE/ERROR and never
 * over-reads / crashes / loops. Build under -fsanitize=address,undefined for the
 * strongest signal; the bounded-return assertions are the CI knob otherwise.
 *
 * Every frame is byte-exact: the builder computes the REAL prelude + message
 * CRCs with aws_es_crc32, so tests exercise the true validation path. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/aws/aws_eventstream.h"

/* ============================ frame builder ============================ */

typedef struct
{
   uint8_t buf[65536];
   size_t len;
} bytebuf_t;

static void bb_reset(bytebuf_t *b)
{
   b->len = 0;
}

static void bb_bytes(bytebuf_t *b, const void *p, size_t n)
{
   assert(b->len + n <= sizeof(b->buf));
   memcpy(b->buf + b->len, p, n);
   b->len += n;
}

static void bb_u8(bytebuf_t *b, uint8_t v)
{
   bb_bytes(b, &v, 1);
}

static void bb_u16be(bytebuf_t *b, uint16_t v)
{
   uint8_t t[2] = {(uint8_t)(v >> 8), (uint8_t)v};
   bb_bytes(b, t, 2);
}

static void bb_u32be(bytebuf_t *b, uint32_t v)
{
   uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
   bb_bytes(b, t, 4);
}

static void bb_u64be(bytebuf_t *b, uint64_t v)
{
   uint8_t t[8];
   for (int i = 0; i < 8; i++)
      t[i] = (uint8_t)(v >> (56 - 8 * i));
   bb_bytes(b, t, 8);
}

/* --- header appenders (into a header-block buffer) --- */

static void hdr_name(bytebuf_t *h, const char *name)
{
   size_t n = strlen(name);
   assert(n <= 255);
   bb_u8(h, (uint8_t)n);
   bb_bytes(h, name, n);
}

static void hdr_string(bytebuf_t *h, const char *name, const char *val)
{
   hdr_name(h, name);
   bb_u8(h, AWS_ES_HDR_STRING);
   size_t n = strlen(val);
   assert(n <= 0xFFFF);
   bb_u16be(h, (uint16_t)n);
   bb_bytes(h, val, n);
}

static void hdr_blob(bytebuf_t *h, const char *name, const void *val, size_t n)
{
   hdr_name(h, name);
   bb_u8(h, AWS_ES_HDR_BYTES);
   assert(n <= 0xFFFF);
   bb_u16be(h, (uint16_t)n);
   bb_bytes(h, val, n);
}

static void hdr_int32(bytebuf_t *h, const char *name, int32_t v)
{
   hdr_name(h, name);
   bb_u8(h, AWS_ES_HDR_INT32);
   bb_u32be(h, (uint32_t)v);
}

static void hdr_int64(bytebuf_t *h, const char *name, int64_t v)
{
   hdr_name(h, name);
   bb_u8(h, AWS_ES_HDR_INT64);
   bb_u64be(h, (uint64_t)v);
}

static void hdr_bool(bytebuf_t *h, const char *name, int v)
{
   hdr_name(h, name);
   bb_u8(h, v ? AWS_ES_HDR_BOOL_TRUE : AWS_ES_HDR_BOOL_FALSE);
}

/* Assemble a full message from a header block + payload, computing the real
 * prelude_crc and message_crc. */
static void build_message(bytebuf_t *out, const bytebuf_t *hdrs, const void *payload,
                          size_t payload_len)
{
   bb_reset(out);
   uint32_t total = (uint32_t)(12 + hdrs->len + payload_len + 4);
   bb_u32be(out, total);
   bb_u32be(out, (uint32_t)hdrs->len);
   uint32_t prelude_crc = aws_es_crc32(out->buf, 8);
   bb_u32be(out, prelude_crc);
   if (hdrs->len)
      bb_bytes(out, hdrs->buf, hdrs->len);
   if (payload_len)
      bb_bytes(out, payload, payload_len);
   uint32_t msg_crc = aws_es_crc32(out->buf, out->len);
   bb_u32be(out, msg_crc);
   assert(out->len == total);
}

/* Build the canonical "event" message used across the tests. */
static void build_event(bytebuf_t *msg)
{
   bytebuf_t h;
   bb_reset(&h);
   hdr_string(&h, ":message-type", "event");
   hdr_string(&h, ":event-type", "contentBlockDelta");
   hdr_string(&h, ":content-type", "application/json");
   hdr_string(&h, "custom", "hello");
   hdr_int32(&h, "seq", 305419896); /* 0x12345678 */
   const char *payload = "{\"delta\":\"hi\"}";
   build_message(msg, &h, payload, strlen(payload));
}

static int view_eq(const aws_es_view_t *v, const char *s)
{
   size_t n = strlen(s);
   return v->len == n && v->ptr && memcmp(v->ptr, s, n) == 0;
}

/* ============================ (a) CRC32 vectors ============================ */

static void test_crc32(void)
{
   assert(aws_es_crc32((const uint8_t *)"", 0) == 0);
   assert(aws_es_crc32(NULL, 0) == 0);
   assert(aws_es_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
   printf("  crc32: empty->0, \"123456789\"->0xCBF43926\n");
}

/* ==================== (b) valid message decode ==================== */

static void test_valid_decode(void)
{
   bytebuf_t msg;
   build_event(&msg);

   aws_es_message_t m;
   size_t consumed = 12345;
   aws_es_status_t st = aws_es_decode(msg.buf, msg.len, &m, &consumed);
   assert(st == AWS_ES_OK);
   assert(consumed == msg.len);

   assert(m.n_headers == 5);
   assert(m.headers_truncated == 0);
   assert(m.msg_type == AWS_ES_MSG_EVENT);

   assert(view_eq(&m.headers[0].name, ":message-type"));
   assert(view_eq(&m.headers[0].value, "event"));
   assert(view_eq(&m.headers[1].name, ":event-type"));
   assert(view_eq(&m.headers[1].value, "contentBlockDelta"));
   assert(view_eq(&m.headers[3].name, "custom"));
   assert(view_eq(&m.headers[3].value, "hello"));

   /* int32 header decoded to HOST order. */
   assert(view_eq(&m.headers[4].name, "seq"));
   assert(m.headers[4].value_type == AWS_ES_HDR_INT32);
   assert(m.headers[4].int_value == 305419896);

   /* convenience views. */
   assert(m.message_type && view_eq(&m.message_type->value, "event"));
   assert(m.event_type && view_eq(&m.event_type->value, "contentBlockDelta"));
   assert(m.content_type && view_eq(&m.content_type->value, "application/json"));
   assert(m.exception_type == NULL);
   assert(m.error_code == NULL);

   /* payload view. */
   const char *pw = "{\"delta\":\"hi\"}";
   assert(m.payload.len == strlen(pw));
   assert(m.payload.ptr && memcmp(m.payload.ptr, pw, m.payload.len) == 0);
   /* zero-copy: payload points INTO the caller buffer. */
   assert(m.payload.ptr >= msg.buf && m.payload.ptr < msg.buf + msg.len);
   printf("  valid decode: 5 headers, int32 host-order, payload view, EVENT, consumed==total\n");
}

/* ==================== (c) NEED_MORE truncation ==================== */

static void test_need_more(void)
{
   bytebuf_t msg;
   build_event(&msg);
   aws_es_message_t m;
   size_t consumed;

   /* truncated prelude (< 12 bytes). */
   for (size_t l = 0; l < 12; l++)
   {
      consumed = 999;
      assert(aws_es_decode(msg.buf, l, &m, &consumed) == AWS_ES_NEED_MORE);
      assert(consumed == 0);
   }

   /* prelude present but body short (12 <= len < total). */
   for (size_t l = 12; l < msg.len; l++)
   {
      consumed = 999;
      assert(aws_es_decode(msg.buf, l, &m, &consumed) == AWS_ES_NEED_MORE);
      assert(consumed == 0);
   }
   printf("  need-more: truncated prelude + truncated body, consumed==0\n");
}

/* ==================== (d) ERROR matrix ==================== */

static void expect_error(const uint8_t *buf, size_t len, const char *why)
{
   aws_es_message_t m;
   size_t consumed = 777;
   aws_es_status_t st = aws_es_decode(buf, len, &m, &consumed);
   if (st != AWS_ES_ERROR || consumed != 0)
   {
      fprintf(stderr, "  expected ERROR (consumed=0) for [%s], got st=%s consumed=%zu\n", why,
              aws_es_status_str(st), consumed);
      assert(0);
   }
}

static void test_error_matrix(void)
{
   bytebuf_t base;
   build_event(&base);

   /* corrupted prelude_crc (byte 8..11). */
   {
      bytebuf_t m = base;
      m.buf[8] ^= 0xFF;
      expect_error(m.buf, m.len, "bad prelude_crc");
   }
   /* corrupted message_crc (last 4 bytes). */
   {
      bytebuf_t m = base;
      m.buf[m.len - 1] ^= 0xFF;
      expect_error(m.buf, m.len, "bad message_crc");
   }
   /* corrupted payload byte -> message_crc mismatch. */
   {
      bytebuf_t m = base;
      m.buf[m.len - 6] ^= 0xFF;
      expect_error(m.buf, m.len, "corrupt payload");
   }
   /* total_length < 16: hand-build a 12-byte prelude with total=15 + valid crc. */
   {
      bytebuf_t m;
      bb_reset(&m);
      bb_u32be(&m, 15);
      bb_u32be(&m, 0);
      bb_u32be(&m, aws_es_crc32(m.buf, 8));
      /* pad so len >= total is not the reason; total<16 must be rejected first. */
      while (m.len < 15)
         bb_u8(&m, 0);
      expect_error(m.buf, m.len, "total_length < 16");
   }
   /* total_length > AWS_ES_MAX_MESSAGE. */
   {
      bytebuf_t m;
      bb_reset(&m);
      bb_u32be(&m, (uint32_t)AWS_ES_MAX_MESSAGE + 1);
      bb_u32be(&m, 0);
      bb_u32be(&m, aws_es_crc32(m.buf, 8));
      expect_error(m.buf, m.len, "total_length > MAX");
   }
   /* headers_length > total_length - 16 (payload underflow guard). Build a
    * frame whose headers_length field exceeds the room, with valid CRCs. */
   {
      bytebuf_t h;
      bb_reset(&h);
      hdr_string(&h, ":message-type", "event");
      bytebuf_t m;
      build_message(&m, &h, "", 0);
      /* overwrite headers_length to total (too big) and re-fix both CRCs so the
       * ONLY defect is the oversized headers_length. */
      uint32_t total = (uint32_t)m.len;
      m.buf[4] = (uint8_t)(total >> 24);
      m.buf[5] = (uint8_t)(total >> 16);
      m.buf[6] = (uint8_t)(total >> 8);
      m.buf[7] = (uint8_t)total;
      uint32_t pc = aws_es_crc32(m.buf, 8);
      m.buf[8] = (uint8_t)(pc >> 24);
      m.buf[9] = (uint8_t)(pc >> 16);
      m.buf[10] = (uint8_t)(pc >> 8);
      m.buf[11] = (uint8_t)pc;
      uint32_t mc = aws_es_crc32(m.buf, m.len - 4);
      m.buf[m.len - 4] = (uint8_t)(mc >> 24);
      m.buf[m.len - 3] = (uint8_t)(mc >> 16);
      m.buf[m.len - 2] = (uint8_t)(mc >> 8);
      m.buf[m.len - 1] = (uint8_t)mc;
      expect_error(m.buf, m.len, "headers_length > total-16");
   }
   /* header name_len overruns the header block. */
   {
      bytebuf_t h;
      bb_reset(&h);
      bb_u8(&h, 200); /* name_len says 200 but block is tiny */
      bb_bytes(&h, "abc", 3);
      bytebuf_t m;
      build_message(&m, &h, "", 0);
      expect_error(m.buf, m.len, "name_len overrun");
   }
   /* string value u16 length overruns the header block. */
   {
      bytebuf_t h;
      bb_reset(&h);
      hdr_name(&h, "k");
      bb_u8(&h, AWS_ES_HDR_STRING);
      bb_u16be(&h, 5000); /* claims 5000 bytes; none follow */
      bb_bytes(&h, "ab", 2);
      bytebuf_t m;
      build_message(&m, &h, "", 0);
      expect_error(m.buf, m.len, "string len overrun");
   }
   /* blob value u16 length overruns. */
   {
      bytebuf_t h;
      bb_reset(&h);
      hdr_name(&h, "k");
      bb_u8(&h, AWS_ES_HDR_BYTES);
      bb_u16be(&h, 5000);
      bytebuf_t m;
      build_message(&m, &h, "", 0);
      expect_error(m.buf, m.len, "blob len overrun");
   }
   /* value_type == 10 (out of [0,9]). */
   {
      bytebuf_t h;
      bb_reset(&h);
      hdr_name(&h, "k");
      bb_u8(&h, 10);
      bytebuf_t m;
      build_message(&m, &h, "", 0);
      expect_error(m.buf, m.len, "value_type==10");
   }
   printf("  error matrix: crc/underflow/overrun/oversize/bad-type all -> ERROR, consumed==0\n");
}

/* ==================== (e) exception + error frames ==================== */

static void test_exception_error_frames(void)
{
   /* exception frame. */
   {
      bytebuf_t h;
      bb_reset(&h);
      hdr_string(&h, ":message-type", "exception");
      hdr_string(&h, ":exception-type", "ThrottlingException");
      const char *body = "{\"message\":\"slow down\"}";
      bytebuf_t msg;
      build_message(&msg, &h, body, strlen(body));

      aws_es_message_t m;
      size_t consumed = 0;
      assert(aws_es_decode(msg.buf, msg.len, &m, &consumed) == AWS_ES_OK);
      assert(m.msg_type == AWS_ES_MSG_EXCEPTION);
      assert(m.exception_type && view_eq(&m.exception_type->value, "ThrottlingException"));
      assert(m.payload.len == strlen(body));
      assert(m.payload.ptr && memcmp(m.payload.ptr, body, m.payload.len) == 0);
   }
   /* error frame. */
   {
      bytebuf_t h;
      bb_reset(&h);
      hdr_string(&h, ":message-type", "error");
      hdr_string(&h, ":error-code", "InternalFailure");
      hdr_string(&h, ":error-message", "boom");
      bytebuf_t msg;
      build_message(&msg, &h, "", 0);

      aws_es_message_t m;
      size_t consumed = 0;
      assert(aws_es_decode(msg.buf, msg.len, &m, &consumed) == AWS_ES_OK);
      assert(m.msg_type == AWS_ES_MSG_ERROR);
      assert(m.error_code && view_eq(&m.error_code->value, "InternalFailure"));
      assert(m.error_message && view_eq(&m.error_message->value, "boom"));
      assert(m.payload.len == 0);
   }
   printf("  exception/error frames: classified, exception/error header views non-NULL\n");
}

/* ==================== (f) two concatenated messages ==================== */

static void test_concatenated(void)
{
   bytebuf_t a, b;
   build_event(&a);

   bytebuf_t h;
   bb_reset(&h);
   hdr_string(&h, ":message-type", "event");
   hdr_string(&h, ":event-type", "messageStop");
   build_message(&b, &h, "END", 3);

   uint8_t stream[65536];
   size_t off = 0;
   memcpy(stream + off, a.buf, a.len);
   off += a.len;
   memcpy(stream + off, b.buf, b.len);
   off += b.len;

   aws_es_message_t m;
   size_t consumed = 0;
   size_t pos = 0;
   assert(aws_es_decode(stream + pos, off - pos, &m, &consumed) == AWS_ES_OK);
   assert(consumed == a.len);
   assert(view_eq(&m.event_type->value, "contentBlockDelta"));
   pos += consumed;

   assert(aws_es_decode(stream + pos, off - pos, &m, &consumed) == AWS_ES_OK);
   assert(consumed == b.len);
   assert(view_eq(&m.event_type->value, "messageStop"));
   assert(m.payload.len == 3 && memcmp(m.payload.ptr, "END", 3) == 0);
   pos += consumed;
   assert(pos == off);
   printf("  concatenated: two messages decode sequentially, consumed counts exact\n");
}

/* ==================== (h) rolling buffer resume ==================== */

static void test_rolling_buffer(void)
{
   bytebuf_t a, b;
   build_event(&a);

   bytebuf_t h;
   bb_reset(&h);
   hdr_string(&h, ":message-type", "event");
   hdr_string(&h, ":event-type", "messageStop");
   build_message(&b, &h, "tail", 4);

   /* rolling buffer = complete A + PARTIAL B (prelude present, body short). */
   uint8_t roll[65536];
   size_t rlen = 0;
   memcpy(roll + rlen, a.buf, a.len);
   rlen += a.len;
   size_t b_partial = 12 + 3; /* prelude + a few body bytes */
   assert(b_partial < b.len);
   memcpy(roll + rlen, b.buf, b_partial);
   rlen += b_partial;

   aws_es_message_t m;
   size_t consumed = 0;

   /* first decode: A OK, consumed == A.total. */
   assert(aws_es_decode(roll, rlen, &m, &consumed) == AWS_ES_OK);
   assert(consumed == a.len);
   size_t pos = consumed;

   /* remainder = partial B only -> NEED_MORE, consumed == 0. */
   assert(aws_es_decode(roll + pos, rlen - pos, &m, &consumed) == AWS_ES_NEED_MORE);
   assert(consumed == 0);

   /* append the rest of B, then it decodes OK. */
   memcpy(roll + rlen, b.buf + b_partial, b.len - b_partial);
   rlen += b.len - b_partial;
   assert(aws_es_decode(roll + pos, rlen - pos, &m, &consumed) == AWS_ES_OK);
   assert(consumed == b.len);
   assert(view_eq(&m.event_type->value, "messageStop"));
   assert(m.payload.len == 4 && memcmp(m.payload.ptr, "tail", 4) == 0);
   printf("  rolling buffer: A OK -> partial B NEED_MORE -> completed B OK\n");
}

/* ==================== (j) BE->host integer swap ==================== */

static void test_integer_byteswap(void)
{
   bytebuf_t h;
   bb_reset(&h);
   hdr_string(&h, ":message-type", "event");
   hdr_int32(&h, "i32", (int32_t)0x01020304);
   hdr_int32(&h, "neg", -2);
   hdr_int64(&h, "i64", (int64_t)0x0102030405060708LL);
   hdr_int64(&h, "negl", -1);
   bytebuf_t msg;
   build_message(&msg, &h, "", 0);

   aws_es_message_t m;
   size_t consumed = 0;
   assert(aws_es_decode(msg.buf, msg.len, &m, &consumed) == AWS_ES_OK);
   assert(m.headers[1].int_value == 0x01020304);
   assert(m.headers[2].int_value == -2);
   assert(m.headers[3].int_value == 0x0102030405060708LL);
   assert(m.headers[4].int_value == -1);
   printf("  integer byteswap: int32/int64 BE->host order, signedness preserved\n");
}

/* ==================== header cap + other value types ==================== */

static void test_header_cap_and_types(void)
{
   bytebuf_t h;
   bb_reset(&h);
   hdr_string(&h, ":message-type", "event");
   /* add > AWS_ES_MAX_HEADERS headers total. */
   char name[16];
   for (int i = 0; i < AWS_ES_MAX_HEADERS + 8; i++)
   {
      snprintf(name, sizeof(name), "h%d", i);
      hdr_bool(&h, name, i & 1);
   }
   bytebuf_t msg;
   build_message(&msg, &h, "", 0);

   aws_es_message_t m;
   size_t consumed = 0;
   assert(aws_es_decode(msg.buf, msg.len, &m, &consumed) == AWS_ES_OK);
   assert(m.n_headers == AWS_ES_MAX_HEADERS);
   assert(m.headers_truncated == 1);
   assert(m.msg_type == AWS_ES_MSG_EVENT); /* still classified via captured [0] */
   printf("  header cap: > MAX headers -> decode OK, n==MAX, headers_truncated set\n");
}

/* ==================== all value types round-trip ==================== */

static void test_all_value_types(void)
{
   bytebuf_t h;
   bb_reset(&h);
   hdr_string(&h, ":message-type", "event");
   hdr_bool(&h, "bt", 1);
   hdr_bool(&h, "bf", 0);
   hdr_name(&h, "i8");
   bb_u8(&h, AWS_ES_HDR_INT8);
   bb_u8(&h, (uint8_t)(int8_t)-5);
   hdr_name(&h, "i16");
   bb_u8(&h, AWS_ES_HDR_INT16);
   bb_u16be(&h, (uint16_t)(int16_t)-300);
   hdr_name(&h, "ts");
   bb_u8(&h, AWS_ES_HDR_TIMESTAMP);
   bb_u64be(&h, 1700000000000ULL);
   uint8_t uuid[16];
   for (int i = 0; i < 16; i++)
      uuid[i] = (uint8_t)(i + 1);
   hdr_name(&h, "id");
   bb_u8(&h, AWS_ES_HDR_UUID);
   bb_bytes(&h, uuid, 16);
   const uint8_t blob[3] = {0xDE, 0xAD, 0xBE};
   hdr_blob(&h, "raw", blob, sizeof(blob));
   bytebuf_t msg;
   build_message(&msg, &h, "", 0);

   aws_es_message_t m;
   size_t consumed = 0;
   assert(aws_es_decode(msg.buf, msg.len, &m, &consumed) == AWS_ES_OK);
   assert(m.headers[1].value_type == AWS_ES_HDR_BOOL_TRUE && m.headers[1].int_value == 1);
   assert(m.headers[2].value_type == AWS_ES_HDR_BOOL_FALSE && m.headers[2].int_value == 0);
   assert(m.headers[3].value_type == AWS_ES_HDR_INT8 && m.headers[3].int_value == -5);
   assert(m.headers[4].value_type == AWS_ES_HDR_INT16 && m.headers[4].int_value == -300);
   assert(m.headers[5].value_type == AWS_ES_HDR_TIMESTAMP &&
          m.headers[5].int_value == 1700000000000LL);
   assert(m.headers[6].value_type == AWS_ES_HDR_UUID && m.headers[6].value.len == 16 &&
          memcmp(m.headers[6].value.ptr, uuid, 16) == 0);
   assert(m.headers[7].value_type == AWS_ES_HDR_BYTES && m.headers[7].value.len == 3 &&
          memcmp(m.headers[7].value.ptr, blob, 3) == 0);
   printf("  all value types: bool/int8/int16/timestamp/uuid/blob decoded correctly\n");
}

/* ==================== (g) deterministic fuzz sweep ==================== */

static void test_fuzz_sweep(void)
{
   bytebuf_t base;
   build_event(&base);
   const uint8_t masks[] = {0x01, 0x80, 0xFF, 0x7F, 0x20};
   unsigned long calls = 0;

   uint8_t work[65536];

   /* (1) single-byte XOR mutations at every offset, at full length. */
   for (size_t off = 0; off < base.len; off++)
   {
      for (size_t mi = 0; mi < sizeof(masks); mi++)
      {
         memcpy(work, base.buf, base.len);
         work[off] ^= masks[mi];
         aws_es_message_t m;
         size_t consumed = 12345;
         aws_es_status_t st = aws_es_decode(work, base.len, &m, &consumed);
         assert(st == AWS_ES_OK || st == AWS_ES_NEED_MORE || st == AWS_ES_ERROR);
         if (st != AWS_ES_OK)
            assert(consumed == 0);
         calls++;
      }
   }

   /* (2) truncate at every prefix length (0..len). */
   for (size_t l = 0; l <= base.len; l++)
   {
      memcpy(work, base.buf, l);
      aws_es_message_t m;
      size_t consumed = 12345;
      aws_es_status_t st = aws_es_decode(work, l, &m, &consumed);
      assert(st == AWS_ES_OK || st == AWS_ES_NEED_MORE || st == AWS_ES_ERROR);
      if (st != AWS_ES_OK)
         assert(consumed == 0);
      calls++;
   }

   /* (3) mutate each length-field byte (offsets 0..7) through many values, at a
    * few buffered lengths (short + full) to exercise NEED_MORE vs ERROR paths. */
   for (size_t off = 0; off < 8; off++)
   {
      for (unsigned v = 0; v < 256; v++)
      {
         memcpy(work, base.buf, base.len);
         work[off] = (uint8_t)v;
         for (size_t li = 0; li < 2; li++)
         {
            size_t l = li ? base.len : 20;
            aws_es_message_t m;
            size_t consumed = 12345;
            aws_es_status_t st = aws_es_decode(work, l, &m, &consumed);
            assert(st == AWS_ES_OK || st == AWS_ES_NEED_MORE || st == AWS_ES_ERROR);
            if (st != AWS_ES_OK)
               assert(consumed == 0);
            calls++;
         }
      }
   }

   /* (4) sweep the headers_length field to arbitrary values with valid-looking
    * total, forcing the underflow guard + header-overrun paths. */
   for (unsigned hl = 0; hl < 70000; hl += 137)
   {
      memcpy(work, base.buf, base.len);
      work[4] = (uint8_t)(hl >> 24);
      work[5] = (uint8_t)(hl >> 16);
      work[6] = (uint8_t)(hl >> 8);
      work[7] = (uint8_t)hl;
      aws_es_message_t m;
      size_t consumed = 12345;
      aws_es_status_t st = aws_es_decode(work, base.len, &m, &consumed);
      assert(st == AWS_ES_OK || st == AWS_ES_NEED_MORE || st == AWS_ES_ERROR);
      if (st != AWS_ES_OK)
         assert(consumed == 0);
      calls++;
   }

   printf("  fuzz sweep: %lu decodes, all bounded (OK/NEED_MORE/ERROR), no crash/OOB\n", calls);
}

int main(void)
{
   printf("test_aws_eventstream:\n");
   test_crc32();
   test_valid_decode();
   test_need_more();
   test_error_matrix();
   test_exception_error_frames();
   test_concatenated();
   test_rolling_buffer();
   test_integer_byteswap();
   test_all_value_types();
   test_header_cap_and_types();
   test_fuzz_sweep();
   printf("test_aws_eventstream: all passed\n");
   return 0;
}
