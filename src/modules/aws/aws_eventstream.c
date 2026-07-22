/* modules/aws/aws_eventstream.c: AWS eventstream framing decoder. See
 * aws_eventstream.h.
 *
 * Pure transform: raw bytes -> a typed, zero-copy message view. Parses
 * attacker-influenced binary length fields, so EVERY length is validated
 * against the remaining bounds BEFORE any read, all arithmetic is done on
 * widened unsigned types, and the payload length is only computed after the
 * underflow guard. No I/O, no clock, no global mutable state. */

#include "aws_eventstream.h"

#include <string.h>

/* --- self-contained IEEE 802.3 CRC32 (reflected, poly 0xEDB88320) --- */

/* Precomputed table (static const => no lazy-init data race; thread-safe for
 * the concurrent stream consumer). Verified: crc("")==0, crc("123456789")==
 * 0xCBF43926. */
static const uint32_t k_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u,
    0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu,
    0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du,
    0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u, 0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu,
    0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u,
    0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u, 0x26D930ACu,
    0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu,
    0xB6662D3Du, 0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu,
    0x9FBFE4A5u, 0xE8B8D433u, 0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu,
    0x086D3D2Du, 0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu,
    0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u, 0x4DB26158u, 0x3AB551CEu,
    0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au,
    0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u,
    0xCE61E49Fu, 0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u,
    0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
    0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u, 0xF00F9344u, 0x8708A3D2u, 0x1E01F268u,
    0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u,
    0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u,
    0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu,
    0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u,
    0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u,
    0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du, 0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au,
    0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu,
    0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u, 0x88085AE6u,
    0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du,
    0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u,
    0x47B2CF7Fu, 0x30B5FFE9u, 0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u,
    0xCDD70693u, 0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du};

uint32_t aws_es_crc32(const uint8_t *buf, size_t len)
{
   uint32_t crc = 0xFFFFFFFFu;
   if (buf)
   {
      for (size_t i = 0; i < len; i++)
         crc = k_crc32_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
   }
   return crc ^ 0xFFFFFFFFu;
}

/* --- big-endian readers (bounds are the CALLER's responsibility) --- */

