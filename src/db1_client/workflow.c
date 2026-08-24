/* db1_client/workflow.c: the workflow family, reached over the bus.
 *
 * WAS GENERATED from the store catalog by scripts/gen_db1_contract.py. Both
 * moved on: the catalog is now server-go/modules/aimee/operations.json, and the
 * generator was deleted with the C module.
 *
 * So this is maintained BY HAND now, and the header used to say "Do not edit"
 * while pointing at a generator that no longer exists and a path that no longer
 * resolves -- which is a dead end at exactly the moment someone needs to change
 * something. Edit it, and keep it agreeing with the catalog:
 * scripts/check-db1-client-contract.py matches every call site here against the
 * catalog by arity and reply width, and runs in lint on every pull request.
 * That check is what replaced the generator.
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
#include "execution_plans.h"
#include "execution_trace.h"
#include "db1_client/pipelines.h"
#include "db1_client/roadmap_runtime.h"
#include "wfe_binding.h"

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

#define DB1_WORKFLOW_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.workflow", "DB1 %s is unreachable (module call result %d)", "workflow",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_WORKFLOW))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_WORKFLOW_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_WORKFLOW, AIMEE_DB1_STAGE_WORKFLOW, 0, deadline,
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
      /* Fewer values than the caller has slots for is the same contract
         mismatch read from the other side, and it used to pass: the unfilled
         slots keep the empty string cleared above, so the caller reads a row
         whose last members are blank and cannot tell that from a row that is
         blank. A list says how many rows it found through filled_out and is
         variable by construction; every other shape has one arity, and a stage
         answering with a different one is a stage built against a different
         version of this contract. Two processes, two binaries, two deployment
         times -- so say it rather than zero-fill. */
      else if (status == (uint32_t)AIMEE_DB1_STATUS_OK && fields_in != slots)
         result = -1;
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


int db1_execution_trace_insert(const db1_execution_trace_insert_row_t *row)
{
   if (!row)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", row->plan_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", row->turn);
   const char *fields[] = {arg0, row->session_id ? row->session_id : "", arg2, row->direction ? row->direction : "", row->content ? row->content : "", row->tool_name ? row->tool_name : "", row->tool_args ? row->tool_args : "", row->tool_result ? row->tool_result : "", row->context_hash ? row->context_hash : ""};
   return write_result(call_stage(AIMEE_DB1_OP_EXECUTION_TRACE_INSERT, fields, 9, NULL, NULL, 0, NULL));
}

int db1_execution_trace_count_for_session(const char *session_id)
{
   const char *fields[] = {session_id ? session_id : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_TRACE_COUNT_FOR_SESSION, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_execution_trace_list_recent(db1_execution_trace_recent_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
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
      wire_values[wire_row * 5u + 0u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 5u + 0u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 5u + 1u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 5u + 1u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 5u + 2u] = out[wire_row].direction;
      wire_caps[wire_row * 5u + 2u] = sizeof out[wire_row].direction;
      wire_values[wire_row * 5u + 3u] = out[wire_row].tool_name;
      wire_caps[wire_row * 5u + 3u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 5u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 5u + 4u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_TRACE_LIST_RECENT, fields, 1, wire_values, wire_caps,
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
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].turn = (int)strtol(wire_scratch[wire_row * 2u + 1u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_execution_trace_get(int trace_id, db1_execution_trace_detail_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", trace_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->direction, out->content, out->tool_name, out->tool_args, out->tool_result, out->context_hash, out->created_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->direction, sizeof out->content, sizeof out->tool_name, sizeof out->tool_args, sizeof out->tool_result, sizeof out->context_hash, sizeof out->created_at};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_TRACE_GET, fields, 1, values, caps, 10, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->plan_id = (int)strtol(slot1, NULL, 10);
   out->turn = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_execution_trace_list_tool_calls(db1_execution_trace_tool_call_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
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
      wire_values[wire_row * 5u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 5u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 5u + 1u] = out[wire_row].direction;
      wire_caps[wire_row * 5u + 1u] = sizeof out[wire_row].direction;
      wire_values[wire_row * 5u + 2u] = out[wire_row].tool_name;
      wire_caps[wire_row * 5u + 2u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 5u + 3u] = out[wire_row].tool_args;
      wire_caps[wire_row * 5u + 3u] = sizeof out[wire_row].tool_args;
      wire_values[wire_row * 5u + 4u] = out[wire_row].tool_result;
      wire_caps[wire_row * 5u + 4u] = sizeof out[wire_row].tool_result;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_TRACE_LIST_TOOL_CALLS, fields, 1, wire_values, wire_caps,
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
      out[wire_row].turn = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_execution_trace_list_after_id(int64_t after_id, db1_execution_trace_mining_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)after_id);
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
      wire_values[wire_row * 7u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 7u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 7u + 1u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 7u + 1u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 7u + 2u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 7u + 2u] = sizeof wire_scratch[wire_row * 3u + 2u];
      wire_values[wire_row * 7u + 3u] = out[wire_row].direction;
      wire_caps[wire_row * 7u + 3u] = sizeof out[wire_row].direction;
      wire_values[wire_row * 7u + 4u] = out[wire_row].tool_name;
      wire_caps[wire_row * 7u + 4u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 7u + 5u] = out[wire_row].tool_args;
      wire_caps[wire_row * 7u + 5u] = sizeof out[wire_row].tool_args;
      wire_values[wire_row * 7u + 6u] = out[wire_row].tool_result;
      wire_caps[wire_row * 7u + 6u] = sizeof out[wire_row].tool_result;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_TRACE_LIST_AFTER_ID, fields, 2, wire_values, wire_caps,
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
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].plan_id = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].turn = (int)strtol(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_wfe_bind(const char *session_id, const char *work_item_id, const char *enforce_stage)
{
   if (!session_id || !session_id[0] || !work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {session_id, work_item_id, enforce_stage ? enforce_stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_BIND, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_binding_get(const char *session_id, char *wi_out, size_t wi_n, char *stage_out, size_t stage_n)
{
   if (!session_id || !session_id[0] || !wi_out || wi_n == 0 || !stage_out || stage_n == 0)
      return -1;
   const char *fields[] = {session_id};
   char *const values[] = {wi_out, stage_out};
   const size_t caps[] = {wi_n, stage_n};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_BINDING_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   return 1;
}

int db1_wfe_unbind(const char *session_id)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_UNBIND, fields, 1, NULL, NULL, 0, NULL));
}

int db1_wfe_lease_renew(const char *session_id, int ttl_secs)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", ttl_secs);
   const char *fields[] = {session_id, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_LEASE_RENEW, fields, 2, NULL, NULL, 0, NULL));
}

int db1_wfe_lease_expiry_get(const char *session_id, char *out, size_t n)
{
   if (!session_id || !session_id[0] || !out || n == 0)
      return -1;
   const char *fields[] = {session_id};
   char *const values[] = {out};
   const size_t caps[] = {n};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_LEASE_EXPIRY_GET, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   return 1;
}

int db1_wfe_lease_stale_work_items(char (*out)[DB1_WFE_WORK_ITEM_ID_LEN], int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
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
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_LEASE_STALE_WORK_ITEMS, fields, 1, wire_values, wire_caps,
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

int db1_wfe_lease_reclaim_stale()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_LEASE_RECLAIM_STALE, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_pipeline_create(const char *task, const char *request_classification, const char *plan_depth, int *out_id)
{
   if (!task || !task[0] || !out_id)
      return -1;
   const char *fields[] = {task, request_classification ? request_classification : "", plan_depth ? plan_depth : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_PIPELINE_CREATE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_pipeline_get(int pipeline_id, db1_pipeline_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->task, out->status, out->current_phase, out->request_classification, out->plan_depth, slot6, slot7, slot8, slot9, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->task, sizeof out->status, sizeof out->current_phase, sizeof out->request_classification, sizeof out->plan_depth, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_PIPELINE_GET, fields, 1, values, caps, 12, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->phase_attempts = (int)strtol(slot6, NULL, 10);
   out->plan_id = (int)strtol(slot7, NULL, 10);
   out->job_id = (int)strtol(slot8, NULL, 10);
   out->clarify_session_id = (int)strtol(slot9, NULL, 10);
   return 0;
}

int db1_pipeline_update(int pipeline_id, const char *status, const char *current_phase, int phase_attempts, int plan_id, int job_id, const char *request_classification, const char *plan_depth, int clarify_session_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", phase_attempts);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", plan_id);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", job_id);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", clarify_session_id);
   const char *fields[] = {arg0, status ? status : "", current_phase ? current_phase : "", arg3, arg4, arg5, request_classification ? request_classification : "", plan_depth ? plan_depth : "", arg8};
   return write_result(call_stage(AIMEE_DB1_OP_PIPELINE_UPDATE, fields, 9, NULL, NULL, 0, NULL));
}

int db1_pipeline_link_plan(int pipeline_id, int plan_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", plan_id);
   const char *fields[] = {arg0, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_PIPELINE_LINK_PLAN, fields, 2, NULL, NULL, 0, NULL));
}

int db1_pipeline_link_job(int pipeline_id, int job_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", job_id);
   const char *fields[] = {arg0, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_PIPELINE_LINK_JOB, fields, 2, NULL, NULL, 0, NULL));
}

int db1_pipeline_cancel(int pipeline_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   const char *fields[] = {arg0};
   return write_result(call_stage(AIMEE_DB1_OP_PIPELINE_CANCEL, fields, 1, NULL, NULL, 0, NULL));
}

int db1_pipeline_list_active(db1_pipeline_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 12u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 12u * sizeof *wire_caps);
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
      wire_values[wire_row * 12u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 12u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 12u + 1u] = out[wire_row].task;
      wire_caps[wire_row * 12u + 1u] = sizeof out[wire_row].task;
      wire_values[wire_row * 12u + 2u] = out[wire_row].status;
      wire_caps[wire_row * 12u + 2u] = sizeof out[wire_row].status;
      wire_values[wire_row * 12u + 3u] = out[wire_row].current_phase;
      wire_caps[wire_row * 12u + 3u] = sizeof out[wire_row].current_phase;
      wire_values[wire_row * 12u + 4u] = out[wire_row].request_classification;
      wire_caps[wire_row * 12u + 4u] = sizeof out[wire_row].request_classification;
      wire_values[wire_row * 12u + 5u] = out[wire_row].plan_depth;
      wire_caps[wire_row * 12u + 5u] = sizeof out[wire_row].plan_depth;
      wire_values[wire_row * 12u + 6u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 12u + 6u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 12u + 7u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 12u + 7u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 12u + 8u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 12u + 8u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 12u + 9u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 12u + 9u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 12u + 10u] = out[wire_row].created_at;
      wire_caps[wire_row * 12u + 10u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 12u + 11u] = out[wire_row].updated_at;
      wire_caps[wire_row * 12u + 11u] = sizeof out[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_PIPELINE_LIST_ACTIVE, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 12), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 12u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 12u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].phase_attempts = (int)strtol(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].plan_id = (int)strtol(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].job_id = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      out[wire_row].clarify_session_id = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_roadmap_dispatch_upsert(const char *roadmap_id, const char *token_profile, int require_slice_discussion, int budget_ceiling_tokens)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", require_slice_discussion);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", budget_ceiling_tokens);
   const char *fields[] = {roadmap_id, token_profile ? token_profile : "", arg2, arg3};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_DISPATCH_UPSERT, fields, 4, NULL, NULL, 0, NULL));
}

