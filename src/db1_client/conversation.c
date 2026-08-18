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
#include "conv_context.h"
#include "db1_windows.h"
#include "payload_rewrite_state.h"
#include "user_memory.h"
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
   /* Zero fields is a legal request: an operation that takes no arguments
      sends the header alone. The upper bound still applies. */
   if (count > AIMEE_DB1_FIELDS_MAX)
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

/* A read answers found(1) / not-found(0) / error(-1), which is what the direct
   implementation returns and what its callers already branch on. */
static int read_result(int status, const char *value_out)
{
   if (status == (int)AIMEE_DB1_STATUS_OK)
      return (value_out && value_out[0]) ? 1 : 0;
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return -1;
}

int db1_payload_rewrite_state_get(const char *session_id, payload_rewrite_state_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   const char *fields[] = {session_id};
   char slot1[32];
   char slot2[32];
   char slot4[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->session_id, slot1, slot2, out->last_prefix_hash, slot4, out->last_rewrite_at, slot6, slot7, slot8, out->rewrite_reason, out->updated_at};
   const size_t caps[] = {sizeof out->session_id, sizeof slot1, sizeof slot2, sizeof out->last_prefix_hash, sizeof slot4, sizeof out->last_rewrite_at, sizeof slot6, sizeof slot7, sizeof slot8, sizeof out->rewrite_reason, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_REWRITE_STATE_GET, fields, 1, values, caps, 11, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
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
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)state->payload_epoch);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)state->compaction_epoch);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", state->last_payload_tokens);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", state->deferred_rewrite_count);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%d", state->consecutive_deferred_count);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", state->bytes_saved_pending);
   const char *fields[] = {state->session_id, arg1, arg2, state->last_prefix_hash, arg4, state->last_rewrite_at, arg6, arg7, arg8, state->rewrite_reason, state->updated_at};
   return write_result(call_stage(AIMEE_DB1_OP_REWRITE_STATE_SET, fields, 11, NULL, NULL, 0, NULL));
}

int db1_wm_set(const char *session_id, const char *key, const char *value, const char *category, int ttl_seconds)
{
   if (!session_id || !session_id[0] || !key || !key[0] || !value || !value[0])
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", ttl_seconds);
   const char *fields[] = {session_id, key, value, category ? category : "", arg4};
   return write_result(call_stage(AIMEE_DB1_OP_WM_SET, fields, 5, NULL, NULL, 0, NULL));
}

int db1_wm_get(const char *session_id, const char *key, wm_entry_t *out)
{
   if (!session_id || !session_id[0] || !key || !key[0] || !out)
      return -1;
   const char *fields[] = {session_id, key};
   char slot0[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->session_id, out->key, out->value, out->category, out->created_at, out->updated_at, out->expires_at};
   const size_t caps[] = {sizeof slot0, sizeof out->session_id, sizeof out->key, sizeof out->value, sizeof out->category, sizeof out->created_at, sizeof out->updated_at, sizeof out->expires_at};
   int wire_status = call_stage(AIMEE_DB1_OP_WM_GET, fields, 2, values, caps, 8, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   return 0;
}

int db1_wm_list(const char *session_id, const char *category, wm_entry_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {session_id, category ? category : "", arg2};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 8u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 8u + 2u] = out[wire_row].key;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].key;
      wire_values[wire_row * 8u + 3u] = out[wire_row].value;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].value;
      wire_values[wire_row * 8u + 4u] = out[wire_row].category;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].category;
      wire_values[wire_row * 8u + 5u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 5u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 8u + 6u] = out[wire_row].updated_at;
      wire_caps[wire_row * 8u + 6u] = sizeof out[wire_row].updated_at;
      wire_values[wire_row * 8u + 7u] = out[wire_row].expires_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].expires_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WM_LIST, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

