/* Wire contract for the governance process's bounded response-policy decision. */
#ifndef AIMEE_GOVERNANCE_MODULE_API_H
#define AIMEE_GOVERNANCE_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_GOVERNANCE_EVENT_EVALUATE 8961u
#define AIMEE_GOVERNANCE_STAGE_EVALUATE 1u
#define AIMEE_GOVERNANCE_REQUEST_MAGIC 0x51564f47u /* "GOVQ" */
#define AIMEE_GOVERNANCE_RESPONSE_MAGIC 0x52564f47u /* "GOVR" */
#define AIMEE_GOVERNANCE_WIRE_VERSION 1u
#define AIMEE_GOVERNANCE_TOOL_COUNT_MAX 16u
#define AIMEE_GOVERNANCE_TOOL_NAME_MAX 31u
#define AIMEE_GOVERNANCE_TOOL_NAME_SLOT 32u
#define AIMEE_GOVERNANCE_STOP_REASON_MAX 31u
#define AIMEE_GOVERNANCE_REQUEST_TOOL_LENGTHS_OFF 24u
#define AIMEE_GOVERNANCE_REQUEST_TOOL_NAMES_OFF 88u
#define AIMEE_GOVERNANCE_REQUEST_STOP_REASON_OFF 600u
#define AIMEE_GOVERNANCE_REQUEST_LEN 632u
#define AIMEE_GOVERNANCE_RESPONSE_STOP_REASON_OFF 24u
#define AIMEE_GOVERNANCE_RESPONSE_LEN 56u

typedef struct
{
   uint32_t keep_mask;
   uint32_t drop_count;
   char stop_reason[AIMEE_GOVERNANCE_STOP_REASON_MAX + 1u];
} aimee_governance_decision_t;

static inline void aimee_governance_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_governance_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_governance_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_governance_nonzero_text(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] == 0)
         return 0;
   return 1;
}

static inline uint32_t aimee_governance_mask_for_count(uint32_t call_count)
{
   return call_count == 0 ? 0u : (1u << call_count) - 1u;
}

static inline uint32_t aimee_governance_popcount(uint32_t value)
{
   uint32_t count = 0;
   while (value)
   {
      count += value & 1u;
      value >>= 1u;
   }
   return count;
}

static inline int aimee_governance_request_encode(
    int policy_active, const char *const *tool_names, uint32_t call_count,
    const char *stop_reason, uint8_t *out, size_t capacity)
{
   const char *reason = stop_reason ? stop_reason : "";
   size_t reason_len = strlen(reason);
   if (!out || capacity < AIMEE_GOVERNANCE_REQUEST_LEN ||
       (policy_active != 0 && policy_active != 1) ||
       call_count > AIMEE_GOVERNANCE_TOOL_COUNT_MAX || (call_count > 0 && !tool_names) ||
       reason_len > AIMEE_GOVERNANCE_STOP_REASON_MAX)
      return -1;

   memset(out, 0, AIMEE_GOVERNANCE_REQUEST_LEN);
   aimee_governance_put_u32(out, AIMEE_GOVERNANCE_REQUEST_MAGIC);
   aimee_governance_put_u32(out + 4, AIMEE_GOVERNANCE_WIRE_VERSION);
   aimee_governance_put_u32(out + 8, (uint32_t)policy_active);
   aimee_governance_put_u32(out + 12, call_count);
   aimee_governance_put_u32(out + 16, (uint32_t)reason_len);
   for (uint32_t i = 0; i < call_count; ++i)
   {
      if (!tool_names[i])
         return -1;
      size_t name_len = strlen(tool_names[i]);
      if (name_len > AIMEE_GOVERNANCE_TOOL_NAME_MAX)
         return -1;
      aimee_governance_put_u32(out + AIMEE_GOVERNANCE_REQUEST_TOOL_LENGTHS_OFF + i * 4u,
                               (uint32_t)name_len);
      if (name_len)
         memcpy(out + AIMEE_GOVERNANCE_REQUEST_TOOL_NAMES_OFF +
                    i * AIMEE_GOVERNANCE_TOOL_NAME_SLOT,
                tool_names[i], name_len);
   }
   if (reason_len)
      memcpy(out + AIMEE_GOVERNANCE_REQUEST_STOP_REASON_OFF, reason, reason_len);
   return 0;
}