static uint32_t rd_u32be(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_u16be(const uint8_t *p)
{
   return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint64_t rd_u64be(const uint8_t *p)
{
   uint64_t v = 0;
   for (int i = 0; i < 8; i++)
      v = (v << 8) | (uint64_t)p[i];
   return v;
}

/* Compare a (non-NUL-terminated) name view to a C string literal. */
static int name_eq(const aws_es_view_t *v, const char *s)
{
   size_t n = strlen(s);
   return v->len == n && v->ptr && memcmp(v->ptr, s, n) == 0;
}

/* --- header parsing ---
 *
 * Parse the header block [start, end). On success cursor lands exactly on end
 * (every read is bounds-checked against the remaining span, so it can never
 * overshoot). Captures up to AWS_ES_MAX_HEADERS headers into `out`, flagging
 * headers_truncated for the excess (still bounds-validated). Returns 0 on
 * success, -1 on any overrun / bad value-type. */
static int parse_headers(const uint8_t *start, const uint8_t *end, aws_es_message_t *out)
{
   const uint8_t *cur = start;
   while (cur < end)
   {
      size_t rem = (size_t)(end - cur);

      /* name_len (u8) + name. */
      if (rem < 1)
         return -1;
      uint8_t name_len = cur[0];
      cur += 1;
      rem = (size_t)(end - cur);
      if (rem < name_len)
         return -1;
      const uint8_t *name_ptr = cur;
      cur += name_len;
      rem = (size_t)(end - cur);

      /* value_type (u8). */
      if (rem < 1)
         return -1;
      uint8_t value_type = cur[0];
      cur += 1;
      rem = (size_t)(end - cur);

      /* Decode the value; validate its size against `rem` BEFORE reading. */
      int64_t int_value = 0;
      const uint8_t *val_ptr = NULL;
      size_t val_len = 0;

      switch (value_type)
      {
      case AWS_ES_HDR_BOOL_TRUE:
         int_value = 1;
         break;
      case AWS_ES_HDR_BOOL_FALSE:
         int_value = 0;
         break;
      case AWS_ES_HDR_INT8:
         if (rem < 1)
            return -1;
         int_value = (int64_t)(int8_t)cur[0];
         cur += 1;
         break;
      case AWS_ES_HDR_INT16:
         if (rem < 2)
            return -1;
         int_value = (int64_t)(int16_t)rd_u16be(cur);
         cur += 2;
         break;
      case AWS_ES_HDR_INT32:
         if (rem < 4)
            return -1;
         int_value = (int64_t)(int32_t)rd_u32be(cur);
         cur += 4;
         break;
      case AWS_ES_HDR_INT64:
      case AWS_ES_HDR_TIMESTAMP:
         if (rem < 8)
            return -1;
         int_value = (int64_t)rd_u64be(cur);
         cur += 8;
         break;
      case AWS_ES_HDR_BYTES:
      case AWS_ES_HDR_STRING:
      {
         if (rem < 2)
            return -1;
         uint16_t vlen = rd_u16be(cur);
         cur += 2;
         rem = (size_t)(end - cur);
         if (rem < vlen)
            return -1;
         val_ptr = cur;
         val_len = vlen;
         cur += vlen;
         break;
      }
      case AWS_ES_HDR_UUID:
         if (rem < 16)
            return -1;
         val_ptr = cur;
         val_len = 16;
         cur += 16;
         break;
      default:
         /* value_type out of [0,9] — malformed. */
         return -1;
      }

      /* Capture up to the cap; excess is bounds-validated but not stored. */
      if (out->n_headers < AWS_ES_MAX_HEADERS)
      {
         aws_es_header_t *h = &out->headers[out->n_headers++];
         h->name.ptr = name_ptr;
         h->name.len = name_len;
         h->value_type = value_type;
         h->int_value = int_value;
         h->value.ptr = val_ptr;
         h->value.len = val_len;
      }
      else
      {
         out->headers_truncated = 1;
      }
   }
   /* cur == end exactly on a well-formed block. */
   return 0;
}

/* Locate a captured header by name (or NULL). */
static const aws_es_header_t *find_header(const aws_es_message_t *out, const char *name)
{
   for (size_t i = 0; i < out->n_headers; i++)
      if (name_eq(&out->headers[i].name, name))
         return &out->headers[i];
   return NULL;
}

static void classify(aws_es_message_t *out)
{
   out->message_type = find_header(out, ":message-type");
   out->event_type = find_header(out, ":event-type");
   out->content_type = find_header(out, ":content-type");
   out->exception_type = find_header(out, ":exception-type");
   out->error_code = find_header(out, ":error-code");
   out->error_message = find_header(out, ":error-message");

   out->msg_type = AWS_ES_MSG_UNKNOWN;
   if (out->message_type && out->message_type->value_type == AWS_ES_HDR_STRING)
   {
      const aws_es_view_t *v = &out->message_type->value;
      if (name_eq(v, "event"))
         out->msg_type = AWS_ES_MSG_EVENT;
      else if (name_eq(v, "exception"))
         out->msg_type = AWS_ES_MSG_EXCEPTION;
      else if (name_eq(v, "error"))
         out->msg_type = AWS_ES_MSG_ERROR;
   }
}

aws_es_status_t aws_es_decode(const uint8_t *buf, size_t len, aws_es_message_t *out,
                              size_t *consumed)
{
   if (consumed)
      *consumed = 0;
   if (!buf || !out || !consumed)
      return AWS_ES_ERROR;

   /* Clear `out` up-front so NO return path (ERROR / NEED_MORE / OK) can leave a
    * caller reading stale struct fields (dangling ptr/len views) from a prior call:
    * on a non-OK return `out` is fully zeroed, not merely `*consumed`. */
   memset(out, 0, sizeof(*out));

   /* Need the full 12-byte prelude before we can trust any length. */
   if (len < 12)
      return AWS_ES_NEED_MORE;

   /* Read the prelude; WIDEN to uint64_t for all arithmetic (no truncation,
    * no signed math, no overflow on 32-bit size_t platforms). */
   uint64_t total_length = rd_u32be(buf);
   uint64_t headers_length = rd_u32be(buf + 4);
   uint32_t prelude_crc = rd_u32be(buf + 8);

   /* Size gate FIRST (before any subtraction): a message is at least the 16 B
    * of framing overhead and at most the hard cap. */
   if (total_length < AWS_ES_FRAMING_OVERHEAD)
      return AWS_ES_ERROR;
   if (total_length > (uint64_t)AWS_ES_MAX_MESSAGE)
      return AWS_ES_ERROR;

   /* Not enough bytes buffered for the whole message yet — retry later. */
   if ((uint64_t)len < total_length)
      return AWS_ES_NEED_MORE;

   /* Prelude CRC covers the first 8 bytes. */
   if (aws_es_crc32(buf, 8) != prelude_crc)
      return AWS_ES_ERROR;

   /* Message CRC (last 4 bytes) covers the first total_length-4 bytes (i.e. it
    * DOES cover the prelude + prelude_crc). total_length >= 16 so this cannot
    * underflow and total_length <= len so both reads are in-bounds. */
   size_t body_len = (size_t)(total_length - 4);
   uint32_t message_crc = rd_u32be(buf + body_len);
   if (aws_es_crc32(buf, body_len) != message_crc)
      return AWS_ES_ERROR;

   /* Underflow guard: NEVER compute total_length - headers_length - 16 before
    * proving headers_length + 16 <= total_length (widened; no wraparound). */
   if (headers_length + (uint64_t)AWS_ES_FRAMING_OVERHEAD > total_length)
      return AWS_ES_ERROR;

   uint64_t payload_length = total_length - headers_length - AWS_ES_FRAMING_OVERHEAD;

   /* Header block: [buf+12, buf+12+headers_length). This is within
    * [0, total_length-4) <= [0, len), so it is fully in-bounds. */
   const uint8_t *headers_start = buf + 12;
   const uint8_t *headers_end = headers_start + headers_length;

   /* `out` was already zeroed up-front and untouched since; parse_headers appends
    * from n_headers==0. */
   if (parse_headers(headers_start, headers_end, out) != 0)
      return AWS_ES_ERROR;

   /* Payload follows the header block. */
   out->payload.ptr = payload_length ? headers_end : NULL;
   out->payload.len = (size_t)payload_length;

   classify(out);

   *consumed = (size_t)total_length;
   return AWS_ES_OK;
}

const char *aws_es_status_str(aws_es_status_t st)
{
   switch (st)
   {
   case AWS_ES_OK:
      return "OK";
   case AWS_ES_NEED_MORE:
      return "NEED_MORE";
   case AWS_ES_ERROR:
      return "ERROR";
   }
   return "?";
}

const char *aws_es_msg_type_str(aws_es_msg_type_t t)
{
   switch (t)
   {
   case AWS_ES_MSG_EVENT:
      return "event";
   case AWS_ES_MSG_EXCEPTION:
      return "exception";
   case AWS_ES_MSG_ERROR:
      return "error";
   case AWS_ES_MSG_UNKNOWN:
      return "unknown";
   }
   return "?";
}
