/* Remaining native memory transport debt: reranking, extraction, embedding and
 * data. Write-gate and PII wire clients are now Go-only. */
#ifndef AIMEE_MEMORY_MODULE_API_H
#define AIMEE_MEMORY_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_MEMORY_EVENT_EXTRACT_INDEX    5889u
#define AIMEE_MEMORY_EVENT_WRITE            5890u
#define AIMEE_MEMORY_EVENT_EMBED            5891u
#define AIMEE_MEMORY_EVENT_RETRIEVE         5892u
#define AIMEE_MEMORY_EVENT_RERANK           5893u
#define AIMEE_MEMORY_EVENT_DECLARE_COMMANDS 5894u
#define AIMEE_MEMORY_EVENT_DATA             5895u
#define AIMEE_MEMORY_EVENT_COMMAND          5896u
#define AIMEE_MEMORY_STAGE_EXTRACT_INDEX    1u
#define AIMEE_MEMORY_STAGE_WRITE            2u
#define AIMEE_MEMORY_STAGE_EMBED            3u
#define AIMEE_MEMORY_STAGE_RETRIEVE         4u
#define AIMEE_MEMORY_STAGE_RERANK           5u
#define AIMEE_MEMORY_STAGE_DECLARE_COMMANDS 6u
#define AIMEE_MEMORY_STAGE_DATA             7u
#define AIMEE_MEMORY_STAGE_COMMAND          8u

/* Dashboard transport. The count is computed by the Go KB placement. */
int memory_prospective_count_by_state(int *armed, int *triggered, int *completed, int *expired);
/* Allocates the full, untruncated content returned by stage 7. Caller frees. */
char *memory_content_dup(int64_t memory_id);
int memory_valid_at(int64_t memory_id, const char *as_of);
#define AIMEE_MEMORY_REQUEST_MAGIC  0x4b4e524du /* "MRNK" */
#define AIMEE_MEMORY_RESPONSE_MAGIC 0x464e434du /* "MCNF" */
#define AIMEE_MEMORY_WIRE_VERSION   1u
#define AIMEE_MEMORY_REQUEST_LEN    16u
#define AIMEE_MEMORY_RESPONSE_LEN   8u

/* EMBED uses the module's bounded JSON envelope because its vector is variable
 * width. Request fields are base_url:string, input_type:"document"|"query",
 * text:string and max_dim:positive integer. A successful response carries
 * vector:number[] and dim; optional truncated, unavailable and unauthorized
 * booleans and error:string keep distinct failure facts distinct. */

/* Bound on the raw relation label accepted by the Go write decision. */
#define AIMEE_MEMORY_REL_TYPE_MAX 256u

typedef enum
{
   AIMEE_MEMORY_CONFIDENCE_LOW = 1,
   AIMEE_MEMORY_CONFIDENCE_MEDIUM = 2,
   AIMEE_MEMORY_CONFIDENCE_HIGH = 3
} aimee_memory_confidence_t;

static inline void aimee_memory_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (i * 8u));
}

static inline uint32_t aimee_memory_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (i * 8u);
   return v;
}

static inline void aimee_memory_put_i64(uint8_t *p, int64_t value)
{
   uint64_t v = (uint64_t)value;
   for (unsigned i = 0; i < 8; ++i)
      p[i] = (uint8_t)(v >> (i * 8u));
}

static inline int64_t aimee_memory_get_i64(const uint8_t *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; ++i)
      v |= (uint64_t)p[i] << (i * 8u);
   return (int64_t)v;
}

static inline int aimee_memory_request_encode(int64_t score_micros, uint8_t *out, size_t cap)
{
   if (!out || cap < AIMEE_MEMORY_REQUEST_LEN)
      return -1;
   aimee_memory_put_u32(out, AIMEE_MEMORY_REQUEST_MAGIC);
   aimee_memory_put_u32(out + 4, AIMEE_MEMORY_WIRE_VERSION);
   aimee_memory_put_i64(out + 8, score_micros);
   return 0;
}

static inline int aimee_memory_response_decode(const uint8_t *in, size_t len,
                                               aimee_memory_confidence_t *confidence)
{
   if (!in || len != AIMEE_MEMORY_RESPONSE_LEN || !confidence ||
       aimee_memory_get_u32(in) != AIMEE_MEMORY_RESPONSE_MAGIC)
      return -1;
   uint32_t value = aimee_memory_get_u32(in + 4);
   if (value < AIMEE_MEMORY_CONFIDENCE_LOW || value > AIMEE_MEMORY_CONFIDENCE_HIGH)
      return -1;
   *confidence = (aimee_memory_confidence_t)value;
   return 0;
}

static inline void aimee_memory_put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v & 0xffu);
   p[1] = (uint8_t)(v >> 8u);
}

/* Pattern-first extraction (stage EXTRACT_INDEX). Its own magics again, so a
 * request routed to the wrong stage is rejected rather than misparsed. */
