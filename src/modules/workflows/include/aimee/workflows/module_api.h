/* Wire contract for the workflows process's bounded advance decision. */
#ifndef AIMEE_WORKFLOWS_MODULE_API_H
#define AIMEE_WORKFLOWS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_WORKFLOWS_EVENT_ADVANCE 9217u
#define AIMEE_WORKFLOWS_STAGE_ADVANCE 1u
/* The roundtable gate ruling. JSON rather than the fixed framing the advance
 * stage uses: a panel is a variable number of verdicts. Stage 2 (control) is
 * declared in server/workflow_control_bus.h, beside its only caller. */
#define AIMEE_WORKFLOWS_EVENT_GATE_DECIDE 9219u
#define AIMEE_WORKFLOWS_STAGE_GATE_DECIDE 3u
/* S4 autonomous-parity routing: which workflows an autonomous run may
 * auto-select. JSON, like the gate ruling. */
#define AIMEE_WORKFLOWS_EVENT_AUTONOMOUS_ROUTE     9220u
#define AIMEE_WORKFLOWS_STAGE_AUTONOMOUS_ROUTE     4u
#define AIMEE_WORKFLOWS_REQUEST_MAGIC              0x51414657u /* "WFAQ" */
#define AIMEE_WORKFLOWS_RESPONSE_MAGIC             0x52414657u /* "WFAR" */
#define AIMEE_WORKFLOWS_WIRE_VERSION               1u
#define AIMEE_WORKFLOWS_WORK_ITEM_MAX              79u
#define AIMEE_WORKFLOWS_WORK_ITEM_SLOT             80u
#define AIMEE_WORKFLOWS_STAGE_MAX                  63u
#define AIMEE_WORKFLOWS_STAGE_SLOT                 64u
#define AIMEE_WORKFLOWS_STATE_MAX                  31u
#define AIMEE_WORKFLOWS_STATE_SLOT                 32u
#define AIMEE_WORKFLOWS_NONCE_MAX                  63u
#define AIMEE_WORKFLOWS_NONCE_SLOT                 64u
#define AIMEE_WORKFLOWS_REQUEST_BOUND_OFF          44u
#define AIMEE_WORKFLOWS_REQUEST_WORK_ITEM_OFF      124u
#define AIMEE_WORKFLOWS_REQUEST_OBSERVED_STAGE_OFF 204u
#define AIMEE_WORKFLOWS_REQUEST_ACTUAL_STAGE_OFF   268u
#define AIMEE_WORKFLOWS_REQUEST_ACTUAL_STATE_OFF   332u
#define AIMEE_WORKFLOWS_REQUEST_NONCE_OFF          364u
#define AIMEE_WORKFLOWS_REQUEST_LAST_NONCE_OFF     428u
#define AIMEE_WORKFLOWS_REQUEST_LEN                492u
#define AIMEE_WORKFLOWS_RESPONSE_LEN               16u

typedef enum
{
   AIMEE_WORKFLOWS_ADVANCE_OK = 0,
   AIMEE_WORKFLOWS_ADVANCE_REPLAY = 1,
   AIMEE_WORKFLOWS_ADVANCE_STALE = 2,
   AIMEE_WORKFLOWS_ADVANCE_UNBOUND = 3,
   AIMEE_WORKFLOWS_ADVANCE_TERMINAL = 4,
   AIMEE_WORKFLOWS_ADVANCE_BADARGS = 5
} aimee_workflows_advance_outcome_t;

typedef struct
{
   char bound_work_item[AIMEE_WORKFLOWS_WORK_ITEM_MAX + 1u];
   char work_item[AIMEE_WORKFLOWS_WORK_ITEM_MAX + 1u];
   char observed_stage[AIMEE_WORKFLOWS_STAGE_MAX + 1u];
   char actual_stage[AIMEE_WORKFLOWS_STAGE_MAX + 1u];
   char actual_state[AIMEE_WORKFLOWS_STATE_MAX + 1u];
   char nonce[AIMEE_WORKFLOWS_NONCE_MAX + 1u];
   char last_nonce[AIMEE_WORKFLOWS_NONCE_MAX + 1u];
   int have_nonce;
} aimee_workflows_advance_request_t;

