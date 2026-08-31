/* db1_client/lifecycle.c: the lifecycle family, reached over the bus.
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
#include "wfe_engine_store.h"
#include "db1_client/wfe_store.h"

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

#define DB1_LIFECYCLE_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.lifecycle", "DB1 %s is unreachable (module call result %d)", "lifecycle",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_LIFECYCLE))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_LIFECYCLE_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_LIFECYCLE, AIMEE_DB1_STAGE_LIFECYCLE, 0, deadline,
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

int db1_work_item_create(const char *work_item_id, const char *repo, const char *proposal_path, const char *workflow_name, const char *workflow_version, const char *start_stage, const char *mode)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, repo ? repo : "", proposal_path ? proposal_path : "", workflow_name ? workflow_name : "", workflow_version ? workflow_version : "", start_stage ? start_stage : "", mode ? mode : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_CREATE, fields, 7, NULL, NULL, 0, NULL));
}

int db1_work_item_get(const char *work_item_id, db1_work_item_t *out)
{
   if (!work_item_id || !work_item_id[0] || !out)
      return -1;
   const char *fields[] = {work_item_id};
   char slot15[32];
   char slot16[32];
   char slot17[32];
   char slot18[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->work_item_id, out->repo, out->proposal_path, out->workflow_name, out->workflow_version, out->current_stage, out->state, out->mode, out->pause_reason, out->paused_state, out->content_hash, out->pr_ref, out->worktree, out->submitter, out->parent_id, slot15, slot16, slot17, slot18, out->reservation_state, out->source_path, out->updated_at};
   const size_t caps[] = {sizeof out->work_item_id, sizeof out->repo, sizeof out->proposal_path, sizeof out->workflow_name, sizeof out->workflow_version, sizeof out->current_stage, sizeof out->state, sizeof out->mode, sizeof out->pause_reason, sizeof out->paused_state, sizeof out->content_hash, sizeof out->pr_ref, sizeof out->worktree, sizeof out->submitter, sizeof out->parent_id, sizeof slot15, sizeof slot16, sizeof slot17, sizeof slot18, sizeof out->reservation_state, sizeof out->source_path, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_GET, fields, 1, values, caps, 22, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   out->cum_cost_usd = strtod(slot15, NULL);
   out->work_item_max_cost_usd = strtod(slot16, NULL);
   out->override_count = (int)strtol(slot17, NULL, 10);
   out->reserved_cost_usd = strtod(slot18, NULL);
   return 1;
}

int db1_work_item_id_by_proposal(const char *repo, const char *proposal_path, char *out, size_t n)
{
   if (!repo || !repo[0] || !proposal_path || !proposal_path[0] || !out || n == 0)
      return -1;
   const char *fields[] = {repo, proposal_path};
   char *const values[] = {out};
   const size_t caps[] = {n};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_ID_BY_PROPOSAL, fields, 2, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

int db1_work_item_id_by_pr_ref(const char *pr_ref, char *out, size_t n)
{
   if (!pr_ref || !pr_ref[0] || !out || n == 0)
      return -1;
   const char *fields[] = {pr_ref};
   char *const values[] = {out};
   const size_t caps[] = {n};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_ID_BY_PR_REF, fields, 1, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

int db1_work_item_set_stage(const char *work_item_id, const char *stage, const char *content_hash)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : "", content_hash ? content_hash : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_STAGE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_work_item_set_pr_ref(const char *work_item_id, const char *pr_ref)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, pr_ref ? pr_ref : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_PR_REF, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_set_worktree(const char *work_item_id, const char *worktree)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, worktree ? worktree : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_WORKTREE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_set_submitter(const char *work_item_id, const char *submitter)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, submitter ? submitter : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_SUBMITTER, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_set_parent(const char *work_item_id, const char *parent_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, parent_id ? parent_id : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_PARENT, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_abandon_children(const char *parent_id)
{
   if (!parent_id || !parent_id[0])
      return -1;
   const char *fields[] = {parent_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_ABANDON_CHILDREN, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_child_counts(const char *parent_id, int *total, int *accepted, int *failed)
{
   if (!parent_id || !parent_id[0] || !total || !accepted || !failed)
      return -1;
   const char *fields[] = {parent_id};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char *const values[] = {slot0, slot1, slot2};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_CHILD_COUNTS, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *total = (int)strtol(slot0, NULL, 10);
   *accepted = (int)strtol(slot1, NULL, 10);
   *failed = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_work_item_count_active_by_submitter(const char *submitter)
{
   if (!submitter || !submitter[0])
      return -1;
   const char *fields[] = {submitter};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_COUNT_ACTIVE_BY_SUBMITTER, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_count_recent_by_submitter(const char *submitter, int secs)
{
   if (!submitter || !submitter[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", secs);
   const char *fields[] = {submitter, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_COUNT_RECENT_BY_SUBMITTER, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_submit_capped(const char *work_item_id, const char *repo, const char *proposal_path, const char *workflow_name, const char *workflow_version, const char *start_stage, const char *submitter, int max_active, int rate_max, int rate_secs)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%d", max_active);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", rate_max);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%d", rate_secs);
   const char *fields[] = {work_item_id, repo ? repo : "", proposal_path ? proposal_path : "", workflow_name ? workflow_name : "", workflow_version ? workflow_version : "", start_stage ? start_stage : "", submitter ? submitter : "", arg7, arg8, arg9};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_SUBMIT_CAPPED, fields, 10, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_set_terminal(const char *work_item_id, const char *state)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, state ? state : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_TERMINAL, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_gate_apply(const char *work_item_id, const char *expect_stage, const char *expect_hash, const char *new_stage, const char *terminal_state)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, expect_stage ? expect_stage : "", expect_hash ? expect_hash : "", new_stage ? new_stage : "", terminal_state ? terminal_state : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_GATE_APPLY, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_set_pause(const char *work_item_id, const char *reason, const char *paused_state)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, reason ? reason : "", paused_state ? paused_state : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_PAUSE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_work_item_clear_pause(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_work_item_clear_pause_if(const char *work_item_id, const char *expect_reason, const char *expect_stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, expect_reason ? expect_reason : "", expect_stage ? expect_stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE_IF, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_add_cost(const char *work_item_id, double cost)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_ADD_COST, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_set_cost_cap(const char *work_item_id, double cap)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%.17g", (double)cap);
   const char *fields[] = {work_item_id, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_SET_COST_CAP, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_inc_override(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_INC_OVERRIDE, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_delete(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_work_item_reap_stale_parks(long grace_secs)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)grace_secs);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_REAP_STALE_PARKS, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_list_bounded(db1_work_item_t **out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   db1_work_item_t *wire_held = calloc((size_t)max, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)max * 22u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 22u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 4u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)max * sizeof *wire_held);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 22u + 0u] = wire_held[wire_row].work_item_id;
      wire_caps[wire_row * 22u + 0u] = sizeof wire_held[wire_row].work_item_id;
      wire_values[wire_row * 22u + 1u] = wire_held[wire_row].repo;
      wire_caps[wire_row * 22u + 1u] = sizeof wire_held[wire_row].repo;
      wire_values[wire_row * 22u + 2u] = wire_held[wire_row].proposal_path;
      wire_caps[wire_row * 22u + 2u] = sizeof wire_held[wire_row].proposal_path;
      wire_values[wire_row * 22u + 3u] = wire_held[wire_row].workflow_name;
      wire_caps[wire_row * 22u + 3u] = sizeof wire_held[wire_row].workflow_name;
      wire_values[wire_row * 22u + 4u] = wire_held[wire_row].workflow_version;
      wire_caps[wire_row * 22u + 4u] = sizeof wire_held[wire_row].workflow_version;
      wire_values[wire_row * 22u + 5u] = wire_held[wire_row].current_stage;
      wire_caps[wire_row * 22u + 5u] = sizeof wire_held[wire_row].current_stage;
      wire_values[wire_row * 22u + 6u] = wire_held[wire_row].state;
      wire_caps[wire_row * 22u + 6u] = sizeof wire_held[wire_row].state;
      wire_values[wire_row * 22u + 7u] = wire_held[wire_row].mode;
      wire_caps[wire_row * 22u + 7u] = sizeof wire_held[wire_row].mode;
      wire_values[wire_row * 22u + 8u] = wire_held[wire_row].pause_reason;
      wire_caps[wire_row * 22u + 8u] = sizeof wire_held[wire_row].pause_reason;
      wire_values[wire_row * 22u + 9u] = wire_held[wire_row].paused_state;
      wire_caps[wire_row * 22u + 9u] = sizeof wire_held[wire_row].paused_state;
      wire_values[wire_row * 22u + 10u] = wire_held[wire_row].content_hash;
      wire_caps[wire_row * 22u + 10u] = sizeof wire_held[wire_row].content_hash;
      wire_values[wire_row * 22u + 11u] = wire_held[wire_row].pr_ref;
      wire_caps[wire_row * 22u + 11u] = sizeof wire_held[wire_row].pr_ref;
      wire_values[wire_row * 22u + 12u] = wire_held[wire_row].worktree;
      wire_caps[wire_row * 22u + 12u] = sizeof wire_held[wire_row].worktree;
      wire_values[wire_row * 22u + 13u] = wire_held[wire_row].submitter;
      wire_caps[wire_row * 22u + 13u] = sizeof wire_held[wire_row].submitter;
      wire_values[wire_row * 22u + 14u] = wire_held[wire_row].parent_id;
      wire_caps[wire_row * 22u + 14u] = sizeof wire_held[wire_row].parent_id;
      wire_values[wire_row * 22u + 15u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 22u + 15u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 22u + 16u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 22u + 16u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 22u + 17u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 22u + 17u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 22u + 18u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 22u + 18u] = sizeof wire_scratch[wire_row * 4u + 3u];
      wire_values[wire_row * 22u + 19u] = wire_held[wire_row].reservation_state;
      wire_caps[wire_row * 22u + 19u] = sizeof wire_held[wire_row].reservation_state;
      wire_values[wire_row * 22u + 20u] = wire_held[wire_row].source_path;
      wire_caps[wire_row * 22u + 20u] = sizeof wire_held[wire_row].source_path;
      wire_values[wire_row * 22u + 21u] = wire_held[wire_row].updated_at;
      wire_caps[wire_row * 22u + 21u] = sizeof wire_held[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 22), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 22u != 0u)
   {
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 22u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      wire_held[wire_row].cum_cost_usd = strtod(wire_scratch[wire_row * 4u + 0u], NULL);
      wire_held[wire_row].work_item_max_cost_usd = strtod(wire_scratch[wire_row * 4u + 1u], NULL);
      wire_held[wire_row].override_count = (int)strtol(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      wire_held[wire_row].reserved_cost_usd = strtod(wire_scratch[wire_row * 4u + 3u], NULL);
   }
   free(wire_scratch);
   *out = wire_held;
   return wire_rows;
}

int db1_work_item_list_lru_bounded(db1_work_item_t **out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   db1_work_item_t *wire_held = calloc((size_t)max, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)max * 22u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 22u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 4u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)max * sizeof *wire_held);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 22u + 0u] = wire_held[wire_row].work_item_id;
      wire_caps[wire_row * 22u + 0u] = sizeof wire_held[wire_row].work_item_id;
      wire_values[wire_row * 22u + 1u] = wire_held[wire_row].repo;
      wire_caps[wire_row * 22u + 1u] = sizeof wire_held[wire_row].repo;
      wire_values[wire_row * 22u + 2u] = wire_held[wire_row].proposal_path;
      wire_caps[wire_row * 22u + 2u] = sizeof wire_held[wire_row].proposal_path;
      wire_values[wire_row * 22u + 3u] = wire_held[wire_row].workflow_name;
      wire_caps[wire_row * 22u + 3u] = sizeof wire_held[wire_row].workflow_name;
      wire_values[wire_row * 22u + 4u] = wire_held[wire_row].workflow_version;
      wire_caps[wire_row * 22u + 4u] = sizeof wire_held[wire_row].workflow_version;
      wire_values[wire_row * 22u + 5u] = wire_held[wire_row].current_stage;
      wire_caps[wire_row * 22u + 5u] = sizeof wire_held[wire_row].current_stage;
      wire_values[wire_row * 22u + 6u] = wire_held[wire_row].state;
      wire_caps[wire_row * 22u + 6u] = sizeof wire_held[wire_row].state;
      wire_values[wire_row * 22u + 7u] = wire_held[wire_row].mode;
      wire_caps[wire_row * 22u + 7u] = sizeof wire_held[wire_row].mode;
      wire_values[wire_row * 22u + 8u] = wire_held[wire_row].pause_reason;
      wire_caps[wire_row * 22u + 8u] = sizeof wire_held[wire_row].pause_reason;
      wire_values[wire_row * 22u + 9u] = wire_held[wire_row].paused_state;
      wire_caps[wire_row * 22u + 9u] = sizeof wire_held[wire_row].paused_state;
      wire_values[wire_row * 22u + 10u] = wire_held[wire_row].content_hash;
      wire_caps[wire_row * 22u + 10u] = sizeof wire_held[wire_row].content_hash;
      wire_values[wire_row * 22u + 11u] = wire_held[wire_row].pr_ref;
      wire_caps[wire_row * 22u + 11u] = sizeof wire_held[wire_row].pr_ref;
      wire_values[wire_row * 22u + 12u] = wire_held[wire_row].worktree;
      wire_caps[wire_row * 22u + 12u] = sizeof wire_held[wire_row].worktree;
      wire_values[wire_row * 22u + 13u] = wire_held[wire_row].submitter;
      wire_caps[wire_row * 22u + 13u] = sizeof wire_held[wire_row].submitter;
      wire_values[wire_row * 22u + 14u] = wire_held[wire_row].parent_id;
      wire_caps[wire_row * 22u + 14u] = sizeof wire_held[wire_row].parent_id;
      wire_values[wire_row * 22u + 15u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 22u + 15u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 22u + 16u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 22u + 16u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 22u + 17u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 22u + 17u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 22u + 18u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 22u + 18u] = sizeof wire_scratch[wire_row * 4u + 3u];
      wire_values[wire_row * 22u + 19u] = wire_held[wire_row].reservation_state;
      wire_caps[wire_row * 22u + 19u] = sizeof wire_held[wire_row].reservation_state;
      wire_values[wire_row * 22u + 20u] = wire_held[wire_row].source_path;
      wire_caps[wire_row * 22u + 20u] = sizeof wire_held[wire_row].source_path;
      wire_values[wire_row * 22u + 21u] = wire_held[wire_row].updated_at;
      wire_caps[wire_row * 22u + 21u] = sizeof wire_held[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WORK_ITEM_LIST_LRU, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 22), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 22u != 0u)
   {
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 22u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      wire_held[wire_row].cum_cost_usd = strtod(wire_scratch[wire_row * 4u + 0u], NULL);
      wire_held[wire_row].work_item_max_cost_usd = strtod(wire_scratch[wire_row * 4u + 1u], NULL);
      wire_held[wire_row].override_count = (int)strtol(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      wire_held[wire_row].reserved_cost_usd = strtod(wire_scratch[wire_row * 4u + 3u], NULL);
   }
   free(wire_scratch);
   *out = wire_held;
   return wire_rows;
}

int db1_lifecycle_event_add(const char *work_item_id, const char *stage, const char *kind, const char *actor, const char *detail, const char *content_hash, double cost)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, stage ? stage : "", kind ? kind : "", actor ? actor : "", detail ? detail : "", content_hash ? content_hash : "", arg6};
   return write_result(call_stage(AIMEE_DB1_OP_LIFECYCLE_EVENT_ADD, fields, 7, NULL, NULL, 0, NULL));
}

int db1_lifecycle_event_list_bounded(const char *work_item_id, db1_lifecycle_event_t **out, int max)
{
   if (!work_item_id || !work_item_id[0] || !out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {work_item_id, arg1};
   db1_lifecycle_event_t *wire_held = calloc((size_t)max, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 2u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)max * sizeof *wire_held);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 8u + 1u] = wire_held[wire_row].stage;
      wire_caps[wire_row * 8u + 1u] = sizeof wire_held[wire_row].stage;
      wire_values[wire_row * 8u + 2u] = wire_held[wire_row].kind;
      wire_caps[wire_row * 8u + 2u] = sizeof wire_held[wire_row].kind;
      wire_values[wire_row * 8u + 3u] = wire_held[wire_row].actor;
      wire_caps[wire_row * 8u + 3u] = sizeof wire_held[wire_row].actor;
      wire_values[wire_row * 8u + 4u] = wire_held[wire_row].detail;
      wire_caps[wire_row * 8u + 4u] = sizeof wire_held[wire_row].detail;
      wire_values[wire_row * 8u + 5u] = wire_held[wire_row].content_hash;
      wire_caps[wire_row * 8u + 5u] = sizeof wire_held[wire_row].content_hash;
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 8u + 7u] = wire_held[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof wire_held[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_LIFECYCLE_EVENT_LIST, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      wire_held[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      wire_held[wire_row].cost_usd = strtod(wire_scratch[wire_row * 2u + 1u], NULL);
   }
   free(wire_scratch);
   *out = wire_held;
   return wire_rows;
}

int db1_stage_attempt_inc(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_STAGE_ATTEMPT_INC, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_stage_attempt_reset(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   return write_result(call_stage(AIMEE_DB1_OP_STAGE_ATTEMPT_RESET, fields, 2, NULL, NULL, 0, NULL));
}

int db1_stage_attempt_get(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_STAGE_ATTEMPT_GET, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_record_outcome(const db1_work_item_outcome_t *outcome)
{
   if (!outcome)
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", outcome->disposition);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", outcome->abandon_children);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%.17g", (double)outcome->cost_usd);
   const char *fields[] = {outcome->work_item_id, outcome->node_id, arg2, outcome->state, outcome->pause_reason, outcome->pause_stage, outcome->next_stage, outcome->pr_ref, arg8, arg9, outcome->event_kind, outcome->event_detail, outcome->event_hash, outcome->park_reason};
   return write_result(call_stage(AIMEE_DB1_OP_WORK_ITEM_RECORD_OUTCOME, fields, 14, NULL, NULL, 0, NULL));
}

int db1_wfe_children_list(const char *parent_id, char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!parent_id || !parent_id[0] || !out_ids || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {parent_id, arg1};
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
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_CHILDREN_LIST, fields, 2, wire_values, wire_caps,
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

int db1_wfe_active_root_count()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_ACTIVE_ROOT_COUNT, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_work_item_id_by_git_proposal(const char *repo, const char *proposal_path, const char *suffix, char *out, size_t n)
{
   if (!repo || !repo[0] || !proposal_path || !proposal_path[0] || !suffix || !suffix[0] || !out || n == 0)
      return -1;
   const char *fields[] = {repo, proposal_path, suffix};
   char *const values[] = {out};
   const size_t caps[] = {n};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_WORK_ITEM_ID_BY_GIT_PROPOSAL, fields, 3, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

int db1_wfe_executed_turn_count(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_EXECUTED_TURN_COUNT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_stage_loop_count(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_STAGE_LOOP_COUNT, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_runner_failures_since_progress(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RUNNER_FAILURES_SINCE_PROGRESS, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_capacity_waits_since_progress(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_CAPACITY_WAITS_SINCE_PROGRESS, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_descendant_ids(const char *work_item_id, char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!work_item_id || !work_item_id[0] || !out_ids || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {work_item_id, arg1};
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
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_DESCENDANT_IDS, fields, 2, wire_values, wire_caps,
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

long long db1_wfe_resume_transient(const char *pause_reason, int older_than_secs)
{
   if (!pause_reason || !pause_reason[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", older_than_secs);
   const char *fields[] = {pause_reason, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RESUME_TRANSIENT, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (long long)strtoll(slot0, NULL, 10);
}

long long db1_wfe_resume_wall_caps(int max_resumes)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max_resumes);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RESUME_WALL_CAPS, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (long long)strtoll(slot0, NULL, 10);
}

long long db1_wfe_abandon_exhausted_wall_caps(int max_resumes, int grace_secs)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max_resumes);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", grace_secs);
   const char *fields[] = {arg0, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_ABANDON_EXHAUSTED_WALL_CAPS, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (long long)strtoll(slot0, NULL, 10);
}

long long db1_wfe_resume_ready_parents()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RESUME_READY_PARENTS, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (long long)strtoll(slot0, NULL, 10);
}

int db1_wfe_delegate_job_save(const char *execution_key, const char *work_item_id, long long job_id, const char *participant_token)
{
   if (!execution_key || !execution_key[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)job_id);
   const char *fields[] = {execution_key, work_item_id ? work_item_id : "", arg2, participant_token ? participant_token : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_DELEGATE_JOB_SAVE, fields, 4, NULL, NULL, 0, NULL));
}

int db1_wfe_delegate_jobs_terminal_claim(db1_wfe_delegate_job_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
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
      wire_values[wire_row * 2u + 0u] = out[wire_row].execution_key;
      wire_caps[wire_row * 2u + 0u] = sizeof out[wire_row].execution_key;
      wire_values[wire_row * 2u + 1u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 2u + 1u] = sizeof wire_scratch[wire_row * 1u + 0u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_DELEGATE_JOBS_TERMINAL_CLAIM, fields, 1, wire_values, wire_caps,
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
      out[wire_row].job_id = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_wfe_budget_reserve(const char *work_item_id, const char *owner, db1_wfe_budget_reservation_t *out)
{
   if (!work_item_id || !work_item_id[0] || !owner || !owner[0] || !out)
      return -1;
   const char *fields[] = {work_item_id, owner};
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   char slot5[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->root_id, slot1, slot2, slot3, slot4, slot5};
   const size_t caps[] = {sizeof out->root_id, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4, sizeof slot5};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_BUDGET_RESERVE, fields, 2, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->max_usd = strtod(slot1, NULL);
   out->amount = strtod(slot2, NULL);
   out->allowed = (int)strtol(slot3, NULL, 10);
   out->busy = (int)strtol(slot4, NULL, 10);
   out->replay_only = (int)strtol(slot5, NULL, 10);
   return 0;
}

int db1_wfe_budget_totals(const char *work_item_id, db1_wfe_budget_totals_t *out)
{
   if (!work_item_id || !work_item_id[0] || !out)
      return -1;
   const char *fields[] = {work_item_id};
   char slot1[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->root_id, slot1, slot2};
   const size_t caps[] = {sizeof out->root_id, sizeof slot1, sizeof slot2};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_BUDGET_TOTALS, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->spent = strtod(slot1, NULL);
   out->max_usd = strtod(slot2, NULL);
   return 0;
}

int db1_wfe_budget_release(const char *work_item_id, const char *owner)
{
   if (!work_item_id || !work_item_id[0] || !owner || !owner[0])
      return -1;
   const char *fields[] = {work_item_id, owner};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_BUDGET_RELEASE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_wfe_budget_heartbeat(const char *work_item_id, const char *owner)
{
   if (!work_item_id || !work_item_id[0] || !owner || !owner[0])
      return -1;
   const char *fields[] = {work_item_id, owner};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_BUDGET_HEARTBEAT, fields, 2, NULL, NULL, 0, NULL));
}

int db1_wfe_budget_reconcile(const char *work_item_id, const char *owner, double actual)
{
   if (!work_item_id || !work_item_id[0] || !owner || !owner[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)actual);
   const char *fields[] = {work_item_id, owner, arg2};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_BUDGET_RECONCILE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_move(const char *work_item_id, const char *from_stage, const char *to_stage, const char *kind, const char *detail, const char *content_hash, double cost)
{
   if (!work_item_id || !work_item_id[0] || !from_stage || !from_stage[0] || !to_stage || !to_stage[0] || !kind || !kind[0])
      return -1;
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, from_stage, to_stage, kind, detail ? detail : "", content_hash ? content_hash : "", arg6};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_MOVE, fields, 7, NULL, NULL, 0, NULL));
}

int db1_wfe_record_retry(const char *work_item_id, const char *stage, const char *to_stage, const char *detail, int max_attempts, double cost)
{
   if (!work_item_id || !work_item_id[0] || !stage || !stage[0])
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", max_attempts);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, stage, to_stage ? to_stage : "", detail ? detail : "", arg4, arg5};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RECORD_RETRY, fields, 6, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_park_with_detail(const char *work_item_id, const char *stage, const char *reason, const char *detail, double cost)
{
   if (!work_item_id || !work_item_id[0] || !stage || !stage[0])
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, stage, reason ? reason : "", detail ? detail : "", arg4};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_PARK_WITH_DETAIL, fields, 5, NULL, NULL, 0, NULL));
}

int db1_wfe_resume(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_RESUME, fields, 1, NULL, NULL, 0, NULL));
}

int db1_wfe_finish(const char *work_item_id, const char *stage, const char *state, const char *detail, const char *content_hash, double cost)
{
   if (!work_item_id || !work_item_id[0] || !state || !state[0])
      return -1;
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, stage ? stage : "", state, detail ? detail : "", content_hash ? content_hash : "", arg5};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_FINISH, fields, 6, NULL, NULL, 0, NULL));
}

int db1_wfe_stop_tree(const char *work_item_id, char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!work_item_id || !work_item_id[0] || !out_ids || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {work_item_id, arg1};
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
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_STOP_TREE, fields, 2, wire_values, wire_caps,
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

int db1_wfe_reconcile_orphans(char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
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
   memset(out_ids, 0, (size_t)max * sizeof *out_ids);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out_ids[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out_ids[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RECONCILE_ORPHANS, fields, 1, wire_values, wire_caps,
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

int db1_wfe_park_budget_tree(const char *root_id, const char *completed_item_id, double added_cost)
{
   if (!root_id || !root_id[0] || !completed_item_id || !completed_item_id[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)added_cost);
   const char *fields[] = {root_id, completed_item_id, arg2};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_PARK_BUDGET_TREE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_wfe_delete_tree(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_DELETE_TREE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_wfe_resolve_gate(const char *work_item_id, const char *from_stage, const char *to_stage, const char *decision, const char *content_hash)
{
   if (!work_item_id || !work_item_id[0] || !to_stage || !to_stage[0])
      return -1;
   const char *fields[] = {work_item_id, from_stage ? from_stage : "", to_stage, decision ? decision : "", content_hash ? content_hash : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_RESOLVE_GATE, fields, 5, NULL, NULL, 0, NULL));
}

int db1_wfe_reject_gate(const char *work_item_id, const char *stage, const char *content_hash)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : "", content_hash ? content_hash : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_REJECT_GATE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_wfe_park_runner_failure(const char *work_item_id, const char *stage, const char *owner, const char *reason, const char *detail, int dispatched, int cost_known, double actual)
{
   if (!work_item_id || !work_item_id[0] || !stage || !stage[0] || !owner || !owner[0] || !reason || !reason[0])
      return -1;
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", dispatched);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", cost_known);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%.17g", (double)actual);
   const char *fields[] = {work_item_id, stage, owner, reason, detail ? detail : "", arg5, arg6, arg7};
   return write_result(call_stage(AIMEE_DB1_OP_WFE_PARK_RUNNER_FAILURE, fields, 8, NULL, NULL, 0, NULL));
}

int db1_wfe_recover_lost_replay(const char *work_item_id, const char *stage, const char *owner)
{
   if (!work_item_id || !work_item_id[0] || !owner || !owner[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : "", owner};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RECOVER_LOST_REPLAY, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_record_requested_changes(const char *work_item_id, const char *gate, const char *plan_stage, const char *plan_hash, const char *feedback_hash, const char *unresolved, int max_iterations, int max_identical, double cost, db1_wfe_review_outcome_t *out)
{
   if (!work_item_id || !work_item_id[0] || !gate || !gate[0] || !plan_stage || !plan_stage[0] || !plan_hash || !plan_hash[0] || !feedback_hash || !feedback_hash[0] || !out)
      return -1;
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", max_iterations);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%d", max_identical);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%.17g", (double)cost);
   const char *fields[] = {work_item_id, gate, plan_stage, plan_hash, feedback_hash, unresolved ? unresolved : "", arg6, arg7, arg8};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->pause_reason};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->pause_reason};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_RECORD_REQUESTED_CHANGES, fields, 9, values, caps, 4, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->attempts = (int)strtol(slot0, NULL, 10);
   out->identical_repeats = (int)strtol(slot1, NULL, 10);
   out->parked = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_wfe_claim_frozen_creates(const db1_wfe_frozen_claim_t *claim, db1_wfe_frozen_conflict_t *out)
{
   if (!claim || !out)
      return -1;
   char arg130[32];
   snprintf(arg130, sizeof arg130, "%d", claim->create_count);
   const char *fields[] = {claim->parent_id, claim->work_item_id, claim->creates[0].path, claim->creates[0].content_hash, claim->creates[1].path, claim->creates[1].content_hash, claim->creates[2].path, claim->creates[2].content_hash, claim->creates[3].path, claim->creates[3].content_hash, claim->creates[4].path, claim->creates[4].content_hash, claim->creates[5].path, claim->creates[5].content_hash, claim->creates[6].path, claim->creates[6].content_hash, claim->creates[7].path, claim->creates[7].content_hash, claim->creates[8].path, claim->creates[8].content_hash, claim->creates[9].path, claim->creates[9].content_hash, claim->creates[10].path, claim->creates[10].content_hash, claim->creates[11].path, claim->creates[11].content_hash, claim->creates[12].path, claim->creates[12].content_hash, claim->creates[13].path, claim->creates[13].content_hash, claim->creates[14].path, claim->creates[14].content_hash, claim->creates[15].path, claim->creates[15].content_hash, claim->creates[16].path, claim->creates[16].content_hash, claim->creates[17].path, claim->creates[17].content_hash, claim->creates[18].path, claim->creates[18].content_hash, claim->creates[19].path, claim->creates[19].content_hash, claim->creates[20].path, claim->creates[20].content_hash, claim->creates[21].path, claim->creates[21].content_hash, claim->creates[22].path, claim->creates[22].content_hash, claim->creates[23].path, claim->creates[23].content_hash, claim->creates[24].path, claim->creates[24].content_hash, claim->creates[25].path, claim->creates[25].content_hash, claim->creates[26].path, claim->creates[26].content_hash, claim->creates[27].path, claim->creates[27].content_hash, claim->creates[28].path, claim->creates[28].content_hash, claim->creates[29].path, claim->creates[29].content_hash, claim->creates[30].path, claim->creates[30].content_hash, claim->creates[31].path, claim->creates[31].content_hash, claim->creates[32].path, claim->creates[32].content_hash, claim->creates[33].path, claim->creates[33].content_hash, claim->creates[34].path, claim->creates[34].content_hash, claim->creates[35].path, claim->creates[35].content_hash, claim->creates[36].path, claim->creates[36].content_hash, claim->creates[37].path, claim->creates[37].content_hash, claim->creates[38].path, claim->creates[38].content_hash, claim->creates[39].path, claim->creates[39].content_hash, claim->creates[40].path, claim->creates[40].content_hash, claim->creates[41].path, claim->creates[41].content_hash, claim->creates[42].path, claim->creates[42].content_hash, claim->creates[43].path, claim->creates[43].content_hash, claim->creates[44].path, claim->creates[44].content_hash, claim->creates[45].path, claim->creates[45].content_hash, claim->creates[46].path, claim->creates[46].content_hash, claim->creates[47].path, claim->creates[47].content_hash, claim->creates[48].path, claim->creates[48].content_hash, claim->creates[49].path, claim->creates[49].content_hash, claim->creates[50].path, claim->creates[50].content_hash, claim->creates[51].path, claim->creates[51].content_hash, claim->creates[52].path, claim->creates[52].content_hash, claim->creates[53].path, claim->creates[53].content_hash, claim->creates[54].path, claim->creates[54].content_hash, claim->creates[55].path, claim->creates[55].content_hash, claim->creates[56].path, claim->creates[56].content_hash, claim->creates[57].path, claim->creates[57].content_hash, claim->creates[58].path, claim->creates[58].content_hash, claim->creates[59].path, claim->creates[59].content_hash, claim->creates[60].path, claim->creates[60].content_hash, claim->creates[61].path, claim->creates[61].content_hash, claim->creates[62].path, claim->creates[62].content_hash, claim->creates[63].path, claim->creates[63].content_hash, arg130};
   memset(out, 0, sizeof *out);
   char *const values[] = {out->path, out->existing_work_item, out->conflicting_work_item};
   const size_t caps[] = {sizeof out->path, sizeof out->existing_work_item, sizeof out->conflicting_work_item};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_CLAIM_FROZEN_CREATES, fields, 131, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   return 0;
}

int db1_wfe_create_work_item(const char *work_item_id, const char *repo, const char *proposal_path, const char *workflow_name, const char *workflow_version, const char *start_stage, const char *mode, const char *submitter, const char *parent_id, const char *source_path, double max_cost_usd, int root_cap)
{
   if (!work_item_id || !work_item_id[0] || !proposal_path || !proposal_path[0] || !workflow_name || !workflow_name[0] || !start_stage || !start_stage[0])
      return -1;
   char arg10[32];
   snprintf(arg10, sizeof arg10, "%.17g", (double)max_cost_usd);
   char arg11[32];
   snprintf(arg11, sizeof arg11, "%d", root_cap);
   const char *fields[] = {work_item_id, repo ? repo : "", proposal_path, workflow_name, workflow_version ? workflow_version : "", start_stage, mode ? mode : "", submitter ? submitter : "", parent_id ? parent_id : "", source_path ? source_path : "", arg10, arg11};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_CREATE_WORK_ITEM, fields, 12, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_wfe_latest_stage_retry_detail(const char *work_item_id, const char *stage, char *out, size_t n)
{
   if (!work_item_id || !work_item_id[0] || !out || n == 0)
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   char *const values[] = {out};
   const size_t caps[] = {n};
   int wire_status = call_stage(AIMEE_DB1_OP_WFE_LATEST_STAGE_RETRY_DETAIL, fields, 2, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

/* clang-format on */
