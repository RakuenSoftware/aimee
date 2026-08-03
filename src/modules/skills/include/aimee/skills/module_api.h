/* Wire contract for the skill-context review nudge. */
#ifndef AIMEE_SKILLS_MODULE_API_H
#define AIMEE_SKILLS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

#define AIMEE_SKILLS_EVENT_CONTEXT    7681u
#define AIMEE_SKILLS_STAGE_CONTEXT    1u
#define AIMEE_SKILLS_REQUEST_MAGIC    0x58435453u /* "STCX" */
#define AIMEE_SKILLS_RESPONSE_MAGIC   0x57454956u /* "VIEW" */
#define AIMEE_SKILLS_WIRE_VERSION     1u
#define AIMEE_SKILLS_REQUEST_LEN      16u
#define AIMEE_SKILLS_RESPONSE_LEN     8u

static inline void aimee_skills_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_skills_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline int aimee_skills_request_encode(int hook_count, int interval, uint8_t *out,
                                               size_t cap)
{
   if (!out || cap < AIMEE_SKILLS_REQUEST_LEN)
      return -1;
   aimee_skills_put_u32(out, AIMEE_SKILLS_REQUEST_MAGIC);
   aimee_skills_put_u32(out + 4, AIMEE_SKILLS_WIRE_VERSION);
   aimee_skills_put_u32(out + 8, (uint32_t)hook_count);
   aimee_skills_put_u32(out + 12, (uint32_t)interval);
   return 0;
}

static inline int aimee_skills_response_decode(const uint8_t *in, size_t len, int *fire)
{
   if (!in || len != AIMEE_SKILLS_RESPONSE_LEN || !fire ||
       aimee_skills_get_u32(in) != AIMEE_SKILLS_RESPONSE_MAGIC ||
       aimee_skills_get_u32(in + 4) > 1u)
      return -1;
   *fire = (int)aimee_skills_get_u32(in + 4);
   return 0;
}

#endif