char *db1_wm_assemble_context(const char *session_id)
{
   if (!session_id || !session_id[0])
      return NULL;
   const char *fields[] = {session_id};
   char *value = malloc(524288u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {524288u};
   int wire_status = call_stage(AIMEE_DB1_OP_WM_ASSEMBLE_CONTEXT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

int db1_payload_rewrite_record(const char *session_id, int deferred, int bytes_saved, int new_payload_tokens, const char *reason, const char *new_prefix_hash)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", deferred);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", bytes_saved);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", new_payload_tokens);
   const char *fields[] = {session_id, arg1, arg2, arg3, reason ? reason : "", new_prefix_hash ? new_prefix_hash : ""};
   return write_result(call_stage(AIMEE_DB1_OP_REWRITE_RECORD, fields, 6, NULL, NULL, 0, NULL));
}

int db1_wm_search_session_ids(const char *query, char (*out)[WM_SESSION_ID_LEN], int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {query, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WM_SEARCH_SESSION_IDS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   return wire_rows;
}

int64_t db1_conv_record_event(const char *session_id, const char *tool_name, const char *tool_input, const char *tool_result, int result_bytes)
{
   if (!session_id || !session_id[0] || !tool_name || !tool_name[0])
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", result_bytes);
   const char *fields[] = {session_id, tool_name, tool_input ? tool_input : "", tool_result ? tool_result : "", arg4};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_RECORD_EVENT, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_conv_set_chain_id(int64_t event_id_first, int64_t event_id_last, int64_t chain_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)event_id_first);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)event_id_last);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)chain_id);
   const char *fields[] = {arg0, arg1, arg2};
   return write_result(call_stage(AIMEE_DB1_OP_CONV_SET_CHAIN_ID, fields, 3, NULL, NULL, 0, NULL));
}

int64_t db1_conv_insert_chain(const char *session_id, int64_t event_id_first, int64_t event_id_last, const char *tools, const char *stub, int raw_bytes, int stub_bytes)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)event_id_first);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)event_id_last);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", raw_bytes);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", stub_bytes);
   const char *fields[] = {session_id, arg1, arg2, tools ? tools : "", stub ? stub : "", arg5, arg6};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_INSERT_CHAIN, fields, 7, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_conv_pending_events(const char *session_id, conv_tool_event_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 8u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 8u + 2u] = out[wire_row].tool_name;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 8u + 3u] = out[wire_row].tool_input;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].tool_input;
      wire_values[wire_row * 8u + 4u] = out[wire_row].tool_result;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].tool_result;
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 3u + 2u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_PENDING_EVENTS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].result_bytes = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].chain_id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_list_chains(const char *session_id, conv_tool_chain_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 5u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 10u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 10u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 10u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 10u + 2u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 10u + 2u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 10u + 3u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 10u + 3u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 10u + 4u] = out[wire_row].tools;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].tools;
      wire_values[wire_row * 10u + 5u] = out[wire_row].stub;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].stub;
      wire_values[wire_row * 10u + 6u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 10u + 6u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 10u + 7u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 10u + 7u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 10u + 8u] = out[wire_row].state;
      wire_caps[wire_row * 10u + 8u] = sizeof out[wire_row].state;
      wire_values[wire_row * 10u + 9u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_LIST_CHAINS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 10), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 10u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 10u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].event_id_first = (int64_t)strtoll(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].event_id_last = (int64_t)strtoll(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].raw_bytes = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      out[wire_row].stub_bytes = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_chain_events(int64_t chain_id, conv_tool_event_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)chain_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 8u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 8u + 2u] = out[wire_row].tool_name;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 8u + 3u] = out[wire_row].tool_input;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].tool_input;
      wire_values[wire_row * 8u + 4u] = out[wire_row].tool_result;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].tool_result;
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 3u + 2u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_CHAIN_EVENTS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].result_bytes = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].chain_id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_search_chains(const char *session_id, const char *query, conv_tool_chain_t *out, int max)
{
   if (!session_id || !session_id[0] || !query || !query[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {session_id, query, arg2};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 5u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 10u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 10u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 10u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 10u + 2u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 10u + 2u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 10u + 3u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 10u + 3u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 10u + 4u] = out[wire_row].tools;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].tools;
      wire_values[wire_row * 10u + 5u] = out[wire_row].stub;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].stub;
      wire_values[wire_row * 10u + 6u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 10u + 6u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 10u + 7u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 10u + 7u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 10u + 8u] = out[wire_row].state;
      wire_caps[wire_row * 10u + 8u] = sizeof out[wire_row].state;
      wire_values[wire_row * 10u + 9u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_SEARCH_CHAINS, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 10), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 10u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 10u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].event_id_first = (int64_t)strtoll(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].event_id_last = (int64_t)strtoll(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].raw_bytes = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      out[wire_row].stub_bytes = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_state_get(const char *session_id, int64_t *last_event_id_out, int *chain_count_out, int *event_count_out)
{
   if (!session_id || !session_id[0] || !last_event_id_out || !chain_count_out || !event_count_out)
      return -1;
   const char *fields[] = {session_id};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char *const values[] = {slot0, slot1, slot2};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2};
   int wire_status = call_stage(AIMEE_DB1_OP_CONV_STATE_GET, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   *last_event_id_out = (int64_t)strtoll(slot0, NULL, 10);
   *chain_count_out = (int)strtol(slot1, NULL, 10);
   *event_count_out = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_conv_state_update(const char *session_id, int64_t last_event_id, int chain_count, int event_count)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)last_event_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", chain_count);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", event_count);
   const char *fields[] = {session_id, arg1, arg2, arg3};
   return write_result(call_stage(AIMEE_DB1_OP_CONV_STATE_UPDATE, fields, 4, NULL, NULL, 0, NULL));
}

