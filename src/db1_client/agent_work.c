/* db1_client/agent_work.c: the agent_work family, reached over the bus.
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
 * Background work queues: the cognify queue's claim/mark bookkeeping is
 * machine-local runtime state, which is why it is DB1's even when the memory
 * it points at lives in DB2. *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
#include "agent_log.h"
#include "cognify_jobs.h"
#include "coord_jobs.h"
#include "db1_trigger.h"

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

#define DB1_AGENT_WORK_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.agent_work", "DB1 %s is unreachable (module call result %d)", "agent work",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_AGENT_WORK))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_AGENT_WORK_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_AGENT_WORK, AIMEE_DB1_STAGE_AGENT_WORK, 0, deadline,
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


int db1_cognify_job_enqueue(int64_t memory_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)memory_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_COGNIFY_ENQUEUE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_cognify_job_status(db1_cognify_job_stats_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   char *const values[] = {slot0, slot1, slot2, slot3, slot4};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_COGNIFY_STATUS, fields, 0, values, caps, 5, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->pending = (int)strtol(slot0, NULL, 10);
   out->running = (int)strtol(slot1, NULL, 10);
   out->done = (int)strtol(slot2, NULL, 10);
   out->failed = (int)strtol(slot3, NULL, 10);
   out->total = (int)strtol(slot4, NULL, 10);
   return 0;
}

int db1_cognify_job_claim_next(db1_cognify_job_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char *const values[] = {slot0, slot1, slot2, slot3, out->kind, out->status, out->claimed_by, out->claimed_at, out->last_error};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof out->kind, sizeof out->status, sizeof out->claimed_by, sizeof out->claimed_at, sizeof out->last_error};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_COGNIFY_CLAIM_NEXT, fields, 0, values, caps, 9, NULL);
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   out->memory_id = (int64_t)strtoll(slot1, NULL, 10);
   out->attempts = (int)strtol(slot2, NULL, 10);
   out->max_attempts = (int)strtol(slot3, NULL, 10);
   return 1;
}

int db1_cognify_job_mark(int64_t job_id, const char *status, const char *error)
{
   if (!status || !status[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)job_id);
   const char *fields[] = {arg0, status, error ? error : ""};
   return write_result(call_stage(AIMEE_DB1_OP_COGNIFY_MARK, fields, 3, NULL, NULL, 0, NULL));
}

long long db1_agent_log_insert(const db1_agent_log_insert_row_t *row)
{
   if (!row)
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", row->prompt_tokens);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", row->completion_tokens);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", row->latency_ms);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", row->success);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%d", row->turns);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", row->tool_calls);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%d", row->confidence);
   const char *fields[] = {row->agent_name, row->role, arg2, arg3, arg4, arg5, row->error ? row->error : "", arg7, arg8, arg9, row->session_id ? row->session_id : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int status = call_stage(AIMEE_DB1_OP_AGENT_LOG_INSERT, fields, 11, values, caps, 1, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_log_list_recent(db1_agent_log_display_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 13u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 13u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 8u * sizeof *wire_scratch);
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
      wire_values[wire_row * 13u + 0u] = wire_scratch[wire_row * 8u + 0u];
      wire_caps[wire_row * 13u + 0u] = sizeof wire_scratch[wire_row * 8u + 0u];
      wire_values[wire_row * 13u + 1u] = out[wire_row].agent_name;
      wire_caps[wire_row * 13u + 1u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 13u + 2u] = out[wire_row].role;
      wire_caps[wire_row * 13u + 2u] = sizeof out[wire_row].role;
      wire_values[wire_row * 13u + 3u] = wire_scratch[wire_row * 8u + 1u];
      wire_caps[wire_row * 13u + 3u] = sizeof wire_scratch[wire_row * 8u + 1u];
      wire_values[wire_row * 13u + 4u] = wire_scratch[wire_row * 8u + 2u];
      wire_caps[wire_row * 13u + 4u] = sizeof wire_scratch[wire_row * 8u + 2u];
      wire_values[wire_row * 13u + 5u] = wire_scratch[wire_row * 8u + 3u];
      wire_caps[wire_row * 13u + 5u] = sizeof wire_scratch[wire_row * 8u + 3u];
      wire_values[wire_row * 13u + 6u] = wire_scratch[wire_row * 8u + 4u];
      wire_caps[wire_row * 13u + 6u] = sizeof wire_scratch[wire_row * 8u + 4u];
      wire_values[wire_row * 13u + 7u] = wire_scratch[wire_row * 8u + 5u];
      wire_caps[wire_row * 13u + 7u] = sizeof wire_scratch[wire_row * 8u + 5u];
      wire_values[wire_row * 13u + 8u] = wire_scratch[wire_row * 8u + 6u];
      wire_caps[wire_row * 13u + 8u] = sizeof wire_scratch[wire_row * 8u + 6u];
      wire_values[wire_row * 13u + 9u] = wire_scratch[wire_row * 8u + 7u];
      wire_caps[wire_row * 13u + 9u] = sizeof wire_scratch[wire_row * 8u + 7u];
      wire_values[wire_row * 13u + 10u] = out[wire_row].session_id;
      wire_caps[wire_row * 13u + 10u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 13u + 11u] = out[wire_row].created_at;
      wire_caps[wire_row * 13u + 11u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 13u + 12u] = out[wire_row].error;
      wire_caps[wire_row * 13u + 12u] = sizeof out[wire_row].error;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_LIST_RECENT, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 13), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 13u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 13u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 8u + 0u], NULL, 10);
      out[wire_row].prompt_tokens = (int)strtol(wire_scratch[wire_row * 8u + 1u], NULL, 10);
      out[wire_row].completion_tokens = (int)strtol(wire_scratch[wire_row * 8u + 2u], NULL, 10);
      out[wire_row].latency_ms = (int)strtol(wire_scratch[wire_row * 8u + 3u], NULL, 10);
      out[wire_row].success = (int)strtol(wire_scratch[wire_row * 8u + 4u], NULL, 10);
      out[wire_row].turns = (int)strtol(wire_scratch[wire_row * 8u + 5u], NULL, 10);
      out[wire_row].tool_calls = (int)strtol(wire_scratch[wire_row * 8u + 6u], NULL, 10);
      out[wire_row].confidence = (int)strtol(wire_scratch[wire_row * 8u + 7u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_list_by_session(const char *session_id, db1_agent_log_display_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 13u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 13u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 8u * sizeof *wire_scratch);
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
      wire_values[wire_row * 13u + 0u] = wire_scratch[wire_row * 8u + 0u];
      wire_caps[wire_row * 13u + 0u] = sizeof wire_scratch[wire_row * 8u + 0u];
      wire_values[wire_row * 13u + 1u] = out[wire_row].agent_name;
      wire_caps[wire_row * 13u + 1u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 13u + 2u] = out[wire_row].role;
      wire_caps[wire_row * 13u + 2u] = sizeof out[wire_row].role;
      wire_values[wire_row * 13u + 3u] = wire_scratch[wire_row * 8u + 1u];
      wire_caps[wire_row * 13u + 3u] = sizeof wire_scratch[wire_row * 8u + 1u];
      wire_values[wire_row * 13u + 4u] = wire_scratch[wire_row * 8u + 2u];
      wire_caps[wire_row * 13u + 4u] = sizeof wire_scratch[wire_row * 8u + 2u];
      wire_values[wire_row * 13u + 5u] = wire_scratch[wire_row * 8u + 3u];
      wire_caps[wire_row * 13u + 5u] = sizeof wire_scratch[wire_row * 8u + 3u];
      wire_values[wire_row * 13u + 6u] = wire_scratch[wire_row * 8u + 4u];
      wire_caps[wire_row * 13u + 6u] = sizeof wire_scratch[wire_row * 8u + 4u];
      wire_values[wire_row * 13u + 7u] = wire_scratch[wire_row * 8u + 5u];
      wire_caps[wire_row * 13u + 7u] = sizeof wire_scratch[wire_row * 8u + 5u];
      wire_values[wire_row * 13u + 8u] = wire_scratch[wire_row * 8u + 6u];
      wire_caps[wire_row * 13u + 8u] = sizeof wire_scratch[wire_row * 8u + 6u];
      wire_values[wire_row * 13u + 9u] = wire_scratch[wire_row * 8u + 7u];
      wire_caps[wire_row * 13u + 9u] = sizeof wire_scratch[wire_row * 8u + 7u];
      wire_values[wire_row * 13u + 10u] = out[wire_row].session_id;
      wire_caps[wire_row * 13u + 10u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 13u + 11u] = out[wire_row].created_at;
      wire_caps[wire_row * 13u + 11u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 13u + 12u] = out[wire_row].error;
      wire_caps[wire_row * 13u + 12u] = sizeof out[wire_row].error;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_LIST_BY_SESSION, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 13), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 13u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 13u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 8u + 0u], NULL, 10);
      out[wire_row].prompt_tokens = (int)strtol(wire_scratch[wire_row * 8u + 1u], NULL, 10);
      out[wire_row].completion_tokens = (int)strtol(wire_scratch[wire_row * 8u + 2u], NULL, 10);
      out[wire_row].latency_ms = (int)strtol(wire_scratch[wire_row * 8u + 3u], NULL, 10);
      out[wire_row].success = (int)strtol(wire_scratch[wire_row * 8u + 4u], NULL, 10);
      out[wire_row].turns = (int)strtol(wire_scratch[wire_row * 8u + 5u], NULL, 10);
      out[wire_row].tool_calls = (int)strtol(wire_scratch[wire_row * 8u + 6u], NULL, 10);
      out[wire_row].confidence = (int)strtol(wire_scratch[wire_row * 8u + 7u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_search_session_ids_by_role(const char *pattern, char (*out_ids)[DB1_AL_SESSION_LEN], int max)
{
   if (!pattern || !pattern[0] || !out_ids || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {pattern, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out_ids, 0, (size_t)max * sizeof *out_ids);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out_ids[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out_ids[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_SEARCH_SESSIONS_BY_ROLE, fields, 2, wire_values, wire_caps,
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

int db1_agent_log_count_per_role(const char *since_or_null, db1_agent_log_role_count_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {since_or_null ? since_or_null : "", arg1};
   char **wire_values = malloc((size_t)max * 2u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 2u * sizeof *wire_caps);
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
      wire_values[wire_row * 2u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 2u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 2u + 1u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 2u + 1u] = sizeof wire_scratch[wire_row * 1u + 0u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_COUNT_PER_ROLE, fields, 2, wire_values, wire_caps,
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
      out[wire_row].count = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_failures_since_seconds(int max_rows, int since_secs, db1_agent_log_failure_t *out)
{
   if (!out || max_rows <= 0)
      return -1;
   if (max_rows > 64)
      max_rows = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max_rows);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", since_secs);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max_rows * 3u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max_rows * 3u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max_rows * sizeof *out);
   for (int wire_row = 0; wire_row < max_rows; ++wire_row)
   {
      wire_values[wire_row * 3u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 3u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 3u + 1u] = out[wire_row].error;
      wire_caps[wire_row * 3u + 1u] = sizeof out[wire_row].error;
      wire_values[wire_row * 3u + 2u] = out[wire_row].created_at;
      wire_caps[wire_row * 3u + 2u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_FAILURES_SINCE, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max_rows * 3), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 3u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 3u);
   return wire_rows;
}

int db1_agent_log_list_recent_errors(int since_days, db1_agent_log_recent_error_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_days);
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
      wire_values[wire_row * 1u + 0u] = out[wire_row].error;
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row].error;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_LIST_RECENT_ERRORS, fields, 2, wire_values, wire_caps,
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

int db1_agent_log_list_delegation_patterns(int since_days, int min_total, db1_agent_log_delegation_pattern_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_days);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", min_total);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {arg0, arg1, arg2};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
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
      wire_values[wire_row * 8u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 8u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 8u + 1u] = out[wire_row].agent_name;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 8u + 2u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 8u + 2u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 8u + 3u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 8u + 3u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 8u + 4u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 8u + 4u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].recent_error;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].recent_error;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_DELEGATION_PATTERNS, fields, 3, wire_values, wire_caps,
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
      out[wire_row].wins = (int)strtol(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].fails = (int)strtol(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].total = (int)strtol(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].avg_turns = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      out[wire_row].avg_tools = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_list_failure_episode_seeds(int since_days, int min_fails, db1_agent_log_failure_episode_seed_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_days);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", min_fails);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {arg0, arg1, arg2};
   char **wire_values = malloc((size_t)max * 4u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 4u * sizeof *wire_caps);
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
      wire_values[wire_row * 4u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 4u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 4u + 1u] = out[wire_row].agent_name;
      wire_caps[wire_row * 4u + 1u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 4u + 2u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 4u + 2u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 4u + 3u] = out[wire_row].errors;
      wire_caps[wire_row * 4u + 3u] = sizeof out[wire_row].errors;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_FAILURE_SEEDS, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 4), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 4u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 4u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].fails = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_metrics_by_role(db1_agent_log_metric_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 7u * sizeof *wire_scratch);
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
      wire_values[wire_row * 8u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 8u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 8u + 1u] = wire_scratch[wire_row * 7u + 0u];
      wire_caps[wire_row * 8u + 1u] = sizeof wire_scratch[wire_row * 7u + 0u];
      wire_values[wire_row * 8u + 2u] = wire_scratch[wire_row * 7u + 1u];
      wire_caps[wire_row * 8u + 2u] = sizeof wire_scratch[wire_row * 7u + 1u];
      wire_values[wire_row * 8u + 3u] = wire_scratch[wire_row * 7u + 2u];
      wire_caps[wire_row * 8u + 3u] = sizeof wire_scratch[wire_row * 7u + 2u];
      wire_values[wire_row * 8u + 4u] = wire_scratch[wire_row * 7u + 3u];
      wire_caps[wire_row * 8u + 4u] = sizeof wire_scratch[wire_row * 7u + 3u];
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 7u + 4u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 7u + 4u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 7u + 5u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 7u + 5u];
      wire_values[wire_row * 8u + 7u] = wire_scratch[wire_row * 7u + 6u];
      wire_caps[wire_row * 8u + 7u] = sizeof wire_scratch[wire_row * 7u + 6u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_METRICS_BY_ROLE, fields, 1, wire_values, wire_caps,
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
      out[wire_row].total = (int)strtol(wire_scratch[wire_row * 7u + 0u], NULL, 10);
      out[wire_row].successes = (int)strtol(wire_scratch[wire_row * 7u + 1u], NULL, 10);
      out[wire_row].avg_latency_ms = (int)strtol(wire_scratch[wire_row * 7u + 2u], NULL, 10);
      out[wire_row].tokens = (int64_t)strtoll(wire_scratch[wire_row * 7u + 3u], NULL, 10);
      out[wire_row].cache_write_tokens = (int64_t)strtoll(wire_scratch[wire_row * 7u + 4u], NULL, 10);
      out[wire_row].cache_read_tokens = (int64_t)strtoll(wire_scratch[wire_row * 7u + 5u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 7u + 6u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_agent_stats(const char *agent_name_or_null, db1_agent_log_agent_stats_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {agent_name_or_null ? agent_name_or_null : "", arg1};
   char **wire_values = malloc((size_t)max * 9u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 9u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 8u * sizeof *wire_scratch);
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
      wire_values[wire_row * 9u + 0u] = out[wire_row].agent_name;
      wire_caps[wire_row * 9u + 0u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 9u + 1u] = wire_scratch[wire_row * 8u + 0u];
      wire_caps[wire_row * 9u + 1u] = sizeof wire_scratch[wire_row * 8u + 0u];
      wire_values[wire_row * 9u + 2u] = wire_scratch[wire_row * 8u + 1u];
      wire_caps[wire_row * 9u + 2u] = sizeof wire_scratch[wire_row * 8u + 1u];
      wire_values[wire_row * 9u + 3u] = wire_scratch[wire_row * 8u + 2u];
      wire_caps[wire_row * 9u + 3u] = sizeof wire_scratch[wire_row * 8u + 2u];
      wire_values[wire_row * 9u + 4u] = wire_scratch[wire_row * 8u + 3u];
      wire_caps[wire_row * 9u + 4u] = sizeof wire_scratch[wire_row * 8u + 3u];
      wire_values[wire_row * 9u + 5u] = wire_scratch[wire_row * 8u + 4u];
      wire_caps[wire_row * 9u + 5u] = sizeof wire_scratch[wire_row * 8u + 4u];
      wire_values[wire_row * 9u + 6u] = wire_scratch[wire_row * 8u + 5u];
      wire_caps[wire_row * 9u + 6u] = sizeof wire_scratch[wire_row * 8u + 5u];
      wire_values[wire_row * 9u + 7u] = wire_scratch[wire_row * 8u + 6u];
      wire_caps[wire_row * 9u + 7u] = sizeof wire_scratch[wire_row * 8u + 6u];
      wire_values[wire_row * 9u + 8u] = wire_scratch[wire_row * 8u + 7u];
      wire_caps[wire_row * 9u + 8u] = sizeof wire_scratch[wire_row * 8u + 7u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_AGENT_STATS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 9), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 9u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 9u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].total_calls = (int)strtol(wire_scratch[wire_row * 8u + 0u], NULL, 10);
      out[wire_row].total_prompt_tokens = (int)strtol(wire_scratch[wire_row * 8u + 1u], NULL, 10);
      out[wire_row].total_completion_tokens = (int)strtol(wire_scratch[wire_row * 8u + 2u], NULL, 10);
      out[wire_row].avg_latency_ms = (int)strtol(wire_scratch[wire_row * 8u + 3u], NULL, 10);
      out[wire_row].success_rate = strtod(wire_scratch[wire_row * 8u + 4u], NULL);
      out[wire_row].total_cache_write_tokens = (int64_t)strtoll(wire_scratch[wire_row * 8u + 5u], NULL, 10);
      out[wire_row].total_cache_read_tokens = (int64_t)strtoll(wire_scratch[wire_row * 8u + 6u], NULL, 10);
      out[wire_row].total_estimated_cost_usd = strtod(wire_scratch[wire_row * 8u + 7u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_hud_summary(db1_agent_log_hud_t *out, int recent_secs)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", recent_secs);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   char slot5[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   char *const values[] = {slot0, slot1, slot2, slot3, slot4, slot5, slot6, slot7, slot8, slot9};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4, sizeof slot5, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_AGENT_LOG_HUD_SUMMARY, fields, 1, values, caps, 10, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->total_calls = (int)strtol(slot0, NULL, 10);
   out->successful_calls = (int)strtol(slot1, NULL, 10);
   out->failed_calls = (int)strtol(slot2, NULL, 10);
   out->total_prompt_tokens = (int64_t)strtoll(slot3, NULL, 10);
   out->total_completion_tokens = (int64_t)strtoll(slot4, NULL, 10);
   out->total_turns = (int)strtol(slot5, NULL, 10);
   out->total_tool_calls = (int)strtol(slot6, NULL, 10);
   out->avg_latency_ms = strtod(slot7, NULL);
   out->recent_calls = (int)strtol(slot8, NULL, 10);
   out->recent_successes = (int)strtol(slot9, NULL, 10);
   return 0;
}

int db1_agent_log_session_outcome(const char *session_id, int *successes_out, int *total_out)
{
   if (!session_id || !session_id[0] || !successes_out || !total_out)
      return -1;
   const char *fields[] = {session_id};
   char slot0[32];
   char slot1[32];
   char *const values[] = {slot0, slot1};
   const size_t caps[] = {sizeof slot0, sizeof slot1};
   int status = call_stage(AIMEE_DB1_OP_AGENT_LOG_SESSION_OUTCOME, fields, 1, values, caps, 2, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   *successes_out = (int)strtol(slot0, NULL, 10);
   *total_out = (int)strtol(slot1, NULL, 10);
   return 0;
}

int db1_agent_log_prometheus(db1_agent_log_prometheus_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 6u * sizeof *wire_scratch);
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
      wire_values[wire_row * 8u + 0u] = out[wire_row].agent_name;
      wire_caps[wire_row * 8u + 0u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 8u + 1u] = out[wire_row].role;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].role;
      wire_values[wire_row * 8u + 2u] = wire_scratch[wire_row * 6u + 0u];
      wire_caps[wire_row * 8u + 2u] = sizeof wire_scratch[wire_row * 6u + 0u];
      wire_values[wire_row * 8u + 3u] = wire_scratch[wire_row * 6u + 1u];
      wire_caps[wire_row * 8u + 3u] = sizeof wire_scratch[wire_row * 6u + 1u];
      wire_values[wire_row * 8u + 4u] = wire_scratch[wire_row * 6u + 2u];
      wire_caps[wire_row * 8u + 4u] = sizeof wire_scratch[wire_row * 6u + 2u];
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 6u + 3u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 6u + 3u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 6u + 4u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 6u + 4u];
      wire_values[wire_row * 8u + 7u] = wire_scratch[wire_row * 6u + 5u];
      wire_caps[wire_row * 8u + 7u] = sizeof wire_scratch[wire_row * 6u + 5u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_PROMETHEUS, fields, 1, wire_values, wire_caps,
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
      out[wire_row].total = (int)strtol(wire_scratch[wire_row * 6u + 0u], NULL, 10);
      out[wire_row].successes = (int)strtol(wire_scratch[wire_row * 6u + 1u], NULL, 10);
      out[wire_row].prompt_tokens = (int)strtol(wire_scratch[wire_row * 6u + 2u], NULL, 10);
      out[wire_row].completion_tokens = (int)strtol(wire_scratch[wire_row * 6u + 3u], NULL, 10);
      out[wire_row].avg_latency_ms = (int)strtol(wire_scratch[wire_row * 6u + 4u], NULL, 10);
      out[wire_row].tool_calls = (int)strtol(wire_scratch[wire_row * 6u + 5u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_log_stats(const char *since_or_null, db1_agent_log_stats_t *out)
{
   if (!out)
      return -1;
   const char *fields[] = {since_or_null ? since_or_null : ""};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   char slot5[32];
   char *const values[] = {slot0, slot1, slot2, slot3, slot4, slot5};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4, sizeof slot5};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_AGENT_LOG_STATS, fields, 1, values, caps, 6, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->total = (int)strtol(slot0, NULL, 10);
   out->turns = (int64_t)strtoll(slot1, NULL, 10);
   out->tool_calls = (int64_t)strtoll(slot2, NULL, 10);
   out->prompt_tokens = (int64_t)strtoll(slot3, NULL, 10);
   out->completion_tokens = (int64_t)strtoll(slot4, NULL, 10);
   out->successes = (int)strtol(slot5, NULL, 10);
   return 0;
}

int db1_trigger_insert(const char *id, const char *source, const char *event, const char *task, const char *workspace, const char *metadata)
{
   if (!id || !id[0] || !source || !source[0] || !task || !task[0] || !metadata || !metadata[0])
      return -1;
   const char *fields[] = {id, source, event ? event : "", task, workspace ? workspace : "", metadata};
   return write_result(call_stage(AIMEE_DB1_OP_TRIGGER_INSERT, fields, 6, NULL, NULL, 0, NULL));
}

int db1_trigger_status_set(const char *id, const char *status, const char *pipeline_id, const char *error)
{
   if (!id || !id[0] || !status || !status[0])
      return -1;
   const char *fields[] = {id, status, pipeline_id ? pipeline_id : "", error ? error : ""};
   return write_result(call_stage(AIMEE_DB1_OP_TRIGGER_STATUS_SET, fields, 4, NULL, NULL, 0, NULL));
}

int db1_trigger_get(const char *id, db1_trigger_run_t *out)
{
   if (!id || !id[0] || !out)
      return -1;
   const char *fields[] = {id};
   char *const values[] = {out->id, out->source, out->event, out->task, out->workspace, out->metadata, out->pipeline_id, out->status, out->queued_at, out->started_at, out->finished_at, out->error};
   const size_t caps[] = {sizeof out->id, sizeof out->source, sizeof out->event, sizeof out->task, sizeof out->workspace, sizeof out->metadata, sizeof out->pipeline_id, sizeof out->status, sizeof out->queued_at, sizeof out->started_at, sizeof out->finished_at, sizeof out->error};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_TRIGGER_GET, fields, 1, values, caps, 12, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return 0;
}

char *db1_trigger_list_json(const char *status_filter)
{
   const char *fields[] = {status_filter ? status_filter : ""};
   char *value = malloc(524288u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {524288u};
   int status = call_stage(AIMEE_DB1_OP_TRIGGER_LIST_JSON, fields, 1, values, caps, 1, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

int db1_coord_job_create(int plan_id, int max_concurrent)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max_concurrent);
   const char *fields[] = {arg0, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int status = call_stage(AIMEE_DB1_OP_COORD_JOB_CREATE, fields, 2, values, caps, 1, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_coord_job_add_task(int job_id, int step_id, const char *files_json, const char *role, const char *prompt, const char *cwd, const char *persona)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", step_id);
   const char *fields[] = {arg0, arg1, files_json ? files_json : "", role ? role : "", prompt ? prompt : "", cwd ? cwd : "", persona ? persona : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int status = call_stage(AIMEE_DB1_OP_COORD_TASK_ADD, fields, 7, values, caps, 1, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_coord_job_claim_next(int job_id, const char *delegate_name, db1_coord_task_t *out)
{
   if (!delegate_name || !delegate_name[0] || !out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0, delegate_name};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot9[32];
   char *const values[] = {slot0, slot1, slot2, out->status, out->claimed_by, out->claimed_at, out->files, out->result, out->error, slot9, out->created_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->status, sizeof out->claimed_by, sizeof out->claimed_at, sizeof out->files, sizeof out->result, sizeof out->error, sizeof slot9, sizeof out->created_at};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_COORD_TASK_CLAIM_NEXT, fields, 2, values, caps, 11, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->id = (int)strtol(slot0, NULL, 10);
   out->job_id = (int)strtol(slot1, NULL, 10);
   out->step_id = (int)strtol(slot2, NULL, 10);
   out->preempt_requeues = (int)strtol(slot9, NULL, 10);
   return out->id;
}

int db1_coord_job_complete_task(int task_id, const char *result)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   const char *fields[] = {arg0, result ? result : ""};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_COMPLETE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_coord_job_fail_task(int task_id, const char *error)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   const char *fields[] = {arg0, error ? error : ""};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_FAIL, fields, 2, NULL, NULL, 0, NULL));
}

int db1_coord_job_complete_task_owned(int task_id, const char *claimed_by, const char *result)
{
   if (!claimed_by || !claimed_by[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   const char *fields[] = {arg0, claimed_by, result ? result : ""};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_COMPLETE_OWNED, fields, 3, NULL, NULL, 0, NULL));
}

int db1_coord_job_fail_task_owned(int task_id, const char *claimed_by, const char *error)
{
   if (!claimed_by || !claimed_by[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   const char *fields[] = {arg0, claimed_by, error ? error : ""};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_FAIL_OWNED, fields, 3, NULL, NULL, 0, NULL));
}

int db1_coord_job_release_task(int task_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_RELEASE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_coord_job_release_task_bounded(int task_id, int max_requeues)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max_requeues);
   const char *fields[] = {arg0, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_RELEASE_BOUNDED, fields, 2, NULL, NULL, 0, NULL));
}

int db1_coord_job_release_task_bounded_owned(int task_id, const char *claimed_by, int max_requeues)
{
   if (!claimed_by || !claimed_by[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max_requeues);
   const char *fields[] = {arg0, claimed_by, arg2};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_TASK_RELEASE_BOUNDED_OWNED, fields, 3, NULL, NULL, 0, NULL));
}

int db1_coord_job_recover_owner(const char *claimed_by, int max_requeues, int *requeued_out, int *failed_out)
{
   if (!claimed_by || !claimed_by[0] || !requeued_out || !failed_out)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max_requeues);
   const char *fields[] = {claimed_by, arg1};
   char slot0[32];
   char slot1[32];
   char *const values[] = {slot0, slot1};
   const size_t caps[] = {sizeof slot0, sizeof slot1};
   int status = call_stage(AIMEE_DB1_OP_COORD_OWNER_RECOVER, fields, 2, values, caps, 2, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   *requeued_out = (int)strtol(slot0, NULL, 10);
   *failed_out = (int)strtol(slot1, NULL, 10);
   return 0;
}

int db1_coord_job_get(int job_id, db1_coord_job_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot3[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   char *const values[] = {slot0, slot1, out->status, slot3, out->created_at, out->updated_at, slot6, slot7, slot8, slot9};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof out->status, sizeof slot3, sizeof out->created_at, sizeof out->updated_at, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9};
   memset(out, 0, sizeof *out);
   int status = call_stage(AIMEE_DB1_OP_COORD_JOB_GET, fields, 1, values, caps, 10, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   out->id = (int)strtol(slot0, NULL, 10);
   out->plan_id = (int)strtol(slot1, NULL, 10);
   out->max_concurrent = (int)strtol(slot3, NULL, 10);
   out->total_tasks = (int)strtol(slot6, NULL, 10);
   out->done_tasks = (int)strtol(slot7, NULL, 10);
   out->failed_tasks = (int)strtol(slot8, NULL, 10);
   out->running_tasks = (int)strtol(slot9, NULL, 10);
   return 0;
}

int db1_coord_job_list_tasks(int job_id, db1_coord_task_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 11u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 11u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 4u * sizeof *wire_scratch);
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
      wire_values[wire_row * 11u + 0u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 11u + 0u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 11u + 1u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 11u + 1u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 11u + 2u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 11u + 2u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 11u + 3u] = out[wire_row].status;
      wire_caps[wire_row * 11u + 3u] = sizeof out[wire_row].status;
      wire_values[wire_row * 11u + 4u] = out[wire_row].claimed_by;
      wire_caps[wire_row * 11u + 4u] = sizeof out[wire_row].claimed_by;
      wire_values[wire_row * 11u + 5u] = out[wire_row].claimed_at;
      wire_caps[wire_row * 11u + 5u] = sizeof out[wire_row].claimed_at;
      wire_values[wire_row * 11u + 6u] = out[wire_row].files;
      wire_caps[wire_row * 11u + 6u] = sizeof out[wire_row].files;
      wire_values[wire_row * 11u + 7u] = out[wire_row].result;
      wire_caps[wire_row * 11u + 7u] = sizeof out[wire_row].result;
      wire_values[wire_row * 11u + 8u] = out[wire_row].error;
      wire_caps[wire_row * 11u + 8u] = sizeof out[wire_row].error;
      wire_values[wire_row * 11u + 9u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 11u + 9u] = sizeof wire_scratch[wire_row * 4u + 3u];
      wire_values[wire_row * 11u + 10u] = out[wire_row].created_at;
      wire_caps[wire_row * 11u + 10u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_COORD_TASK_LIST, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 11), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 11u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 11u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].job_id = (int)strtol(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].step_id = (int)strtol(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].preempt_requeues = (int)strtol(wire_scratch[wire_row * 4u + 3u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_coord_job_cancel(int job_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_JOB_CANCEL, fields, 1, NULL, NULL, 0, NULL));
}

int db1_coord_job_refresh_status(int job_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_COORD_JOB_REFRESH_STATUS, fields, 1, NULL, NULL, 0, NULL));
}

int db1_coord_job_has_file_conflict(int job_id, const char *files_json)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0, files_json ? files_json : ""};
   int status = call_stage(AIMEE_DB1_OP_COORD_JOB_FILE_CONFLICT, fields, 2, NULL, NULL, 0, NULL);
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return status == (int)AIMEE_DB1_STATUS_OK ? 1 : -1;
}

int db1_coord_job_list_recent(db1_coord_job_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 7u * sizeof *wire_scratch);
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
      wire_values[wire_row * 10u + 0u] = wire_scratch[wire_row * 7u + 0u];
      wire_caps[wire_row * 10u + 0u] = sizeof wire_scratch[wire_row * 7u + 0u];
      wire_values[wire_row * 10u + 1u] = wire_scratch[wire_row * 7u + 1u];
      wire_caps[wire_row * 10u + 1u] = sizeof wire_scratch[wire_row * 7u + 1u];
      wire_values[wire_row * 10u + 2u] = out[wire_row].status;
      wire_caps[wire_row * 10u + 2u] = sizeof out[wire_row].status;
      wire_values[wire_row * 10u + 3u] = wire_scratch[wire_row * 7u + 2u];
      wire_caps[wire_row * 10u + 3u] = sizeof wire_scratch[wire_row * 7u + 2u];
      wire_values[wire_row * 10u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 10u + 5u] = out[wire_row].updated_at;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].updated_at;
      wire_values[wire_row * 10u + 6u] = wire_scratch[wire_row * 7u + 3u];
      wire_caps[wire_row * 10u + 6u] = sizeof wire_scratch[wire_row * 7u + 3u];
      wire_values[wire_row * 10u + 7u] = wire_scratch[wire_row * 7u + 4u];
      wire_caps[wire_row * 10u + 7u] = sizeof wire_scratch[wire_row * 7u + 4u];
      wire_values[wire_row * 10u + 8u] = wire_scratch[wire_row * 7u + 5u];
      wire_caps[wire_row * 10u + 8u] = sizeof wire_scratch[wire_row * 7u + 5u];
      wire_values[wire_row * 10u + 9u] = wire_scratch[wire_row * 7u + 6u];
      wire_caps[wire_row * 10u + 9u] = sizeof wire_scratch[wire_row * 7u + 6u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_COORD_JOB_LIST_RECENT, fields, 1, wire_values, wire_caps,
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
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 7u + 0u], NULL, 10);
      out[wire_row].plan_id = (int)strtol(wire_scratch[wire_row * 7u + 1u], NULL, 10);
      out[wire_row].max_concurrent = (int)strtol(wire_scratch[wire_row * 7u + 2u], NULL, 10);
      out[wire_row].total_tasks = (int)strtol(wire_scratch[wire_row * 7u + 3u], NULL, 10);
      out[wire_row].done_tasks = (int)strtol(wire_scratch[wire_row * 7u + 4u], NULL, 10);
      out[wire_row].failed_tasks = (int)strtol(wire_scratch[wire_row * 7u + 5u], NULL, 10);
      out[wire_row].running_tasks = (int)strtol(wire_scratch[wire_row * 7u + 6u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_coord_job_list_active_ids(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
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
   int wire_status = call_stage(AIMEE_DB1_OP_COORD_JOB_LIST_ACTIVE_IDS, fields, 1, wire_values, wire_caps,
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
      out_ids[wire_row] = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_coord_task_get_dispatch(int task_id, char *role_out, size_t role_cap, char *prompt_out, size_t prompt_cap, char *files_out, size_t files_cap, char *cwd_out, size_t cwd_cap, char *persona_out, size_t persona_cap)
{
   if (!role_out || role_cap == 0 || !prompt_out || prompt_cap == 0 || !files_out || files_cap == 0 || !cwd_out || cwd_cap == 0 || !persona_out || persona_cap == 0)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", task_id);
   const char *fields[] = {arg0};
   char *const values[] = {role_out, prompt_out, files_out, cwd_out, persona_out};
   const size_t caps[] = {role_cap, prompt_cap, files_cap, cwd_cap, persona_cap};
   int status = call_stage(AIMEE_DB1_OP_COORD_TASK_GET_DISPATCH, fields, 1, values, caps, 5, NULL);
   if (status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return 0;
}

/* clang-format on */