static inline int aimee_governance_request_decode(
    const uint8_t *in, size_t len, int *policy_active,
    char tool_names[AIMEE_GOVERNANCE_TOOL_COUNT_MAX][AIMEE_GOVERNANCE_TOOL_NAME_MAX + 1u],
    uint32_t *call_count, char stop_reason[AIMEE_GOVERNANCE_STOP_REASON_MAX + 1u])
{
   if (!in || len != AIMEE_GOVERNANCE_REQUEST_LEN || !policy_active || !tool_names ||
       !call_count || !stop_reason ||
       aimee_governance_get_u32(in) != AIMEE_GOVERNANCE_REQUEST_MAGIC ||
       aimee_governance_get_u32(in + 4) != AIMEE_GOVERNANCE_WIRE_VERSION ||
       aimee_governance_get_u32(in + 8) > 1u ||
       aimee_governance_get_u32(in + 12) > AIMEE_GOVERNANCE_TOOL_COUNT_MAX ||
       aimee_governance_get_u32(in + 16) > AIMEE_GOVERNANCE_STOP_REASON_MAX ||
       aimee_governance_get_u32(in + 20) != 0)
      return -1;

   uint32_t count = aimee_governance_get_u32(in + 12);
   for (uint32_t i = 0; i < AIMEE_GOVERNANCE_TOOL_COUNT_MAX; ++i)
   {
      uint32_t name_len = aimee_governance_get_u32(
          in + AIMEE_GOVERNANCE_REQUEST_TOOL_LENGTHS_OFF + i * 4u);
      const uint8_t *slot = in + AIMEE_GOVERNANCE_REQUEST_TOOL_NAMES_OFF +
                            i * AIMEE_GOVERNANCE_TOOL_NAME_SLOT;
      if (name_len > AIMEE_GOVERNANCE_TOOL_NAME_MAX || (i >= count && name_len != 0) ||
          !aimee_governance_nonzero_text(slot, name_len) ||
          !aimee_governance_zero_padding(slot + name_len,
                                         AIMEE_GOVERNANCE_TOOL_NAME_SLOT - name_len))
         return -1;
      if (i < count)
      {
         if (name_len)
            memcpy(tool_names[i], slot, name_len);
         tool_names[i][name_len] = '\0';
      }
      else
         tool_names[i][0] = '\0';
   }

   uint32_t reason_len = aimee_governance_get_u32(in + 16);
   const uint8_t *reason = in + AIMEE_GOVERNANCE_REQUEST_STOP_REASON_OFF;
   if (!aimee_governance_nonzero_text(reason, reason_len) ||
       !aimee_governance_zero_padding(reason + reason_len,
                                      AIMEE_GOVERNANCE_TOOL_NAME_SLOT - reason_len))
      return -1;
   *policy_active = (int)aimee_governance_get_u32(in + 8);
   *call_count = count;
   if (reason_len)
      memcpy(stop_reason, reason, reason_len);
   stop_reason[reason_len] = '\0';
   return 0;
}

static inline int aimee_governance_response_encode(
    uint32_t keep_mask, uint32_t drop_count, const char *stop_reason,
    uint8_t *out, size_t capacity)
{
   const char *reason = stop_reason ? stop_reason : "";
   size_t reason_len = strlen(reason);
   if (!out || capacity < AIMEE_GOVERNANCE_RESPONSE_LEN ||
       reason_len > AIMEE_GOVERNANCE_STOP_REASON_MAX)
      return -1;
   memset(out, 0, AIMEE_GOVERNANCE_RESPONSE_LEN);
   aimee_governance_put_u32(out, AIMEE_GOVERNANCE_RESPONSE_MAGIC);
   aimee_governance_put_u32(out + 4, AIMEE_GOVERNANCE_WIRE_VERSION);
   aimee_governance_put_u32(out + 8, keep_mask);
   aimee_governance_put_u32(out + 12, drop_count);
   aimee_governance_put_u32(out + 16, (uint32_t)reason_len);
   if (reason_len)
      memcpy(out + AIMEE_GOVERNANCE_RESPONSE_STOP_REASON_OFF, reason, reason_len);
   return 0;
}

static inline int aimee_governance_response_decode(
    const uint8_t *in, size_t len, uint32_t call_count, aimee_governance_decision_t *decision)
{
   if (!in || len != AIMEE_GOVERNANCE_RESPONSE_LEN || !decision ||
       call_count > AIMEE_GOVERNANCE_TOOL_COUNT_MAX ||
       aimee_governance_get_u32(in) != AIMEE_GOVERNANCE_RESPONSE_MAGIC ||
       aimee_governance_get_u32(in + 4) != AIMEE_GOVERNANCE_WIRE_VERSION ||
       aimee_governance_get_u32(in + 16) > AIMEE_GOVERNANCE_STOP_REASON_MAX ||
       aimee_governance_get_u32(in + 20) != 0)
      return -1;
   uint32_t mask = aimee_governance_get_u32(in + 8);
   uint32_t drops = aimee_governance_get_u32(in + 12);
   uint32_t allowed_mask = aimee_governance_mask_for_count(call_count);
   uint32_t reason_len = aimee_governance_get_u32(in + 16);
   const uint8_t *reason = in + AIMEE_GOVERNANCE_RESPONSE_STOP_REASON_OFF;
   if ((mask & ~allowed_mask) != 0 || drops > call_count ||
       aimee_governance_popcount(mask) + drops != call_count ||
       !aimee_governance_nonzero_text(reason, reason_len) ||
       !aimee_governance_zero_padding(reason + reason_len,
                                      AIMEE_GOVERNANCE_TOOL_NAME_SLOT - reason_len))
      return -1;
   decision->keep_mask = mask;
   decision->drop_count = drops;
   if (reason_len)
      memcpy(decision->stop_reason, reason, reason_len);
   decision->stop_reason[reason_len] = '\0';
   return 0;
}

#endif
