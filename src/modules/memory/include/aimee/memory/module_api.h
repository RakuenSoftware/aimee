/* Wire contract for the memory process's reranking confidence stage. */
#ifndef AIMEE_MEMORY_MODULE_API_H
#define AIMEE_MEMORY_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

#define AIMEE_MEMORY_EVENT_RERANK       5893u
#define AIMEE_MEMORY_STAGE_RERANK       5u
#define AIMEE_MEMORY_REQUEST_MAGIC      0x4b4e524du /* "MRNK" */
#define AIMEE_MEMORY_RESPONSE_MAGIC     0x464e434du /* "MCNF" */
#define AIMEE_MEMORY_WIRE_VERSION       1u
#define AIMEE_MEMORY_REQUEST_LEN        16u
#define AIMEE_MEMORY_RESPONSE_LEN       8u

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

#endif
