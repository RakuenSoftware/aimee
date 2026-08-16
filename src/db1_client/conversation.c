/* db1_client/conversation.c: the conversation family, reached over the bus.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * Same functions, same contract, different side of the boundary: the daemon
 * links this instead of the DB1 domain, so nothing that calls these had to
 * change.
 *
 * It lives OUTSIDE modules/db1 deliberately. The module's descriptor owns every
 * .c beside it and compiles them into the DB1 process, so a client with these
 * names in that directory would be linked twice into the one binary that must
 * not have it -- once as the caller and once as the implementation.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
#include "payload_rewrite_state.h"
#include "wm.h"

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_CONVERSATION_CALL_TIMEOUT_MS 2000

static void warn_unreachable(int reason)
{
   static int warned;
   if (warned)
      return;
   warned = 1;
   /* Said once per process: enough to tell a store that is down from one that
      is quiet, without one line per call. The numeric
      aimee_module_call_result_t, not its name, so this does not pull the whole
      event-bus library in behind the client for one string. */
   LOG_WARN("db1.conversation", "DB1 %s is unreachable (module call result %d)", "conversation",
            reason);
}

/* Size the frame from the arguments themselves.

   These carry prompts, results and JSON documents, not just identifiers, and
   in-process callers have always passed them whole. A fixed cap here would
   refuse exactly those calls and return the same -1 as a broken store -- fine
   in a test with short strings, wrong the first time a real prompt arrives. The
   bus bounds the message instead. */
static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      /* Empty is legal on the wire: an optional field the caller left out
         travels as zero length. Which fields may be empty is the operation's
         business, checked before the frame is built. */
      if (!fields[i])
         return -1;
      size_t n = strlen(fields[i]);
      if (n > AIMEE_MODULE_MESSAGE_MAX_BODY - need - 4u)
         return -1;
      need += 4u + n;
   }
   *need_out = need;
   return 0;
}

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static void encode(uint8_t *out, uint32_t op, const char *const *fields, uint32_t count)
{
   uint32_t at = 0;
   aimee_db1_put_u32(out + at, op);
   at += 4u;
   aimee_db1_put_u32(out + at, count);
   at += 4u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(fields[i]);
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, fields[i], n);
      at += n;
   }
}

/* Returns the module's status, or -1 when the call never produced one. */
/* Fills up to `slots` reply values, each into the buffer and capacity the
   caller supplied. A write passes none; a read passes one; a row passes one per
   member; a list passes one per member per row it is willing to accept.

   `filled_out` reports how many values the reply actually carried, which is how
   a list learns its length: the rows are not counted separately on the wire
   because an operation already knows how wide its rows are. Callers that expect
   a fixed shape pass NULL. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *const *values,
                      const size_t *caps, uint32_t slots, uint32_t *filled_out)
{
   if (filled_out)
      *filled_out = 0u;
   for (uint32_t i = 0; i < slots; ++i)
      if (values[i] && caps[i])
         values[i][0] = '\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_CONVERSATION))
   {
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }

   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
      return -1;
   /* The reply is bounded by the caller's own buffer: it asked for at most
      value_len bytes, so there is no reason to hold more than that. */
   size_t response_cap = 8u;
   for (uint32_t i = 0; i < slots; ++i)
      response_cap += 4u + caps[i];
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   encode(request, op, fields, count);

   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_CONVERSATION_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_CONVERSATION, AIMEE_DB1_STAGE_CONVERSATION, 0, deadline,
                           request, (uint32_t)request_len, response, (uint32_t)response_cap,
                           &response_len, NULL, NULL);
   free(request);

   int result = -1;
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
      warn_unreachable((int)rc);
   else
   {
      uint32_t status = aimee_db1_get_u32(response);
      uint32_t fields_in = aimee_db1_get_u32(response + 4u);
      /* Read the reply's own count rather than assuming an arity: a status with
         no values is how a write answers, one value is a read, and a member
         apiece is a row. */
      result = (int)status;
      /* More values than the caller has room for is a contract mismatch, not
         something to read the first few of: the caller asked for at most this
         many rows, and a stage answering with more is not answering this call. */
      if (fields_in > slots)
         result = -1;
      else if (filled_out)
         *filled_out = fields_in;
      uint32_t at = 8u;
      for (uint32_t i = 0; i < fields_in && result != -1; ++i)
      {
         if (at + 4u > response_len)
         {
            result = -1;
            break;
         }
         uint32_t n = aimee_db1_get_u32(response + at);
         at += 4u;
         /* A reply whose declared length runs past what arrived is not a reply
            to read part of. */
         if (at + n > response_len)
         {
            result = -1;
            break;
         }
         if (i < slots && values[i] && caps[i])
         {
            /* No room for the terminator is no room: writing it would land one
               byte past the buffer the caller owns. */
            if (n >= caps[i])
               result = -1;
            else
            {
               memcpy(values[i], response + at, n);
               values[i][n] = '\0';
            }
         }
         at += n;
      }
   }
   free(response);
   return result;
}

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_payload_rewrite_state_get(const char *session_id, payload_rewrite_state_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   const char *fields[] = {session_id};
   char slot1[24];
   char slot2[24];
   char slot4[24];
   char slot6[24];
   char slot7[24];
   char slot8[24];
   char *const values[] = {out->session_id, slot1, slot2, out->last_prefix_hash, slot4, out->last_rewrite_at, slot6, slot7, slot8, out->rewrite_reason, out->updated_at};
   const size_t caps[] = {sizeof out->session_id, sizeof slot1, sizeof slot2, sizeof out->last_prefix_hash, sizeof slot4, sizeof out->last_rewrite_at, sizeof slot6, sizeof slot7, sizeof slot8, sizeof out->rewrite_reason, sizeof out->updated_at};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_REWRITE_STATE_GET, fields, 1, values, caps, 11, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->payload_epoch = (int64_t)strtoll(slot1, NULL, 10);
   out->compaction_epoch = (int64_t)strtoll(slot2, NULL, 10);
   out->last_payload_tokens = (int)strtol(slot4, NULL, 10);
   out->deferred_rewrite_count = (int)strtol(slot6, NULL, 10);
   out->consecutive_deferred_count = (int)strtol(slot7, NULL, 10);
   out->bytes_saved_pending = (int)strtol(slot8, NULL, 10);
   return 0;
}

