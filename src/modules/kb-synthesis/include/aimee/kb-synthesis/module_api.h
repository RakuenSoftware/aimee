/* Wire contract for the KB synthesis process's bounded grounding decision. */
#ifndef AIMEE_KB_SYNTHESIS_MODULE_API_H
#define AIMEE_KB_SYNTHESIS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_KB_SYNTHESIS_EVENT_GROUNDING 9729u
#define AIMEE_KB_SYNTHESIS_STAGE_GROUNDING 1u
#define AIMEE_KB_SYNTHESIS_REQUEST_MAGIC 0x5147534bu /* "KSGQ" */
#define AIMEE_KB_SYNTHESIS_RESPONSE_MAGIC 0x5247534bu /* "KSGR" */
#define AIMEE_KB_SYNTHESIS_WIRE_VERSION 1u
#define AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX 16u
#define AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX 64u
#define AIMEE_KB_SYNTHESIS_TEXT_MAX 63u
#define AIMEE_KB_SYNTHESIS_TEXT_SLOT 64u
#define AIMEE_KB_SYNTHESIS_REQUEST_CLAIM_LENGTHS_OFF 24u
#define AIMEE_KB_SYNTHESIS_REQUEST_CALLEE_LENGTHS_OFF 88u
#define AIMEE_KB_SYNTHESIS_REQUEST_CLAIMS_OFF 344u
#define AIMEE_KB_SYNTHESIS_REQUEST_CALLEES_OFF 1368u
#define AIMEE_KB_SYNTHESIS_REQUEST_LEN 5464u
#define AIMEE_KB_SYNTHESIS_RESPONSE_REASON_OFF 24u
#define AIMEE_KB_SYNTHESIS_RESPONSE_LEN 88u

typedef enum
{
   /* A missing or JSON-null side_effects value. */
   AIMEE_KB_SYNTHESIS_CLAIM_NONE = 0,
   /* One JSON string. */
   AIMEE_KB_SYNTHESIS_CLAIM_STRING = 1,
   /* A JSON array containing only strings. */
   AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY = 2,
   /* Any other JSON value, including an array containing a non-string. */
   AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING = 3
} aimee_kb_synthesis_claim_kind_t;

typedef struct
{
   aimee_kb_synthesis_claim_kind_t claim_kind;
   uint32_t claim_count;
   uint32_t callee_count;
   char claims[AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX][AIMEE_KB_SYNTHESIS_TEXT_MAX + 1u];
   char callees[AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX][AIMEE_KB_SYNTHESIS_TEXT_MAX + 1u];
} aimee_kb_synthesis_grounding_request_t;

typedef struct
{
   int contradicts;
   char reason[AIMEE_KB_SYNTHESIS_TEXT_MAX + 1u];
} aimee_kb_synthesis_grounding_decision_t;

static inline void aimee_kb_synthesis_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_kb_synthesis_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_kb_synthesis_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_kb_synthesis_nonzero_text(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] == 0)
         return 0;
   return 1;
}

static inline int aimee_kb_synthesis_claim_shape_valid(
    aimee_kb_synthesis_claim_kind_t kind, uint32_t count)
{
   if (kind == AIMEE_KB_SYNTHESIS_CLAIM_NONE ||
       kind == AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING)
      return count == 0;
   if (kind == AIMEE_KB_SYNTHESIS_CLAIM_STRING)
      return count == 1;
   return kind == AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY &&
          count <= AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX;
}

