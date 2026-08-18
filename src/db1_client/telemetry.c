/* db1_client/telemetry.c: the telemetry family, reached over the bus.
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
#include "cost_fold.h"
#include "diagnose.h"
#include "eval.h"
#include "guardrail_events.h"
#include "interaction_events.h"
#include "token_audit.h"

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

#define DB1_TELEMETRY_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.telemetry", "DB1 %s is unreachable (module call result %d)", "telemetry",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_TELEMETRY))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_TELEMETRY_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_TELEMETRY, AIMEE_DB1_STAGE_TELEMETRY, 0, deadline,
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


int db1_token_audit_insert(const db1_token_audit_row_t *row)
{
   if (!row)
      return -1;
   char arg10[32];
   snprintf(arg10, sizeof arg10, "%lld", (long long)row->agent_log_id);
   char arg13[32];
   snprintf(arg13, sizeof arg13, "%d", row->attempt);
   char arg16[32];
   snprintf(arg16, sizeof arg16, "%d", row->duration_ms);
   char arg18[32];
   snprintf(arg18, sizeof arg18, "%d", row->prompt_tokens);
   char arg19[32];
   snprintf(arg19, sizeof arg19, "%d", row->completion_tokens);
   char arg20[32];
   snprintf(arg20, sizeof arg20, "%d", row->cache_write_tokens);
   char arg21[32];
   snprintf(arg21, sizeof arg21, "%d", row->cache_read_tokens);
   char arg22[32];
   snprintf(arg22, sizeof arg22, "%.17g", (double)row->estimated_cost_usd);
   const char *fields[] = {row->session_id ? row->session_id : "", row->delegation_id ? row->delegation_id : "", row->project_name ? row->project_name : "", row->tool_name ? row->tool_name : "", row->role ? row->role : "", row->model ? row->model : "", row->source ? row->source : "", row->requested_model ? row->requested_model : "", row->stop_reason ? row->stop_reason : "", row->usage_kind ? row->usage_kind : "", arg10, row->request_id ? row->request_id : "", row->idempotency_key ? row->idempotency_key : "", arg13, row->principal ? row->principal : "", row->served_model ? row->served_model : "", arg16, row->metadata ? row->metadata : "", arg18, arg19, arg20, arg21, arg22};
   return write_result(call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_INSERT, fields, 23, NULL, NULL, 0, NULL));
}

void db1_token_audit_ensure_idem_index()
{
   const char *const *fields = NULL;
   (void)call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_ENSURE_IDEM_INDEX, fields, 0, NULL, NULL, 0, NULL);
}

double db1_token_audit_cost_for_delegation(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   const char *fields[] = {delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_COST_FOR_DELEGATION, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (double)strtod(slot0, NULL);
}

int db1_token_audit_cost_for_delegation_ex(const char *delegation_id, double *out_cost)
{
   if (!delegation_id || !delegation_id[0] || !out_cost)
      return -1;
   const char *fields[] = {delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_COST_FOR_DELEGATION_EX, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_cost = strtod(slot0, NULL);
   return 0;
}

int db1_token_audit_session_split(const char *session_id, db1_token_audit_session_split_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   const char *fields[] = {session_id};
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
   char slot10[32];
   char slot11[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, slot3, slot4, slot5, slot6, slot7, slot8, slot9, slot10, slot11};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4, sizeof slot5, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9, sizeof slot10, sizeof slot11};
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_SESSION_SPLIT, fields, 1, values, caps, 12, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->supervisor_calls = (int)strtol(slot0, NULL, 10);
   out->supervisor_prompt_tokens = (int64_t)strtoll(slot1, NULL, 10);
   out->supervisor_completion_tokens = (int64_t)strtoll(slot2, NULL, 10);
   out->supervisor_cache_write_tokens = (int64_t)strtoll(slot3, NULL, 10);
   out->supervisor_cache_read_tokens = (int64_t)strtoll(slot4, NULL, 10);
   out->supervisor_cost_usd = strtod(slot5, NULL);
   out->worker_calls = (int)strtol(slot6, NULL, 10);
   out->worker_prompt_tokens = (int64_t)strtoll(slot7, NULL, 10);
   out->worker_completion_tokens = (int64_t)strtoll(slot8, NULL, 10);
   out->worker_cache_write_tokens = (int64_t)strtoll(slot9, NULL, 10);
   out->worker_cache_read_tokens = (int64_t)strtoll(slot10, NULL, 10);
   out->worker_cost_usd = strtod(slot11, NULL);
   return 0;
}

int db1_token_audit_totals(int since_hours, db1_token_audit_totals_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   char slot5[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, slot3, slot4, slot5};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4, sizeof slot5};
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_TOTALS, fields, 1, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->total_calls = (int)strtol(slot0, NULL, 10);
   out->prompt_tokens = (int64_t)strtoll(slot1, NULL, 10);
   out->completion_tokens = (int64_t)strtoll(slot2, NULL, 10);
   out->cache_write_tokens = (int64_t)strtoll(slot3, NULL, 10);
   out->cache_read_tokens = (int64_t)strtoll(slot4, NULL, 10);
   out->estimated_cost_usd = strtod(slot5, NULL);
   return 0;
}

int db1_token_audit_spend_breakdown(int since_hours, db1_token_audit_spend_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, slot3, slot4};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4};
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_SPEND_BREAKDOWN, fields, 1, values, caps, 5, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->realized_cost_usd = strtod(slot0, NULL);
   out->estimated_cost_usd = strtod(slot1, NULL);
   out->avoided_cost_usd = strtod(slot2, NULL);
   out->partial_cost_usd = strtod(slot3, NULL);
   out->spend_cost_usd = strtod(slot4, NULL);
   return 0;
}

int db1_token_audit_by_role(int since_hours, db1_token_audit_role_summary_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
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
      wire_values[wire_row * 5u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 5u + 1u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 5u + 1u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 5u + 4u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 5u + 4u] = sizeof wire_scratch[wire_row * 4u + 3u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_BY_ROLE, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
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
      out[wire_row].calls = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].prompt_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].completion_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 4u + 3u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_token_audit_by_tool(int since_hours, db1_token_audit_tool_summary_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
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
      wire_values[wire_row * 5u + 0u] = out[wire_row].tool_name;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 5u + 1u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 5u + 1u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 5u + 4u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 5u + 4u] = sizeof wire_scratch[wire_row * 4u + 3u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_BY_TOOL, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
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
      out[wire_row].calls = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].prompt_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].completion_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 4u + 3u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_token_audit_by_model(int since_hours, db1_token_audit_model_summary_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
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
      wire_values[wire_row * 5u + 0u] = out[wire_row].model;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].model;
      wire_values[wire_row * 5u + 1u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 5u + 1u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 5u + 4u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 5u + 4u] = sizeof wire_scratch[wire_row * 4u + 3u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_BY_MODEL, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
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
      out[wire_row].calls = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].prompt_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].completion_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 4u + 3u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_token_audit_by_source(int since_hours, db1_token_audit_source_summary_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
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
      wire_values[wire_row * 5u + 0u] = out[wire_row].source;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].source;
      wire_values[wire_row * 5u + 1u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 5u + 1u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 5u + 4u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 5u + 4u] = sizeof wire_scratch[wire_row * 4u + 3u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_BY_SOURCE, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
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
      out[wire_row].calls = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].prompt_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].completion_tokens = (int64_t)strtoll(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 4u + 3u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_token_audit_list_dashboard(db1_token_audit_dashboard_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 9u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 9u * sizeof *wire_caps);
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
      wire_values[wire_row * 9u + 0u] = out[wire_row].tool_name;
      wire_caps[wire_row * 9u + 0u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 9u + 1u] = out[wire_row].role;
      wire_caps[wire_row * 9u + 1u] = sizeof out[wire_row].role;
      wire_values[wire_row * 9u + 2u] = wire_scratch[wire_row * 6u + 0u];
      wire_caps[wire_row * 9u + 2u] = sizeof wire_scratch[wire_row * 6u + 0u];
      wire_values[wire_row * 9u + 3u] = wire_scratch[wire_row * 6u + 1u];
      wire_caps[wire_row * 9u + 3u] = sizeof wire_scratch[wire_row * 6u + 1u];
      wire_values[wire_row * 9u + 4u] = wire_scratch[wire_row * 6u + 2u];
      wire_caps[wire_row * 9u + 4u] = sizeof wire_scratch[wire_row * 6u + 2u];
      wire_values[wire_row * 9u + 5u] = wire_scratch[wire_row * 6u + 3u];
      wire_caps[wire_row * 9u + 5u] = sizeof wire_scratch[wire_row * 6u + 3u];
      wire_values[wire_row * 9u + 6u] = wire_scratch[wire_row * 6u + 4u];
      wire_caps[wire_row * 9u + 6u] = sizeof wire_scratch[wire_row * 6u + 4u];
      wire_values[wire_row * 9u + 7u] = wire_scratch[wire_row * 6u + 5u];
      wire_caps[wire_row * 9u + 7u] = sizeof wire_scratch[wire_row * 6u + 5u];
      wire_values[wire_row * 9u + 8u] = out[wire_row].last_seen;
      wire_caps[wire_row * 9u + 8u] = sizeof out[wire_row].last_seen;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_TOKEN_AUDIT_LIST_DASHBOARD, fields, 1, wire_values, wire_caps,
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
      out[wire_row].prompt_tokens = (int64_t)strtoll(wire_scratch[wire_row * 6u + 0u], NULL, 10);
      out[wire_row].completion_tokens = (int64_t)strtoll(wire_scratch[wire_row * 6u + 1u], NULL, 10);
      out[wire_row].cache_write_tokens = (int64_t)strtoll(wire_scratch[wire_row * 6u + 2u], NULL, 10);
      out[wire_row].cache_read_tokens = (int64_t)strtoll(wire_scratch[wire_row * 6u + 3u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 6u + 4u], NULL);
      out[wire_row].call_count = (int)strtol(wire_scratch[wire_row * 6u + 5u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_insights_by_platform(int since_hours, db1_insights_platform_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
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
      wire_values[wire_row * 2u + 0u] = out[wire_row].platform;
      wire_caps[wire_row * 2u + 0u] = sizeof out[wire_row].platform;
      wire_values[wire_row * 2u + 1u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 2u + 1u] = sizeof wire_scratch[wire_row * 1u + 0u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_INSIGHTS_BY_PLATFORM, fields, 2, wire_values, wire_caps,
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
      out[wire_row].session_count = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_insights_top_sessions(int since_hours, db1_insights_top_session_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 7u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 7u * sizeof *wire_caps);
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
      wire_values[wire_row * 7u + 0u] = out[wire_row].session_id;
      wire_caps[wire_row * 7u + 0u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 7u + 1u] = out[wire_row].title;
      wire_caps[wire_row * 7u + 1u] = sizeof out[wire_row].title;
      wire_values[wire_row * 7u + 2u] = out[wire_row].model;
      wire_caps[wire_row * 7u + 2u] = sizeof out[wire_row].model;
      wire_values[wire_row * 7u + 3u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 7u + 3u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 7u + 4u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 7u + 4u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 7u + 5u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 7u + 5u] = sizeof wire_scratch[wire_row * 3u + 2u];
      wire_values[wire_row * 7u + 6u] = out[wire_row].created_at;
      wire_caps[wire_row * 7u + 6u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_INSIGHTS_TOP_SESSIONS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 7), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 7u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 7u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].prompt_tokens = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].completion_tokens = (int64_t)strtoll(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].estimated_cost_usd = strtod(wire_scratch[wire_row * 3u + 2u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_insights_delegates_by_role(int since_hours, db1_insights_delegate_role_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", since_hours);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 3u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 3u * sizeof *wire_caps);
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
      wire_values[wire_row * 3u + 0u] = out[wire_row].role;
      wire_caps[wire_row * 3u + 0u] = sizeof out[wire_row].role;
      wire_values[wire_row * 3u + 1u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 3u + 1u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 3u + 2u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 3u + 2u] = sizeof wire_scratch[wire_row * 2u + 1u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_INSIGHTS_DELEGATES_BY_ROLE, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 3), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 3u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 3u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].total = (int)strtol(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].completed = (int)strtol(wire_scratch[wire_row * 2u + 1u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_cost_fold_record(const char *parent_session_id, const char *child_session_id, double cost_usd, const char *source)
{
   if (!parent_session_id || !parent_session_id[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)cost_usd);
   const char *fields[] = {parent_session_id, child_session_id ? child_session_id : "", arg2, source ? source : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_COST_FOLD_RECORD, fields, 4, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

double db1_cost_fold_total(const char *parent_session_id)
{
   if (!parent_session_id || !parent_session_id[0])
      return -1;
   const char *fields[] = {parent_session_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_COST_FOLD_TOTAL, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (double)strtod(slot0, NULL);
}

int db1_interaction_event_record(const char *session_id, const char *type_name, const char *actor, const char *payload_json, const char *outcome)
{
   const char *fields[] = {session_id ? session_id : "", type_name ? type_name : "", actor ? actor : "", payload_json ? payload_json : "", outcome ? outcome : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_RECORD, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_interaction_event_list_unreflected(const char *session_id, ie_event_row_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 7u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 7u * sizeof *wire_caps);
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
      wire_values[wire_row * 7u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 7u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 7u + 1u] = out[wire_row].created_at;
      wire_caps[wire_row * 7u + 1u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 7u + 2u] = out[wire_row].session_id;
      wire_caps[wire_row * 7u + 2u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 7u + 3u] = out[wire_row].event_type;
      wire_caps[wire_row * 7u + 3u] = sizeof out[wire_row].event_type;
      wire_values[wire_row * 7u + 4u] = out[wire_row].actor;
      wire_caps[wire_row * 7u + 4u] = sizeof out[wire_row].actor;
      wire_values[wire_row * 7u + 5u] = out[wire_row].payload;
      wire_caps[wire_row * 7u + 5u] = sizeof out[wire_row].payload;
      wire_values[wire_row * 7u + 6u] = out[wire_row].outcome;
      wire_caps[wire_row * 7u + 6u] = sizeof out[wire_row].outcome;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_LIST_UNREFLECTED, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 7), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 7u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 7u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_interaction_event_list_for_session(const char *session_id, ie_event_row_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 7u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 7u * sizeof *wire_caps);
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
      wire_values[wire_row * 7u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 7u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 7u + 1u] = out[wire_row].created_at;
      wire_caps[wire_row * 7u + 1u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 7u + 2u] = out[wire_row].session_id;
      wire_caps[wire_row * 7u + 2u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 7u + 3u] = out[wire_row].event_type;
      wire_caps[wire_row * 7u + 3u] = sizeof out[wire_row].event_type;
      wire_values[wire_row * 7u + 4u] = out[wire_row].actor;
      wire_caps[wire_row * 7u + 4u] = sizeof out[wire_row].actor;
      wire_values[wire_row * 7u + 5u] = out[wire_row].payload;
      wire_caps[wire_row * 7u + 5u] = sizeof out[wire_row].payload;
      wire_values[wire_row * 7u + 6u] = out[wire_row].outcome;
      wire_caps[wire_row * 7u + 6u] = sizeof out[wire_row].outcome;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_LIST_FOR_SESSION, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 7), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 7u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 7u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_interaction_event_list_promotion_feed(ie_event_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 7u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 7u * sizeof *wire_caps);
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
      wire_values[wire_row * 7u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 7u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 7u + 1u] = out[wire_row].created_at;
      wire_caps[wire_row * 7u + 1u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 7u + 2u] = out[wire_row].session_id;
      wire_caps[wire_row * 7u + 2u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 7u + 3u] = out[wire_row].event_type;
      wire_caps[wire_row * 7u + 3u] = sizeof out[wire_row].event_type;
      wire_values[wire_row * 7u + 4u] = out[wire_row].actor;
      wire_caps[wire_row * 7u + 4u] = sizeof out[wire_row].actor;
      wire_values[wire_row * 7u + 5u] = out[wire_row].payload;
      wire_caps[wire_row * 7u + 5u] = sizeof out[wire_row].payload;
      wire_values[wire_row * 7u + 6u] = out[wire_row].outcome;
      wire_caps[wire_row * 7u + 6u] = sizeof out[wire_row].outcome;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_LIST_PROMOTION_FEED, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 7), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 7u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 7u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_interaction_event_mark_reflected(const int *ids, int count)
{
   if (!ids || count <= 0 || count > 512)
      return -1;
   const char *fields[512];
   char (*wire_rendered)[32] = malloc((size_t)count * sizeof *wire_rendered);
   if (!wire_rendered)
      return -1;
   for (int at = 0; at < count; ++at)
   {
      snprintf(wire_rendered[at], 32, "%d", ids[at]);
      fields[0 + at] = wire_rendered[at];
   }
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_MARK_REFLECTED, fields, (uint32_t)(0 + count), values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_interaction_event_mark_promoted(const int *ids, int count)
{
   if (!ids || count <= 0 || count > 512)
      return -1;
   const char *fields[512];
   char (*wire_rendered)[32] = malloc((size_t)count * sizeof *wire_rendered);
   if (!wire_rendered)
      return -1;
   for (int at = 0; at < count; ++at)
   {
      snprintf(wire_rendered[at], 32, "%d", ids[at]);
      fields[0 + at] = wire_rendered[at];
   }
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_MARK_PROMOTED, fields, (uint32_t)(0 + count), values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_interaction_event_evict_if_needed(int cap)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", cap);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_INTERACTION_EVENT_EVICT_IF_NEEDED, fields, 1, NULL, NULL, 0, NULL));
}

int db1_guardrail_event_insert(const guardrail_event_t *e)
{
   if (!e)
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)e->overall_risk);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%.17g", (double)e->action_risk);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%.17g", (double)e->diff_risk);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%.17g", (double)e->drift_risk);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%.17g", (double)e->antipattern_similarity);
   char arg11[32];
   snprintf(arg11, sizeof arg11, "%d", e->dry_run);
   const char *fields[] = {e->session_id, e->tool_name, arg2, arg3, arg4, arg5, arg6, e->recommendation, e->labels, e->final_action, e->explanation, arg11};
   return write_result(call_stage(AIMEE_DB1_OP_GUARDRAIL_EVENT_INSERT, fields, 12, NULL, NULL, 0, NULL));
}

int db1_guardrail_event_counts_7d(guardrail_event_counts_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, slot3};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3};
   int wire_status = call_stage(AIMEE_DB1_OP_GUARDRAIL_EVENT_COUNTS_7D, fields, 0, values, caps, 4, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->dry_run = (int)strtol(slot0, NULL, 10);
   out->warn = (int)strtol(slot1, NULL, 10);
   out->prompt = (int)strtol(slot2, NULL, 10);
   out->block = (int)strtol(slot3, NULL, 10);
   return 0;
}

int db1_guardrail_event_session_advisory_count(const char *session_id, int *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   const char *fields[] = {session_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_GUARDRAIL_EVENT_SESSION_ADVISORY_COUNT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_guardrail_event_list(int limit, int only_advisory, guardrail_event_row_t *rows, int *count)
{
   if (!rows || !count)
      return -1;
   if (limit > 256)
      limit = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", limit);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", only_advisory);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)limit * 9u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)limit * 9u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)limit * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(rows, 0, (size_t)limit * sizeof *rows);
   for (int wire_row = 0; wire_row < limit; ++wire_row)
   {
      wire_values[wire_row * 9u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 9u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 9u + 1u] = rows[wire_row].recorded_at;
      wire_caps[wire_row * 9u + 1u] = sizeof rows[wire_row].recorded_at;
      wire_values[wire_row * 9u + 2u] = rows[wire_row].session_id;
      wire_caps[wire_row * 9u + 2u] = sizeof rows[wire_row].session_id;
      wire_values[wire_row * 9u + 3u] = rows[wire_row].tool_name;
      wire_caps[wire_row * 9u + 3u] = sizeof rows[wire_row].tool_name;
      wire_values[wire_row * 9u + 4u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 9u + 4u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 9u + 5u] = rows[wire_row].labels;
      wire_caps[wire_row * 9u + 5u] = sizeof rows[wire_row].labels;
      wire_values[wire_row * 9u + 6u] = rows[wire_row].final_action;
      wire_caps[wire_row * 9u + 6u] = sizeof rows[wire_row].final_action;
      wire_values[wire_row * 9u + 7u] = rows[wire_row].explanation;
      wire_caps[wire_row * 9u + 7u] = sizeof rows[wire_row].explanation;
      wire_values[wire_row * 9u + 8u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 9u + 8u] = sizeof wire_scratch[wire_row * 3u + 2u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_GUARDRAIL_EVENT_LIST, fields, 2, wire_values, wire_caps,
                           (uint32_t)(limit * 9), &wire_filled);
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
      rows[wire_row].id = (int)strtol(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      rows[wire_row].overall_risk = strtod(wire_scratch[wire_row * 3u + 1u], NULL);
      rows[wire_row].dry_run = (int)strtol(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   *count = wire_rows;
   return 0;
}

int db1_eval_result_insert(const db1_eval_result_row_t *row)
{
   if (!row)
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", row->success);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", row->turns);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", row->tool_calls);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%d", row->tool_call_failures);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", row->rescue_recoveries);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%d", row->prompt_tokens);
   char arg10[32];
   snprintf(arg10, sizeof arg10, "%d", row->completion_tokens);
   char arg11[32];
   snprintf(arg11, sizeof arg11, "%d", row->latency_ms);
   char arg18[32];
   snprintf(arg18, sizeof arg18, "%d", row->seed);
   const char *fields[] = {row->suite ? row->suite : "", row->task_name ? row->task_name : "", row->agent_name ? row->agent_name : "", row->ablation ? row->ablation : "", arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, row->response ? row->response : "", row->error ? row->error : "", row->dataset_hash ? row->dataset_hash : "", row->target_hash ? row->target_hash : "", row->harness_version ? row->harness_version : "", row->hardware_profile ? row->hardware_profile : "", arg18};
   return write_result(call_stage(AIMEE_DB1_OP_EVAL_RESULT_INSERT, fields, 19, NULL, NULL, 0, NULL));
}

int db1_eval_failed_tasks_recent(db1_eval_failed_task_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 2u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 2u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 2u + 0u] = out[wire_row].task_name;
      wire_caps[wire_row * 2u + 0u] = sizeof out[wire_row].task_name;
      wire_values[wire_row * 2u + 1u] = out[wire_row].error;
      wire_caps[wire_row * 2u + 1u] = sizeof out[wire_row].error;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EVAL_FAILED_TASKS_RECENT, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 2), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 2u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 2u);
   return wire_rows;
}

int db1_eval_passed_tasks_recent(db1_eval_passed_task_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
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
      wire_values[wire_row * 1u + 0u] = out[wire_row].task_name;
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row].task_name;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EVAL_PASSED_TASKS_RECENT, fields, 1, wire_values, wire_caps,
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

int db1_eval_results_list(const char *suite_or_null, db1_eval_display_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {suite_or_null ? suite_or_null : "", arg1};
   char **wire_values = malloc((size_t)max * 11u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 11u * sizeof *wire_caps);
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
      wire_values[wire_row * 11u + 0u] = out[wire_row].suite;
      wire_caps[wire_row * 11u + 0u] = sizeof out[wire_row].suite;
      wire_values[wire_row * 11u + 1u] = out[wire_row].task_name;
      wire_caps[wire_row * 11u + 1u] = sizeof out[wire_row].task_name;
      wire_values[wire_row * 11u + 2u] = out[wire_row].agent_name;
      wire_caps[wire_row * 11u + 2u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 11u + 3u] = out[wire_row].ablation;
      wire_caps[wire_row * 11u + 3u] = sizeof out[wire_row].ablation;
      wire_values[wire_row * 11u + 4u] = wire_scratch[wire_row * 6u + 0u];
      wire_caps[wire_row * 11u + 4u] = sizeof wire_scratch[wire_row * 6u + 0u];
      wire_values[wire_row * 11u + 5u] = wire_scratch[wire_row * 6u + 1u];
      wire_caps[wire_row * 11u + 5u] = sizeof wire_scratch[wire_row * 6u + 1u];
      wire_values[wire_row * 11u + 6u] = wire_scratch[wire_row * 6u + 2u];
      wire_caps[wire_row * 11u + 6u] = sizeof wire_scratch[wire_row * 6u + 2u];
      wire_values[wire_row * 11u + 7u] = wire_scratch[wire_row * 6u + 3u];
      wire_caps[wire_row * 11u + 7u] = sizeof wire_scratch[wire_row * 6u + 3u];
      wire_values[wire_row * 11u + 8u] = wire_scratch[wire_row * 6u + 4u];
      wire_caps[wire_row * 11u + 8u] = sizeof wire_scratch[wire_row * 6u + 4u];
      wire_values[wire_row * 11u + 9u] = wire_scratch[wire_row * 6u + 5u];
      wire_caps[wire_row * 11u + 9u] = sizeof wire_scratch[wire_row * 6u + 5u];
      wire_values[wire_row * 11u + 10u] = out[wire_row].created_at;
      wire_caps[wire_row * 11u + 10u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EVAL_RESULTS_LIST, fields, 2, wire_values, wire_caps,
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
      out[wire_row].success = (int)strtol(wire_scratch[wire_row * 6u + 0u], NULL, 10);
      out[wire_row].turns = (int)strtol(wire_scratch[wire_row * 6u + 1u], NULL, 10);
      out[wire_row].tool_calls = (int)strtol(wire_scratch[wire_row * 6u + 2u], NULL, 10);
      out[wire_row].tool_call_failures = (int)strtol(wire_scratch[wire_row * 6u + 3u], NULL, 10);
      out[wire_row].rescue_recoveries = (int)strtol(wire_scratch[wire_row * 6u + 4u], NULL, 10);
      out[wire_row].latency_ms = (int)strtol(wire_scratch[wire_row * 6u + 5u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_diagnose_start(const char *symptom)
{
   if (!symptom || !symptom[0])
      return -1;
   const char *fields[] = {symptom};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_START, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_diagnose_add_observation(int diag_id, const char *content, const char *source)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   const char *fields[] = {arg0, content ? content : "", source ? source : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_ADD_OBSERVATION, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_diagnose_add_hypothesis(int diag_id, const char *content)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   const char *fields[] = {arg0, content ? content : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_ADD_HYPOTHESIS, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_diagnose_add_evidence(int diag_id, int hypothesis_id, const char *kind, const char *content, const char *source, int rank)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", hypothesis_id);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", rank);
   const char *fields[] = {arg0, arg1, kind ? kind : "", content ? content : "", source ? source : "", arg5};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_ADD_EVIDENCE, fields, 6, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_diagnose_add_probe(int diag_id, int hypothesis_id, const char *content)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", hypothesis_id);
   const char *fields[] = {arg0, arg1, content ? content : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_ADD_PROBE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_diagnose_get(int diag_id, diagnosis_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot4[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->symptom, out->status, out->conclusion, slot4, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->symptom, sizeof out->status, sizeof out->conclusion, sizeof slot4, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_GET, fields, 1, values, caps, 7, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->confidence = strtod(slot4, NULL);
   return 0;
}

int db1_diagnose_list(diagnosis_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 7u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 7u * sizeof *wire_caps);
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
      wire_values[wire_row * 7u + 0u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 7u + 0u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 7u + 1u] = out[wire_row].symptom;
      wire_caps[wire_row * 7u + 1u] = sizeof out[wire_row].symptom;
      wire_values[wire_row * 7u + 2u] = out[wire_row].status;
      wire_caps[wire_row * 7u + 2u] = sizeof out[wire_row].status;
      wire_values[wire_row * 7u + 3u] = out[wire_row].conclusion;
      wire_caps[wire_row * 7u + 3u] = sizeof out[wire_row].conclusion;
      wire_values[wire_row * 7u + 4u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 7u + 4u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 7u + 5u] = out[wire_row].created_at;
      wire_caps[wire_row * 7u + 5u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 7u + 6u] = out[wire_row].updated_at;
      wire_caps[wire_row * 7u + 6u] = sizeof out[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 7), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 7u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 7u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].confidence = strtod(wire_scratch[wire_row * 2u + 1u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_diagnose_list_items(int diag_id, diagnosis_item_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
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
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 8u + 1u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 8u + 1u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 8u + 2u] = out[wire_row].kind;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].kind;
      wire_values[wire_row * 8u + 3u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 8u + 3u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 8u + 4u] = out[wire_row].content;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].content;
      wire_values[wire_row * 8u + 5u] = out[wire_row].source;
      wire_caps[wire_row * 8u + 5u] = sizeof out[wire_row].source;
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 4u + 3u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_LIST_ITEMS, fields, 2, wire_values, wire_caps,
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
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].diagnosis_id = (int)strtol(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].parent_id = (int)strtol(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].evidence_rank = (int)strtol(wire_scratch[wire_row * 4u + 3u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_diagnose_list_hypotheses(int diag_id, diagnosis_item_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
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
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 8u + 1u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 8u + 1u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 8u + 2u] = out[wire_row].kind;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].kind;
      wire_values[wire_row * 8u + 3u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 8u + 3u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 8u + 4u] = out[wire_row].content;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].content;
      wire_values[wire_row * 8u + 5u] = out[wire_row].source;
      wire_caps[wire_row * 8u + 5u] = sizeof out[wire_row].source;
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 4u + 3u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_LIST_HYPOTHESES, fields, 2, wire_values, wire_caps,
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
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      out[wire_row].diagnosis_id = (int)strtol(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      out[wire_row].parent_id = (int)strtol(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      out[wire_row].evidence_rank = (int)strtol(wire_scratch[wire_row * 4u + 3u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_diagnose_rank_hypotheses(int diag_id, diagnosis_ranking_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 13u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 13u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 9u * sizeof *wire_scratch);
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
      wire_values[wire_row * 13u + 0u] = wire_scratch[wire_row * 9u + 0u];
      wire_caps[wire_row * 13u + 0u] = sizeof wire_scratch[wire_row * 9u + 0u];
      wire_values[wire_row * 13u + 1u] = wire_scratch[wire_row * 9u + 1u];
      wire_caps[wire_row * 13u + 1u] = sizeof wire_scratch[wire_row * 9u + 1u];
      wire_values[wire_row * 13u + 2u] = out[wire_row].hypothesis.kind;
      wire_caps[wire_row * 13u + 2u] = sizeof out[wire_row].hypothesis.kind;
      wire_values[wire_row * 13u + 3u] = wire_scratch[wire_row * 9u + 2u];
      wire_caps[wire_row * 13u + 3u] = sizeof wire_scratch[wire_row * 9u + 2u];
      wire_values[wire_row * 13u + 4u] = out[wire_row].hypothesis.content;
      wire_caps[wire_row * 13u + 4u] = sizeof out[wire_row].hypothesis.content;
      wire_values[wire_row * 13u + 5u] = out[wire_row].hypothesis.source;
      wire_caps[wire_row * 13u + 5u] = sizeof out[wire_row].hypothesis.source;
      wire_values[wire_row * 13u + 6u] = wire_scratch[wire_row * 9u + 3u];
      wire_caps[wire_row * 13u + 6u] = sizeof wire_scratch[wire_row * 9u + 3u];
      wire_values[wire_row * 13u + 7u] = out[wire_row].hypothesis.created_at;
      wire_caps[wire_row * 13u + 7u] = sizeof out[wire_row].hypothesis.created_at;
      wire_values[wire_row * 13u + 8u] = wire_scratch[wire_row * 9u + 4u];
      wire_caps[wire_row * 13u + 8u] = sizeof wire_scratch[wire_row * 9u + 4u];
      wire_values[wire_row * 13u + 9u] = wire_scratch[wire_row * 9u + 5u];
      wire_caps[wire_row * 13u + 9u] = sizeof wire_scratch[wire_row * 9u + 5u];
      wire_values[wire_row * 13u + 10u] = wire_scratch[wire_row * 9u + 6u];
      wire_caps[wire_row * 13u + 10u] = sizeof wire_scratch[wire_row * 9u + 6u];
      wire_values[wire_row * 13u + 11u] = wire_scratch[wire_row * 9u + 7u];
      wire_caps[wire_row * 13u + 11u] = sizeof wire_scratch[wire_row * 9u + 7u];
      wire_values[wire_row * 13u + 12u] = wire_scratch[wire_row * 9u + 8u];
      wire_caps[wire_row * 13u + 12u] = sizeof wire_scratch[wire_row * 9u + 8u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_RANK_HYPOTHESES, fields, 2, wire_values, wire_caps,
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
      out[wire_row].hypothesis.id = (int)strtol(wire_scratch[wire_row * 9u + 0u], NULL, 10);
      out[wire_row].hypothesis.diagnosis_id = (int)strtol(wire_scratch[wire_row * 9u + 1u], NULL, 10);
      out[wire_row].hypothesis.parent_id = (int)strtol(wire_scratch[wire_row * 9u + 2u], NULL, 10);
      out[wire_row].hypothesis.evidence_rank = (int)strtol(wire_scratch[wire_row * 9u + 3u], NULL, 10);
      out[wire_row].evidence_for_count = (int)strtol(wire_scratch[wire_row * 9u + 4u], NULL, 10);
      out[wire_row].evidence_against_count = (int)strtol(wire_scratch[wire_row * 9u + 5u], NULL, 10);
      out[wire_row].strongest_for_rank = (int)strtol(wire_scratch[wire_row * 9u + 6u], NULL, 10);
      out[wire_row].strongest_against_rank = (int)strtol(wire_scratch[wire_row * 9u + 7u], NULL, 10);
      out[wire_row].confidence = strtod(wire_scratch[wire_row * 9u + 8u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_diagnose_conclude(int diag_id, const char *conclusion, double confidence)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)confidence);
   const char *fields[] = {arg0, conclusion ? conclusion : "", arg2};
   return write_result(call_stage(AIMEE_DB1_OP_DIAGNOSE_CONCLUDE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_diagnose_abandon(int diag_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_DIAGNOSE_ABANDON, fields, 1, NULL, NULL, 0, NULL));
}

int db1_diagnose_suggest_probes(int diag_id, diagnosis_probe_suggestion_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", diag_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 3u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 3u * sizeof *wire_caps);
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
      wire_values[wire_row * 3u + 0u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 3u + 0u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 3u + 1u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 3u + 1u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 3u + 2u] = out[wire_row].suggestion;
      wire_caps[wire_row * 3u + 2u] = sizeof out[wire_row].suggestion;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_DIAGNOSE_SUGGEST_PROBES, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 3), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 3u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 3u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].hypothesis_a_id = (int)strtol(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].hypothesis_b_id = (int)strtol(wire_scratch[wire_row * 2u + 1u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

/* clang-format on */
