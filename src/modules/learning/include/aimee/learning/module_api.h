/* Wire contract for learning-signal observation classification. */
#ifndef AIMEE_LEARNING_MODULE_API_H
#define AIMEE_LEARNING_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_LEARNING_EVENT_OBSERVE    6145u
#define AIMEE_LEARNING_STAGE_OBSERVE    1u
#define AIMEE_LEARNING_REQUEST_MAGIC    0x53424f4cu /* "LOBS" */
#define AIMEE_LEARNING_RESPONSE_MAGIC   0x4b53414cu /* "LASK" */
#define AIMEE_LEARNING_WIRE_VERSION     1u
#define AIMEE_LEARNING_SIGNAL_MAX       31u
#define AIMEE_LEARNING_REQUEST_LEN      40u
#define AIMEE_LEARNING_RESPONSE_LEN     8u

#define AIMEE_LEARNING_SINK_RERANKER  0x01u
#define AIMEE_LEARNING_SINK_SUPERSEDE 0x02u
#define AIMEE_LEARNING_SINK_RULE      0x04u
#define AIMEE_LEARNING_SINK_WORKFLOW  0x08u

static inline void aimee_learning_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_learning_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline int aimee_learning_request_encode(const char *signal, uint8_t *out, size_t cap)
{
   size_t len = signal ? strlen(signal) : 0;
   if (!out || cap < AIMEE_LEARNING_REQUEST_LEN || len == 0 || len > AIMEE_LEARNING_SIGNAL_MAX)
      return -1;
   memset(out, 0, AIMEE_LEARNING_REQUEST_LEN);
   aimee_learning_put_u32(out, AIMEE_LEARNING_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_LEARNING_WIRE_VERSION;
   out[6] = (uint8_t)len;
   memcpy(out + 8, signal, len);
   return 0;
}

static inline int aimee_learning_request_decode(const uint8_t *in, size_t len, char *signal,
                                                 size_t signal_cap)
{
   if (!in || len != AIMEE_LEARNING_REQUEST_LEN || !signal || signal_cap == 0 ||
       aimee_learning_get_u32(in) != AIMEE_LEARNING_REQUEST_MAGIC ||
       in[4] != AIMEE_LEARNING_WIRE_VERSION || in[5] != 0 || in[7] != 0 || in[6] == 0 ||
       in[6] > AIMEE_LEARNING_SIGNAL_MAX || (size_t)in[6] >= signal_cap)
      return -1;
   memcpy(signal, in + 8, in[6]);
   signal[in[6]] = '\0';
   return 0;
}

static inline int aimee_learning_response_decode(const uint8_t *in, size_t len,
                                                  uint32_t *sink_mask)
{
   if (!in || len != AIMEE_LEARNING_RESPONSE_LEN || !sink_mask ||
       aimee_learning_get_u32(in) != AIMEE_LEARNING_RESPONSE_MAGIC)
      return -1;
   *sink_mask = aimee_learning_get_u32(in + 4);
   return (*sink_mask & ~(AIMEE_LEARNING_SINK_RERANKER | AIMEE_LEARNING_SINK_SUPERSEDE |
                          AIMEE_LEARNING_SINK_RULE | AIMEE_LEARNING_SINK_WORKFLOW))
              ? -1
              : 0;
}

#endif