#define AIMEE_MEMORY_EXTRACT_REQUEST_MAGIC       0x51525458u /* "XTRQ" */
#define AIMEE_MEMORY_EXTRACT_RESPONSE_MAGIC      0x53525458u /* "XTRS" */
#define AIMEE_MEMORY_EXTRACT_REQUEST_HEADER_LEN  16u
#define AIMEE_MEMORY_EXTRACT_RESPONSE_HEADER_LEN 8u

/* Field capacities of one extracted triple, mirroring pattern_triple_t's
 * buffers (subject, rel_type, object) including the NUL. Only the adapter can
 * see both this header and pattern_triple_t, so that is where they are checked
 * against each other rather than trusted to stay in step. */
#define AIMEE_MEMORY_TRIPLE_SUBJECT_MAX  128u
#define AIMEE_MEMORY_TRIPLE_REL_TYPE_MAX 64u
#define AIMEE_MEMORY_TRIPLE_OBJECT_MAX   128u

/* Wire size of one triple at its largest: two kinds, three length prefixes and
 * three fields at capacity (the stored NUL is not carried). */
#define AIMEE_MEMORY_TRIPLE_WIRE_MAX                                                               \
   (8u + 12u + (AIMEE_MEMORY_TRIPLE_SUBJECT_MAX - 1u) + (AIMEE_MEMORY_TRIPLE_REL_TYPE_MAX - 1u) +  \
    (AIMEE_MEMORY_TRIPLE_OBJECT_MAX - 1u))

/* Response capacity for a request that asked for at most `max` triples. */
#define AIMEE_MEMORY_EXTRACT_RESPONSE_MAX(max)                                                     \
   (AIMEE_MEMORY_EXTRACT_RESPONSE_HEADER_LEN + (size_t)(max) * AIMEE_MEMORY_TRIPLE_WIRE_MAX)

typedef struct
{
   uint32_t subject_kind;
   uint32_t object_kind;
   char subject[AIMEE_MEMORY_TRIPLE_SUBJECT_MAX];
   char rel_type[AIMEE_MEMORY_TRIPLE_REL_TYPE_MAX];
   char object[AIMEE_MEMORY_TRIPLE_OBJECT_MAX];
} aimee_memory_triple_t;

/* Size of the request carrying `text`. The text is length-prefixed and carries
 * no bound of its own: unlike a relation label, there is no length past which a
 * turn stops being a turn, so the only limit is the bus body cap. In practice
 * the production caller passes memory_t.content, which is 2048 bytes. */
static inline size_t aimee_memory_extract_request_size(const char *text)
{
   size_t len = text ? strlen(text) : 0;
   if (len > UINT32_MAX - AIMEE_MEMORY_EXTRACT_REQUEST_HEADER_LEN)
      return 0;
   return AIMEE_MEMORY_EXTRACT_REQUEST_HEADER_LEN + len;
}

static inline int aimee_memory_extract_request_encode(const char *text, uint32_t max_triples,
                                                      uint8_t *out, size_t cap)
{
   size_t needed = aimee_memory_extract_request_size(text);
   if (!needed || !out || cap < needed || max_triples == 0)
      return -1;
   size_t len = needed - AIMEE_MEMORY_EXTRACT_REQUEST_HEADER_LEN;
   aimee_memory_put_u32(out, AIMEE_MEMORY_EXTRACT_REQUEST_MAGIC);
   aimee_memory_put_u32(out + 4, AIMEE_MEMORY_WIRE_VERSION);
   aimee_memory_put_u32(out + 8, max_triples);
   aimee_memory_put_u32(out + 12, (uint32_t)len);
   if (len)
      memcpy(out + 16, text, len);
   return 0;
}

/* Copies one length-prefixed field out of the response into a fixed buffer.
 * Returns the bytes consumed, or 0 if the field is truncated or too long for
 * its destination -- a field that does not fit is a malformed response, never a
 * silently shortened value, because a truncated relation label normalizes to a
 * different name. */
static inline size_t aimee_memory_extract_field(const uint8_t *in, size_t len, size_t offset,
                                                char *out, size_t out_cap)
{
   if (len < offset + 4u)
      return 0;
   uint32_t field_len = aimee_memory_get_u32(in + offset);
   if (field_len >= out_cap || len - offset - 4u < field_len)
      return 0;
   memcpy(out, in + offset + 4u, field_len);
   out[field_len] = '\0';
   return 4u + (size_t)field_len;
}