int db1_windows_session_scan_state(const char *session_id, int *count_out, int *max_seq_out)
{
   if (!session_id || !session_id[0] || !count_out || !max_seq_out)
      return -1;
   const char *fields[] = {session_id};
   char slot0[32];
   char slot1[32];
   char *const values[] = {slot0, slot1};
   const size_t caps[] = {sizeof slot0, sizeof slot1};
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_SCAN_STATE, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   *count_out = (int)strtol(slot0, NULL, 10);
   *max_seq_out = (int)strtol(slot1, NULL, 10);
   return 0;
}

int db1_window_session_id(int64_t window_id, char *out, size_t out_sz)
{
   if (!out || out_sz == 0)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0};
   char *const values[] = {out};
   const size_t caps[] = {out_sz};
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_SESSION_ID, fields, 1, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

int64_t db1_window_create_raw(const char *session_id, int seq, const char *summary, const char *created_at)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", seq);
   const char *fields[] = {session_id, arg1, summary ? summary : "", created_at ? created_at : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_CREATE_RAW, fields, 4, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_window_add_term(int64_t window_id, const char *term)
{
   if (!term || !term[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0, term};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_ADD_TERM, fields, 2, NULL, NULL, 0, NULL));
}

int db1_window_add_file(int64_t window_id, const char *file_path)
{
   if (!file_path || !file_path[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0, file_path};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_ADD_FILE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_windows_list_ids_by_tier_before_days(const char *tier, int older_than_days, int64_t *out_ids, int max)
{
   if (!tier || !tier[0] || !out_ids || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", older_than_days);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {tier, arg1, arg2};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out_ids, 0, (size_t)max * sizeof *out_ids);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 1u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_IDS_BY_TIER, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out_ids[wire_row] = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_windows_find_candidates_by_terms(const char *const *terms, int term_count, db1_window_search_candidate_t *out, int max)
{
   if (!terms || term_count <= 0 || term_count > 32 || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[33];
   fields[0] = arg0;
   for (int at = 0; at < term_count; ++at)
      fields[1 + at] = terms[at] ? terms[at] : "";
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 6u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 6u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 6u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 6u + 2u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 6u + 2u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 6u + 3u] = out[wire_row].summary;
      wire_caps[wire_row * 6u + 3u] = sizeof out[wire_row].summary;
      wire_values[wire_row * 6u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 6u + 4u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 6u + 5u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 6u + 5u] = sizeof wire_scratch[wire_row * 3u + 2u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_CANDIDATES_BY_TERMS, fields, (uint32_t)(1 + term_count), wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].window_id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].seq = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].match_count = (int)strtol(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_window_list_files(int64_t window_id, char (*out)[MAX_PATH_LEN], int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_LIST_FILES, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   return wire_rows;
}

int db1_window_index_summary(int64_t window_id, const char *summary)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0, summary ? summary : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_INDEX_SUMMARY, fields, 2, NULL, NULL, 0, NULL));
}

int db1_windows_find_lexical_hits(const char *const *terms, int term_count, db1_window_lexical_hit_t *out, int max)
{
   if (!terms || term_count <= 0 || term_count > 32 || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[33];
   fields[0] = arg0;
   for (int at = 0; at < term_count; ++at)
      fields[1 + at] = terms[at] ? terms[at] : "";
   char **wire_values = malloc((size_t)max * 2u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 2u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 2u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 2u + 0u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 2u + 0u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 2u + 1u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 2u + 1u] = sizeof wire_scratch[wire_row * 2u + 1u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WINDOW_LEXICAL_HITS, fields, (uint32_t)(1 + term_count), wire_values, wire_caps,
                           (uint32_t)(max * 2), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 2u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 2u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].window_id = (int64_t)strtoll(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].rank = strtod(wire_scratch[wire_row * 2u + 1u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_window_set_tier(int64_t window_id, const char *tier)
{
   if (!tier || !tier[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0, tier};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_SET_TIER, fields, 2, NULL, NULL, 0, NULL));
}

int db1_window_prune_terms_keep_top(int64_t window_id, int keep)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", keep);
   const char *fields[] = {arg0, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_PRUNE_TERMS, fields, 2, NULL, NULL, 0, NULL));
}

int db1_window_delete_all_files(int64_t window_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_DELETE_ALL_FILES, fields, 1, NULL, NULL, 0, NULL));
}

