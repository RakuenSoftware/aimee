/* Wire contract for the benchmarks process's deterministic IR scoring stage. */
#ifndef AIMEE_BENCHMARKS_MODULE_API_H
#define AIMEE_BENCHMARKS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_BENCHMARKS_EVENT_RUN             10497u
#define AIMEE_BENCHMARKS_STAGE_RUN             1u
#define AIMEE_BENCHMARKS_REQUEST_MAGIC         0x51524942u /* "BIRQ" */
#define AIMEE_BENCHMARKS_RESPONSE_MAGIC        0x53524942u /* "BIRS" */
#define AIMEE_BENCHMARKS_WIRE_VERSION          1u
#define AIMEE_BENCHMARKS_MAX_RESULTS           32u
#define AIMEE_BENCHMARKS_REQUEST_RETRIEVED_OFF 24u
#define AIMEE_BENCHMARKS_REQUEST_RELEVANT_OFF  280u
#define AIMEE_BENCHMARKS_REQUEST_LEN           536u
#define AIMEE_BENCHMARKS_RESPONSE_LEN          32u

typedef struct
{
   double mrr;
   double ndcg;
   double recall;
} aimee_benchmarks_ir_scores_t;

static inline void aimee_benchmarks_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_benchmarks_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline void aimee_benchmarks_put_u64(uint8_t *p, uint64_t value)
{
   for (unsigned i = 0; i < 8; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint64_t aimee_benchmarks_get_u64(const uint8_t *p)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < 8; ++i)
      value |= (uint64_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_benchmarks_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_benchmarks_request_encode(
    const int64_t *retrieved, uint32_t retrieved_count, const int64_t *relevant,
    uint32_t relevant_count, uint32_t k, uint8_t *out, size_t capacity)
{
   if (!out || capacity < AIMEE_BENCHMARKS_REQUEST_LEN || k == 0 ||
       k > AIMEE_BENCHMARKS_MAX_RESULTS || retrieved_count > AIMEE_BENCHMARKS_MAX_RESULTS ||
       relevant_count > AIMEE_BENCHMARKS_MAX_RESULTS || (retrieved_count && !retrieved) ||
       (relevant_count && !relevant))
      return -1;
   memset(out, 0, AIMEE_BENCHMARKS_REQUEST_LEN);
   aimee_benchmarks_put_u32(out, AIMEE_BENCHMARKS_REQUEST_MAGIC);
   aimee_benchmarks_put_u32(out + 4, AIMEE_BENCHMARKS_WIRE_VERSION);
   aimee_benchmarks_put_u32(out + 8, k);
   aimee_benchmarks_put_u32(out + 12, retrieved_count);
   aimee_benchmarks_put_u32(out + 16, relevant_count);
   for (uint32_t i = 0; i < retrieved_count; ++i)
      aimee_benchmarks_put_u64(out + AIMEE_BENCHMARKS_REQUEST_RETRIEVED_OFF + i * 8u,
                               (uint64_t)retrieved[i]);
   for (uint32_t i = 0; i < relevant_count; ++i)
      aimee_benchmarks_put_u64(out + AIMEE_BENCHMARKS_REQUEST_RELEVANT_OFF + i * 8u,
                               (uint64_t)relevant[i]);
   return 0;
}

static inline int aimee_benchmarks_request_decode(
    const uint8_t *in, size_t len, int64_t *retrieved, size_t retrieved_capacity,
    uint32_t *retrieved_count, int64_t *relevant, size_t relevant_capacity,
    uint32_t *relevant_count, uint32_t *k)
{
   if (!in || len != AIMEE_BENCHMARKS_REQUEST_LEN || !retrieved_count || !relevant_count || !k ||
       aimee_benchmarks_get_u32(in) != AIMEE_BENCHMARKS_REQUEST_MAGIC ||
       aimee_benchmarks_get_u32(in + 4) != AIMEE_BENCHMARKS_WIRE_VERSION ||
       aimee_benchmarks_get_u32(in + 8) == 0 ||
       aimee_benchmarks_get_u32(in + 8) > AIMEE_BENCHMARKS_MAX_RESULTS ||
       aimee_benchmarks_get_u32(in + 12) > AIMEE_BENCHMARKS_MAX_RESULTS ||
       aimee_benchmarks_get_u32(in + 16) > AIMEE_BENCHMARKS_MAX_RESULTS ||
       aimee_benchmarks_get_u32(in + 20) != 0)
      return -1;
   uint32_t nr = aimee_benchmarks_get_u32(in + 12);
   uint32_t ng = aimee_benchmarks_get_u32(in + 16);
   if ((nr && (!retrieved || retrieved_capacity < nr)) ||
       (ng && (!relevant || relevant_capacity < ng)) ||
       !aimee_benchmarks_zero_padding(
           in + AIMEE_BENCHMARKS_REQUEST_RETRIEVED_OFF + nr * 8u,
           (AIMEE_BENCHMARKS_MAX_RESULTS - nr) * 8u) ||
       !aimee_benchmarks_zero_padding(in + AIMEE_BENCHMARKS_REQUEST_RELEVANT_OFF + ng * 8u,
                                      (AIMEE_BENCHMARKS_MAX_RESULTS - ng) * 8u))
      return -1;
   for (uint32_t i = 0; i < nr; ++i)
      retrieved[i] =
          (int64_t)aimee_benchmarks_get_u64(in + AIMEE_BENCHMARKS_REQUEST_RETRIEVED_OFF + i * 8u);
   for (uint32_t i = 0; i < ng; ++i)
      relevant[i] =
          (int64_t)aimee_benchmarks_get_u64(in + AIMEE_BENCHMARKS_REQUEST_RELEVANT_OFF + i * 8u);
   *retrieved_count = nr;
   *relevant_count = ng;
   *k = aimee_benchmarks_get_u32(in + 8);
   return 0;
}

static inline int aimee_benchmarks_response_encode(const aimee_benchmarks_ir_scores_t *scores,
                                                    uint8_t *out, size_t capacity)
{
   if (!scores || !out || capacity < AIMEE_BENCHMARKS_RESPONSE_LEN ||
       sizeof(double) != sizeof(uint64_t))
      return -1;
   uint64_t bits = 0;
   memset(out, 0, AIMEE_BENCHMARKS_RESPONSE_LEN);
   aimee_benchmarks_put_u32(out, AIMEE_BENCHMARKS_RESPONSE_MAGIC);
   aimee_benchmarks_put_u32(out + 4, AIMEE_BENCHMARKS_WIRE_VERSION);
   memcpy(&bits, &scores->mrr, sizeof(bits));
   aimee_benchmarks_put_u64(out + 8, bits);
   memcpy(&bits, &scores->ndcg, sizeof(bits));
   aimee_benchmarks_put_u64(out + 16, bits);
   memcpy(&bits, &scores->recall, sizeof(bits));
   aimee_benchmarks_put_u64(out + 24, bits);
   return 0;
}

static inline int aimee_benchmarks_response_decode(const uint8_t *in, size_t len,
                                                    aimee_benchmarks_ir_scores_t *scores)
{
   if (!in || len != AIMEE_BENCHMARKS_RESPONSE_LEN || !scores ||
       sizeof(double) != sizeof(uint64_t) ||
       aimee_benchmarks_get_u32(in) != AIMEE_BENCHMARKS_RESPONSE_MAGIC ||
       aimee_benchmarks_get_u32(in + 4) != AIMEE_BENCHMARKS_WIRE_VERSION)
      return -1;
   uint64_t bits = aimee_benchmarks_get_u64(in + 8);
   memcpy(&scores->mrr, &bits, sizeof(bits));
   bits = aimee_benchmarks_get_u64(in + 16);
   memcpy(&scores->ndcg, &bits, sizeof(bits));
   bits = aimee_benchmarks_get_u64(in + 24);
   memcpy(&scores->recall, &bits, sizeof(bits));
   return 0;
}

#endif
