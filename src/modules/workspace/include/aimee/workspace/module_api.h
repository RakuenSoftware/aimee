/* Wire contract for workspace project-reference admission. */
#ifndef AIMEE_WORKSPACE_MODULE_API_H
#define AIMEE_WORKSPACE_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_WORKSPACE_EVENT_ACCESS    7169u
#define AIMEE_WORKSPACE_STAGE_ACCESS    1u
#define AIMEE_WORKSPACE_REQUEST_MAGIC   0x46455257u /* "WREF" */
#define AIMEE_WORKSPACE_RESPONSE_MAGIC  0x4b4f5757u /* "WWOK" */
#define AIMEE_WORKSPACE_WIRE_VERSION    1u
#define AIMEE_WORKSPACE_REF_MAX         129u
#define AIMEE_WORKSPACE_REQUEST_LEN     140u
#define AIMEE_WORKSPACE_RESPONSE_LEN    8u

static inline void aimee_workspace_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_workspace_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline void aimee_workspace_put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8u);
}

static inline uint16_t aimee_workspace_get_u16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static inline int aimee_workspace_request_encode(const char *ref, size_t ref_len, uint8_t *out,
                                                  size_t cap)
{
   if (!out || cap < AIMEE_WORKSPACE_REQUEST_LEN || !ref || ref_len == 0 ||
       ref_len > AIMEE_WORKSPACE_REF_MAX)
      return -1;
   memset(out, 0, AIMEE_WORKSPACE_REQUEST_LEN);
   aimee_workspace_put_u32(out, AIMEE_WORKSPACE_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_WORKSPACE_WIRE_VERSION;
   aimee_workspace_put_u16(out + 6, (uint16_t)ref_len);
   memcpy(out + 8, ref, ref_len);
   return 0;
}

static inline int aimee_workspace_response_decode(const uint8_t *in, size_t len, int *allowed)
{
   if (!in || len != AIMEE_WORKSPACE_RESPONSE_LEN || !allowed ||
       aimee_workspace_get_u32(in) != AIMEE_WORKSPACE_RESPONSE_MAGIC ||
       aimee_workspace_get_u32(in + 4) > 1u)
      return -1;
   *allowed = (int)aimee_workspace_get_u32(in + 4);
   return 0;
}

#endif
