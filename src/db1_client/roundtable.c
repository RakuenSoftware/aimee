/* db1_client/roundtable.c: the roundtable family, reached over the bus.
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
#include "roundtable_pipeline.h"

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

#define DB1_ROUNDTABLE_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.roundtable", "DB1 %s is unreachable (module call result %d)", "roundtable",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_ROUNDTABLE))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_ROUNDTABLE_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_ROUNDTABLE, AIMEE_DB1_STAGE_ROUNDTABLE, 0, deadline,
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


int db1_roundtable_run_create(const char *idea, const char *done_bar, const char *repo_root, const char *base_branch, int *out_id)
{
   if (!idea || !idea[0] || !out_id)
      return -1;
   const char *fields[] = {idea, done_bar ? done_bar : "", repo_root ? repo_root : "", base_branch ? base_branch : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_CREATE, fields, 4, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_roundtable_run_get(int id, rtp_run_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot5[32];
   char slot23[32];
   char slot25[32];
   char slot29[32];
   char slot30[32];
   char slot31[32];
   char slot32[32];
   char slot33[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->idea, out->state, out->phase, out->admission_class, slot5, out->done_bar, out->brief, out->gate_digest, out->proposal_ref, out->proposal_origin_hash, out->diff_ref, out->diff_origin_hash, out->chunk_index_ref, out->repo_root, out->remote, out->base_branch, out->head_branch, out->workspace_id, out->workspace_provider, out->worktree_path, out->head_sha, out->base_sha, slot23, out->proposal_pr_url, slot25, out->impl_pr_url, out->cost_scope, out->cost_source, slot29, slot30, slot31, slot32, slot33, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->idea, sizeof out->state, sizeof out->phase, sizeof out->admission_class, sizeof slot5, sizeof out->done_bar, sizeof out->brief, sizeof out->gate_digest, sizeof out->proposal_ref, sizeof out->proposal_origin_hash, sizeof out->diff_ref, sizeof out->diff_origin_hash, sizeof out->chunk_index_ref, sizeof out->repo_root, sizeof out->remote, sizeof out->base_branch, sizeof out->head_branch, sizeof out->workspace_id, sizeof out->workspace_provider, sizeof out->worktree_path, sizeof out->head_sha, sizeof out->base_sha, sizeof slot23, sizeof out->proposal_pr_url, sizeof slot25, sizeof out->impl_pr_url, sizeof out->cost_scope, sizeof out->cost_source, sizeof slot29, sizeof slot30, sizeof slot31, sizeof slot32, sizeof slot33, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_GET, fields, 1, values, caps, 36, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->schema_version = (int)strtol(slot5, NULL, 10);
   out->proposal_pr_number = (int)strtol(slot23, NULL, 10);
   out->impl_pr_number = (int)strtol(slot25, NULL, 10);
   out->cost_version = (int)strtol(slot29, NULL, 10);
   out->proposal_phase_cost_usd = strtod(slot30, NULL);
   out->impl_phase_cost_usd = strtod(slot31, NULL);
   out->total_cost_usd = strtod(slot32, NULL);
   out->accepted_question_count = (int)strtol(slot33, NULL, 10);
   return 0;
}

int db1_roundtable_run_update(const rtp_run_t *run)
{
   if (!run)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", run->id);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", run->schema_version);
   char arg23[32];
   snprintf(arg23, sizeof arg23, "%d", run->proposal_pr_number);
   char arg25[32];
   snprintf(arg25, sizeof arg25, "%d", run->impl_pr_number);
   char arg29[32];
   snprintf(arg29, sizeof arg29, "%d", run->cost_version);
   char arg30[32];
   snprintf(arg30, sizeof arg30, "%.17g", (double)run->proposal_phase_cost_usd);
   char arg31[32];
   snprintf(arg31, sizeof arg31, "%.17g", (double)run->impl_phase_cost_usd);
   char arg32[32];
   snprintf(arg32, sizeof arg32, "%.17g", (double)run->total_cost_usd);
   char arg33[32];
   snprintf(arg33, sizeof arg33, "%d", run->accepted_question_count);
   const char *fields[] = {arg0, run->idea, run->state, run->phase, run->admission_class, arg5, run->done_bar, run->brief, run->gate_digest, run->proposal_ref, run->proposal_origin_hash, run->diff_ref, run->diff_origin_hash, run->chunk_index_ref, run->repo_root, run->remote, run->base_branch, run->head_branch, run->workspace_id, run->workspace_provider, run->worktree_path, run->head_sha, run->base_sha, arg23, run->proposal_pr_url, arg25, run->impl_pr_url, run->cost_scope, run->cost_source, arg29, arg30, arg31, arg32, arg33, run->created_at, run->updated_at};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_UPDATE, fields, 36, NULL, NULL, 0, NULL));
}

int db1_roundtable_run_set_state(int id, const char *state, const char *phase)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, state ? state : "", phase ? phase : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_SET_STATE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_roundtable_run_cas_state(int id, const char *expected, const char *next)
{
   if (!expected || !expected[0] || !next || !next[0])
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, expected, next};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_CAS_STATE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_roundtable_run_list(const char *state_filter, rtp_run_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {state_filter ? state_filter : "", arg1};
   char **wire_values = malloc((size_t)max * 36u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 36u * sizeof *wire_caps);
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
      wire_values[wire_row * 36u + 0u] = wire_scratch[wire_row * 9u + 0u];
      wire_caps[wire_row * 36u + 0u] = sizeof wire_scratch[wire_row * 9u + 0u];
      wire_values[wire_row * 36u + 1u] = out[wire_row].idea;
      wire_caps[wire_row * 36u + 1u] = sizeof out[wire_row].idea;
      wire_values[wire_row * 36u + 2u] = out[wire_row].state;
      wire_caps[wire_row * 36u + 2u] = sizeof out[wire_row].state;
      wire_values[wire_row * 36u + 3u] = out[wire_row].phase;
      wire_caps[wire_row * 36u + 3u] = sizeof out[wire_row].phase;
      wire_values[wire_row * 36u + 4u] = out[wire_row].admission_class;
      wire_caps[wire_row * 36u + 4u] = sizeof out[wire_row].admission_class;
      wire_values[wire_row * 36u + 5u] = wire_scratch[wire_row * 9u + 1u];
      wire_caps[wire_row * 36u + 5u] = sizeof wire_scratch[wire_row * 9u + 1u];
      wire_values[wire_row * 36u + 6u] = out[wire_row].done_bar;
      wire_caps[wire_row * 36u + 6u] = sizeof out[wire_row].done_bar;
      wire_values[wire_row * 36u + 7u] = out[wire_row].brief;
      wire_caps[wire_row * 36u + 7u] = sizeof out[wire_row].brief;
      wire_values[wire_row * 36u + 8u] = out[wire_row].gate_digest;
      wire_caps[wire_row * 36u + 8u] = sizeof out[wire_row].gate_digest;
      wire_values[wire_row * 36u + 9u] = out[wire_row].proposal_ref;
      wire_caps[wire_row * 36u + 9u] = sizeof out[wire_row].proposal_ref;
      wire_values[wire_row * 36u + 10u] = out[wire_row].proposal_origin_hash;
      wire_caps[wire_row * 36u + 10u] = sizeof out[wire_row].proposal_origin_hash;
      wire_values[wire_row * 36u + 11u] = out[wire_row].diff_ref;
      wire_caps[wire_row * 36u + 11u] = sizeof out[wire_row].diff_ref;
      wire_values[wire_row * 36u + 12u] = out[wire_row].diff_origin_hash;
      wire_caps[wire_row * 36u + 12u] = sizeof out[wire_row].diff_origin_hash;
      wire_values[wire_row * 36u + 13u] = out[wire_row].chunk_index_ref;
      wire_caps[wire_row * 36u + 13u] = sizeof out[wire_row].chunk_index_ref;
      wire_values[wire_row * 36u + 14u] = out[wire_row].repo_root;
      wire_caps[wire_row * 36u + 14u] = sizeof out[wire_row].repo_root;
      wire_values[wire_row * 36u + 15u] = out[wire_row].remote;
      wire_caps[wire_row * 36u + 15u] = sizeof out[wire_row].remote;
      wire_values[wire_row * 36u + 16u] = out[wire_row].base_branch;
      wire_caps[wire_row * 36u + 16u] = sizeof out[wire_row].base_branch;
      wire_values[wire_row * 36u + 17u] = out[wire_row].head_branch;
      wire_caps[wire_row * 36u + 17u] = sizeof out[wire_row].head_branch;
      wire_values[wire_row * 36u + 18u] = out[wire_row].workspace_id;
      wire_caps[wire_row * 36u + 18u] = sizeof out[wire_row].workspace_id;
      wire_values[wire_row * 36u + 19u] = out[wire_row].workspace_provider;
      wire_caps[wire_row * 36u + 19u] = sizeof out[wire_row].workspace_provider;
      wire_values[wire_row * 36u + 20u] = out[wire_row].worktree_path;
      wire_caps[wire_row * 36u + 20u] = sizeof out[wire_row].worktree_path;
      wire_values[wire_row * 36u + 21u] = out[wire_row].head_sha;
      wire_caps[wire_row * 36u + 21u] = sizeof out[wire_row].head_sha;
      wire_values[wire_row * 36u + 22u] = out[wire_row].base_sha;
      wire_caps[wire_row * 36u + 22u] = sizeof out[wire_row].base_sha;
      wire_values[wire_row * 36u + 23u] = wire_scratch[wire_row * 9u + 2u];
      wire_caps[wire_row * 36u + 23u] = sizeof wire_scratch[wire_row * 9u + 2u];
      wire_values[wire_row * 36u + 24u] = out[wire_row].proposal_pr_url;
      wire_caps[wire_row * 36u + 24u] = sizeof out[wire_row].proposal_pr_url;
      wire_values[wire_row * 36u + 25u] = wire_scratch[wire_row * 9u + 3u];
      wire_caps[wire_row * 36u + 25u] = sizeof wire_scratch[wire_row * 9u + 3u];
      wire_values[wire_row * 36u + 26u] = out[wire_row].impl_pr_url;
      wire_caps[wire_row * 36u + 26u] = sizeof out[wire_row].impl_pr_url;
      wire_values[wire_row * 36u + 27u] = out[wire_row].cost_scope;
      wire_caps[wire_row * 36u + 27u] = sizeof out[wire_row].cost_scope;
      wire_values[wire_row * 36u + 28u] = out[wire_row].cost_source;
      wire_caps[wire_row * 36u + 28u] = sizeof out[wire_row].cost_source;
      wire_values[wire_row * 36u + 29u] = wire_scratch[wire_row * 9u + 4u];
      wire_caps[wire_row * 36u + 29u] = sizeof wire_scratch[wire_row * 9u + 4u];
      wire_values[wire_row * 36u + 30u] = wire_scratch[wire_row * 9u + 5u];
      wire_caps[wire_row * 36u + 30u] = sizeof wire_scratch[wire_row * 9u + 5u];
      wire_values[wire_row * 36u + 31u] = wire_scratch[wire_row * 9u + 6u];
      wire_caps[wire_row * 36u + 31u] = sizeof wire_scratch[wire_row * 9u + 6u];
      wire_values[wire_row * 36u + 32u] = wire_scratch[wire_row * 9u + 7u];
      wire_caps[wire_row * 36u + 32u] = sizeof wire_scratch[wire_row * 9u + 7u];
      wire_values[wire_row * 36u + 33u] = wire_scratch[wire_row * 9u + 8u];
      wire_caps[wire_row * 36u + 33u] = sizeof wire_scratch[wire_row * 9u + 8u];
      wire_values[wire_row * 36u + 34u] = out[wire_row].created_at;
      wire_caps[wire_row * 36u + 34u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 36u + 35u] = out[wire_row].updated_at;
      wire_caps[wire_row * 36u + 35u] = sizeof out[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_LIST, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 36), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 36u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 36u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int)strtol(wire_scratch[wire_row * 9u + 0u], NULL, 10);
      out[wire_row].schema_version = (int)strtol(wire_scratch[wire_row * 9u + 1u], NULL, 10);
      out[wire_row].proposal_pr_number = (int)strtol(wire_scratch[wire_row * 9u + 2u], NULL, 10);
      out[wire_row].impl_pr_number = (int)strtol(wire_scratch[wire_row * 9u + 3u], NULL, 10);
      out[wire_row].cost_version = (int)strtol(wire_scratch[wire_row * 9u + 4u], NULL, 10);
      out[wire_row].proposal_phase_cost_usd = strtod(wire_scratch[wire_row * 9u + 5u], NULL);
      out[wire_row].impl_phase_cost_usd = strtod(wire_scratch[wire_row * 9u + 6u], NULL);
      out[wire_row].total_cost_usd = strtod(wire_scratch[wire_row * 9u + 7u], NULL);
      out[wire_row].accepted_question_count = (int)strtol(wire_scratch[wire_row * 9u + 8u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_roundtable_run_count_active()
{
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_COUNT_ACTIVE, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_roundtable_run_branch_owner(const char *repo_root, const char *head_branch, int exclude_id)
{
   if (!head_branch || !head_branch[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", exclude_id);
   const char *fields[] = {repo_root ? repo_root : "", head_branch, arg2};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_RUN_BRANCH_OWNER, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_roundtable_pass_create(int pipeline_id, const char *phase, const char *mode, int pass_no, const char *artifact_hash, int *out_id)
{
   if (!out_id)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", pass_no);
   const char *fields[] = {arg0, phase ? phase : "", mode ? mode : "", arg3, artifact_hash ? artifact_hash : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_CREATE, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_roundtable_pass_get(int id, rtp_pass_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot4[32];
   char slot7[32];
   char slot8[32];
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
   char slot22[32];
   char slot23[32];
   char slot24[32];
   char slot25[32];
   char slot26[32];
   char slot27[32];
   char slot28[32];
   char slot29[32];
   char slot30[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, out->phase, out->mode, slot4, out->status, out->artifact_hash, slot7, slot8, slot9, slot10, slot11, slot12, slot13, slot14, slot15, slot16, slot17, slot18, out->result_hash, slot20, slot21, slot22, slot23, slot24, slot25, slot26, slot27, slot28, slot29, slot30, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof out->phase, sizeof out->mode, sizeof slot4, sizeof out->status, sizeof out->artifact_hash, sizeof slot7, sizeof slot8, sizeof slot9, sizeof slot10, sizeof slot11, sizeof slot12, sizeof slot13, sizeof slot14, sizeof slot15, sizeof slot16, sizeof slot17, sizeof slot18, sizeof out->result_hash, sizeof slot20, sizeof slot21, sizeof slot22, sizeof slot23, sizeof slot24, sizeof slot25, sizeof slot26, sizeof slot27, sizeof slot28, sizeof slot29, sizeof slot30, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_GET, fields, 1, values, caps, 33, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->pipeline_id = (int)strtol(slot1, NULL, 10);
   out->pass_no = (int)strtol(slot4, NULL, 10);
   out->converged = (int)strtol(slot7, NULL, 10);
   out->envelope_valid = (int)strtol(slot8, NULL, 10);
   out->blocking_count = (int)strtol(slot9, NULL, 10);
   out->suggestion_count = (int)strtol(slot10, NULL, 10);
   out->nit_count = (int)strtol(slot11, NULL, 10);
   out->open_questions = (int)strtol(slot12, NULL, 10);
   out->coverage_gaps = (int)strtol(slot13, NULL, 10);
   out->items_round = (int)strtol(slot14, NULL, 10);
   out->artifact_round = (int)strtol(slot15, NULL, 10);
   out->best_round = (int)strtol(slot16, NULL, 10);
   out->rounds_run = (int)strtol(slot17, NULL, 10);
   out->cost_usd = strtod(slot18, NULL);
   out->is_chunked = (int)strtol(slot20, NULL, 10);
   out->chunk_total = (int)strtol(slot21, NULL, 10);
   out->chunk_done = (int)strtol(slot22, NULL, 10);
   out->synthesis_done = (int)strtol(slot23, NULL, 10);
   out->chunk_group = (int)strtol(slot24, NULL, 10);
   out->chunk_index = (int)strtol(slot25, NULL, 10);
   out->answered_count = (int)strtol(slot26, NULL, 10);
   out->chunk_offset = (int)strtol(slot27, NULL, 10);
   out->chunk_len = (int)strtol(slot28, NULL, 10);
   out->chunk_omitted = (int)strtol(slot29, NULL, 10);
   out->chunk_over_budget = (int)strtol(slot30, NULL, 10);
   return 0;
}

int db1_roundtable_pass_update(const rtp_pass_t *pass)
{
   if (!pass)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pass->id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", pass->pipeline_id);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", pass->pass_no);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%d", pass->converged);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", pass->envelope_valid);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%d", pass->blocking_count);
   char arg10[32];
   snprintf(arg10, sizeof arg10, "%d", pass->suggestion_count);
   char arg11[32];
   snprintf(arg11, sizeof arg11, "%d", pass->nit_count);
   char arg12[32];
   snprintf(arg12, sizeof arg12, "%d", pass->open_questions);
   char arg13[32];
   snprintf(arg13, sizeof arg13, "%d", pass->coverage_gaps);
   char arg14[32];
   snprintf(arg14, sizeof arg14, "%d", pass->items_round);
   char arg15[32];
   snprintf(arg15, sizeof arg15, "%d", pass->artifact_round);
   char arg16[32];
   snprintf(arg16, sizeof arg16, "%d", pass->best_round);
   char arg17[32];
   snprintf(arg17, sizeof arg17, "%d", pass->rounds_run);
   char arg18[32];
   snprintf(arg18, sizeof arg18, "%.17g", (double)pass->cost_usd);
   char arg20[32];
   snprintf(arg20, sizeof arg20, "%d", pass->is_chunked);
   char arg21[32];
   snprintf(arg21, sizeof arg21, "%d", pass->chunk_total);
   char arg22[32];
   snprintf(arg22, sizeof arg22, "%d", pass->chunk_done);
   char arg23[32];
   snprintf(arg23, sizeof arg23, "%d", pass->synthesis_done);
   char arg24[32];
   snprintf(arg24, sizeof arg24, "%d", pass->chunk_group);
   char arg25[32];
   snprintf(arg25, sizeof arg25, "%d", pass->chunk_index);
   char arg26[32];
   snprintf(arg26, sizeof arg26, "%d", pass->answered_count);
   char arg27[32];
   snprintf(arg27, sizeof arg27, "%d", pass->chunk_offset);
   char arg28[32];
   snprintf(arg28, sizeof arg28, "%d", pass->chunk_len);
   char arg29[32];
   snprintf(arg29, sizeof arg29, "%d", pass->chunk_omitted);
   char arg30[32];
   snprintf(arg30, sizeof arg30, "%d", pass->chunk_over_budget);
   const char *fields[] = {arg0, arg1, pass->phase, pass->mode, arg4, pass->status, pass->artifact_hash, arg7, arg8, arg9, arg10, arg11, arg12, arg13, arg14, arg15, arg16, arg17, arg18, pass->result_hash, arg20, arg21, arg22, arg23, arg24, arg25, arg26, arg27, arg28, arg29, arg30, pass->created_at, pass->updated_at};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_UPDATE, fields, 33, NULL, NULL, 0, NULL));
}

int db1_roundtable_pass_latest(int pipeline_id, const char *phase, rtp_pass_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   const char *fields[] = {arg0, phase ? phase : ""};
   char slot0[32];
   char slot1[32];
   char slot4[32];
   char slot7[32];
   char slot8[32];
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
   char slot22[32];
   char slot23[32];
   char slot24[32];
   char slot25[32];
   char slot26[32];
   char slot27[32];
   char slot28[32];
   char slot29[32];
   char slot30[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, out->phase, out->mode, slot4, out->status, out->artifact_hash, slot7, slot8, slot9, slot10, slot11, slot12, slot13, slot14, slot15, slot16, slot17, slot18, out->result_hash, slot20, slot21, slot22, slot23, slot24, slot25, slot26, slot27, slot28, slot29, slot30, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof out->phase, sizeof out->mode, sizeof slot4, sizeof out->status, sizeof out->artifact_hash, sizeof slot7, sizeof slot8, sizeof slot9, sizeof slot10, sizeof slot11, sizeof slot12, sizeof slot13, sizeof slot14, sizeof slot15, sizeof slot16, sizeof slot17, sizeof slot18, sizeof out->result_hash, sizeof slot20, sizeof slot21, sizeof slot22, sizeof slot23, sizeof slot24, sizeof slot25, sizeof slot26, sizeof slot27, sizeof slot28, sizeof slot29, sizeof slot30, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_LATEST, fields, 2, values, caps, 33, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->pipeline_id = (int)strtol(slot1, NULL, 10);
   out->pass_no = (int)strtol(slot4, NULL, 10);
   out->converged = (int)strtol(slot7, NULL, 10);
   out->envelope_valid = (int)strtol(slot8, NULL, 10);
   out->blocking_count = (int)strtol(slot9, NULL, 10);
   out->suggestion_count = (int)strtol(slot10, NULL, 10);
   out->nit_count = (int)strtol(slot11, NULL, 10);
   out->open_questions = (int)strtol(slot12, NULL, 10);
   out->coverage_gaps = (int)strtol(slot13, NULL, 10);
   out->items_round = (int)strtol(slot14, NULL, 10);
   out->artifact_round = (int)strtol(slot15, NULL, 10);
   out->best_round = (int)strtol(slot16, NULL, 10);
   out->rounds_run = (int)strtol(slot17, NULL, 10);
   out->cost_usd = strtod(slot18, NULL);
   out->is_chunked = (int)strtol(slot20, NULL, 10);
   out->chunk_total = (int)strtol(slot21, NULL, 10);
   out->chunk_done = (int)strtol(slot22, NULL, 10);
   out->synthesis_done = (int)strtol(slot23, NULL, 10);
   out->chunk_group = (int)strtol(slot24, NULL, 10);
   out->chunk_index = (int)strtol(slot25, NULL, 10);
   out->answered_count = (int)strtol(slot26, NULL, 10);
   out->chunk_offset = (int)strtol(slot27, NULL, 10);
   out->chunk_len = (int)strtol(slot28, NULL, 10);
   out->chunk_omitted = (int)strtol(slot29, NULL, 10);
   out->chunk_over_budget = (int)strtol(slot30, NULL, 10);
   return 0;
}

int db1_roundtable_pass_max_no(int pipeline_id, const char *phase)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   const char *fields[] = {arg0, phase ? phase : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_MAX_NO, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_roundtable_pass_max_group(int pipeline_id, const char *phase)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   const char *fields[] = {arg0, phase ? phase : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_MAX_GROUP, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_roundtable_pass_group_agg(int pipeline_id, const char *phase, int chunk_group, rtp_group_agg_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", chunk_group);
   const char *fields[] = {arg0, phase ? phase : "", arg2};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   char slot5[32];
   char slot6[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, slot3, slot4, slot5, slot6};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof slot3, sizeof slot4, sizeof slot5, sizeof slot6};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_PASS_GROUP_AGG, fields, 3, values, caps, 7, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->total = (int)strtol(slot0, NULL, 10);
   out->done = (int)strtol(slot1, NULL, 10);
   out->invalid = (int)strtol(slot2, NULL, 10);
   out->synthesis_present = (int)strtol(slot3, NULL, 10);
   out->synthesis_done = (int)strtol(slot4, NULL, 10);
   out->blocking_count = (int)strtol(slot5, NULL, 10);
   out->suggestion_count = (int)strtol(slot6, NULL, 10);
   return 0;
}

int db1_roundtable_attempt_create(int pass_id, int attempt_no, const char *run_id, int *out_id)
{
   if (!out_id)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pass_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", attempt_no);
   const char *fields[] = {arg0, arg1, run_id ? run_id : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_CREATE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_roundtable_attempt_get_by_run(const char *run_id, rtp_attempt_t *out)
{
   if (!run_id || !run_id[0] || !out)
      return -1;
   const char *fields[] = {run_id};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot4[32];
   char slot8[32];
   char slot9[32];
   char slot10[32];
   char slot11[32];
   char slot12[32];
   char slot13[32];
   char slot14[32];
   char slot15[32];
   char slot18[32];
   char slot19[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->run_id, slot4, out->capture_status, out->terminal_status, out->parse_status, slot8, slot9, slot10, slot11, slot12, slot13, slot14, slot15, out->result_hash, out->result_snapshot, slot18, slot19, out->submitted_at, out->terminal_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->run_id, sizeof slot4, sizeof out->capture_status, sizeof out->terminal_status, sizeof out->parse_status, sizeof slot8, sizeof slot9, sizeof slot10, sizeof slot11, sizeof slot12, sizeof slot13, sizeof slot14, sizeof slot15, sizeof out->result_hash, sizeof out->result_snapshot, sizeof slot18, sizeof slot19, sizeof out->submitted_at, sizeof out->terminal_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_GET_BY_RUN, fields, 1, values, caps, 22, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->pass_id = (int)strtol(slot1, NULL, 10);
   out->attempt_no = (int)strtol(slot2, NULL, 10);
   out->is_current = (int)strtol(slot4, NULL, 10);
   out->envelope_valid = (int)strtol(slot8, NULL, 10);
   out->items_truncated = (int)strtol(slot9, NULL, 10);
   out->truncated = (int)strtol(slot10, NULL, 10);
   out->degraded = (int)strtol(slot11, NULL, 10);
   out->cost_capped = (int)strtol(slot12, NULL, 10);
   out->deadline_hit = (int)strtol(slot13, NULL, 10);
   out->cancelled = (int)strtol(slot14, NULL, 10);
   out->lost_result = (int)strtol(slot15, NULL, 10);
   out->cost_usd = strtod(slot18, NULL);
   out->cost_known = (int)strtol(slot19, NULL, 10);
   return 0;
}

int db1_roundtable_attempt_current(int pass_id, rtp_attempt_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pass_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot4[32];
   char slot8[32];
   char slot9[32];
   char slot10[32];
   char slot11[32];
   char slot12[32];
   char slot13[32];
   char slot14[32];
   char slot15[32];
   char slot18[32];
   char slot19[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->run_id, slot4, out->capture_status, out->terminal_status, out->parse_status, slot8, slot9, slot10, slot11, slot12, slot13, slot14, slot15, out->result_hash, out->result_snapshot, slot18, slot19, out->submitted_at, out->terminal_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->run_id, sizeof slot4, sizeof out->capture_status, sizeof out->terminal_status, sizeof out->parse_status, sizeof slot8, sizeof slot9, sizeof slot10, sizeof slot11, sizeof slot12, sizeof slot13, sizeof slot14, sizeof slot15, sizeof out->result_hash, sizeof out->result_snapshot, sizeof slot18, sizeof slot19, sizeof out->submitted_at, sizeof out->terminal_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_CURRENT, fields, 1, values, caps, 22, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->pass_id = (int)strtol(slot1, NULL, 10);
   out->attempt_no = (int)strtol(slot2, NULL, 10);
   out->is_current = (int)strtol(slot4, NULL, 10);
   out->envelope_valid = (int)strtol(slot8, NULL, 10);
   out->items_truncated = (int)strtol(slot9, NULL, 10);
   out->truncated = (int)strtol(slot10, NULL, 10);
   out->degraded = (int)strtol(slot11, NULL, 10);
   out->cost_capped = (int)strtol(slot12, NULL, 10);
   out->deadline_hit = (int)strtol(slot13, NULL, 10);
   out->cancelled = (int)strtol(slot14, NULL, 10);
   out->lost_result = (int)strtol(slot15, NULL, 10);
   out->cost_usd = strtod(slot18, NULL);
   out->cost_known = (int)strtol(slot19, NULL, 10);
   return 0;
}

int db1_roundtable_attempt_update(const rtp_attempt_t *attempt)
{
   if (!attempt)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", attempt->id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", attempt->pass_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", attempt->attempt_no);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", attempt->is_current);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%d", attempt->envelope_valid);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%d", attempt->items_truncated);
   char arg10[32];
   snprintf(arg10, sizeof arg10, "%d", attempt->truncated);
   char arg11[32];
   snprintf(arg11, sizeof arg11, "%d", attempt->degraded);
   char arg12[32];
   snprintf(arg12, sizeof arg12, "%d", attempt->cost_capped);
   char arg13[32];
   snprintf(arg13, sizeof arg13, "%d", attempt->deadline_hit);
   char arg14[32];
   snprintf(arg14, sizeof arg14, "%d", attempt->cancelled);
   char arg15[32];
   snprintf(arg15, sizeof arg15, "%d", attempt->lost_result);
   char arg18[32];
   snprintf(arg18, sizeof arg18, "%.17g", (double)attempt->cost_usd);
   char arg19[32];
   snprintf(arg19, sizeof arg19, "%d", attempt->cost_known);
   const char *fields[] = {arg0, arg1, arg2, attempt->run_id, arg4, attempt->capture_status, attempt->terminal_status, attempt->parse_status, arg8, arg9, arg10, arg11, arg12, arg13, arg14, arg15, attempt->result_hash, attempt->result_snapshot, arg18, arg19, attempt->submitted_at, attempt->terminal_at};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_UPDATE, fields, 22, NULL, NULL, 0, NULL));
}

int db1_roundtable_attempt_max_no(int pass_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pass_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_MAX_NO, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_roundtable_attempt_supersede_others(int pass_id, int keep_attempt_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pass_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", keep_attempt_id);
   const char *fields[] = {arg0, arg1};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_SUPERSEDE_OTHERS, fields, 2, NULL, NULL, 0, NULL));
}

int db1_roundtable_gate_create(int pipeline_id, int gate_no, int pr_number, const char *expected_head_sha, int *out_id)
{
   if (!out_id)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", gate_no);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", pr_number);
   const char *fields[] = {arg0, arg1, arg2, expected_head_sha ? expected_head_sha : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_GATE_CREATE, fields, 4, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *out_id = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_roundtable_gate_get(int pipeline_id, int gate_no, rtp_gate_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", gate_no);
   const char *fields[] = {arg0, arg1};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char slot6[32];
   char slot12[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->verdict, out->reason, out->actor, slot6, out->expected_head_sha, out->merge_sha, out->merge_executor, out->merge_command, out->merge_output, slot12, out->resolved_at, out->created_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->verdict, sizeof out->reason, sizeof out->actor, sizeof slot6, sizeof out->expected_head_sha, sizeof out->merge_sha, sizeof out->merge_executor, sizeof out->merge_command, sizeof out->merge_output, sizeof slot12, sizeof out->resolved_at, sizeof out->created_at};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_GATE_GET, fields, 2, values, caps, 15, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->pipeline_id = (int)strtol(slot1, NULL, 10);
   out->gate_no = (int)strtol(slot2, NULL, 10);
   out->pr_number = (int)strtol(slot6, NULL, 10);
   out->merge_exit_code = (int)strtol(slot12, NULL, 10);
   return 0;
}

int db1_roundtable_gate_update(const rtp_gate_t *gate)
{
   if (!gate)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", gate->id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", gate->pipeline_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", gate->gate_no);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", gate->pr_number);
   char arg12[32];
   snprintf(arg12, sizeof arg12, "%d", gate->merge_exit_code);
   const char *fields[] = {arg0, arg1, arg2, gate->verdict, gate->reason, gate->actor, arg6, gate->expected_head_sha, gate->merge_sha, gate->merge_executor, gate->merge_command, gate->merge_output, arg12, gate->resolved_at, gate->created_at};
   return write_result(call_stage(AIMEE_DB1_OP_ROUNDTABLE_GATE_UPDATE, fields, 15, NULL, NULL, 0, NULL));
}

int db1_roundtable_gate_age_exceeds_hours(int pipeline_id, int gate_no, int hours)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", pipeline_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", gate_no);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", hours);
   const char *fields[] = {arg0, arg1, arg2};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_ROUNDTABLE_GATE_AGE_EXCEEDS_HOURS, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

/* clang-format on */