int db1_roadmap_dispatch_get(const char *roadmap_id, rdm_dispatch_t *out)
{
   if (!roadmap_id || !roadmap_id[0] || !out)
      return -1;
   const char *fields[] = {roadmap_id};
   char slot0[32];
   char slot5[32];
   char slot6[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->roadmap_id, out->status, out->phase, out->token_profile, slot5, slot6, out->exit_reason, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->roadmap_id, sizeof out->status, sizeof out->phase, sizeof out->token_profile, sizeof slot5, sizeof slot6, sizeof out->exit_reason, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROADMAP_DISPATCH_GET, fields, 1, values, caps, 10, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->require_slice_discussion = (int)strtol(slot5, NULL, 10);
   out->budget_ceiling_tokens = (int)strtol(slot6, NULL, 10);
   return 0;
}

int db1_roadmap_dispatch_set_status(const char *roadmap_id, const char *status, const char *exit_reason)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   const char *fields[] = {roadmap_id, status ? status : "", exit_reason ? exit_reason : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_DISPATCH_SET_STATUS, fields, 3, NULL, NULL, 0, NULL));
}

int db1_roadmap_dispatch_set_phase(const char *roadmap_id, const char *phase)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   const char *fields[] = {roadmap_id, phase ? phase : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_DISPATCH_SET_PHASE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_ensure(const char *roadmap_id, const char *unit_id, const char *level, const char *tool_policy_mode)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   const char *fields[] = {roadmap_id, unit_id, level ? level : "", tool_policy_mode ? tool_policy_mode : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_ENSURE, fields, 4, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_get(const char *roadmap_id, const char *unit_id, rdm_unit_dispatch_t *out)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0] || !out)
      return -1;
   const char *fields[] = {roadmap_id, unit_id};
   char slot0[32];
   char slot9[32];
   char slot10[32];
   char slot12[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->roadmap_id, out->unit_id, out->level, out->state, out->tool_policy_mode, out->claimed_by, out->claimed_at, out->heartbeat_at, slot9, slot10, out->worktree_path, slot12, out->result, out->error, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->roadmap_id, sizeof out->unit_id, sizeof out->level, sizeof out->state, sizeof out->tool_policy_mode, sizeof out->claimed_by, sizeof out->claimed_at, sizeof out->heartbeat_at, sizeof slot9, sizeof slot10, sizeof out->worktree_path, sizeof slot12, sizeof out->result, sizeof out->error, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_GET, fields, 2, values, caps, 17, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->verify_attempts = (int)strtol(slot9, NULL, 10);
   out->dispatch_attempts = (int)strtol(slot10, NULL, 10);
   out->coord_job_id = (int)strtol(slot12, NULL, 10);
   return 0;
}