int db1_window_prune_files_keep_top(int64_t window_id, int keep)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", keep);
   const char *fields[] = {arg0, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOW_PRUNE_FILES, fields, 2, NULL, NULL, 0, NULL));
}

int db1_windows_delete_after_turn(const char *session_id, int turn)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", turn);
   const char *fields[] = {session_id, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_WINDOWS_DELETE_AFTER_TURN, fields, 2, NULL, NULL, 0, NULL));
}

int db1_user_memory_list_recall(db1_user_recall_section_t section, db1_user_memory_row_t *rows, int cap)
{
   if (!rows || cap <= 0)
      return -1;
   if (cap > 32)
      cap = 32;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", section);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", cap);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)cap * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)cap * 5u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)cap * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(rows, 0, (size_t)cap * sizeof *rows);
   for (int wire_row = 0; wire_row < cap; ++wire_row)
   {
      wire_values[wire_row * 5u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 5u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 5u + 1u] = rows[wire_row].tier;
      wire_caps[wire_row * 5u + 1u] = sizeof rows[wire_row].tier;
      wire_values[wire_row * 5u + 2u] = rows[wire_row].kind;
      wire_caps[wire_row * 5u + 2u] = sizeof rows[wire_row].kind;
      wire_values[wire_row * 5u + 3u] = rows[wire_row].key;
      wire_caps[wire_row * 5u + 3u] = sizeof rows[wire_row].key;
      wire_values[wire_row * 5u + 4u] = rows[wire_row].content;
      wire_caps[wire_row * 5u + 4u] = sizeof rows[wire_row].content;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_USER_MEMORY_LIST_RECALL, fields, 2, wire_values, wire_caps,
                           (uint32_t)(cap * 5), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 5u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 5u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      rows[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_user_memory_any()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_USER_MEMORY_ANY, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_user_memory_upsert(const char *kind, const char *tier, const char *key, const char *content, double confidence, const char *source_session)
{
   if (!kind || !kind[0] || !tier || !tier[0] || !key || !key[0] || !content || !content[0])
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%.17g", (double)confidence);
   const char *fields[] = {kind, tier, key, content, arg4, source_session ? source_session : ""};
   return write_result(call_stage(AIMEE_DB1_OP_USER_MEMORY_UPSERT, fields, 6, NULL, NULL, 0, NULL));
}

/* clang-format on */