static inline int aimee_kb_synthesis_request_encode(
    aimee_kb_synthesis_claim_kind_t claim_kind, const char *const *claims,
    uint32_t claim_count, const char *const *callees, uint32_t callee_count,
    uint8_t *out, size_t capacity)
{
   if (!out || capacity < AIMEE_KB_SYNTHESIS_REQUEST_LEN ||
       !aimee_kb_synthesis_claim_shape_valid(claim_kind, claim_count) ||
       callee_count > AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX ||
       (claim_count > 0 && !claims) || (callee_count > 0 && !callees))
      return -1;
   memset(out, 0, AIMEE_KB_SYNTHESIS_REQUEST_LEN);
   aimee_kb_synthesis_put_u32(out, AIMEE_KB_SYNTHESIS_REQUEST_MAGIC);
   aimee_kb_synthesis_put_u32(out + 4, AIMEE_KB_SYNTHESIS_WIRE_VERSION);
   aimee_kb_synthesis_put_u32(out + 8, (uint32_t)claim_kind);
   aimee_kb_synthesis_put_u32(out + 12, claim_count);
   aimee_kb_synthesis_put_u32(out + 16, callee_count);
   for (uint32_t i = 0; i < claim_count; ++i)
   {
      if (!claims[i])
         return -1;
      size_t text_len = strlen(claims[i]);
      if (text_len > AIMEE_KB_SYNTHESIS_TEXT_MAX)
         return -1;
      aimee_kb_synthesis_put_u32(out + AIMEE_KB_SYNTHESIS_REQUEST_CLAIM_LENGTHS_OFF + i * 4u,
                                 (uint32_t)text_len);
      if (text_len)
         memcpy(out + AIMEE_KB_SYNTHESIS_REQUEST_CLAIMS_OFF +
                    i * AIMEE_KB_SYNTHESIS_TEXT_SLOT,
                claims[i], text_len);
   }
   for (uint32_t i = 0; i < callee_count; ++i)
   {
      if (!callees[i])
         return -1;
      size_t text_len = strlen(callees[i]);
      if (text_len > AIMEE_KB_SYNTHESIS_TEXT_MAX)
         return -1;
      aimee_kb_synthesis_put_u32(out + AIMEE_KB_SYNTHESIS_REQUEST_CALLEE_LENGTHS_OFF + i * 4u,
                                 (uint32_t)text_len);
      if (text_len)
         memcpy(out + AIMEE_KB_SYNTHESIS_REQUEST_CALLEES_OFF +
                    i * AIMEE_KB_SYNTHESIS_TEXT_SLOT,
                callees[i], text_len);
   }
   return 0;
}

static inline int aimee_kb_synthesis_decode_text_array(
    const uint8_t *in, uint32_t count, uint32_t maximum, uint32_t lengths_off,
    uint32_t values_off, char out[][AIMEE_KB_SYNTHESIS_TEXT_MAX + 1u])
{
   for (uint32_t i = 0; i < maximum; ++i)
   {
      uint32_t text_len = aimee_kb_synthesis_get_u32(in + lengths_off + i * 4u);
      const uint8_t *slot = in + values_off + i * AIMEE_KB_SYNTHESIS_TEXT_SLOT;
      if (text_len > AIMEE_KB_SYNTHESIS_TEXT_MAX || (i >= count && text_len != 0) ||
          !aimee_kb_synthesis_nonzero_text(slot, text_len) ||
          !aimee_kb_synthesis_zero_padding(slot + text_len,
                                           AIMEE_KB_SYNTHESIS_TEXT_SLOT - text_len))
         return -1;
      if (i < count)
      {
         if (text_len)
            memcpy(out[i], slot, text_len);
         out[i][text_len] = '\0';
      }
      else
         out[i][0] = '\0';
   }
   return 0;
}

