/* Wire contract for response-composition dedup-key generation. */
#ifndef AIMEE_RESPONSE_COMPOSITION_MODULE_API_H
#define AIMEE_RESPONSE_COMPOSITION_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_RESPONSE_EVENT_COMPOSE      7937u
#define AIMEE_RESPONSE_STAGE_COMPOSE      1u
#define AIMEE_RESPONSE_REQUEST_MAGIC      0x59454b52u /* "RKEY" */
#define AIMEE_RESPONSE_RESPONSE_MAGIC     0x504d4f43u /* "COMP" */
#define AIMEE_RESPONSE_WIRE_VERSION       1u
#define AIMEE_RESPONSE_FIELD_COUNT        9u
#define AIMEE_RESPONSE_REQUEST_HEADER_LEN 12u
#define AIMEE_RESPONSE_KEY_MAX            255u

typedef struct
{
   const char *principal;
   const char *source;
   const char *provider;
   const char *model;
   const char *endpoint;
   const char *idempotency_key;
   const char *body;
   const char *context;
   const char *behavior_flags;
   int stream;
} aimee_response_key_input_t;

static inline void aimee_response_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_response_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline size_t aimee_response_request_size(const aimee_response_key_input_t *in)
{
   if (!in)
      return 0;
   const char *fields[AIMEE_RESPONSE_FIELD_COUNT] = {
       in->principal, in->source, in->provider, in->model, in->endpoint,
       in->idempotency_key, in->body, in->context, in->behavior_flags};
   size_t total = AIMEE_RESPONSE_REQUEST_HEADER_LEN;
   for (size_t i = 0; i < AIMEE_RESPONSE_FIELD_COUNT; ++i)
   {
      size_t len = fields[i] ? strlen(fields[i]) : 0;
      if (len > UINT32_MAX || total > SIZE_MAX - 4u - len)
         return 0;
      total += 4u + len;
   }
   return total;
}

static inline int aimee_response_request_encode(const aimee_response_key_input_t *in,
                                                 uint8_t *out, size_t cap)
{
   size_t needed = aimee_response_request_size(in);
   if (!needed || !out || cap < needed)
      return -1;
   const char *fields[AIMEE_RESPONSE_FIELD_COUNT] = {
       in->principal, in->source, in->provider, in->model, in->endpoint,
       in->idempotency_key, in->body, in->context, in->behavior_flags};
   aimee_response_put_u32(out, AIMEE_RESPONSE_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_RESPONSE_WIRE_VERSION;
   out[5] = 0;
   out[6] = (uint8_t)AIMEE_RESPONSE_FIELD_COUNT;
   out[7] = 0;
   aimee_response_put_u32(out + 8, in->stream ? 1u : 0u);
   size_t offset = AIMEE_RESPONSE_REQUEST_HEADER_LEN;
   for (size_t i = 0; i < AIMEE_RESPONSE_FIELD_COUNT; ++i)
   {
      uint32_t len = fields[i] ? (uint32_t)strlen(fields[i]) : 0;
      aimee_response_put_u32(out + offset, len);
      offset += 4;
      if (len)
         memcpy(out + offset, fields[i], len);
      offset += len;
   }
   return 0;
}

static inline int aimee_response_response_decode(const uint8_t *in, size_t len, char *key,
                                                  size_t key_cap)
{
   if (!in || len < 4 || len - 4 > AIMEE_RESPONSE_KEY_MAX || !key || key_cap == 0 ||
       len - 4 >= key_cap || aimee_response_get_u32(in) != AIMEE_RESPONSE_RESPONSE_MAGIC)
      return -1;
   memcpy(key, in + 4, len - 4);
   key[len - 4] = '\0';
   return 0;
}

#endif
