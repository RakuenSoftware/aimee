/* db1_client/workflow.c: the workflow family, reached over the bus.
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
#include "execution_trace.h"
#include "pipelines.h"
#include "roadmap_runtime.h"
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

/* clang-format on */