/* Decodes up to `max` triples. On success writes the count to *count. */
static inline int aimee_memory_extract_response_decode(const uint8_t *in, size_t len,
                                                       aimee_memory_triple_t *out, uint32_t max,
                                                       uint32_t *count)
{
   if (!in || !out || !count || len < AIMEE_MEMORY_EXTRACT_RESPONSE_HEADER_LEN ||
       aimee_memory_get_u32(in) != AIMEE_MEMORY_EXTRACT_RESPONSE_MAGIC)
      return -1;
   uint32_t found = aimee_memory_get_u32(in + 4);
   if (found > max)
      return -1;
   size_t offset = AIMEE_MEMORY_EXTRACT_RESPONSE_HEADER_LEN;
   for (uint32_t i = 0; i < found; ++i)
   {
      if (len < offset + 8u)
         return -1;
      out[i].subject_kind = aimee_memory_get_u32(in + offset);
      out[i].object_kind = aimee_memory_get_u32(in + offset + 4u);
      offset += 8u;
      size_t used =
          aimee_memory_extract_field(in, len, offset, out[i].subject, sizeof(out[i].subject));
      if (!used)
         return -1;
      offset += used;
      used = aimee_memory_extract_field(in, len, offset, out[i].rel_type, sizeof(out[i].rel_type));
      if (!used)
         return -1;
      offset += used;
      used = aimee_memory_extract_field(in, len, offset, out[i].object, sizeof(out[i].object));
      if (!used)
         return -1;
      offset += used;
   }
   /* Trailing bytes mean the two sides disagree about the shape; refuse rather
    * than accept the prefix that happened to parse. */
   if (offset != len)
      return -1;
   *count = found;
   return 0;
}

/* Retraction scan (stage EXTRACT_INDEX, second shape). Answers both halves of
 * the §4 correction pre-scan in one call, because its caller asks them together
 * once per turn. Shares the stage with extraction, told apart by its magic. */
#define AIMEE_MEMORY_SCAN_REQUEST_MAGIC       0x51525452u /* "RTRQ" */
#define AIMEE_MEMORY_SCAN_RESPONSE_MAGIC      0x53525452u /* "RTRS" */
#define AIMEE_MEMORY_SCAN_REQUEST_HEADER_LEN  12u
#define AIMEE_MEMORY_SCAN_RESPONSE_HEADER_LEN 16u
/* Mirrors the attribute buffer the production caller uses; checked against
 * memory_pattern_turn_t in the adapter, the one place that sees both. */
#define AIMEE_MEMORY_SCAN_ATTR_MAX 128u
#define AIMEE_MEMORY_SCAN_RESPONSE_MAX                                                             \
   (AIMEE_MEMORY_SCAN_RESPONSE_HEADER_LEN + AIMEE_MEMORY_SCAN_ATTR_MAX - 1u)

static inline size_t aimee_memory_scan_request_size(const char *text)
{
   size_t len = text ? strlen(text) : 0;
   if (len > UINT32_MAX - AIMEE_MEMORY_SCAN_REQUEST_HEADER_LEN)
      return 0;
   return AIMEE_MEMORY_SCAN_REQUEST_HEADER_LEN + len;
}

static inline int aimee_memory_scan_request_encode(const char *text, uint8_t *out, size_t cap)
{
   size_t needed = aimee_memory_scan_request_size(text);
   if (!needed || !out || cap < needed)
      return -1;
   size_t len = needed - AIMEE_MEMORY_SCAN_REQUEST_HEADER_LEN;
   aimee_memory_put_u32(out, AIMEE_MEMORY_SCAN_REQUEST_MAGIC);
   aimee_memory_put_u32(out + 4, AIMEE_MEMORY_WIRE_VERSION);
   aimee_memory_put_u32(out + 8, (uint32_t)len);
   if (len)
      memcpy(out + 12, text, len);
   return 0;
}

/* Decodes the scan. The flags are 0 or 1 and nothing else -- this drives a
 * deletion, so a truthy-looking 2 is a broken module, not a yes. An attribute
 * that does not fit is refused rather than shortened, because a shortened
 * attribute normalizes to a different relation and would retract that one. */
static inline int aimee_memory_scan_response_decode(const uint8_t *in, size_t len,
                                                    int *is_retraction, int *has_attr, char *attr,
                                                    size_t attr_cap)
{
   if (!in || !is_retraction || !has_attr || !attr || attr_cap == 0 ||
       len < AIMEE_MEMORY_SCAN_RESPONSE_HEADER_LEN ||
       aimee_memory_get_u32(in) != AIMEE_MEMORY_SCAN_RESPONSE_MAGIC)
      return -1;
   uint32_t retraction = aimee_memory_get_u32(in + 4);
   uint32_t possessive = aimee_memory_get_u32(in + 8);
   uint32_t attr_len = aimee_memory_get_u32(in + 12);
   if (retraction > 1u || possessive > 1u || attr_len >= attr_cap ||
       len != AIMEE_MEMORY_SCAN_RESPONSE_HEADER_LEN + (size_t)attr_len)
      return -1;
   /* An attribute without the flag, or a flag without one, means the two sides
    * disagree about what was found. */
   if ((possessive == 0u) != (attr_len == 0u))
      return -1;
   memcpy(attr, in + AIMEE_MEMORY_SCAN_RESPONSE_HEADER_LEN, attr_len);
   attr[attr_len] = '\0';
   *is_retraction = (int)retraction;
   *has_attr = (int)possessive;
   return 0;
}

#endif