static inline void aimee_workflows_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_workflows_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_workflows_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_workflows_nonzero_text(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] == 0)
         return 0;
   return 1;
}

static inline int aimee_workflows_decode_slot(const uint8_t *slot, uint32_t length,
                                              uint32_t slot_size, char *out)
{
   if (length >= slot_size || !aimee_workflows_nonzero_text(slot, length) ||
       !aimee_workflows_zero_padding(slot + length, slot_size - length))
      return -1;
   if (length)
      memcpy(out, slot, length);
   out[length] = '\0';
   return 0;
}

static inline int aimee_workflows_request_encode(const char *bound_work_item, const char *work_item,
                                                 const char *observed_stage,
                                                 const char *actual_stage, const char *actual_state,
                                                 int have_nonce, const char *nonce,
                                                 const char *last_nonce, uint8_t *out,
                                                 size_t capacity)
{
   const char *values[] = {bound_work_item ? bound_work_item : "",
                           work_item ? work_item : "",
                           observed_stage ? observed_stage : "",
                           actual_stage ? actual_stage : "",
                           actual_state ? actual_state : "",
                           nonce ? nonce : "",
                           last_nonce ? last_nonce : ""};
   const uint32_t maxima[] = {AIMEE_WORKFLOWS_WORK_ITEM_MAX, AIMEE_WORKFLOWS_WORK_ITEM_MAX,
                              AIMEE_WORKFLOWS_STAGE_MAX,     AIMEE_WORKFLOWS_STAGE_MAX,
                              AIMEE_WORKFLOWS_STATE_MAX,     AIMEE_WORKFLOWS_NONCE_MAX,
                              AIMEE_WORKFLOWS_NONCE_MAX};
   const uint32_t offsets[] = {
       AIMEE_WORKFLOWS_REQUEST_BOUND_OFF,          AIMEE_WORKFLOWS_REQUEST_WORK_ITEM_OFF,
       AIMEE_WORKFLOWS_REQUEST_OBSERVED_STAGE_OFF, AIMEE_WORKFLOWS_REQUEST_ACTUAL_STAGE_OFF,
       AIMEE_WORKFLOWS_REQUEST_ACTUAL_STATE_OFF,   AIMEE_WORKFLOWS_REQUEST_NONCE_OFF,
       AIMEE_WORKFLOWS_REQUEST_LAST_NONCE_OFF};
   size_t lengths[7];
   if (!out || capacity < AIMEE_WORKFLOWS_REQUEST_LEN || (have_nonce != 0 && have_nonce != 1))
      return -1;
   for (size_t i = 0; i < 7; ++i)
   {
      lengths[i] = strlen(values[i]);
      if (lengths[i] > maxima[i])
         return -1;
   }
   if ((have_nonce && lengths[5] == 0) || (!have_nonce && lengths[5] != 0))
      return -1;

   memset(out, 0, AIMEE_WORKFLOWS_REQUEST_LEN);
   aimee_workflows_put_u32(out, AIMEE_WORKFLOWS_REQUEST_MAGIC);
   aimee_workflows_put_u32(out + 4, AIMEE_WORKFLOWS_WIRE_VERSION);
   aimee_workflows_put_u32(out + 8, (uint32_t)have_nonce);
   for (size_t i = 0; i < 7; ++i)
   {
      aimee_workflows_put_u32(out + 12 + i * 4u, (uint32_t)lengths[i]);
      if (lengths[i])
         memcpy(out + offsets[i], values[i], lengths[i]);
   }
   return 0;
}