int db1_roadmap_unit_set_state(const char *roadmap_id, const char *unit_id, const char *state)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   const char *fields[] = {roadmap_id, unit_id, state ? state : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_SET_STATE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_claim(const char *roadmap_id, const char *unit_id, const char *owner, const char *worktree_path)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   const char *fields[] = {roadmap_id, unit_id, owner ? owner : "", worktree_path ? worktree_path : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_CLAIM, fields, 4, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_heartbeat(const char *roadmap_id, const char *unit_id)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   const char *fields[] = {roadmap_id, unit_id};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_HEARTBEAT, fields, 2, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_finish(const char *roadmap_id, const char *unit_id, const char *state, const char *result, const char *error)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   const char *fields[] = {roadmap_id, unit_id, state ? state : "", result ? result : "", error ? error : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_FINISH, fields, 5, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_set_coord_job(const char *roadmap_id, const char *unit_id, int coord_job_id)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", coord_job_id);
   const char *fields[] = {roadmap_id, unit_id, arg2};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_SET_COORD_JOB, fields, 3, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_increment_verify_attempts(const char *roadmap_id, const char *unit_id)
{
   if (!roadmap_id || !roadmap_id[0] || !unit_id || !unit_id[0])
      return -1;
   const char *fields[] = {roadmap_id, unit_id};
   return write_result(call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_INCREMENT_VERIFY_ATTEMPTS, fields, 2, NULL, NULL, 0, NULL));
}

int db1_roadmap_unit_select_next(const char *roadmap_id, char *out_unit_id, size_t len)
{
   if (!roadmap_id || !roadmap_id[0] || !out_unit_id || len == 0)
      return -1;
   const char *fields[] = {roadmap_id};
   char slot_rc[32];
   char *const values[] = {out_unit_id, slot_rc};
   const size_t caps[] = {len, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_ROADMAP_UNIT_SELECT_NEXT, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_execution_plan_create(const char *agent_name, const char *task, const char *steps_json)
{
   if (!agent_name || !agent_name[0] || !task || !task[0] || !steps_json || !steps_json[0])
      return -1;
   const char *fields[] = {agent_name, task, steps_json};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_CREATE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_execution_plan_get(int plan_id, plan_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot4[32];
   char slot9[32];
   char slot10[32];
   char slot11[32];
   char slot12[32];
   char slot13[32];
   char slot14[32];
   char slot15[32];
   char slot16[32];
   char slot17[32];
   char slot18[32];
   char slot20[32];
   char slot21[32];
   char slot26[32];
   char slot27[32];
   char slot28[32];
   char slot29[32];
   char slot30[32];
   char slot31[32];
   char slot32[32];
   char slot33[32];
   char slot34[32];
   char slot35[32];
   char slot37[32];
   char slot38[32];
   char slot43[32];
   char slot44[32];
   char slot45[32];
   char slot46[32];
   char slot47[32];
   char slot48[32];
   char slot49[32];
   char slot50[32];
   char slot51[32];
   char slot52[32];
   char slot54[32];
   char slot55[32];
   char slot60[32];
   char slot61[32];
   char slot62[32];
   char slot63[32];
   char slot64[32];
   char slot65[32];
   char slot66[32];
   char slot67[32];
   char slot68[32];
   char slot69[32];
   char slot71[32];
   char slot72[32];
   char slot77[32];
   char slot78[32];
   char slot79[32];
   char slot80[32];
   char slot81[32];
   char slot82[32];
   char slot83[32];
   char slot84[32];
   char slot85[32];
   char slot86[32];
   char slot88[32];
   char slot89[32];
   char slot94[32];
   char slot95[32];
   char slot96[32];
   char slot97[32];
   char slot98[32];
   char slot99[32];
   char slot100[32];
   char slot101[32];
   char slot102[32];
   char slot103[32];
   char slot105[32];
   char slot106[32];
   char slot111[32];
   char slot112[32];
   char slot113[32];
   char slot114[32];
   char slot115[32];
   char slot116[32];
   char slot117[32];
   char slot118[32];
   char slot119[32];
   char slot120[32];
   char slot122[32];
   char slot123[32];
   char slot128[32];
   char slot129[32];
   char slot130[32];
   char slot131[32];
   char slot132[32];
   char slot133[32];
   char slot134[32];
   char slot135[32];
   char slot136[32];
   char slot137[32];
   char slot139[32];
   char slot140[32];
   char slot145[32];
   char slot146[32];
   char slot147[32];
   char slot148[32];
   char slot149[32];
   char slot150[32];
   char slot151[32];
   char slot152[32];
   char slot153[32];
   char slot154[32];
   char slot156[32];
   char slot157[32];
   char slot162[32];
   char slot163[32];
   char slot164[32];
   char slot165[32];
   char slot166[32];
   char slot167[32];
   char slot168[32];
   char slot169[32];
   char slot170[32];
   char slot171[32];
   char slot173[32];
   char slot174[32];
   char slot179[32];
   char slot180[32];
   char slot181[32];
   char slot182[32];
   char slot183[32];
   char slot184[32];
   char slot185[32];
   char slot186[32];
   char slot187[32];
   char slot188[32];
   char slot190[32];
   char slot191[32];
   char slot196[32];
   char slot197[32];
   char slot198[32];
   char slot199[32];
   char slot200[32];
   char slot201[32];
   char slot202[32];
   char slot203[32];
   char slot204[32];
   char slot205[32];
   char slot207[32];
   char slot208[32];
   char slot213[32];
   char slot214[32];
   char slot215[32];
   char slot216[32];
   char slot217[32];
   char slot218[32];
   char slot219[32];
   char slot220[32];
   char slot221[32];
   char slot222[32];
   char slot224[32];
   char slot225[32];
   char slot230[32];
   char slot231[32];
   char slot232[32];
   char slot233[32];
   char slot234[32];
   char slot235[32];
   char slot236[32];
   char slot237[32];
   char slot238[32];
   char slot239[32];
   char slot241[32];
   char slot242[32];
   char slot247[32];
   char slot248[32];
   char slot249[32];
   char slot250[32];
   char slot251[32];
   char slot252[32];
   char slot253[32];
   char slot254[32];
   char slot255[32];
   char slot256[32];
   char slot258[32];
   char slot259[32];
   char slot264[32];
   char slot265[32];
   char slot266[32];
   char slot267[32];
   char slot268[32];
   char slot269[32];
   char slot270[32];
   char slot271[32];
   char slot272[32];
   char slot273[32];
   char slot275[32];
   char slot276[32];
   char slot281[32];
   char slot282[32];
   char slot283[32];
   char slot284[32];
   char slot285[32];
   char slot286[32];
   char slot287[32];
   char slot288[32];
   char slot289[32];
   char slot290[32];
   char slot292[32];
   char slot293[32];
   char slot298[32];
   char slot299[32];
   char slot300[32];
   char slot301[32];
   char slot302[32];
   char slot303[32];
   char slot304[32];
   char slot305[32];
   char slot306[32];
   char slot307[32];
   char slot309[32];
   char slot310[32];
   char slot315[32];
   char slot316[32];
   char slot317[32];
   char slot318[32];
   char slot319[32];
   char slot320[32];
   char slot321[32];
   char slot322[32];
   char slot323[32];
   char slot324[32];
   char slot326[32];
   char slot327[32];
   char slot332[32];
   char slot333[32];
   char slot334[32];
   char slot335[32];
   char slot336[32];
   char slot337[32];
   char slot338[32];
   char slot339[32];
   char slot340[32];
   char slot341[32];
   char slot343[32];
   char slot344[32];
   char slot349[32];
   char slot350[32];
   char slot351[32];
   char slot352[32];
   char slot353[32];
   char slot354[32];
   char slot355[32];
   char slot356[32];
   char slot357[32];
   char slot358[32];
   char slot360[32];
   char slot361[32];
   char slot366[32];
   char slot367[32];
   char slot368[32];
   char slot369[32];
   char slot370[32];
   char slot371[32];
   char slot372[32];
   char slot373[32];
   char slot374[32];
   char slot375[32];
   char slot377[32];
   char slot378[32];
   char slot383[32];
   char slot384[32];
   char slot385[32];
   char slot386[32];
   char slot387[32];
   char slot388[32];
   char slot389[32];
   char slot390[32];
   char slot391[32];
   char slot392[32];
   char slot394[32];
   char slot395[32];
   char slot400[32];
   char slot401[32];
   char slot402[32];
   char slot403[32];
   char slot404[32];
   char slot405[32];
   char slot406[32];
   char slot407[32];
   char slot408[32];
   char slot409[32];
   char slot411[32];
   char slot412[32];
   char slot417[32];
   char slot418[32];
   char slot419[32];
   char slot420[32];
   char slot421[32];
   char slot422[32];
   char slot423[32];
   char slot424[32];
   char slot425[32];
   char slot426[32];
   char slot428[32];
   char slot429[32];
   char slot434[32];
   char slot435[32];
   char slot436[32];
   char slot437[32];
   char slot438[32];
   char slot439[32];
   char slot440[32];
   char slot441[32];
   char slot442[32];
   char slot443[32];
   char slot445[32];
   char slot446[32];
   char slot451[32];
   char slot452[32];
   char slot453[32];
   char slot454[32];
   char slot455[32];
   char slot456[32];
   char slot457[32];
   char slot458[32];
   char slot459[32];
   char slot460[32];
   char slot462[32];
   char slot463[32];
   char slot468[32];
   char slot469[32];
   char slot470[32];
   char slot471[32];
   char slot472[32];
   char slot473[32];
   char slot474[32];
   char slot475[32];
   char slot476[32];
   char slot477[32];
   char slot479[32];
   char slot480[32];
   char slot485[32];
   char slot486[32];
   char slot487[32];
   char slot488[32];
   char slot489[32];
   char slot490[32];
   char slot491[32];
   char slot492[32];
   char slot493[32];
   char slot494[32];
   char slot496[32];
   char slot497[32];
   char slot502[32];
   char slot503[32];
   char slot504[32];
   char slot505[32];
   char slot506[32];
   char slot507[32];
   char slot508[32];
   char slot509[32];
   char slot510[32];
   char slot511[32];
   char slot513[32];
   char slot514[32];
   char slot519[32];
   char slot520[32];
   char slot521[32];
   char slot522[32];
   char slot523[32];
   char slot524[32];
   char slot525[32];
   char slot526[32];
   char slot527[32];
   char slot528[32];
   char slot530[32];
   char slot531[32];
   char slot536[32];
   char slot537[32];
   char slot538[32];
   char slot539[32];
   char slot540[32];
   char slot541[32];
   char slot542[32];
   char slot543[32];
   char slot544[32];
   char slot545[32];
   char slot547[32];
   char slot548[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->agent_name, out->task, out->status, slot4, out->steps[0].action, out->steps[0].precondition, out->steps[0].success_predicate, out->steps[0].rollback, slot9, slot10, slot11, slot12, slot13, slot14, slot15, slot16, slot17, slot18, out->steps[0].output, slot20, slot21, out->steps[1].action, out->steps[1].precondition, out->steps[1].success_predicate, out->steps[1].rollback, slot26, slot27, slot28, slot29, slot30, slot31, slot32, slot33, slot34, slot35, out->steps[1].output, slot37, slot38, out->steps[2].action, out->steps[2].precondition, out->steps[2].success_predicate, out->steps[2].rollback, slot43, slot44, slot45, slot46, slot47, slot48, slot49, slot50, slot51, slot52, out->steps[2].output, slot54, slot55, out->steps[3].action, out->steps[3].precondition, out->steps[3].success_predicate, out->steps[3].rollback, slot60, slot61, slot62, slot63, slot64, slot65, slot66, slot67, slot68, slot69, out->steps[3].output, slot71, slot72, out->steps[4].action, out->steps[4].precondition, out->steps[4].success_predicate, out->steps[4].rollback, slot77, slot78, slot79, slot80, slot81, slot82, slot83, slot84, slot85, slot86, out->steps[4].output, slot88, slot89, out->steps[5].action, out->steps[5].precondition, out->steps[5].success_predicate, out->steps[5].rollback, slot94, slot95, slot96, slot97, slot98, slot99, slot100, slot101, slot102, slot103, out->steps[5].output, slot105, slot106, out->steps[6].action, out->steps[6].precondition, out->steps[6].success_predicate, out->steps[6].rollback, slot111, slot112, slot113, slot114, slot115, slot116, slot117, slot118, slot119, slot120, out->steps[6].output, slot122, slot123, out->steps[7].action, out->steps[7].precondition, out->steps[7].success_predicate, out->steps[7].rollback, slot128, slot129, slot130, slot131, slot132, slot133, slot134, slot135, slot136, slot137, out->steps[7].output, slot139, slot140, out->steps[8].action, out->steps[8].precondition, out->steps[8].success_predicate, out->steps[8].rollback, slot145, slot146, slot147, slot148, slot149, slot150, slot151, slot152, slot153, slot154, out->steps[8].output, slot156, slot157, out->steps[9].action, out->steps[9].precondition, out->steps[9].success_predicate, out->steps[9].rollback, slot162, slot163, slot164, slot165, slot166, slot167, slot168, slot169, slot170, slot171, out->steps[9].output, slot173, slot174, out->steps[10].action, out->steps[10].precondition, out->steps[10].success_predicate, out->steps[10].rollback, slot179, slot180, slot181, slot182, slot183, slot184, slot185, slot186, slot187, slot188, out->steps[10].output, slot190, slot191, out->steps[11].action, out->steps[11].precondition, out->steps[11].success_predicate, out->steps[11].rollback, slot196, slot197, slot198, slot199, slot200, slot201, slot202, slot203, slot204, slot205, out->steps[11].output, slot207, slot208, out->steps[12].action, out->steps[12].precondition, out->steps[12].success_predicate, out->steps[12].rollback, slot213, slot214, slot215, slot216, slot217, slot218, slot219, slot220, slot221, slot222, out->steps[12].output, slot224, slot225, out->steps[13].action, out->steps[13].precondition, out->steps[13].success_predicate, out->steps[13].rollback, slot230, slot231, slot232, slot233, slot234, slot235, slot236, slot237, slot238, slot239, out->steps[13].output, slot241, slot242, out->steps[14].action, out->steps[14].precondition, out->steps[14].success_predicate, out->steps[14].rollback, slot247, slot248, slot249, slot250, slot251, slot252, slot253, slot254, slot255, slot256, out->steps[14].output, slot258, slot259, out->steps[15].action, out->steps[15].precondition, out->steps[15].success_predicate, out->steps[15].rollback, slot264, slot265, slot266, slot267, slot268, slot269, slot270, slot271, slot272, slot273, out->steps[15].output, slot275, slot276, out->steps[16].action, out->steps[16].precondition, out->steps[16].success_predicate, out->steps[16].rollback, slot281, slot282, slot283, slot284, slot285, slot286, slot287, slot288, slot289, slot290, out->steps[16].output, slot292, slot293, out->steps[17].action, out->steps[17].precondition, out->steps[17].success_predicate, out->steps[17].rollback, slot298, slot299, slot300, slot301, slot302, slot303, slot304, slot305, slot306, slot307, out->steps[17].output, slot309, slot310, out->steps[18].action, out->steps[18].precondition, out->steps[18].success_predicate, out->steps[18].rollback, slot315, slot316, slot317, slot318, slot319, slot320, slot321, slot322, slot323, slot324, out->steps[18].output, slot326, slot327, out->steps[19].action, out->steps[19].precondition, out->steps[19].success_predicate, out->steps[19].rollback, slot332, slot333, slot334, slot335, slot336, slot337, slot338, slot339, slot340, slot341, out->steps[19].output, slot343, slot344, out->steps[20].action, out->steps[20].precondition, out->steps[20].success_predicate, out->steps[20].rollback, slot349, slot350, slot351, slot352, slot353, slot354, slot355, slot356, slot357, slot358, out->steps[20].output, slot360, slot361, out->steps[21].action, out->steps[21].precondition, out->steps[21].success_predicate, out->steps[21].rollback, slot366, slot367, slot368, slot369, slot370, slot371, slot372, slot373, slot374, slot375, out->steps[21].output, slot377, slot378, out->steps[22].action, out->steps[22].precondition, out->steps[22].success_predicate, out->steps[22].rollback, slot383, slot384, slot385, slot386, slot387, slot388, slot389, slot390, slot391, slot392, out->steps[22].output, slot394, slot395, out->steps[23].action, out->steps[23].precondition, out->steps[23].success_predicate, out->steps[23].rollback, slot400, slot401, slot402, slot403, slot404, slot405, slot406, slot407, slot408, slot409, out->steps[23].output, slot411, slot412, out->steps[24].action, out->steps[24].precondition, out->steps[24].success_predicate, out->steps[24].rollback, slot417, slot418, slot419, slot420, slot421, slot422, slot423, slot424, slot425, slot426, out->steps[24].output, slot428, slot429, out->steps[25].action, out->steps[25].precondition, out->steps[25].success_predicate, out->steps[25].rollback, slot434, slot435, slot436, slot437, slot438, slot439, slot440, slot441, slot442, slot443, out->steps[25].output, slot445, slot446, out->steps[26].action, out->steps[26].precondition, out->steps[26].success_predicate, out->steps[26].rollback, slot451, slot452, slot453, slot454, slot455, slot456, slot457, slot458, slot459, slot460, out->steps[26].output, slot462, slot463, out->steps[27].action, out->steps[27].precondition, out->steps[27].success_predicate, out->steps[27].rollback, slot468, slot469, slot470, slot471, slot472, slot473, slot474, slot475, slot476, slot477, out->steps[27].output, slot479, slot480, out->steps[28].action, out->steps[28].precondition, out->steps[28].success_predicate, out->steps[28].rollback, slot485, slot486, slot487, slot488, slot489, slot490, slot491, slot492, slot493, slot494, out->steps[28].output, slot496, slot497, out->steps[29].action, out->steps[29].precondition, out->steps[29].success_predicate, out->steps[29].rollback, slot502, slot503, slot504, slot505, slot506, slot507, slot508, slot509, slot510, slot511, out->steps[29].output, slot513, slot514, out->steps[30].action, out->steps[30].precondition, out->steps[30].success_predicate, out->steps[30].rollback, slot519, slot520, slot521, slot522, slot523, slot524, slot525, slot526, slot527, slot528, out->steps[30].output, slot530, slot531, out->steps[31].action, out->steps[31].precondition, out->steps[31].success_predicate, out->steps[31].rollback, slot536, slot537, slot538, slot539, slot540, slot541, slot542, slot543, slot544, slot545, out->steps[31].output, slot547, slot548};
   const size_t caps[] = {sizeof slot0, sizeof out->agent_name, sizeof out->task, sizeof out->status, sizeof slot4, sizeof out->steps[0].action, sizeof out->steps[0].precondition, sizeof out->steps[0].success_predicate, sizeof out->steps[0].rollback, sizeof slot9, sizeof slot10, sizeof slot11, sizeof slot12, sizeof slot13, sizeof slot14, sizeof slot15, sizeof slot16, sizeof slot17, sizeof slot18, sizeof out->steps[0].output, sizeof slot20, sizeof slot21, sizeof out->steps[1].action, sizeof out->steps[1].precondition, sizeof out->steps[1].success_predicate, sizeof out->steps[1].rollback, sizeof slot26, sizeof slot27, sizeof slot28, sizeof slot29, sizeof slot30, sizeof slot31, sizeof slot32, sizeof slot33, sizeof slot34, sizeof slot35, sizeof out->steps[1].output, sizeof slot37, sizeof slot38, sizeof out->steps[2].action, sizeof out->steps[2].precondition, sizeof out->steps[2].success_predicate, sizeof out->steps[2].rollback, sizeof slot43, sizeof slot44, sizeof slot45, sizeof slot46, sizeof slot47, sizeof slot48, sizeof slot49, sizeof slot50, sizeof slot51, sizeof slot52, sizeof out->steps[2].output, sizeof slot54, sizeof slot55, sizeof out->steps[3].action, sizeof out->steps[3].precondition, sizeof out->steps[3].success_predicate, sizeof out->steps[3].rollback, sizeof slot60, sizeof slot61, sizeof slot62, sizeof slot63, sizeof slot64, sizeof slot65, sizeof slot66, sizeof slot67, sizeof slot68, sizeof slot69, sizeof out->steps[3].output, sizeof slot71, sizeof slot72, sizeof out->steps[4].action, sizeof out->steps[4].precondition, sizeof out->steps[4].success_predicate, sizeof out->steps[4].rollback, sizeof slot77, sizeof slot78, sizeof slot79, sizeof slot80, sizeof slot81, sizeof slot82, sizeof slot83, sizeof slot84, sizeof slot85, sizeof slot86, sizeof out->steps[4].output, sizeof slot88, sizeof slot89, sizeof out->steps[5].action, sizeof out->steps[5].precondition, sizeof out->steps[5].success_predicate, sizeof out->steps[5].rollback, sizeof slot94, sizeof slot95, sizeof slot96, sizeof slot97, sizeof slot98, sizeof slot99, sizeof slot100, sizeof slot101, sizeof slot102, sizeof slot103, sizeof out->steps[5].output, sizeof slot105, sizeof slot106, sizeof out->steps[6].action, sizeof out->steps[6].precondition, sizeof out->steps[6].success_predicate, sizeof out->steps[6].rollback, sizeof slot111, sizeof slot112, sizeof slot113, sizeof slot114, sizeof slot115, sizeof slot116, sizeof slot117, sizeof slot118, sizeof slot119, sizeof slot120, sizeof out->steps[6].output, sizeof slot122, sizeof slot123, sizeof out->steps[7].action, sizeof out->steps[7].precondition, sizeof out->steps[7].success_predicate, sizeof out->steps[7].rollback, sizeof slot128, sizeof slot129, sizeof slot130, sizeof slot131, sizeof slot132, sizeof slot133, sizeof slot134, sizeof slot135, sizeof slot136, sizeof slot137, sizeof out->steps[7].output, sizeof slot139, sizeof slot140, sizeof out->steps[8].action, sizeof out->steps[8].precondition, sizeof out->steps[8].success_predicate, sizeof out->steps[8].rollback, sizeof slot145, sizeof slot146, sizeof slot147, sizeof slot148, sizeof slot149, sizeof slot150, sizeof slot151, sizeof slot152, sizeof slot153, sizeof slot154, sizeof out->steps[8].output, sizeof slot156, sizeof slot157, sizeof out->steps[9].action, sizeof out->steps[9].precondition, sizeof out->steps[9].success_predicate, sizeof out->steps[9].rollback, sizeof slot162, sizeof slot163, sizeof slot164, sizeof slot165, sizeof slot166, sizeof slot167, sizeof slot168, sizeof slot169, sizeof slot170, sizeof slot171, sizeof out->steps[9].output, sizeof slot173, sizeof slot174, sizeof out->steps[10].action, sizeof out->steps[10].precondition, sizeof out->steps[10].success_predicate, sizeof out->steps[10].rollback, sizeof slot179, sizeof slot180, sizeof slot181, sizeof slot182, sizeof slot183, sizeof slot184, sizeof slot185, sizeof slot186, sizeof slot187, sizeof slot188, sizeof out->steps[10].output, sizeof slot190, sizeof slot191, sizeof out->steps[11].action, sizeof out->steps[11].precondition, sizeof out->steps[11].success_predicate, sizeof out->steps[11].rollback, sizeof slot196, sizeof slot197, sizeof slot198, sizeof slot199, sizeof slot200, sizeof slot201, sizeof slot202, sizeof slot203, sizeof slot204, sizeof slot205, sizeof out->steps[11].output, sizeof slot207, sizeof slot208, sizeof out->steps[12].action, sizeof out->steps[12].precondition, sizeof out->steps[12].success_predicate, sizeof out->steps[12].rollback, sizeof slot213, sizeof slot214, sizeof slot215, sizeof slot216, sizeof slot217, sizeof slot218, sizeof slot219, sizeof slot220, sizeof slot221, sizeof slot222, sizeof out->steps[12].output, sizeof slot224, sizeof slot225, sizeof out->steps[13].action, sizeof out->steps[13].precondition, sizeof out->steps[13].success_predicate, sizeof out->steps[13].rollback, sizeof slot230, sizeof slot231, sizeof slot232, sizeof slot233, sizeof slot234, sizeof slot235, sizeof slot236, sizeof slot237, sizeof slot238, sizeof slot239, sizeof out->steps[13].output, sizeof slot241, sizeof slot242, sizeof out->steps[14].action, sizeof out->steps[14].precondition, sizeof out->steps[14].success_predicate, sizeof out->steps[14].rollback, sizeof slot247, sizeof slot248, sizeof slot249, sizeof slot250, sizeof slot251, sizeof slot252, sizeof slot253, sizeof slot254, sizeof slot255, sizeof slot256, sizeof out->steps[14].output, sizeof slot258, sizeof slot259, sizeof out->steps[15].action, sizeof out->steps[15].precondition, sizeof out->steps[15].success_predicate, sizeof out->steps[15].rollback, sizeof slot264, sizeof slot265, sizeof slot266, sizeof slot267, sizeof slot268, sizeof slot269, sizeof slot270, sizeof slot271, sizeof slot272, sizeof slot273, sizeof out->steps[15].output, sizeof slot275, sizeof slot276, sizeof out->steps[16].action, sizeof out->steps[16].precondition, sizeof out->steps[16].success_predicate, sizeof out->steps[16].rollback, sizeof slot281, sizeof slot282, sizeof slot283, sizeof slot284, sizeof slot285, sizeof slot286, sizeof slot287, sizeof slot288, sizeof slot289, sizeof slot290, sizeof out->steps[16].output, sizeof slot292, sizeof slot293, sizeof out->steps[17].action, sizeof out->steps[17].precondition, sizeof out->steps[17].success_predicate, sizeof out->steps[17].rollback, sizeof slot298, sizeof slot299, sizeof slot300, sizeof slot301, sizeof slot302, sizeof slot303, sizeof slot304, sizeof slot305, sizeof slot306, sizeof slot307, sizeof out->steps[17].output, sizeof slot309, sizeof slot310, sizeof out->steps[18].action, sizeof out->steps[18].precondition, sizeof out->steps[18].success_predicate, sizeof out->steps[18].rollback, sizeof slot315, sizeof slot316, sizeof slot317, sizeof slot318, sizeof slot319, sizeof slot320, sizeof slot321, sizeof slot322, sizeof slot323, sizeof slot324, sizeof out->steps[18].output, sizeof slot326, sizeof slot327, sizeof out->steps[19].action, sizeof out->steps[19].precondition, sizeof out->steps[19].success_predicate, sizeof out->steps[19].rollback, sizeof slot332, sizeof slot333, sizeof slot334, sizeof slot335, sizeof slot336, sizeof slot337, sizeof slot338, sizeof slot339, sizeof slot340, sizeof slot341, sizeof out->steps[19].output, sizeof slot343, sizeof slot344, sizeof out->steps[20].action, sizeof out->steps[20].precondition, sizeof out->steps[20].success_predicate, sizeof out->steps[20].rollback, sizeof slot349, sizeof slot350, sizeof slot351, sizeof slot352, sizeof slot353, sizeof slot354, sizeof slot355, sizeof slot356, sizeof slot357, sizeof slot358, sizeof out->steps[20].output, sizeof slot360, sizeof slot361, sizeof out->steps[21].action, sizeof out->steps[21].precondition, sizeof out->steps[21].success_predicate, sizeof out->steps[21].rollback, sizeof slot366, sizeof slot367, sizeof slot368, sizeof slot369, sizeof slot370, sizeof slot371, sizeof slot372, sizeof slot373, sizeof slot374, sizeof slot375, sizeof out->steps[21].output, sizeof slot377, sizeof slot378, sizeof out->steps[22].action, sizeof out->steps[22].precondition, sizeof out->steps[22].success_predicate, sizeof out->steps[22].rollback, sizeof slot383, sizeof slot384, sizeof slot385, sizeof slot386, sizeof slot387, sizeof slot388, sizeof slot389, sizeof slot390, sizeof slot391, sizeof slot392, sizeof out->steps[22].output, sizeof slot394, sizeof slot395, sizeof out->steps[23].action, sizeof out->steps[23].precondition, sizeof out->steps[23].success_predicate, sizeof out->steps[23].rollback, sizeof slot400, sizeof slot401, sizeof slot402, sizeof slot403, sizeof slot404, sizeof slot405, sizeof slot406, sizeof slot407, sizeof slot408, sizeof slot409, sizeof out->steps[23].output, sizeof slot411, sizeof slot412, sizeof out->steps[24].action, sizeof out->steps[24].precondition, sizeof out->steps[24].success_predicate, sizeof out->steps[24].rollback, sizeof slot417, sizeof slot418, sizeof slot419, sizeof slot420, sizeof slot421, sizeof slot422, sizeof slot423, sizeof slot424, sizeof slot425, sizeof slot426, sizeof out->steps[24].output, sizeof slot428, sizeof slot429, sizeof out->steps[25].action, sizeof out->steps[25].precondition, sizeof out->steps[25].success_predicate, sizeof out->steps[25].rollback, sizeof slot434, sizeof slot435, sizeof slot436, sizeof slot437, sizeof slot438, sizeof slot439, sizeof slot440, sizeof slot441, sizeof slot442, sizeof slot443, sizeof out->steps[25].output, sizeof slot445, sizeof slot446, sizeof out->steps[26].action, sizeof out->steps[26].precondition, sizeof out->steps[26].success_predicate, sizeof out->steps[26].rollback, sizeof slot451, sizeof slot452, sizeof slot453, sizeof slot454, sizeof slot455, sizeof slot456, sizeof slot457, sizeof slot458, sizeof slot459, sizeof slot460, sizeof out->steps[26].output, sizeof slot462, sizeof slot463, sizeof out->steps[27].action, sizeof out->steps[27].precondition, sizeof out->steps[27].success_predicate, sizeof out->steps[27].rollback, sizeof slot468, sizeof slot469, sizeof slot470, sizeof slot471, sizeof slot472, sizeof slot473, sizeof slot474, sizeof slot475, sizeof slot476, sizeof slot477, sizeof out->steps[27].output, sizeof slot479, sizeof slot480, sizeof out->steps[28].action, sizeof out->steps[28].precondition, sizeof out->steps[28].success_predicate, sizeof out->steps[28].rollback, sizeof slot485, sizeof slot486, sizeof slot487, sizeof slot488, sizeof slot489, sizeof slot490, sizeof slot491, sizeof slot492, sizeof slot493, sizeof slot494, sizeof out->steps[28].output, sizeof slot496, sizeof slot497, sizeof out->steps[29].action, sizeof out->steps[29].precondition, sizeof out->steps[29].success_predicate, sizeof out->steps[29].rollback, sizeof slot502, sizeof slot503, sizeof slot504, sizeof slot505, sizeof slot506, sizeof slot507, sizeof slot508, sizeof slot509, sizeof slot510, sizeof slot511, sizeof out->steps[29].output, sizeof slot513, sizeof slot514, sizeof out->steps[30].action, sizeof out->steps[30].precondition, sizeof out->steps[30].success_predicate, sizeof out->steps[30].rollback, sizeof slot519, sizeof slot520, sizeof slot521, sizeof slot522, sizeof slot523, sizeof slot524, sizeof slot525, sizeof slot526, sizeof slot527, sizeof slot528, sizeof out->steps[30].output, sizeof slot530, sizeof slot531, sizeof out->steps[31].action, sizeof out->steps[31].precondition, sizeof out->steps[31].success_predicate, sizeof out->steps[31].rollback, sizeof slot536, sizeof slot537, sizeof slot538, sizeof slot539, sizeof slot540, sizeof slot541, sizeof slot542, sizeof slot543, sizeof slot544, sizeof slot545, sizeof out->steps[31].output, sizeof slot547, sizeof slot548};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_GET, fields, 1, values, caps, 549, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->steps[0].id = (int)strtol(slot4, NULL, 10);
   out->steps[0].depends_on[0] = (int)strtol(slot9, NULL, 10);
   out->steps[0].depends_on[1] = (int)strtol(slot10, NULL, 10);
   out->steps[0].depends_on[2] = (int)strtol(slot11, NULL, 10);
   out->steps[0].depends_on[3] = (int)strtol(slot12, NULL, 10);
   out->steps[0].depends_on[4] = (int)strtol(slot13, NULL, 10);
   out->steps[0].depends_on[5] = (int)strtol(slot14, NULL, 10);
   out->steps[0].depends_on[6] = (int)strtol(slot15, NULL, 10);
   out->steps[0].depends_on[7] = (int)strtol(slot16, NULL, 10);
   out->steps[0].dep_count = (int)strtol(slot17, NULL, 10);
   out->steps[0].status = (int)strtol(slot18, NULL, 10);
   out->steps[0].wave = (int)strtol(slot20, NULL, 10);
   out->steps[1].id = (int)strtol(slot21, NULL, 10);
   out->steps[1].depends_on[0] = (int)strtol(slot26, NULL, 10);
   out->steps[1].depends_on[1] = (int)strtol(slot27, NULL, 10);
   out->steps[1].depends_on[2] = (int)strtol(slot28, NULL, 10);
   out->steps[1].depends_on[3] = (int)strtol(slot29, NULL, 10);
   out->steps[1].depends_on[4] = (int)strtol(slot30, NULL, 10);
   out->steps[1].depends_on[5] = (int)strtol(slot31, NULL, 10);
   out->steps[1].depends_on[6] = (int)strtol(slot32, NULL, 10);
   out->steps[1].depends_on[7] = (int)strtol(slot33, NULL, 10);
   out->steps[1].dep_count = (int)strtol(slot34, NULL, 10);
   out->steps[1].status = (int)strtol(slot35, NULL, 10);
   out->steps[1].wave = (int)strtol(slot37, NULL, 10);
   out->steps[2].id = (int)strtol(slot38, NULL, 10);
   out->steps[2].depends_on[0] = (int)strtol(slot43, NULL, 10);
   out->steps[2].depends_on[1] = (int)strtol(slot44, NULL, 10);
   out->steps[2].depends_on[2] = (int)strtol(slot45, NULL, 10);
   out->steps[2].depends_on[3] = (int)strtol(slot46, NULL, 10);
   out->steps[2].depends_on[4] = (int)strtol(slot47, NULL, 10);
   out->steps[2].depends_on[5] = (int)strtol(slot48, NULL, 10);
   out->steps[2].depends_on[6] = (int)strtol(slot49, NULL, 10);
   out->steps[2].depends_on[7] = (int)strtol(slot50, NULL, 10);
   out->steps[2].dep_count = (int)strtol(slot51, NULL, 10);
   out->steps[2].status = (int)strtol(slot52, NULL, 10);
   out->steps[2].wave = (int)strtol(slot54, NULL, 10);
   out->steps[3].id = (int)strtol(slot55, NULL, 10);
   out->steps[3].depends_on[0] = (int)strtol(slot60, NULL, 10);
   out->steps[3].depends_on[1] = (int)strtol(slot61, NULL, 10);
   out->steps[3].depends_on[2] = (int)strtol(slot62, NULL, 10);
   out->steps[3].depends_on[3] = (int)strtol(slot63, NULL, 10);
   out->steps[3].depends_on[4] = (int)strtol(slot64, NULL, 10);
   out->steps[3].depends_on[5] = (int)strtol(slot65, NULL, 10);
   out->steps[3].depends_on[6] = (int)strtol(slot66, NULL, 10);
   out->steps[3].depends_on[7] = (int)strtol(slot67, NULL, 10);
   out->steps[3].dep_count = (int)strtol(slot68, NULL, 10);
   out->steps[3].status = (int)strtol(slot69, NULL, 10);
   out->steps[3].wave = (int)strtol(slot71, NULL, 10);
   out->steps[4].id = (int)strtol(slot72, NULL, 10);
   out->steps[4].depends_on[0] = (int)strtol(slot77, NULL, 10);
   out->steps[4].depends_on[1] = (int)strtol(slot78, NULL, 10);
   out->steps[4].depends_on[2] = (int)strtol(slot79, NULL, 10);
   out->steps[4].depends_on[3] = (int)strtol(slot80, NULL, 10);
   out->steps[4].depends_on[4] = (int)strtol(slot81, NULL, 10);
   out->steps[4].depends_on[5] = (int)strtol(slot82, NULL, 10);
   out->steps[4].depends_on[6] = (int)strtol(slot83, NULL, 10);
   out->steps[4].depends_on[7] = (int)strtol(slot84, NULL, 10);
   out->steps[4].dep_count = (int)strtol(slot85, NULL, 10);
   out->steps[4].status = (int)strtol(slot86, NULL, 10);
   out->steps[4].wave = (int)strtol(slot88, NULL, 10);
   out->steps[5].id = (int)strtol(slot89, NULL, 10);
   out->steps[5].depends_on[0] = (int)strtol(slot94, NULL, 10);
   out->steps[5].depends_on[1] = (int)strtol(slot95, NULL, 10);
   out->steps[5].depends_on[2] = (int)strtol(slot96, NULL, 10);
   out->steps[5].depends_on[3] = (int)strtol(slot97, NULL, 10);
   out->steps[5].depends_on[4] = (int)strtol(slot98, NULL, 10);
   out->steps[5].depends_on[5] = (int)strtol(slot99, NULL, 10);
   out->steps[5].depends_on[6] = (int)strtol(slot100, NULL, 10);
   out->steps[5].depends_on[7] = (int)strtol(slot101, NULL, 10);
   out->steps[5].dep_count = (int)strtol(slot102, NULL, 10);
   out->steps[5].status = (int)strtol(slot103, NULL, 10);
   out->steps[5].wave = (int)strtol(slot105, NULL, 10);
   out->steps[6].id = (int)strtol(slot106, NULL, 10);
   out->steps[6].depends_on[0] = (int)strtol(slot111, NULL, 10);
   out->steps[6].depends_on[1] = (int)strtol(slot112, NULL, 10);
   out->steps[6].depends_on[2] = (int)strtol(slot113, NULL, 10);
   out->steps[6].depends_on[3] = (int)strtol(slot114, NULL, 10);
   out->steps[6].depends_on[4] = (int)strtol(slot115, NULL, 10);
   out->steps[6].depends_on[5] = (int)strtol(slot116, NULL, 10);
   out->steps[6].depends_on[6] = (int)strtol(slot117, NULL, 10);
   out->steps[6].depends_on[7] = (int)strtol(slot118, NULL, 10);
   out->steps[6].dep_count = (int)strtol(slot119, NULL, 10);
   out->steps[6].status = (int)strtol(slot120, NULL, 10);
   out->steps[6].wave = (int)strtol(slot122, NULL, 10);
   out->steps[7].id = (int)strtol(slot123, NULL, 10);
   out->steps[7].depends_on[0] = (int)strtol(slot128, NULL, 10);
   out->steps[7].depends_on[1] = (int)strtol(slot129, NULL, 10);
   out->steps[7].depends_on[2] = (int)strtol(slot130, NULL, 10);
   out->steps[7].depends_on[3] = (int)strtol(slot131, NULL, 10);
   out->steps[7].depends_on[4] = (int)strtol(slot132, NULL, 10);
   out->steps[7].depends_on[5] = (int)strtol(slot133, NULL, 10);
   out->steps[7].depends_on[6] = (int)strtol(slot134, NULL, 10);
   out->steps[7].depends_on[7] = (int)strtol(slot135, NULL, 10);
   out->steps[7].dep_count = (int)strtol(slot136, NULL, 10);
   out->steps[7].status = (int)strtol(slot137, NULL, 10);
   out->steps[7].wave = (int)strtol(slot139, NULL, 10);
   out->steps[8].id = (int)strtol(slot140, NULL, 10);
   out->steps[8].depends_on[0] = (int)strtol(slot145, NULL, 10);
   out->steps[8].depends_on[1] = (int)strtol(slot146, NULL, 10);
   out->steps[8].depends_on[2] = (int)strtol(slot147, NULL, 10);
   out->steps[8].depends_on[3] = (int)strtol(slot148, NULL, 10);
   out->steps[8].depends_on[4] = (int)strtol(slot149, NULL, 10);
   out->steps[8].depends_on[5] = (int)strtol(slot150, NULL, 10);
   out->steps[8].depends_on[6] = (int)strtol(slot151, NULL, 10);
   out->steps[8].depends_on[7] = (int)strtol(slot152, NULL, 10);
   out->steps[8].dep_count = (int)strtol(slot153, NULL, 10);
   out->steps[8].status = (int)strtol(slot154, NULL, 10);
   out->steps[8].wave = (int)strtol(slot156, NULL, 10);
   out->steps[9].id = (int)strtol(slot157, NULL, 10);
   out->steps[9].depends_on[0] = (int)strtol(slot162, NULL, 10);
   out->steps[9].depends_on[1] = (int)strtol(slot163, NULL, 10);
   out->steps[9].depends_on[2] = (int)strtol(slot164, NULL, 10);
   out->steps[9].depends_on[3] = (int)strtol(slot165, NULL, 10);
   out->steps[9].depends_on[4] = (int)strtol(slot166, NULL, 10);
   out->steps[9].depends_on[5] = (int)strtol(slot167, NULL, 10);
   out->steps[9].depends_on[6] = (int)strtol(slot168, NULL, 10);
   out->steps[9].depends_on[7] = (int)strtol(slot169, NULL, 10);
   out->steps[9].dep_count = (int)strtol(slot170, NULL, 10);
   out->steps[9].status = (int)strtol(slot171, NULL, 10);
   out->steps[9].wave = (int)strtol(slot173, NULL, 10);
   out->steps[10].id = (int)strtol(slot174, NULL, 10);
   out->steps[10].depends_on[0] = (int)strtol(slot179, NULL, 10);
   out->steps[10].depends_on[1] = (int)strtol(slot180, NULL, 10);
   out->steps[10].depends_on[2] = (int)strtol(slot181, NULL, 10);
   out->steps[10].depends_on[3] = (int)strtol(slot182, NULL, 10);
   out->steps[10].depends_on[4] = (int)strtol(slot183, NULL, 10);
   out->steps[10].depends_on[5] = (int)strtol(slot184, NULL, 10);
   out->steps[10].depends_on[6] = (int)strtol(slot185, NULL, 10);
   out->steps[10].depends_on[7] = (int)strtol(slot186, NULL, 10);
   out->steps[10].dep_count = (int)strtol(slot187, NULL, 10);
   out->steps[10].status = (int)strtol(slot188, NULL, 10);
   out->steps[10].wave = (int)strtol(slot190, NULL, 10);
   out->steps[11].id = (int)strtol(slot191, NULL, 10);
   out->steps[11].depends_on[0] = (int)strtol(slot196, NULL, 10);
   out->steps[11].depends_on[1] = (int)strtol(slot197, NULL, 10);
   out->steps[11].depends_on[2] = (int)strtol(slot198, NULL, 10);
   out->steps[11].depends_on[3] = (int)strtol(slot199, NULL, 10);
   out->steps[11].depends_on[4] = (int)strtol(slot200, NULL, 10);
   out->steps[11].depends_on[5] = (int)strtol(slot201, NULL, 10);
   out->steps[11].depends_on[6] = (int)strtol(slot202, NULL, 10);
   out->steps[11].depends_on[7] = (int)strtol(slot203, NULL, 10);
   out->steps[11].dep_count = (int)strtol(slot204, NULL, 10);
   out->steps[11].status = (int)strtol(slot205, NULL, 10);
   out->steps[11].wave = (int)strtol(slot207, NULL, 10);
   out->steps[12].id = (int)strtol(slot208, NULL, 10);
   out->steps[12].depends_on[0] = (int)strtol(slot213, NULL, 10);
   out->steps[12].depends_on[1] = (int)strtol(slot214, NULL, 10);
   out->steps[12].depends_on[2] = (int)strtol(slot215, NULL, 10);
   out->steps[12].depends_on[3] = (int)strtol(slot216, NULL, 10);
   out->steps[12].depends_on[4] = (int)strtol(slot217, NULL, 10);
   out->steps[12].depends_on[5] = (int)strtol(slot218, NULL, 10);
   out->steps[12].depends_on[6] = (int)strtol(slot219, NULL, 10);
   out->steps[12].depends_on[7] = (int)strtol(slot220, NULL, 10);
   out->steps[12].dep_count = (int)strtol(slot221, NULL, 10);
   out->steps[12].status = (int)strtol(slot222, NULL, 10);
   out->steps[12].wave = (int)strtol(slot224, NULL, 10);
   out->steps[13].id = (int)strtol(slot225, NULL, 10);
   out->steps[13].depends_on[0] = (int)strtol(slot230, NULL, 10);
   out->steps[13].depends_on[1] = (int)strtol(slot231, NULL, 10);
   out->steps[13].depends_on[2] = (int)strtol(slot232, NULL, 10);
   out->steps[13].depends_on[3] = (int)strtol(slot233, NULL, 10);
   out->steps[13].depends_on[4] = (int)strtol(slot234, NULL, 10);
   out->steps[13].depends_on[5] = (int)strtol(slot235, NULL, 10);
   out->steps[13].depends_on[6] = (int)strtol(slot236, NULL, 10);
   out->steps[13].depends_on[7] = (int)strtol(slot237, NULL, 10);
   out->steps[13].dep_count = (int)strtol(slot238, NULL, 10);
   out->steps[13].status = (int)strtol(slot239, NULL, 10);
   out->steps[13].wave = (int)strtol(slot241, NULL, 10);
   out->steps[14].id = (int)strtol(slot242, NULL, 10);
   out->steps[14].depends_on[0] = (int)strtol(slot247, NULL, 10);
   out->steps[14].depends_on[1] = (int)strtol(slot248, NULL, 10);
   out->steps[14].depends_on[2] = (int)strtol(slot249, NULL, 10);
   out->steps[14].depends_on[3] = (int)strtol(slot250, NULL, 10);
   out->steps[14].depends_on[4] = (int)strtol(slot251, NULL, 10);
   out->steps[14].depends_on[5] = (int)strtol(slot252, NULL, 10);
   out->steps[14].depends_on[6] = (int)strtol(slot253, NULL, 10);
   out->steps[14].depends_on[7] = (int)strtol(slot254, NULL, 10);
   out->steps[14].dep_count = (int)strtol(slot255, NULL, 10);
   out->steps[14].status = (int)strtol(slot256, NULL, 10);
   out->steps[14].wave = (int)strtol(slot258, NULL, 10);
   out->steps[15].id = (int)strtol(slot259, NULL, 10);
   out->steps[15].depends_on[0] = (int)strtol(slot264, NULL, 10);
   out->steps[15].depends_on[1] = (int)strtol(slot265, NULL, 10);
   out->steps[15].depends_on[2] = (int)strtol(slot266, NULL, 10);
   out->steps[15].depends_on[3] = (int)strtol(slot267, NULL, 10);
   out->steps[15].depends_on[4] = (int)strtol(slot268, NULL, 10);
   out->steps[15].depends_on[5] = (int)strtol(slot269, NULL, 10);
   out->steps[15].depends_on[6] = (int)strtol(slot270, NULL, 10);
   out->steps[15].depends_on[7] = (int)strtol(slot271, NULL, 10);
   out->steps[15].dep_count = (int)strtol(slot272, NULL, 10);
   out->steps[15].status = (int)strtol(slot273, NULL, 10);
   out->steps[15].wave = (int)strtol(slot275, NULL, 10);
   out->steps[16].id = (int)strtol(slot276, NULL, 10);
   out->steps[16].depends_on[0] = (int)strtol(slot281, NULL, 10);
   out->steps[16].depends_on[1] = (int)strtol(slot282, NULL, 10);
   out->steps[16].depends_on[2] = (int)strtol(slot283, NULL, 10);
   out->steps[16].depends_on[3] = (int)strtol(slot284, NULL, 10);
   out->steps[16].depends_on[4] = (int)strtol(slot285, NULL, 10);
   out->steps[16].depends_on[5] = (int)strtol(slot286, NULL, 10);
   out->steps[16].depends_on[6] = (int)strtol(slot287, NULL, 10);
   out->steps[16].depends_on[7] = (int)strtol(slot288, NULL, 10);
   out->steps[16].dep_count = (int)strtol(slot289, NULL, 10);
   out->steps[16].status = (int)strtol(slot290, NULL, 10);
   out->steps[16].wave = (int)strtol(slot292, NULL, 10);
   out->steps[17].id = (int)strtol(slot293, NULL, 10);
   out->steps[17].depends_on[0] = (int)strtol(slot298, NULL, 10);
   out->steps[17].depends_on[1] = (int)strtol(slot299, NULL, 10);
   out->steps[17].depends_on[2] = (int)strtol(slot300, NULL, 10);
   out->steps[17].depends_on[3] = (int)strtol(slot301, NULL, 10);
   out->steps[17].depends_on[4] = (int)strtol(slot302, NULL, 10);
   out->steps[17].depends_on[5] = (int)strtol(slot303, NULL, 10);
   out->steps[17].depends_on[6] = (int)strtol(slot304, NULL, 10);
   out->steps[17].depends_on[7] = (int)strtol(slot305, NULL, 10);
   out->steps[17].dep_count = (int)strtol(slot306, NULL, 10);
   out->steps[17].status = (int)strtol(slot307, NULL, 10);
   out->steps[17].wave = (int)strtol(slot309, NULL, 10);
   out->steps[18].id = (int)strtol(slot310, NULL, 10);
   out->steps[18].depends_on[0] = (int)strtol(slot315, NULL, 10);
   out->steps[18].depends_on[1] = (int)strtol(slot316, NULL, 10);
   out->steps[18].depends_on[2] = (int)strtol(slot317, NULL, 10);
   out->steps[18].depends_on[3] = (int)strtol(slot318, NULL, 10);
   out->steps[18].depends_on[4] = (int)strtol(slot319, NULL, 10);
   out->steps[18].depends_on[5] = (int)strtol(slot320, NULL, 10);
   out->steps[18].depends_on[6] = (int)strtol(slot321, NULL, 10);
   out->steps[18].depends_on[7] = (int)strtol(slot322, NULL, 10);
   out->steps[18].dep_count = (int)strtol(slot323, NULL, 10);
   out->steps[18].status = (int)strtol(slot324, NULL, 10);
   out->steps[18].wave = (int)strtol(slot326, NULL, 10);
   out->steps[19].id = (int)strtol(slot327, NULL, 10);
   out->steps[19].depends_on[0] = (int)strtol(slot332, NULL, 10);
   out->steps[19].depends_on[1] = (int)strtol(slot333, NULL, 10);
   out->steps[19].depends_on[2] = (int)strtol(slot334, NULL, 10);
   out->steps[19].depends_on[3] = (int)strtol(slot335, NULL, 10);
   out->steps[19].depends_on[4] = (int)strtol(slot336, NULL, 10);
   out->steps[19].depends_on[5] = (int)strtol(slot337, NULL, 10);
   out->steps[19].depends_on[6] = (int)strtol(slot338, NULL, 10);
   out->steps[19].depends_on[7] = (int)strtol(slot339, NULL, 10);
   out->steps[19].dep_count = (int)strtol(slot340, NULL, 10);
   out->steps[19].status = (int)strtol(slot341, NULL, 10);
   out->steps[19].wave = (int)strtol(slot343, NULL, 10);
   out->steps[20].id = (int)strtol(slot344, NULL, 10);
   out->steps[20].depends_on[0] = (int)strtol(slot349, NULL, 10);
   out->steps[20].depends_on[1] = (int)strtol(slot350, NULL, 10);
   out->steps[20].depends_on[2] = (int)strtol(slot351, NULL, 10);
   out->steps[20].depends_on[3] = (int)strtol(slot352, NULL, 10);
   out->steps[20].depends_on[4] = (int)strtol(slot353, NULL, 10);
   out->steps[20].depends_on[5] = (int)strtol(slot354, NULL, 10);
   out->steps[20].depends_on[6] = (int)strtol(slot355, NULL, 10);
   out->steps[20].depends_on[7] = (int)strtol(slot356, NULL, 10);
   out->steps[20].dep_count = (int)strtol(slot357, NULL, 10);
   out->steps[20].status = (int)strtol(slot358, NULL, 10);
   out->steps[20].wave = (int)strtol(slot360, NULL, 10);
   out->steps[21].id = (int)strtol(slot361, NULL, 10);
   out->steps[21].depends_on[0] = (int)strtol(slot366, NULL, 10);
   out->steps[21].depends_on[1] = (int)strtol(slot367, NULL, 10);
   out->steps[21].depends_on[2] = (int)strtol(slot368, NULL, 10);
   out->steps[21].depends_on[3] = (int)strtol(slot369, NULL, 10);
   out->steps[21].depends_on[4] = (int)strtol(slot370, NULL, 10);
   out->steps[21].depends_on[5] = (int)strtol(slot371, NULL, 10);
   out->steps[21].depends_on[6] = (int)strtol(slot372, NULL, 10);
   out->steps[21].depends_on[7] = (int)strtol(slot373, NULL, 10);
   out->steps[21].dep_count = (int)strtol(slot374, NULL, 10);
   out->steps[21].status = (int)strtol(slot375, NULL, 10);
   out->steps[21].wave = (int)strtol(slot377, NULL, 10);
   out->steps[22].id = (int)strtol(slot378, NULL, 10);
   out->steps[22].depends_on[0] = (int)strtol(slot383, NULL, 10);
   out->steps[22].depends_on[1] = (int)strtol(slot384, NULL, 10);
   out->steps[22].depends_on[2] = (int)strtol(slot385, NULL, 10);
   out->steps[22].depends_on[3] = (int)strtol(slot386, NULL, 10);
   out->steps[22].depends_on[4] = (int)strtol(slot387, NULL, 10);
   out->steps[22].depends_on[5] = (int)strtol(slot388, NULL, 10);
   out->steps[22].depends_on[6] = (int)strtol(slot389, NULL, 10);
   out->steps[22].depends_on[7] = (int)strtol(slot390, NULL, 10);
   out->steps[22].dep_count = (int)strtol(slot391, NULL, 10);
   out->steps[22].status = (int)strtol(slot392, NULL, 10);
   out->steps[22].wave = (int)strtol(slot394, NULL, 10);
   out->steps[23].id = (int)strtol(slot395, NULL, 10);
   out->steps[23].depends_on[0] = (int)strtol(slot400, NULL, 10);
   out->steps[23].depends_on[1] = (int)strtol(slot401, NULL, 10);
   out->steps[23].depends_on[2] = (int)strtol(slot402, NULL, 10);
   out->steps[23].depends_on[3] = (int)strtol(slot403, NULL, 10);
   out->steps[23].depends_on[4] = (int)strtol(slot404, NULL, 10);
   out->steps[23].depends_on[5] = (int)strtol(slot405, NULL, 10);
   out->steps[23].depends_on[6] = (int)strtol(slot406, NULL, 10);
   out->steps[23].depends_on[7] = (int)strtol(slot407, NULL, 10);
   out->steps[23].dep_count = (int)strtol(slot408, NULL, 10);
   out->steps[23].status = (int)strtol(slot409, NULL, 10);
   out->steps[23].wave = (int)strtol(slot411, NULL, 10);
   out->steps[24].id = (int)strtol(slot412, NULL, 10);
   out->steps[24].depends_on[0] = (int)strtol(slot417, NULL, 10);
   out->steps[24].depends_on[1] = (int)strtol(slot418, NULL, 10);
   out->steps[24].depends_on[2] = (int)strtol(slot419, NULL, 10);
   out->steps[24].depends_on[3] = (int)strtol(slot420, NULL, 10);
   out->steps[24].depends_on[4] = (int)strtol(slot421, NULL, 10);
   out->steps[24].depends_on[5] = (int)strtol(slot422, NULL, 10);
   out->steps[24].depends_on[6] = (int)strtol(slot423, NULL, 10);
   out->steps[24].depends_on[7] = (int)strtol(slot424, NULL, 10);
   out->steps[24].dep_count = (int)strtol(slot425, NULL, 10);
   out->steps[24].status = (int)strtol(slot426, NULL, 10);
   out->steps[24].wave = (int)strtol(slot428, NULL, 10);
   out->steps[25].id = (int)strtol(slot429, NULL, 10);
   out->steps[25].depends_on[0] = (int)strtol(slot434, NULL, 10);
   out->steps[25].depends_on[1] = (int)strtol(slot435, NULL, 10);
   out->steps[25].depends_on[2] = (int)strtol(slot436, NULL, 10);
   out->steps[25].depends_on[3] = (int)strtol(slot437, NULL, 10);
   out->steps[25].depends_on[4] = (int)strtol(slot438, NULL, 10);
   out->steps[25].depends_on[5] = (int)strtol(slot439, NULL, 10);
   out->steps[25].depends_on[6] = (int)strtol(slot440, NULL, 10);
   out->steps[25].depends_on[7] = (int)strtol(slot441, NULL, 10);
   out->steps[25].dep_count = (int)strtol(slot442, NULL, 10);
   out->steps[25].status = (int)strtol(slot443, NULL, 10);
   out->steps[25].wave = (int)strtol(slot445, NULL, 10);
   out->steps[26].id = (int)strtol(slot446, NULL, 10);
   out->steps[26].depends_on[0] = (int)strtol(slot451, NULL, 10);
   out->steps[26].depends_on[1] = (int)strtol(slot452, NULL, 10);
   out->steps[26].depends_on[2] = (int)strtol(slot453, NULL, 10);
   out->steps[26].depends_on[3] = (int)strtol(slot454, NULL, 10);
   out->steps[26].depends_on[4] = (int)strtol(slot455, NULL, 10);
   out->steps[26].depends_on[5] = (int)strtol(slot456, NULL, 10);
   out->steps[26].depends_on[6] = (int)strtol(slot457, NULL, 10);
   out->steps[26].depends_on[7] = (int)strtol(slot458, NULL, 10);
   out->steps[26].dep_count = (int)strtol(slot459, NULL, 10);
   out->steps[26].status = (int)strtol(slot460, NULL, 10);
   out->steps[26].wave = (int)strtol(slot462, NULL, 10);
   out->steps[27].id = (int)strtol(slot463, NULL, 10);
   out->steps[27].depends_on[0] = (int)strtol(slot468, NULL, 10);
   out->steps[27].depends_on[1] = (int)strtol(slot469, NULL, 10);
   out->steps[27].depends_on[2] = (int)strtol(slot470, NULL, 10);
   out->steps[27].depends_on[3] = (int)strtol(slot471, NULL, 10);
   out->steps[27].depends_on[4] = (int)strtol(slot472, NULL, 10);
   out->steps[27].depends_on[5] = (int)strtol(slot473, NULL, 10);
   out->steps[27].depends_on[6] = (int)strtol(slot474, NULL, 10);
   out->steps[27].depends_on[7] = (int)strtol(slot475, NULL, 10);
   out->steps[27].dep_count = (int)strtol(slot476, NULL, 10);
   out->steps[27].status = (int)strtol(slot477, NULL, 10);
   out->steps[27].wave = (int)strtol(slot479, NULL, 10);
   out->steps[28].id = (int)strtol(slot480, NULL, 10);
   out->steps[28].depends_on[0] = (int)strtol(slot485, NULL, 10);
   out->steps[28].depends_on[1] = (int)strtol(slot486, NULL, 10);
   out->steps[28].depends_on[2] = (int)strtol(slot487, NULL, 10);
   out->steps[28].depends_on[3] = (int)strtol(slot488, NULL, 10);
   out->steps[28].depends_on[4] = (int)strtol(slot489, NULL, 10);
   out->steps[28].depends_on[5] = (int)strtol(slot490, NULL, 10);
   out->steps[28].depends_on[6] = (int)strtol(slot491, NULL, 10);
   out->steps[28].depends_on[7] = (int)strtol(slot492, NULL, 10);
   out->steps[28].dep_count = (int)strtol(slot493, NULL, 10);
   out->steps[28].status = (int)strtol(slot494, NULL, 10);
   out->steps[28].wave = (int)strtol(slot496, NULL, 10);
   out->steps[29].id = (int)strtol(slot497, NULL, 10);
   out->steps[29].depends_on[0] = (int)strtol(slot502, NULL, 10);
   out->steps[29].depends_on[1] = (int)strtol(slot503, NULL, 10);
   out->steps[29].depends_on[2] = (int)strtol(slot504, NULL, 10);
   out->steps[29].depends_on[3] = (int)strtol(slot505, NULL, 10);
   out->steps[29].depends_on[4] = (int)strtol(slot506, NULL, 10);
   out->steps[29].depends_on[5] = (int)strtol(slot507, NULL, 10);
   out->steps[29].depends_on[6] = (int)strtol(slot508, NULL, 10);
   out->steps[29].depends_on[7] = (int)strtol(slot509, NULL, 10);
   out->steps[29].dep_count = (int)strtol(slot510, NULL, 10);
   out->steps[29].status = (int)strtol(slot511, NULL, 10);
   out->steps[29].wave = (int)strtol(slot513, NULL, 10);
   out->steps[30].id = (int)strtol(slot514, NULL, 10);
   out->steps[30].depends_on[0] = (int)strtol(slot519, NULL, 10);
   out->steps[30].depends_on[1] = (int)strtol(slot520, NULL, 10);
   out->steps[30].depends_on[2] = (int)strtol(slot521, NULL, 10);
   out->steps[30].depends_on[3] = (int)strtol(slot522, NULL, 10);
   out->steps[30].depends_on[4] = (int)strtol(slot523, NULL, 10);
   out->steps[30].depends_on[5] = (int)strtol(slot524, NULL, 10);
   out->steps[30].depends_on[6] = (int)strtol(slot525, NULL, 10);
   out->steps[30].depends_on[7] = (int)strtol(slot526, NULL, 10);
   out->steps[30].dep_count = (int)strtol(slot527, NULL, 10);
   out->steps[30].status = (int)strtol(slot528, NULL, 10);
   out->steps[30].wave = (int)strtol(slot530, NULL, 10);
   out->steps[31].id = (int)strtol(slot531, NULL, 10);
   out->steps[31].depends_on[0] = (int)strtol(slot536, NULL, 10);
   out->steps[31].depends_on[1] = (int)strtol(slot537, NULL, 10);
   out->steps[31].depends_on[2] = (int)strtol(slot538, NULL, 10);
   out->steps[31].depends_on[3] = (int)strtol(slot539, NULL, 10);
   out->steps[31].depends_on[4] = (int)strtol(slot540, NULL, 10);
   out->steps[31].depends_on[5] = (int)strtol(slot541, NULL, 10);
   out->steps[31].depends_on[6] = (int)strtol(slot542, NULL, 10);
   out->steps[31].depends_on[7] = (int)strtol(slot543, NULL, 10);
   out->steps[31].dep_count = (int)strtol(slot544, NULL, 10);
   out->steps[31].status = (int)strtol(slot545, NULL, 10);
   out->steps[31].wave = (int)strtol(slot547, NULL, 10);
   out->step_count = (int)strtol(slot548, NULL, 10);
   return 0;
}

int db1_execution_plan_list_ids(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
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
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_LIST_IDS, fields, 1, wire_values, wire_caps,
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

int db1_execution_plan_exists(int plan_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   const char *fields[] = {arg0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_EXISTS, fields, 1, NULL, NULL, 0, NULL);
   if (wire_status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return wire_status == (int)AIMEE_DB1_STATUS_OK ? 1 : -1;
}

int db1_execution_plan_count_steps(int plan_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_COUNT_STEPS, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_execution_plan_list_running_ids(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
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
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_LIST_RUNNING_IDS, fields, 1, wire_values, wire_caps,
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

int db1_execution_plan_list_recent_summaries(db1_execution_plan_summary_t *out, int max)
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
      wire_values[wire_row * 7u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 7u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 7u + 1u] = out[wire_row].agent_name;
      wire_caps[wire_row * 7u + 1u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 7u + 2u] = out[wire_row].task;
      wire_caps[wire_row * 7u + 2u] = sizeof out[wire_row].task;
      wire_values[wire_row * 7u + 3u] = out[wire_row].status;
      wire_caps[wire_row * 7u + 3u] = sizeof out[wire_row].status;
      wire_values[wire_row * 7u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 7u + 4u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 7u + 5u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 7u + 5u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 7u + 6u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 7u + 6u] = sizeof wire_scratch[wire_row * 3u + 2u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_LIST_RECENT_SUMMARIES, fields, 1, wire_values, wire_caps,
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
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].total_steps = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].done_steps = (int)strtol(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_execution_plan_set_status(int plan_id, const char *status)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   const char *fields[] = {arg0, status ? status : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_SET_STATUS, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_execution_plan_cancel_by_id(int plan_id, const char *reason)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   const char *fields[] = {arg0, reason ? reason : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_CANCEL_BY_ID, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_execution_plan_cancel_stale(int threshold_seconds, const char *reason)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", threshold_seconds);
   const char *fields[] = {arg0, reason ? reason : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_EXECUTION_PLAN_CANCEL_STALE, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_plan_step_set_status(int step_id, const char *status)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", step_id);
   const char *fields[] = {arg0, status ? status : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_PLAN_STEP_SET_STATUS, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_plan_step_set_status_output(int step_id, const char *status, const char *output)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", step_id);
   const char *fields[] = {arg0, status ? status : "", output ? output : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_PLAN_STEP_SET_STATUS_OUTPUT, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_plan_step_cancel_active_for_plan(int plan_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_PLAN_STEP_CANCEL_ACTIVE_FOR_PLAN, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_plan_step_cancel_orphans()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_PLAN_STEP_CANCEL_ORPHANS, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_step_evidence_insert(int plan_id, int step_id, const char *kind, const char *content, int passed, const char *strength)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", plan_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", step_id);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", passed);
   const char *fields[] = {arg0, arg1, kind ? kind : "", content ? content : "", arg4, strength ? strength : ""};
   return write_result(call_stage(AIMEE_DB1_OP_STEP_EVIDENCE_INSERT, fields, 6, NULL, NULL, 0, NULL));
}

int db1_step_evidence_get_latest(int step_id, db1_step_evidence_latest_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", step_id);
   const char *fields[] = {arg0};
   char slot1[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->strength, slot1, out->kind, out->created_at};
   const size_t caps[] = {sizeof out->strength, sizeof slot1, sizeof out->kind, sizeof out->created_at};
   int wire_status = call_stage(AIMEE_DB1_OP_STEP_EVIDENCE_GET_LATEST, fields, 1, values, caps, 4, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->passed = (int)strtol(slot1, NULL, 10);
   return 0;
}

/* clang-format on */