int db1_payload_rewrite_state_set(const payload_rewrite_state_t *state)
{
   if (!state)
      return -1;
   char arg1[24];
   snprintf(arg1, sizeof arg1, "%lld", (long long)state->payload_epoch);
   char arg2[24];
   snprintf(arg2, sizeof arg2, "%lld", (long long)state->compaction_epoch);
   char arg4[24];
   snprintf(arg4, sizeof arg4, "%d", state->last_payload_tokens);
   char arg6[24];
   snprintf(arg6, sizeof arg6, "%d", state->deferred_rewrite_count);
   char arg7[24];
   snprintf(arg7, sizeof arg7, "%d", state->consecutive_deferred_count);
   char arg8[24];
   snprintf(arg8, sizeof arg8, "%d", state->bytes_saved_pending);
   const char *fields[] = {state->session_id, arg1, arg2, state->last_prefix_hash, arg4, state->last_rewrite_at, arg6, arg7, arg8, state->rewrite_reason, state->updated_at};
   return write_result(call_stage(AIMEE_DB1_OP_REWRITE_STATE_SET, fields, 11, NULL, NULL, 0, NULL));
}

int db1_wm_set(const char *session_id, const char *key, const char *value, const char *category, int ttl_seconds)
{
   if (!session_id || !session_id[0] || !key || !key[0] || !value || !value[0])
      return -1;
   char arg4[24];
   snprintf(arg4, sizeof arg4, "%d", ttl_seconds);
   const char *fields[] = {session_id, key, value, category ? category : "", arg4};
   return write_result(call_stage(AIMEE_DB1_OP_WM_SET, fields, 5, NULL, NULL, 0, NULL));
}

int db1_wm_get(const char *session_id, const char *key, wm_entry_t *out)
{
   if (!session_id || !session_id[0] || !key || !key[0] || !out)
      return -1;
   const char *fields[] = {session_id, key};
   char slot0[24];
   char *const values[] = {slot0, out->session_id, out->key, out->value, out->category, out->created_at, out->updated_at, out->expires_at};
   const size_t caps[] = {sizeof slot0, sizeof out->session_id, sizeof out->key, sizeof out->value, sizeof out->category, sizeof out->created_at, sizeof out->updated_at, sizeof out->expires_at};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_WM_GET, fields, 2, values, caps, 8, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   return 0;
}

int db1_wm_list(const char *session_id, const char *category, wm_entry_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg2[24];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {session_id, category ? category : "", arg2};
   char **values = malloc((size_t)max * 8u * sizeof *values);
   size_t *caps = malloc((size_t)max * 8u * sizeof *caps);
   char (*scratch)[24] = malloc((size_t)max * 1u * sizeof *scratch);
   if (!values || !caps || !scratch)
   {
      free(values);
      free(caps);
      free(scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int row = 0; row < max; ++row)
   {
      values[row * 8u + 0u] = scratch[row * 1u + 0u];
      caps[row * 8u + 0u] = sizeof scratch[row * 1u + 0u];
      values[row * 8u + 1u] = out[row].session_id;
      caps[row * 8u + 1u] = sizeof out[row].session_id;
      values[row * 8u + 2u] = out[row].key;
      caps[row * 8u + 2u] = sizeof out[row].key;
      values[row * 8u + 3u] = out[row].value;
      caps[row * 8u + 3u] = sizeof out[row].value;
      values[row * 8u + 4u] = out[row].category;
      caps[row * 8u + 4u] = sizeof out[row].category;
      values[row * 8u + 5u] = out[row].created_at;
      caps[row * 8u + 5u] = sizeof out[row].created_at;
      values[row * 8u + 6u] = out[row].updated_at;
      caps[row * 8u + 6u] = sizeof out[row].updated_at;
      values[row * 8u + 7u] = out[row].expires_at;
      caps[row * 8u + 7u] = sizeof out[row].expires_at;
   }
   uint32_t filled = 0;
   int status = call_stage(AIMEE_DB1_OP_WM_LIST, fields, 3, values, caps,
                           (uint32_t)(max * 8), &filled);
   free(values);
   free(caps);
   if (status != (int)AIMEE_DB1_STATUS_OK || filled % 8u != 0u)
   {
      free(scratch);
      return -1;
   }
   int rows = (int)(filled / 8u);
   for (int row = 0; row < rows; ++row)
   {
      out[row].id = (int64_t)strtoll(scratch[row * 1u + 0u], NULL, 10);
   }
   free(scratch);
   return rows;
}

/* clang-format on */