static inline int aimee_kb_synthesis_request_decode(
    const uint8_t *in, size_t len, aimee_kb_synthesis_grounding_request_t *request)
{
   if (!in || len != AIMEE_KB_SYNTHESIS_REQUEST_LEN || !request ||
       aimee_kb_synthesis_get_u32(in) != AIMEE_KB_SYNTHESIS_REQUEST_MAGIC ||
       aimee_kb_synthesis_get_u32(in + 4) != AIMEE_KB_SYNTHESIS_WIRE_VERSION ||
       aimee_kb_synthesis_get_u32(in + 8) > AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING ||
       aimee_kb_synthesis_get_u32(in + 12) > AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX ||
       aimee_kb_synthesis_get_u32(in + 16) > AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX ||
       aimee_kb_synthesis_get_u32(in + 20) != 0)
      return -1;
   memset(request, 0, sizeof *request);
   request->claim_kind =
       (aimee_kb_synthesis_claim_kind_t)aimee_kb_synthesis_get_u32(in + 8);
   request->claim_count = aimee_kb_synthesis_get_u32(in + 12);
   request->callee_count = aimee_kb_synthesis_get_u32(in + 16);
   if (!aimee_kb_synthesis_claim_shape_valid(request->claim_kind, request->claim_count) ||
       aimee_kb_synthesis_decode_text_array(
           in, request->claim_count, AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX,
           AIMEE_KB_SYNTHESIS_REQUEST_CLAIM_LENGTHS_OFF,
           AIMEE_KB_SYNTHESIS_REQUEST_CLAIMS_OFF, request->claims) != 0 ||
       aimee_kb_synthesis_decode_text_array(
           in, request->callee_count, AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX,
           AIMEE_KB_SYNTHESIS_REQUEST_CALLEE_LENGTHS_OFF,
           AIMEE_KB_SYNTHESIS_REQUEST_CALLEES_OFF, request->callees) != 0)
      return -1;
   return 0;
}

static inline int aimee_kb_synthesis_response_encode(int contradicts, const char *reason,
                                                      uint8_t *out, size_t capacity)
{
   const char *value = reason ? reason : "";
   size_t reason_len = strlen(value);
   if (!out || capacity < AIMEE_KB_SYNTHESIS_RESPONSE_LEN ||
       (contradicts != 0 && contradicts != 1) ||
       reason_len > AIMEE_KB_SYNTHESIS_TEXT_MAX ||
       ((contradicts != 0) != (reason_len != 0)))
      return -1;
   memset(out, 0, AIMEE_KB_SYNTHESIS_RESPONSE_LEN);
   aimee_kb_synthesis_put_u32(out, AIMEE_KB_SYNTHESIS_RESPONSE_MAGIC);
   aimee_kb_synthesis_put_u32(out + 4, AIMEE_KB_SYNTHESIS_WIRE_VERSION);
   aimee_kb_synthesis_put_u32(out + 8, (uint32_t)contradicts);
   aimee_kb_synthesis_put_u32(out + 12, (uint32_t)reason_len);
   if (reason_len)
      memcpy(out + AIMEE_KB_SYNTHESIS_RESPONSE_REASON_OFF, value, reason_len);
   return 0;
}

static inline int aimee_kb_synthesis_response_decode(
    const uint8_t *in, size_t len, aimee_kb_synthesis_grounding_decision_t *decision)
{
   if (!in || len != AIMEE_KB_SYNTHESIS_RESPONSE_LEN || !decision ||
       aimee_kb_synthesis_get_u32(in) != AIMEE_KB_SYNTHESIS_RESPONSE_MAGIC ||
       aimee_kb_synthesis_get_u32(in + 4) != AIMEE_KB_SYNTHESIS_WIRE_VERSION ||
       aimee_kb_synthesis_get_u32(in + 8) > 1u ||
       aimee_kb_synthesis_get_u32(in + 12) > AIMEE_KB_SYNTHESIS_TEXT_MAX ||
       aimee_kb_synthesis_get_u32(in + 16) != 0 || aimee_kb_synthesis_get_u32(in + 20) != 0)
      return -1;
   uint32_t reason_len = aimee_kb_synthesis_get_u32(in + 12);
   const uint8_t *reason = in + AIMEE_KB_SYNTHESIS_RESPONSE_REASON_OFF;
   int contradicts = (int)aimee_kb_synthesis_get_u32(in + 8);
   if ((contradicts != 0) != (reason_len != 0) ||
       !aimee_kb_synthesis_nonzero_text(reason, reason_len) ||
       !aimee_kb_synthesis_zero_padding(reason + reason_len,
                                        AIMEE_KB_SYNTHESIS_TEXT_SLOT - reason_len))
      return -1;
   decision->contradicts = contradicts;
   if (reason_len)
      memcpy(decision->reason, reason, reason_len);
   decision->reason[reason_len] = '\0';
   return 0;
}

#endif
