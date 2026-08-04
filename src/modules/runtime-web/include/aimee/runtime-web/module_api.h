/* Wire contract for the runtime-web process's bounded RPC fault classification. */
#ifndef AIMEE_RUNTIME_WEB_MODULE_API_H
#define AIMEE_RUNTIME_WEB_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_RUNTIME_WEB_EVENT_CLASSIFY 9985u
#define AIMEE_RUNTIME_WEB_STAGE_CLASSIFY 1u
#define AIMEE_RUNTIME_WEB_REQUEST_MAGIC 0x51455752u /* "RWEQ" */
#define AIMEE_RUNTIME_WEB_RESPONSE_MAGIC 0x53455752u /* "RWES" */
#define AIMEE_RUNTIME_WEB_WIRE_VERSION 1u
#define AIMEE_RUNTIME_WEB_KIND_MAX 31u
#define AIMEE_RUNTIME_WEB_REQUEST_KIND_OFF 16u
#define AIMEE_RUNTIME_WEB_REQUEST_LEN 48u
#define AIMEE_RUNTIME_WEB_RESPONSE_LEN 16u

static inline void aimee_runtime_web_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_runtime_web_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_runtime_web_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_runtime_web_nonzero_text(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] == 0)
         return 0;
   return 1;
}

static inline int aimee_runtime_web_request_encode(const char *kind, uint8_t *out,
                                                    size_t capacity)
{
   const char *value = kind ? kind : "";
   size_t kind_len = strlen(value);
   if (!out || capacity < AIMEE_RUNTIME_WEB_REQUEST_LEN ||
       kind_len > AIMEE_RUNTIME_WEB_KIND_MAX)
      return -1;
   memset(out, 0, AIMEE_RUNTIME_WEB_REQUEST_LEN);
   aimee_runtime_web_put_u32(out, AIMEE_RUNTIME_WEB_REQUEST_MAGIC);
   aimee_runtime_web_put_u32(out + 4, AIMEE_RUNTIME_WEB_WIRE_VERSION);
   aimee_runtime_web_put_u32(out + 8, (uint32_t)kind_len);
   if (kind_len)
      memcpy(out + AIMEE_RUNTIME_WEB_REQUEST_KIND_OFF, value, kind_len);
   return 0;
}

static inline int aimee_runtime_web_request_decode(const uint8_t *in, size_t len,
                                                    char kind[AIMEE_RUNTIME_WEB_KIND_MAX + 1u])
{
   if (!in || len != AIMEE_RUNTIME_WEB_REQUEST_LEN || !kind ||
       aimee_runtime_web_get_u32(in) != AIMEE_RUNTIME_WEB_REQUEST_MAGIC ||
       aimee_runtime_web_get_u32(in + 4) != AIMEE_RUNTIME_WEB_WIRE_VERSION ||
       aimee_runtime_web_get_u32(in + 8) > AIMEE_RUNTIME_WEB_KIND_MAX ||
       aimee_runtime_web_get_u32(in + 12) != 0)
      return -1;
   uint32_t kind_len = aimee_runtime_web_get_u32(in + 8);
   const uint8_t *slot = in + AIMEE_RUNTIME_WEB_REQUEST_KIND_OFF;
   if (!aimee_runtime_web_nonzero_text(slot, kind_len) ||
       !aimee_runtime_web_zero_padding(slot + kind_len,
                                       AIMEE_RUNTIME_WEB_KIND_MAX + 1u - kind_len))
      return -1;
   if (kind_len)
      memcpy(kind, slot, kind_len);
   kind[kind_len] = '\0';
   return 0;
}

static inline int aimee_runtime_web_status_valid(uint32_t status)
{
   return status == 400u || status == 403u || status == 404u || status == 502u ||
          status == 503u;
}

static inline int aimee_runtime_web_response_encode(uint32_t status, uint8_t *out,
                                                     size_t capacity)
{
   if (!out || capacity < AIMEE_RUNTIME_WEB_RESPONSE_LEN ||
       !aimee_runtime_web_status_valid(status))
      return -1;
   memset(out, 0, AIMEE_RUNTIME_WEB_RESPONSE_LEN);
   aimee_runtime_web_put_u32(out, AIMEE_RUNTIME_WEB_RESPONSE_MAGIC);
   aimee_runtime_web_put_u32(out + 4, AIMEE_RUNTIME_WEB_WIRE_VERSION);
   aimee_runtime_web_put_u32(out + 8, status);
   return 0;
}

static inline int aimee_runtime_web_response_decode(const uint8_t *in, size_t len,
                                                     uint32_t *status)
{
   if (!in || len != AIMEE_RUNTIME_WEB_RESPONSE_LEN || !status ||
       aimee_runtime_web_get_u32(in) != AIMEE_RUNTIME_WEB_RESPONSE_MAGIC ||
       aimee_runtime_web_get_u32(in + 4) != AIMEE_RUNTIME_WEB_WIRE_VERSION ||
       !aimee_runtime_web_status_valid(aimee_runtime_web_get_u32(in + 8)) ||
       aimee_runtime_web_get_u32(in + 12) != 0)
      return -1;
   *status = aimee_runtime_web_get_u32(in + 8);
   return 0;
}

#endif
