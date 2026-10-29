/* lifecycle.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "wfe_engine_store.h"
#include "db1_client/wfe_store.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static db1_family_t family = {"db1.lifecycle", "lifecycle", AIMEE_DB1_EVENT_LIFECYCLE, AIMEE_DB1_STAGE_LIFECYCLE, 2000, 0};

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
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_CREATE, fields, 7, NULL, NULL, 0, NULL));
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
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_GET, fields, 1, values, caps, 22, NULL);
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
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_ID_BY_PROPOSAL, fields, 2, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

int db1_work_item_id_by_pr_ref(const char *pr_ref, char *out, size_t n)
{
   if (!pr_ref || !pr_ref[0] || !out || n == 0)
      return -1;
   const char *fields[] = {pr_ref};
   char *const values[] = {out};
   const size_t caps[] = {n};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_ID_BY_PR_REF, fields, 1, values, caps, 1, NULL);
   return read_result(wire_status, out);
}

int db1_work_item_abandon_children(const char *parent_id)
{
   if (!parent_id || !parent_id[0])
      return -1;
   const char *fields[] = {parent_id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_ABANDON_CHILDREN, fields, 1, values, caps, 1, NULL);
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
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_SUBMIT_CAPPED, fields, 10, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_set_terminal(const char *work_item_id, const char *state)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, state ? state : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_SET_TERMINAL, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_gate_apply(const char *work_item_id, const char *expect_stage, const char *expect_hash, const char *new_stage, const char *terminal_state)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, expect_stage ? expect_stage : "", expect_hash ? expect_hash : "", new_stage ? new_stage : "", terminal_state ? terminal_state : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_GATE_APPLY, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_work_item_set_pause(const char *work_item_id, const char *reason, const char *paused_state)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, reason ? reason : "", paused_state ? paused_state : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_SET_PAUSE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_work_item_clear_pause(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_work_item_set_cost_cap(const char *work_item_id, double cap)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%.17g", (double)cap);
   const char *fields[] = {work_item_id, arg1};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_SET_COST_CAP, fields, 2, NULL, NULL, 0, NULL));
}

int db1_work_item_delete(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_DELETE, fields, 1, NULL, NULL, 0, NULL));
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
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_LIST, fields, 1, wire_values, wire_caps,
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
   return write_result(db1_call(&family, AIMEE_DB1_OP_LIFECYCLE_EVENT_ADD, fields, 7, NULL, NULL, 0, NULL));
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
   int wire_status = db1_call(&family, AIMEE_DB1_OP_LIFECYCLE_EVENT_LIST, fields, 2, wire_values, wire_caps,
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
   int wire_status = db1_call(&family, AIMEE_DB1_OP_STAGE_ATTEMPT_INC, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_stage_attempt_reset(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *fields[] = {work_item_id, stage ? stage : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_STAGE_ATTEMPT_RESET, fields, 2, NULL, NULL, 0, NULL));
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
   return write_result(db1_call(&family, AIMEE_DB1_OP_WORK_ITEM_RECORD_OUTCOME, fields, 14, NULL, NULL, 0, NULL));
}

/* clang-format on */
