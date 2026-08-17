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
#include "cognify_jobs.h"

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

/* clang-format on */
