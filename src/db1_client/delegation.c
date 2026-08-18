/* db1_client/delegation.c: the delegation family, reached over the bus.
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
#include "agent_jobs.h"
#include "delegate_learning.h"
#include "delegate_reservation.h"
#include "delegation_checkpoint.h"
#include "delegations.h"

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

#define DB1_DELEGATION_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.delegation", "DB1 %s is unreachable (module call result %d)", "delegation",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_DELEGATION))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_DELEGATION_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_DELEGATION, AIMEE_DB1_STAGE_DELEGATION, 0, deadline,
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


int db1_delegation_message_record(const char *delegation_id, const char *direction, const char *content)
{
   if (!delegation_id || !delegation_id[0] || !direction || !direction[0])
      return -1;
   const char *fields[] = {delegation_id, direction, content ? content : ""};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATION_MESSAGE_RECORD, fields, 3, NULL, NULL, 0, NULL));
}

int db1_delegation_spawn_record(const char *delegation_id, const char *parent_delegation_id, const char *session_id, int depth, const char *role)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", depth);
   const char *fields[] = {delegation_id, parent_delegation_id ? parent_delegation_id : "", session_id ? session_id : "", arg3, role ? role : ""};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_RECORD, fields, 5, NULL, NULL, 0, NULL));
}

int db1_delegation_spawn_complete(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   const char *fields[] = {delegation_id};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_COMPLETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_delegation_spawn_preempt(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   const char *fields[] = {delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_PREEMPT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_status(const char *delegation_id, char *out, size_t out_sz)
{
   if (!delegation_id || !delegation_id[0] || !out || out_sz == 0)
      return -1;
   const char *fields[] = {delegation_id};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_sz, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_STATUS, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz)
{
   if (!delegation_id || !delegation_id[0] || !out || out_sz == 0)
      return -1;
   const char *fields[] = {delegation_id};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_sz, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_STOP_REASON, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_delegation_spawn_is_stopped(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   const char *fields[] = {delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_IS_STOPPED, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_is_cancelled(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   const char *fields[] = {delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_IS_CANCELLED, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_is_active(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   const char *fields[] = {delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_IS_ACTIVE, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_count_total(const char *session_id)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_COUNT_TOTAL, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_find_root(const char *delegation_id, char *out, size_t out_sz)
{
   if (!delegation_id || !delegation_id[0] || !out || out_sz == 0)
      return -1;
   const char *fields[] = {delegation_id};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_sz, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_FIND_ROOT, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_delegation_spawn_count_descendants(const char *root_delegation_id)
{
   if (!root_delegation_id || !root_delegation_id[0])
      return -1;
   const char *fields[] = {root_delegation_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_COUNT_DESCENDANTS, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_list_active(int *out_ids, int max)
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
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_LIST_ACTIVE, fields, 1, wire_values, wire_caps,
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

int db1_delegation_spawn_cancel_by_id(int spawn_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", spawn_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_BY_ID, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_cancel_recursive(int spawn_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", spawn_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_RECURSIVE, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_spawn_cancel_stale()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_STALE, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegate_reservation_get(const char *execution_key, int *out_job_id, char *participant, size_t participant_cap)
{
   if (!execution_key || !execution_key[0] || !out_job_id || !participant || participant_cap == 0)
      return -1;
   const char *fields[] = {execution_key};
   char slot0[32];
   char *const values[] = {slot0, participant};
   const size_t caps[] = {sizeof slot0, participant_cap};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATE_RESERVATION_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_job_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_delegate_reservation_adopt_sole_legacy(const char *execution_key, const char *work_item_id, int *out_job_id, char *participant, size_t participant_cap)
{
   if (!execution_key || !execution_key[0] || !work_item_id || !work_item_id[0] || !out_job_id || !participant || participant_cap == 0)
      return -1;
   const char *fields[] = {execution_key, work_item_id};
   char slot0[32];
   char *const values[] = {slot0, participant};
   const size_t caps[] = {sizeof slot0, participant_cap};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATE_RESERVATION_ADOPT, fields, 2, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_job_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_delegate_reservation_save(const char *execution_key, const char *work_item_id, int job_id, const char *participant)
{
   if (!execution_key || !execution_key[0] || !work_item_id || !work_item_id[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", job_id);
   const char *fields[] = {execution_key, work_item_id, arg2, participant ? participant : ""};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATE_RESERVATION_SAVE, fields, 4, NULL, NULL, 0, NULL));
}

int db1_delegate_reservation_forget(const char *execution_key)
{
   if (!execution_key || !execution_key[0])
      return -1;
   const char *fields[] = {execution_key};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATE_RESERVATION_FORGET, fields, 1, NULL, NULL, 0, NULL));
}

int db1_delegate_reservation_forget_if_matches(const char *execution_key, int job_id)
{
   if (!execution_key || !execution_key[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", job_id);
   const char *fields[] = {execution_key, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATE_RESERVATION_FORGET_IF_MATCHES, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_delegation_checkpoint_save(const char *delegation_id, const char *job_id, int attempt, const char *steps_json, const char *last_output, const char *error)
{
   if (!delegation_id || !delegation_id[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", attempt);
   const char *fields[] = {delegation_id, job_id ? job_id : "", arg2, steps_json ? steps_json : "", last_output ? last_output : "", error ? error : ""};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATION_CHECKPOINT_SAVE, fields, 6, NULL, NULL, 0, NULL));
}

int db1_delegation_checkpoint_load(const char *delegation_id, char *steps_out, size_t steps_cap, char *error_out, size_t error_cap, char *output_out, size_t output_cap)
{
   if (!delegation_id || !delegation_id[0] || !steps_out || steps_cap == 0 || !error_out || error_cap == 0 || !output_out || output_cap == 0)
      return -1;
   const char *fields[] = {delegation_id};
   char *const values[] = {steps_out, error_out, output_out};
   const size_t caps[] = {steps_cap, error_cap, output_cap};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATION_CHECKPOINT_LOAD, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   return 0;
}

int db1_agent_job_create(const char *role, const char *prompt, const char *agent_name, const char *lease_owner)
{
   if (!role || !role[0])
      return -1;
   const char *fields[] = {role, prompt ? prompt : "", agent_name ? agent_name : "", lease_owner ? lease_owner : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_CREATE, fields, 4, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

void db1_agent_job_update(int job_id, const char *status, int cursor_turn, const char *result)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", cursor_turn);
   const char *fields[] = {arg0, status ? status : "", arg2, result ? result : ""};
   (void)call_stage(AIMEE_DB1_OP_AGENT_JOB_UPDATE, fields, 4, NULL, NULL, 0, NULL);
}

int db1_agent_job_complete(int job_id, const char *status, int cursor_turn, const char *result, int has_cost, double cost_usd)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", cursor_turn);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", has_cost);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%.17g", (double)cost_usd);
   const char *fields[] = {arg0, status ? status : "", arg2, result ? result : "", arg4, arg5};
   return write_result(call_stage(AIMEE_DB1_OP_AGENT_JOB_COMPLETE, fields, 6, NULL, NULL, 0, NULL));
}

void db1_agent_job_set_agent(int job_id, const char *agent_name)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0, agent_name ? agent_name : ""};
   (void)call_stage(AIMEE_DB1_OP_AGENT_JOB_SET_AGENT, fields, 2, NULL, NULL, 0, NULL);
}

void db1_agent_job_heartbeat(int job_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0};
   (void)call_stage(AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT, fields, 1, NULL, NULL, 0, NULL);
}

void db1_agent_job_heartbeat_ext(int job_id, const char *current_tool, int api_call_count)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", api_call_count);
   const char *fields[] = {arg0, current_tool ? current_tool : "", arg2};
   (void)call_stage(AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT_EXT, fields, 3, NULL, NULL, 0, NULL);
}

int db1_agent_job_is_cancelled(int job_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_IS_CANCELLED, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_job_classify_stale(int job_id, int idle_threshold_secs, int in_tool_threshold_secs, char *out_state, size_t out_state_cap)
{
   if (!out_state || out_state_cap == 0)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", idle_threshold_secs);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", in_tool_threshold_secs);
   const char *fields[] = {arg0, arg1, arg2};
   char slot_rc[32];
   char *const values[] = {out_state, slot_rc};
   const size_t caps[] = {out_state_cap, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_CLASSIFY_STALE, fields, 3, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_agent_job_get(int job_id, db1_agent_job_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot7[32];
   char slot11[32];
   char slot12[32];
   char slot13[32];
   memset(out, 0, sizeof *out);
   out->prompt = malloc(1048576u);
   if (!out->prompt)
   {
      free(out->prompt);
      free(out->result);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->prompt[0] = '\0';
   out->result = malloc(1048576u);
   if (!out->result)
   {
      free(out->prompt);
      free(out->result);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->result[0] = '\0';
   char *const values[] = {slot0, out->role, out->prompt, out->agent_name, out->participant_token, out->status, out->result, slot7, out->lease_owner, out->heartbeat_at, out->current_tool, slot11, slot12, slot13, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->role, 1048576u, sizeof out->agent_name, sizeof out->participant_token, sizeof out->status, 1048576u, sizeof slot7, sizeof out->lease_owner, sizeof out->heartbeat_at, sizeof out->current_tool, sizeof slot11, sizeof slot12, sizeof slot13, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_GET, fields, 1, values, caps, 16, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->prompt);
      free(out->result);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->cursor_turn = (int)strtol(slot7, NULL, 10);
   out->api_call_count = (int)strtol(slot11, NULL, 10);
   out->cost_usd = strtod(slot12, NULL);
   out->cost_known = (int)strtol(slot13, NULL, 10);
   char *shrunk_prompt = realloc(out->prompt, strlen(out->prompt) + 1u);
   if (shrunk_prompt)
      out->prompt = shrunk_prompt;
   char *shrunk_result = realloc(out->result, strlen(out->result) + 1u);
   if (shrunk_result)
      out->result = shrunk_result;
   return 0;
}

int db1_agent_job_get_by_participant(const char *participant_token, db1_agent_job_t *out)
{
   if (!participant_token || !participant_token[0] || !out)
      return -1;
   const char *fields[] = {participant_token};
   char slot0[32];
   char slot7[32];
   char slot11[32];
   char slot12[32];
   char slot13[32];
   memset(out, 0, sizeof *out);
   out->prompt = malloc(1048576u);
   if (!out->prompt)
   {
      free(out->prompt);
      free(out->result);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->prompt[0] = '\0';
   out->result = malloc(1048576u);
   if (!out->result)
   {
      free(out->prompt);
      free(out->result);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->result[0] = '\0';
   char *const values[] = {slot0, out->role, out->prompt, out->agent_name, out->participant_token, out->status, out->result, slot7, out->lease_owner, out->heartbeat_at, out->current_tool, slot11, slot12, slot13, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->role, 1048576u, sizeof out->agent_name, sizeof out->participant_token, sizeof out->status, 1048576u, sizeof slot7, sizeof out->lease_owner, sizeof out->heartbeat_at, sizeof out->current_tool, sizeof slot11, sizeof slot12, sizeof slot13, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_GET_BY_PARTICIPANT, fields, 1, values, caps, 16, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->prompt);
      free(out->result);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->cursor_turn = (int)strtol(slot7, NULL, 10);
   out->api_call_count = (int)strtol(slot11, NULL, 10);
   out->cost_usd = strtod(slot12, NULL);
   out->cost_known = (int)strtol(slot13, NULL, 10);
   char *shrunk_prompt = realloc(out->prompt, strlen(out->prompt) + 1u);
   if (shrunk_prompt)
      out->prompt = shrunk_prompt;
   char *shrunk_result = realloc(out->result, strlen(out->result) + 1u);
   if (shrunk_result)
      out->result = shrunk_result;
   return 0;
}

int db1_agent_job_heartbeat_is_stale(const char *heartbeat_at, int stale_minutes)
{
   if (!heartbeat_at || !heartbeat_at[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", stale_minutes);
   const char *fields[] = {heartbeat_at, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT_IS_STALE, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_job_take_lease(int job_id, const char *owner)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0, owner ? owner : ""};
   return write_result(call_stage(AIMEE_DB1_OP_AGENT_JOB_TAKE_LEASE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_agent_job_list_recent(db1_agent_job_t *out, int max, int include_heavy)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", include_heavy);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 16u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 16u * sizeof *wire_caps);
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
      wire_values[wire_row * 16u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 16u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 16u + 1u] = out[wire_row].role;
      wire_caps[wire_row * 16u + 1u] = sizeof out[wire_row].role;
      out[wire_row].prompt = malloc(1048576u);
      if (!out[wire_row].prompt)
      {
         for (int wire_done = 0; wire_done < wire_row; ++wire_done)
         {
            free(out[wire_done].prompt);
            out[wire_done].prompt = NULL;
            free(out[wire_done].result);
            out[wire_done].result = NULL;
         }
         free(wire_values);
         free(wire_caps);
         free(wire_scratch);
         return -1;
      }
      out[wire_row].prompt[0] = '\0';
      wire_values[wire_row * 16u + 2u] = out[wire_row].prompt;
      wire_caps[wire_row * 16u + 2u] = 1048576u;
      wire_values[wire_row * 16u + 3u] = out[wire_row].agent_name;
      wire_caps[wire_row * 16u + 3u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 16u + 4u] = out[wire_row].participant_token;
      wire_caps[wire_row * 16u + 4u] = sizeof out[wire_row].participant_token;
      wire_values[wire_row * 16u + 5u] = out[wire_row].status;
      wire_caps[wire_row * 16u + 5u] = sizeof out[wire_row].status;
      out[wire_row].result = malloc(1048576u);
      if (!out[wire_row].result)
      {
         for (int wire_done = 0; wire_done < wire_row; ++wire_done)
         {
            free(out[wire_done].prompt);
            out[wire_done].prompt = NULL;
            free(out[wire_done].result);
            out[wire_done].result = NULL;
         }
         free(wire_values);
         free(wire_caps);
         free(wire_scratch);
         return -1;
      }
      out[wire_row].result[0] = '\0';
      wire_values[wire_row * 16u + 6u] = out[wire_row].result;
      wire_caps[wire_row * 16u + 6u] = 1048576u;
      wire_values[wire_row * 16u + 7u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 16u + 7u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 16u + 8u] = out[wire_row].lease_owner;
      wire_caps[wire_row * 16u + 8u] = sizeof out[wire_row].lease_owner;
      wire_values[wire_row * 16u + 9u] = out[wire_row].heartbeat_at;
      wire_caps[wire_row * 16u + 9u] = sizeof out[wire_row].heartbeat_at;
      wire_values[wire_row * 16u + 10u] = out[wire_row].current_tool;
      wire_caps[wire_row * 16u + 10u] = sizeof out[wire_row].current_tool;
      wire_values[wire_row * 16u + 11u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 16u + 11u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 16u + 12u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 16u + 12u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 16u + 13u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 16u + 13u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 16u + 14u] = out[wire_row].created_at;
      wire_caps[wire_row * 16u + 14u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 16u + 15u] = out[wire_row].updated_at;
      wire_caps[wire_row * 16u + 15u] = sizeof out[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_LIST_RECENT, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 16), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 16u != 0u)
   {
      free(wire_scratch);
      for (int wire_done = 0; wire_done < max; ++wire_done)
      {
         free(out[wire_done].prompt);
         out[wire_done].prompt = NULL;
         free(out[wire_done].result);
         out[wire_done].result = NULL;
      }
      return -1;
   }
   int wire_rows = (int)(wire_filled / 16u);
   for (int wire_row = wire_rows; wire_row < max; ++wire_row)
   {
      free(out[wire_row].prompt);
      out[wire_row].prompt = NULL;
      free(out[wire_row].result);
      out[wire_row].result = NULL;
   }
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      char *wire_shrunk = realloc(out[wire_row].prompt, strlen(out[wire_row].prompt) + 1u);
      if (wire_shrunk)
         out[wire_row].prompt = wire_shrunk;
   }
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      char *wire_shrunk = realloc(out[wire_row].result, strlen(out[wire_row].result) + 1u);
      if (wire_shrunk)
         out[wire_row].result = wire_shrunk;
   }
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].cursor_turn = (int)strtol(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].api_call_count = (int)strtol(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].cost_usd = strtod(wire_scratch[wire_row * 5u + 3u], NULL);
      out[wire_row].cost_known = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_agent_job_list_running_ids(int *out_ids, int max)
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
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_LIST_RUNNING_IDS, fields, 1, wire_values, wire_caps,
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

int db1_agent_job_cancel_by_id(int job_id, const char *reason)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   const char *fields[] = {arg0, reason ? reason : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_CANCEL_BY_ID, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_job_cancel_unassigned(int job_id, const char *reason, int min_age_secs)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", job_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", min_age_secs);
   const char *fields[] = {arg0, reason ? reason : "", arg2};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_CANCEL_UNASSIGNED, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_job_cancel_nonterminal_on_restart(const char *reason)
{
   const char *fields[] = {reason ? reason : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_CANCEL_NONTERMINAL_ON_RESTART, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_job_cancel_stale(int threshold_seconds, const char *reason)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", threshold_seconds);
   const char *fields[] = {arg0, reason ? reason : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_JOB_CANCEL_STALE, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_agent_log_list(const char *agent_filter, db1_agent_log_entry_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {agent_filter ? agent_filter : "", arg1};
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
      wire_values[wire_row * 10u + 0u] = out[wire_row].agent_name;
      wire_caps[wire_row * 10u + 0u] = sizeof out[wire_row].agent_name;
      wire_values[wire_row * 10u + 1u] = out[wire_row].role;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].role;
      wire_values[wire_row * 10u + 2u] = wire_scratch[wire_row * 7u + 0u];
      wire_caps[wire_row * 10u + 2u] = sizeof wire_scratch[wire_row * 7u + 0u];
      wire_values[wire_row * 10u + 3u] = wire_scratch[wire_row * 7u + 1u];
      wire_caps[wire_row * 10u + 3u] = sizeof wire_scratch[wire_row * 7u + 1u];
      wire_values[wire_row * 10u + 4u] = wire_scratch[wire_row * 7u + 2u];
      wire_caps[wire_row * 10u + 4u] = sizeof wire_scratch[wire_row * 7u + 2u];
      wire_values[wire_row * 10u + 5u] = wire_scratch[wire_row * 7u + 3u];
      wire_caps[wire_row * 10u + 5u] = sizeof wire_scratch[wire_row * 7u + 3u];
      wire_values[wire_row * 10u + 6u] = wire_scratch[wire_row * 7u + 4u];
      wire_caps[wire_row * 10u + 6u] = sizeof wire_scratch[wire_row * 7u + 4u];
      wire_values[wire_row * 10u + 7u] = wire_scratch[wire_row * 7u + 5u];
      wire_caps[wire_row * 10u + 7u] = sizeof wire_scratch[wire_row * 7u + 5u];
      wire_values[wire_row * 10u + 8u] = wire_scratch[wire_row * 7u + 6u];
      wire_caps[wire_row * 10u + 8u] = sizeof wire_scratch[wire_row * 7u + 6u];
      wire_values[wire_row * 10u + 9u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_LOG_ENTRY_LIST, fields, 2, wire_values, wire_caps,
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
      out[wire_row].turns = (int)strtol(wire_scratch[wire_row * 7u + 0u], NULL, 10);
      out[wire_row].tool_calls = (int)strtol(wire_scratch[wire_row * 7u + 1u], NULL, 10);
      out[wire_row].success = (int)strtol(wire_scratch[wire_row * 7u + 2u], NULL, 10);
      out[wire_row].confidence = (int)strtol(wire_scratch[wire_row * 7u + 3u], NULL, 10);
      out[wire_row].prompt_tokens = (int)strtol(wire_scratch[wire_row * 7u + 4u], NULL, 10);
      out[wire_row].completion_tokens = (int)strtol(wire_scratch[wire_row * 7u + 5u], NULL, 10);
      out[wire_row].latency_ms = (int)strtol(wire_scratch[wire_row * 7u + 6u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_delegate_learning_record(const char *session_id, const char *role, const char *failure_mode, const char *lesson, const char *evidence_json, double confidence)
{
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%.17g", (double)confidence);
   const char *fields[] = {session_id ? session_id : "", role ? role : "", failure_mode ? failure_mode : "", lesson ? lesson : "", evidence_json ? evidence_json : "", arg5};
   return write_result(call_stage(AIMEE_DB1_OP_DELEGATE_LEARNING_RECORD, fields, 6, NULL, NULL, 0, NULL));
}

char *db1_delegate_learning_inject_prompt(const char *role, const char *system_prompt, int top_n)
{
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", top_n);
   const char *fields[] = {role ? role : "", system_prompt ? system_prompt : "", arg2};
   char *value = malloc(1048576u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {1048576u};
   int wire_status = call_stage(AIMEE_DB1_OP_DELEGATE_LEARNING_INJECT_PROMPT, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

/* clang-format on */