static inline int aimee_workflows_request_decode(const uint8_t *in, size_t len,
                                                 aimee_workflows_advance_request_t *request)
{
   const uint32_t maxima[] = {AIMEE_WORKFLOWS_WORK_ITEM_MAX, AIMEE_WORKFLOWS_WORK_ITEM_MAX,
                              AIMEE_WORKFLOWS_STAGE_MAX,     AIMEE_WORKFLOWS_STAGE_MAX,
                              AIMEE_WORKFLOWS_STATE_MAX,     AIMEE_WORKFLOWS_NONCE_MAX,
                              AIMEE_WORKFLOWS_NONCE_MAX};
   const uint32_t slots[] = {AIMEE_WORKFLOWS_WORK_ITEM_SLOT, AIMEE_WORKFLOWS_WORK_ITEM_SLOT,
                             AIMEE_WORKFLOWS_STAGE_SLOT,     AIMEE_WORKFLOWS_STAGE_SLOT,
                             AIMEE_WORKFLOWS_STATE_SLOT,     AIMEE_WORKFLOWS_NONCE_SLOT,
                             AIMEE_WORKFLOWS_NONCE_SLOT};
   const uint32_t offsets[] = {
       AIMEE_WORKFLOWS_REQUEST_BOUND_OFF,          AIMEE_WORKFLOWS_REQUEST_WORK_ITEM_OFF,
       AIMEE_WORKFLOWS_REQUEST_OBSERVED_STAGE_OFF, AIMEE_WORKFLOWS_REQUEST_ACTUAL_STAGE_OFF,
       AIMEE_WORKFLOWS_REQUEST_ACTUAL_STATE_OFF,   AIMEE_WORKFLOWS_REQUEST_NONCE_OFF,
       AIMEE_WORKFLOWS_REQUEST_LAST_NONCE_OFF};
   char *outputs[] = {
       request ? request->bound_work_item : NULL, request ? request->work_item : NULL,
       request ? request->observed_stage : NULL,  request ? request->actual_stage : NULL,
       request ? request->actual_state : NULL,    request ? request->nonce : NULL,
       request ? request->last_nonce : NULL};
   if (!in || len != AIMEE_WORKFLOWS_REQUEST_LEN || !request ||
       aimee_workflows_get_u32(in) != AIMEE_WORKFLOWS_REQUEST_MAGIC ||
       aimee_workflows_get_u32(in + 4) != AIMEE_WORKFLOWS_WIRE_VERSION ||
       aimee_workflows_get_u32(in + 8) > 1u || aimee_workflows_get_u32(in + 40) != 0)
      return -1;
   memset(request, 0, sizeof *request);
   for (size_t i = 0; i < 7; ++i)
   {
      uint32_t length = aimee_workflows_get_u32(in + 12 + i * 4u);
      if (length > maxima[i] ||
          aimee_workflows_decode_slot(in + offsets[i], length, slots[i], outputs[i]) != 0)
         return -1;
   }
   request->have_nonce = (int)aimee_workflows_get_u32(in + 8);
   if ((request->have_nonce && !request->nonce[0]) || (!request->have_nonce && request->nonce[0]))
      return -1;
   return 0;
}

static inline int aimee_workflows_response_encode(aimee_workflows_advance_outcome_t outcome,
                                                  uint8_t *out, size_t capacity)
{
   if (!out || capacity < AIMEE_WORKFLOWS_RESPONSE_LEN || outcome < AIMEE_WORKFLOWS_ADVANCE_OK ||
       outcome > AIMEE_WORKFLOWS_ADVANCE_BADARGS)
      return -1;
   memset(out, 0, AIMEE_WORKFLOWS_RESPONSE_LEN);
   aimee_workflows_put_u32(out, AIMEE_WORKFLOWS_RESPONSE_MAGIC);
   aimee_workflows_put_u32(out + 4, AIMEE_WORKFLOWS_WIRE_VERSION);
   aimee_workflows_put_u32(out + 8, (uint32_t)outcome);
   return 0;
}

static inline int aimee_workflows_response_decode(const uint8_t *in, size_t len,
                                                  aimee_workflows_advance_outcome_t *outcome)
{
   if (!in || len != AIMEE_WORKFLOWS_RESPONSE_LEN || !outcome ||
       aimee_workflows_get_u32(in) != AIMEE_WORKFLOWS_RESPONSE_MAGIC ||
       aimee_workflows_get_u32(in + 4) != AIMEE_WORKFLOWS_WIRE_VERSION ||
       aimee_workflows_get_u32(in + 8) > AIMEE_WORKFLOWS_ADVANCE_BADARGS ||
       aimee_workflows_get_u32(in + 12) != 0)
      return -1;
   *outcome = (aimee_workflows_advance_outcome_t)aimee_workflows_get_u32(in + 8);
   return 0;
}

#endif
