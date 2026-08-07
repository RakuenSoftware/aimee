/* Wire contract for the roundtable process's deterministic verification rubric. */
#ifndef AIMEE_ROUNDTABLE_MODULE_API_H
#define AIMEE_ROUNDTABLE_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_ROUNDTABLE_EVENT_DELIBERATE 9473u
#define AIMEE_ROUNDTABLE_STAGE_DELIBERATE 1u

/* The review stage. Unlike deliberate, which is a fixed 40-byte rubric, a review
 * carries JSON in both directions -- the same body the private HTTP proxy used
 * to carry, so moving the transport did not change the contract. The kind is
 * fixed by the process contract at 4096 + ordinal*256 + stage; roundtable is
 * ordinal 21, so review is deliberate's successor and not a free choice. */
#define AIMEE_ROUNDTABLE_EVENT_REVIEW 9474u
#define AIMEE_ROUNDTABLE_STAGE_REVIEW 2u
/* Chunk planning and the synthesis assembly. JSON: the artifact is arbitrarily
 * large and the plan is a variable number of spans. */
#define AIMEE_ROUNDTABLE_EVENT_CHUNK_PLAN      9475u
#define AIMEE_ROUNDTABLE_STAGE_CHUNK_PLAN      3u
#define AIMEE_ROUNDTABLE_REQUEST_MAGIC         0x52475452u /* "RTGR" */
#define AIMEE_ROUNDTABLE_RESPONSE_MAGIC        0x44475452u /* "RTGD" */
#define AIMEE_ROUNDTABLE_WIRE_VERSION          1u
#define AIMEE_ROUNDTABLE_SEVERITY_MAX          15u
#define AIMEE_ROUNDTABLE_REQUEST_LEN           40u
#define AIMEE_ROUNDTABLE_RESPONSE_LEN          32u
#define AIMEE_ROUNDTABLE_REQUEST_SEVERITY_OFF  24u
#define AIMEE_ROUNDTABLE_RESPONSE_SEVERITY_OFF 16u

typedef enum
{
   AIMEE_ROUNDTABLE_REPLAY_MATCH = 0,
   AIMEE_ROUNDTABLE_REPLAY_CORRECTED = 1,
   AIMEE_ROUNDTABLE_REPLAY_CONTRADICTED = 2,
   AIMEE_ROUNDTABLE_REPLAY_NO_EVIDENCE = 3,
   AIMEE_ROUNDTABLE_REPLAY_VACUOUS = 4,
   AIMEE_ROUNDTABLE_REPLAY_INDEX_UNAVAILABLE = 5
} aimee_roundtable_replay_status_t;

typedef enum
{
   AIMEE_ROUNDTABLE_VERIFY_KEEP = 0,
   AIMEE_ROUNDTABLE_VERIFY_CAP = 1,
   AIMEE_ROUNDTABLE_VERIFY_DEGRADE = 2,
   AIMEE_ROUNDTABLE_VERIFY_REJECT = 3
} aimee_roundtable_verify_action_t;

static inline void aimee_roundtable_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_roundtable_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_roundtable_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_roundtable_request_encode(aimee_roundtable_replay_status_t status,
                                                  int factual, const char *severity, uint8_t *out,
                                                  size_t capacity)
{
   size_t severity_len = severity ? strlen(severity) : 0;
   if (!out || capacity < AIMEE_ROUNDTABLE_REQUEST_LEN ||
       status > AIMEE_ROUNDTABLE_REPLAY_INDEX_UNAVAILABLE || (factual != 0 && factual != 1) ||
       severity_len > AIMEE_ROUNDTABLE_SEVERITY_MAX)
      return -1;
   memset(out, 0, AIMEE_ROUNDTABLE_REQUEST_LEN);
   aimee_roundtable_put_u32(out, AIMEE_ROUNDTABLE_REQUEST_MAGIC);
   aimee_roundtable_put_u32(out + 4, AIMEE_ROUNDTABLE_WIRE_VERSION);
   aimee_roundtable_put_u32(out + 8, (uint32_t)status);
   aimee_roundtable_put_u32(out + 12, (uint32_t)factual);
   aimee_roundtable_put_u32(out + 16, (uint32_t)severity_len);
   if (severity_len)
      memcpy(out + AIMEE_ROUNDTABLE_REQUEST_SEVERITY_OFF, severity, severity_len);
   return 0;
}

static inline int aimee_roundtable_request_decode(const uint8_t *in, size_t len,
                                                  aimee_roundtable_replay_status_t *status,
                                                  int *factual, char *severity,
                                                  size_t severity_capacity)
{
   if (!in || len != AIMEE_ROUNDTABLE_REQUEST_LEN || !status || !factual || !severity ||
       aimee_roundtable_get_u32(in) != AIMEE_ROUNDTABLE_REQUEST_MAGIC ||
       aimee_roundtable_get_u32(in + 4) != AIMEE_ROUNDTABLE_WIRE_VERSION ||
       aimee_roundtable_get_u32(in + 8) > AIMEE_ROUNDTABLE_REPLAY_INDEX_UNAVAILABLE ||
       aimee_roundtable_get_u32(in + 12) > 1u ||
       aimee_roundtable_get_u32(in + 16) > AIMEE_ROUNDTABLE_SEVERITY_MAX ||
       aimee_roundtable_get_u32(in + 20) != 0)
      return -1;
   size_t severity_len = aimee_roundtable_get_u32(in + 16);
   if (severity_capacity <= severity_len ||
       !aimee_roundtable_zero_padding(in + AIMEE_ROUNDTABLE_REQUEST_SEVERITY_OFF + severity_len,
                                      AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u - severity_len))
      return -1;
   *status = (aimee_roundtable_replay_status_t)aimee_roundtable_get_u32(in + 8);
   *factual = (int)aimee_roundtable_get_u32(in + 12);
   if (severity_len)
      memcpy(severity, in + AIMEE_ROUNDTABLE_REQUEST_SEVERITY_OFF, severity_len);
   severity[severity_len] = '\0';
   return 0;
}

static inline int aimee_roundtable_response_encode(aimee_roundtable_verify_action_t action,
                                                   const char *severity, uint8_t *out,
                                                   size_t capacity)
{
   size_t severity_len = severity ? strlen(severity) : 0;
   if (!out || capacity < AIMEE_ROUNDTABLE_RESPONSE_LEN ||
       action > AIMEE_ROUNDTABLE_VERIFY_REJECT || severity_len > AIMEE_ROUNDTABLE_SEVERITY_MAX)
      return -1;
   memset(out, 0, AIMEE_ROUNDTABLE_RESPONSE_LEN);
   aimee_roundtable_put_u32(out, AIMEE_ROUNDTABLE_RESPONSE_MAGIC);
   aimee_roundtable_put_u32(out + 4, (uint32_t)action);
   aimee_roundtable_put_u32(out + 8, (uint32_t)severity_len);
   if (severity_len)
      memcpy(out + AIMEE_ROUNDTABLE_RESPONSE_SEVERITY_OFF, severity, severity_len);
   return 0;
}

static inline int aimee_roundtable_response_decode(const uint8_t *in, size_t len,
                                                   aimee_roundtable_verify_action_t *action,
                                                   char *severity, size_t severity_capacity)
{
   if (!in || len != AIMEE_ROUNDTABLE_RESPONSE_LEN || !action || !severity ||
       aimee_roundtable_get_u32(in) != AIMEE_ROUNDTABLE_RESPONSE_MAGIC ||
       aimee_roundtable_get_u32(in + 4) > AIMEE_ROUNDTABLE_VERIFY_REJECT ||
       aimee_roundtable_get_u32(in + 8) > AIMEE_ROUNDTABLE_SEVERITY_MAX ||
       aimee_roundtable_get_u32(in + 12) != 0)
      return -1;
   size_t severity_len = aimee_roundtable_get_u32(in + 8);
   if (severity_capacity <= severity_len ||
       !aimee_roundtable_zero_padding(in + AIMEE_ROUNDTABLE_RESPONSE_SEVERITY_OFF + severity_len,
                                      AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u - severity_len))
      return -1;
   *action = (aimee_roundtable_verify_action_t)aimee_roundtable_get_u32(in + 4);
   if (severity_len)
      memcpy(severity, in + AIMEE_ROUNDTABLE_RESPONSE_SEVERITY_OFF, severity_len);
   severity[severity_len] = '\0';
   return 0;
}

#endif
