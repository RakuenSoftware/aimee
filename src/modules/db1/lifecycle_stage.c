/* modules/db1/lifecycle_stage.c: the lifecycle stage handler.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * The serving half of the boundary: decode the frame the client encoded, call
 * the domain, and answer. The domain itself is hand-written and untouched --
 * only the wire around it is generated.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits. */
/* clang-format off */
#include "db1_stages.h"

#include "db1_module_api.h"
#include "wfe_engine_store.h"
#include "wfe_store.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy one counted field out of the frame, NUL-terminated.

   The frame bounds the field, not a fixed cap: these carry prompts, results and
   JSON documents, and an in-process caller has always passed them whole. An
   embedded NUL is still refused -- every field is spliced into a query
   parameter, and a NUL would silently shorten it into a different row. */
static int read_counted(const uint8_t *body, uint32_t len, uint32_t *offset, char **cursor,
                        const char **out)
{
   if (*offset + 4u > len)
      return 1;
   uint32_t n = aimee_db1_get_u32(body + *offset);
   *offset += 4u;
   if (n > len || *offset + n > len)
      return 1;
   if (memchr(body + *offset, 0, n) != NULL)
      return 1;
   memcpy(*cursor, body + *offset, n);
   (*cursor)[n] = '\0';
   *out = *cursor;
   *cursor += n + 1u;
   *offset += n;
   return 0;
}

/* Parse a field the catalog declared as an integer. Refuses anything that is
   not exactly a number: a partial parse would turn "12abc" into 12 and act on a
   value the caller never sent. */
static int parse_int(const char *text, int *out)
{{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   long value = strtol(text, &end, 10);
   if (errno != 0 || !end || *end != '\0' || value < INT_MIN || value > INT_MAX)
      return 1;
   *out = (int)value;
   return 0;
}}
/* The same, for a member the catalog declared as a 64-bit integer. */
static int parse_int64(const char *text, int64_t *out)
{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   long long value = strtoll(text, &end, 10);
   if (errno != 0 || !end || *end != '\0')
      return 1;
   *out = (int64_t)value;
   return 0;
}

/* The same, for a value the catalog declared as a double. A cost parsed as an
   integer is a different number, and one that still looks like a price. */
static int parse_double(const char *text, double *out)
{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   double value = strtod(text, &end);
   if (errno != 0 || !end || *end != '\0')
      return 1;
   *out = value;
   return 0;
}

/* status(u32) | field_count(u32) | (len(u32) | bytes) * count. A write answers
   with no values, a read with one, a row with a value per member. */
static uint32_t write_reply(uint8_t *out, uint32_t cap, uint32_t *out_len, uint32_t status,
                            const char *const *values, uint32_t count)
{
   uint32_t at = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(values[i]);
      if (cap < at + 4u + n)
         return AIMEE_DB1_STATUS_FAILED;
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      if (n)
         memcpy(out + at, values[i], n);
      at += n;
   }
   if (cap < 8u)
      return AIMEE_DB1_STATUS_FAILED;
   aimee_db1_put_u32(out, status);
   aimee_db1_put_u32(out + 4u, count);
   *out_len = at;
   return status;
}

aimee_module_status_t aimee_db1_stage_lifecycle(const uint8_t *request_body, uint32_t request_len,
                                             uint8_t *response_body, uint32_t response_capacity,
                                             uint32_t *response_len)
{
   if (request_len < 8u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t op = aimee_db1_get_u32(request_body);
   uint32_t count = aimee_db1_get_u32(request_body + 4u);
   /* Bounds the fixed array below. Without it a well-formed frame declaring
      more fields than any operation takes writes past it. Zero is allowed:
      an operation that takes no arguments decodes no fields, and the arity
      check in its own case is what refuses a frame that carries some. */
   if (count > AIMEE_DB1_FIELDS_MAX)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   /* One allocation for every field, sized by the frame that carried them: the
      fields plus a NUL each cannot exceed this. */
   const char *field[AIMEE_DB1_FIELDS_MAX];
   char *scratch = malloc((size_t)request_len + AIMEE_DB1_FIELDS_MAX);
   if (!scratch)
      return AIMEE_MODULE_STATUS_INTERNAL;
   char *cursor = scratch;
   aimee_module_status_t decoded = AIMEE_MODULE_STATUS_OK;

   uint32_t offset = 8u;
   for (uint32_t i = 0; i < count; ++i)
      if (read_counted(request_body, request_len, &offset, &cursor, &field[i]) != 0)
         decoded = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* Trailing bytes mean the caller and the module disagree about the op's
      arity, which is a contract mismatch rather than something to tolerate. */
   if (offset != request_len)
      decoded = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (decoded != AIMEE_MODULE_STATUS_OK)
   {
      free(scratch);
      return decoded;
   }

   char value[AIMEE_DB1_VALUE_MAX];
   value[0] = '\0';
   int rc = -1;
   int reads = 0;
   /* A row answers with a value per member; a plain read answers with one; a
      list answers with a value per member per row. */
   const char *const *rows = NULL;
   uint32_t row_count = 0u;
   /* A list returns its length in rc, where a read returns found/not-found, so
      the two cannot share a status mapping. The three owned blocks below hold
      the domain's rows, the cell pointers into them and the text for numeric
      members: all three must outlive write_reply, because that is what reads
      them. Declared unconditionally so this stays one readable flow -- unlike
      the static helpers above, an unused local costs nothing. */
   int listed = 0;
   /* Set by an operation whose C return says 1 found / 0 nothing / negative
      failed. A row-returning domain usually answers 0 or -1 and has no such
      distinction; one that does must not have it flattened, or "the queue is
      empty" and "the queue is broken" reach the caller as the same answer. */
   int found = 0;
   db1_work_item_t row_db1_work_item_t;
   db1_wfe_budget_reservation_t row_db1_wfe_budget_reservation_t;
   db1_wfe_budget_totals_t row_db1_wfe_budget_totals_t;
   db1_wfe_review_outcome_t row_db1_wfe_review_outcome_t;
   db1_wfe_frozen_conflict_t row_db1_wfe_frozen_conflict_t;
   const char *row_slots[22];
   char row_text[5][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;

   switch (op)
   {
   case AIMEE_DB1_OP_WORK_ITEM_CREATE:
      if (count != 7u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_create(field[0], field[1], field[2], field[3], field[4], field[5], field[6]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_GET:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_work_item_t, 0, sizeof row_db1_work_item_t);
      rc = db1_work_item_get(field[0], &row_db1_work_item_t);
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)row_db1_work_item_t.cum_cost_usd);
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)row_db1_work_item_t.work_item_max_cost_usd);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_work_item_t.override_count);
      snprintf(row_text[3], sizeof row_text[3], "%.17g", (double)row_db1_work_item_t.reserved_cost_usd);
      row_slots[0] = row_db1_work_item_t.work_item_id;
      row_slots[1] = row_db1_work_item_t.repo;
      row_slots[2] = row_db1_work_item_t.proposal_path;
      row_slots[3] = row_db1_work_item_t.workflow_name;
      row_slots[4] = row_db1_work_item_t.workflow_version;
      row_slots[5] = row_db1_work_item_t.current_stage;
      row_slots[6] = row_db1_work_item_t.state;
      row_slots[7] = row_db1_work_item_t.mode;
      row_slots[8] = row_db1_work_item_t.pause_reason;
      row_slots[9] = row_db1_work_item_t.paused_state;
      row_slots[10] = row_db1_work_item_t.content_hash;
      row_slots[11] = row_db1_work_item_t.pr_ref;
      row_slots[12] = row_db1_work_item_t.worktree;
      row_slots[13] = row_db1_work_item_t.submitter;
      row_slots[14] = row_db1_work_item_t.parent_id;
      row_slots[15] = row_text[0];
      row_slots[16] = row_text[1];
      row_slots[17] = row_text[2];
      row_slots[18] = row_text[3];
      row_slots[19] = row_db1_work_item_t.reservation_state;
      row_slots[20] = row_db1_work_item_t.source_path;
      row_slots[21] = row_db1_work_item_t.updated_at;
      rows = row_slots;
      row_count = 22u;
      reads = 1;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_ID_BY_PROPOSAL:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_id_by_proposal(field[0], field[1], value, sizeof value);
      reads = 1;
      found = 1;
      break;
   case AIMEE_DB1_OP_WORK_ITEM_ID_BY_PR_REF:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_id_by_pr_ref(field[0], value, sizeof value);
      reads = 1;
      found = 1;
      break;
   case AIMEE_DB1_OP_WORK_ITEM_SET_STAGE:
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_stage(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_SET_PR_REF:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_pr_ref(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_SET_WORKTREE:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_worktree(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_SET_SUBMITTER:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_submitter(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_SET_PARENT:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_parent(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_ABANDON_CHILDREN:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_work_item_abandon_children(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_CHILD_COUNTS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int scalar0 = 0;
      int scalar1 = 0;
      int scalar2 = 0;
      rc = db1_work_item_child_counts(field[0], &scalar0, &scalar1, &scalar2);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%d", scalar1);
      row_slots[1] = row_text[1];
      snprintf(row_text[2], sizeof row_text[2], "%d", scalar2);
      row_slots[2] = row_text[2];
      rows = row_slots;
      row_count = 3u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_COUNT_ACTIVE_BY_SUBMITTER:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_work_item_count_active_by_submitter(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_COUNT_RECENT_BY_SUBMITTER:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_work_item_count_recent_by_submitter(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_SUBMIT_CAPPED:
   {
      if (count != 10u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed7;
      if (parse_int(field[7], &parsed7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed8;
      if (parse_int(field[8], &parsed8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed9;
      if (parse_int(field[9], &parsed9) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_submit_capped(field[0], field[1], field[2], field[3], field[4], field[5], field[6], parsed7, parsed8, parsed9);
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_SET_TERMINAL:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_terminal(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_GATE_APPLY:
   {
      if (count != 5u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_work_item_gate_apply(field[0], field[1], field[2], field[3], field[4]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_SET_PAUSE:
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_pause(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_clear_pause(field[0]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE_IF:
   {
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_work_item_clear_pause_if(field[0], field[1], field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_ADD_COST:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed1;
      if (parse_double(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_add_cost(field[0], parsed1);
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_SET_COST_CAP:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed1;
      if (parse_double(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_set_cost_cap(field[0], parsed1);
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_INC_OVERRIDE:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_inc_override(field[0]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_DELETE:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_work_item_delete(field[0]);
      break;
   case AIMEE_DB1_OP_WORK_ITEM_REAP_STALE_PARKS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_work_item_reap_stale_parks(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_LIST:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed0 <= 0 || parsed0 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_work_item_t *found = NULL;
      rc = db1_work_item_list_bounded(&found, parsed0);
      domain_rows = found;
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 22u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 4u * sizeof *numbers);
         if (!cells || !numbers)
         {
            free(cells);
            free(numbers);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         numeric_owned = numbers;
         for (uint32_t row = 0; row < produced; ++row)
         {
            snprintf(numbers[row * 4u + 0u], 32,
                     "%.17g", (double)found[row].cum_cost_usd);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%.17g", (double)found[row].work_item_max_cost_usd);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%d", found[row].override_count);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%.17g", (double)found[row].reserved_cost_usd);
            cells[row * 22u + 0u] = found[row].work_item_id;
            cells[row * 22u + 1u] = found[row].repo;
            cells[row * 22u + 2u] = found[row].proposal_path;
            cells[row * 22u + 3u] = found[row].workflow_name;
            cells[row * 22u + 4u] = found[row].workflow_version;
            cells[row * 22u + 5u] = found[row].current_stage;
            cells[row * 22u + 6u] = found[row].state;
            cells[row * 22u + 7u] = found[row].mode;
            cells[row * 22u + 8u] = found[row].pause_reason;
            cells[row * 22u + 9u] = found[row].paused_state;
            cells[row * 22u + 10u] = found[row].content_hash;
            cells[row * 22u + 11u] = found[row].pr_ref;
            cells[row * 22u + 12u] = found[row].worktree;
            cells[row * 22u + 13u] = found[row].submitter;
            cells[row * 22u + 14u] = found[row].parent_id;
            cells[row * 22u + 15u] = numbers[row * 4u + 0u];
            cells[row * 22u + 16u] = numbers[row * 4u + 1u];
            cells[row * 22u + 17u] = numbers[row * 4u + 2u];
            cells[row * 22u + 18u] = numbers[row * 4u + 3u];
            cells[row * 22u + 19u] = found[row].reservation_state;
            cells[row * 22u + 20u] = found[row].source_path;
            cells[row * 22u + 21u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 22u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_LIST_LRU:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed0 <= 0 || parsed0 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_work_item_t *found = NULL;
      rc = db1_work_item_list_lru_bounded(&found, parsed0);
      domain_rows = found;
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 22u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 4u * sizeof *numbers);
         if (!cells || !numbers)
         {
            free(cells);
            free(numbers);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         numeric_owned = numbers;
         for (uint32_t row = 0; row < produced; ++row)
         {
            snprintf(numbers[row * 4u + 0u], 32,
                     "%.17g", (double)found[row].cum_cost_usd);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%.17g", (double)found[row].work_item_max_cost_usd);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%d", found[row].override_count);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%.17g", (double)found[row].reserved_cost_usd);
            cells[row * 22u + 0u] = found[row].work_item_id;
            cells[row * 22u + 1u] = found[row].repo;
            cells[row * 22u + 2u] = found[row].proposal_path;
            cells[row * 22u + 3u] = found[row].workflow_name;
            cells[row * 22u + 4u] = found[row].workflow_version;
            cells[row * 22u + 5u] = found[row].current_stage;
            cells[row * 22u + 6u] = found[row].state;
            cells[row * 22u + 7u] = found[row].mode;
            cells[row * 22u + 8u] = found[row].pause_reason;
            cells[row * 22u + 9u] = found[row].paused_state;
            cells[row * 22u + 10u] = found[row].content_hash;
            cells[row * 22u + 11u] = found[row].pr_ref;
            cells[row * 22u + 12u] = found[row].worktree;
            cells[row * 22u + 13u] = found[row].submitter;
            cells[row * 22u + 14u] = found[row].parent_id;
            cells[row * 22u + 15u] = numbers[row * 4u + 0u];
            cells[row * 22u + 16u] = numbers[row * 4u + 1u];
            cells[row * 22u + 17u] = numbers[row * 4u + 2u];
            cells[row * 22u + 18u] = numbers[row * 4u + 3u];
            cells[row * 22u + 19u] = found[row].reservation_state;
            cells[row * 22u + 20u] = found[row].source_path;
            cells[row * 22u + 21u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 22u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_LIFECYCLE_EVENT_ADD:
   {
      if (count != 7u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed6;
      if (parse_double(field[6], &parsed6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_lifecycle_event_add(field[0], field[1], field[2], field[3], field[4], field[5], parsed6);
      break;
   }
   case AIMEE_DB1_OP_LIFECYCLE_EVENT_LIST:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed1 <= 0 || parsed1 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_lifecycle_event_t *found = NULL;
      rc = db1_lifecycle_event_list_bounded(field[0], &found, parsed1);
      domain_rows = found;
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 2u * sizeof *numbers);
         if (!cells || !numbers)
         {
            free(cells);
            free(numbers);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         numeric_owned = numbers;
         for (uint32_t row = 0; row < produced; ++row)
         {
            snprintf(numbers[row * 2u + 0u], 32,
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 2u + 1u], 32,
                     "%.17g", (double)found[row].cost_usd);
            cells[row * 8u + 0u] = numbers[row * 2u + 0u];
            cells[row * 8u + 1u] = found[row].stage;
            cells[row * 8u + 2u] = found[row].kind;
            cells[row * 8u + 3u] = found[row].actor;
            cells[row * 8u + 4u] = found[row].detail;
            cells[row * 8u + 5u] = found[row].content_hash;
            cells[row * 8u + 6u] = numbers[row * 2u + 1u];
            cells[row * 8u + 7u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_STAGE_ATTEMPT_INC:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_stage_attempt_inc(field[0], field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_STAGE_ATTEMPT_RESET:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_stage_attempt_reset(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_STAGE_ATTEMPT_GET:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_stage_attempt_get(field[0], field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORK_ITEM_RECORD_OUTCOME:
   {
      if (count != 14u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_work_item_outcome_t row;
      memset(&row, 0, sizeof row);
      snprintf(row.work_item_id, sizeof row.work_item_id, "%s", field[0]);
      snprintf(row.node_id, sizeof row.node_id, "%s", field[1]);
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.disposition = member_2;
      snprintf(row.state, sizeof row.state, "%s", field[3]);
      snprintf(row.pause_reason, sizeof row.pause_reason, "%s", field[4]);
      snprintf(row.pause_stage, sizeof row.pause_stage, "%s", field[5]);
      snprintf(row.next_stage, sizeof row.next_stage, "%s", field[6]);
      snprintf(row.pr_ref, sizeof row.pr_ref, "%s", field[7]);
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.abandon_children = member_8;
      double member_9 = 0;
      if (parse_double(field[9], &member_9) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cost_usd = member_9;
      snprintf(row.event_kind, sizeof row.event_kind, "%s", field[10]);
      snprintf(row.event_detail, sizeof row.event_detail, "%s", field[11]);
      snprintf(row.event_hash, sizeof row.event_hash, "%s", field[12]);
      snprintf(row.park_reason, sizeof row.park_reason, "%s", field[13]);
      rc = db1_work_item_record_outcome(&row);
      break;
   }
   case AIMEE_DB1_OP_WFE_CHILDREN_LIST:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed1 <= 0 || parsed1 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_WFE_ID_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wfe_children_list(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
         if (!cells)
         {
            free(cells);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         for (uint32_t row = 0; row < produced; ++row)
         {
            cells[row * 1u + 0u] = found[row];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_ACTIVE_ROOT_COUNT:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_active_root_count();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_WORK_ITEM_ID_BY_GIT_PROPOSAL:
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_work_item_id_by_git_proposal(field[0], field[1], field[2], value, sizeof value);
      reads = 1;
      found = 1;
      break;
   case AIMEE_DB1_OP_WFE_EXECUTED_TURN_COUNT:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_executed_turn_count(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_STAGE_LOOP_COUNT:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_stage_loop_count(field[0], field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_RUNNER_FAILURES_SINCE_PROGRESS:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_runner_failures_since_progress(field[0], field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_CAPACITY_WAITS_SINCE_PROGRESS:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_capacity_waits_since_progress(field[0], field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_DESCENDANT_IDS:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed1 <= 0 || parsed1 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_WFE_ID_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wfe_descendant_ids(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
         if (!cells)
         {
            free(cells);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         for (uint32_t row = 0; row < produced; ++row)
         {
            cells[row * 1u + 0u] = found[row];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_RESUME_TRANSIENT:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      long long produced = db1_wfe_resume_transient(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_RESUME_WALL_CAPS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      long long produced = db1_wfe_resume_wall_caps(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_ABANDON_EXHAUSTED_WALL_CAPS:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      long long produced = db1_wfe_abandon_exhausted_wall_caps(parsed0, parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_RESUME_READY_PARENTS:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      long long produced = db1_wfe_resume_ready_parents();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_DELEGATE_JOB_SAVE:
   {
      if (count != 4u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t parsed2;
      if (parse_int64(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_delegate_job_save(field[0], field[1], parsed2, field[3]);
      break;
   }
   case AIMEE_DB1_OP_WFE_DELEGATE_JOBS_TERMINAL_CLAIM:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed0 <= 0 || parsed0 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_wfe_delegate_job_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wfe_delegate_jobs_terminal_claim(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 2u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 1u * sizeof *numbers);
         if (!cells || !numbers)
         {
            free(cells);
            free(numbers);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         numeric_owned = numbers;
         for (uint32_t row = 0; row < produced; ++row)
         {
            snprintf(numbers[row * 1u + 0u], 32,
                     "%lld", (long long)found[row].job_id);
            cells[row * 2u + 0u] = found[row].execution_key;
            cells[row * 2u + 1u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 2u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_BUDGET_RESERVE:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_wfe_budget_reservation_t, 0, sizeof row_db1_wfe_budget_reservation_t);
      rc = db1_wfe_budget_reserve(field[0], field[1], &row_db1_wfe_budget_reservation_t);
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)row_db1_wfe_budget_reservation_t.max_usd);
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)row_db1_wfe_budget_reservation_t.amount);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_wfe_budget_reservation_t.allowed);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_db1_wfe_budget_reservation_t.busy);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_db1_wfe_budget_reservation_t.replay_only);
      row_slots[0] = row_db1_wfe_budget_reservation_t.root_id;
      row_slots[1] = row_text[0];
      row_slots[2] = row_text[1];
      row_slots[3] = row_text[2];
      row_slots[4] = row_text[3];
      row_slots[5] = row_text[4];
      rows = row_slots;
      row_count = 6u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_BUDGET_TOTALS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_wfe_budget_totals_t, 0, sizeof row_db1_wfe_budget_totals_t);
      rc = db1_wfe_budget_totals(field[0], &row_db1_wfe_budget_totals_t);
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)row_db1_wfe_budget_totals_t.spent);
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)row_db1_wfe_budget_totals_t.max_usd);
      row_slots[0] = row_db1_wfe_budget_totals_t.root_id;
      row_slots[1] = row_text[0];
      row_slots[2] = row_text[1];
      rows = row_slots;
      row_count = 3u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_BUDGET_RELEASE:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_budget_release(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WFE_BUDGET_HEARTBEAT:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_budget_heartbeat(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_WFE_BUDGET_RECONCILE:
   {
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed2;
      if (parse_double(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_budget_reconcile(field[0], field[1], parsed2);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_MOVE:
   {
      if (count != 7u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[3][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed6;
      if (parse_double(field[6], &parsed6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_move(field[0], field[1], field[2], field[3], field[4], field[5], parsed6);
      break;
   }
   case AIMEE_DB1_OP_WFE_RECORD_RETRY:
   {
      if (count != 6u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[4][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed4;
      if (parse_int(field[4], &parsed4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed5;
      if (parse_double(field[5], &parsed5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_record_retry(field[0], field[1], field[2], field[3], parsed4, parsed5);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_PARK_WITH_DETAIL:
   {
      if (count != 5u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed4;
      if (parse_double(field[4], &parsed4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_park_with_detail(field[0], field[1], field[2], field[3], parsed4);
      break;
   }
   case AIMEE_DB1_OP_WFE_RESUME:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_resume(field[0]);
      break;
   case AIMEE_DB1_OP_WFE_FINISH:
   {
      if (count != 6u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed5;
      if (parse_double(field[5], &parsed5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_finish(field[0], field[1], field[2], field[3], field[4], parsed5);
      break;
   }
   case AIMEE_DB1_OP_WFE_STOP_TREE:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed1 <= 0 || parsed1 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_WFE_ID_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wfe_stop_tree(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
         if (!cells)
         {
            free(cells);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         for (uint32_t row = 0; row < produced; ++row)
         {
            cells[row * 1u + 0u] = found[row];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_RECONCILE_ORPHANS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed0 <= 0 || parsed0 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_WFE_ID_LEN] = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wfe_reconcile_orphans(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
         if (!cells)
         {
            free(cells);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         for (uint32_t row = 0; row < produced; ++row)
         {
            cells[row * 1u + 0u] = found[row];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_PARK_BUDGET_TREE:
   {
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed2;
      if (parse_double(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_park_budget_tree(field[0], field[1], parsed2);
      break;
   }
   case AIMEE_DB1_OP_WFE_DELETE_TREE:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_delete_tree(field[0]);
      break;
   case AIMEE_DB1_OP_WFE_RESOLVE_GATE:
      if (count != 5u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_resolve_gate(field[0], field[1], field[2], field[3], field[4]);
      break;
   case AIMEE_DB1_OP_WFE_REJECT_GATE:
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_reject_gate(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_WFE_PARK_RUNNER_FAILURE:
   {
      if (count != 8u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[3][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[5][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[6][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed5;
      if (parse_int(field[5], &parsed5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed6;
      if (parse_int(field[6], &parsed6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed7;
      if (parse_double(field[7], &parsed7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_park_runner_failure(field[0], field[1], field[2], field[3], field[4], parsed5, parsed6, parsed7);
      break;
   }
   case AIMEE_DB1_OP_WFE_RECOVER_LOST_REPLAY:
   {
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_recover_lost_replay(field[0], field[1], field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_RECORD_REQUESTED_CHANGES:
   {
      if (count != 9u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[3][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[4][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[6][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[7][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed6;
      if (parse_int(field[6], &parsed6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed7;
      if (parse_int(field[7], &parsed7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed8;
      if (parse_double(field[8], &parsed8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_wfe_review_outcome_t, 0, sizeof row_db1_wfe_review_outcome_t);
      rc = db1_wfe_record_requested_changes(field[0], field[1], field[2], field[3], field[4], field[5], parsed6, parsed7, parsed8, &row_db1_wfe_review_outcome_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_wfe_review_outcome_t.attempts);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_wfe_review_outcome_t.identical_repeats);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_wfe_review_outcome_t.parked);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_db1_wfe_review_outcome_t.pause_reason;
      rows = row_slots;
      row_count = 4u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_CLAIM_FROZEN_CREATES:
   {
      if (count != 131u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[130][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_wfe_frozen_claim_t row;
      memset(&row, 0, sizeof row);
      snprintf(row.parent_id, sizeof row.parent_id, "%s", field[0]);
      snprintf(row.work_item_id, sizeof row.work_item_id, "%s", field[1]);
      snprintf(row.creates[0].path, sizeof row.creates[0].path, "%s", field[2]);
      snprintf(row.creates[0].content_hash, sizeof row.creates[0].content_hash, "%s", field[3]);
      snprintf(row.creates[1].path, sizeof row.creates[1].path, "%s", field[4]);
      snprintf(row.creates[1].content_hash, sizeof row.creates[1].content_hash, "%s", field[5]);
      snprintf(row.creates[2].path, sizeof row.creates[2].path, "%s", field[6]);
      snprintf(row.creates[2].content_hash, sizeof row.creates[2].content_hash, "%s", field[7]);
      snprintf(row.creates[3].path, sizeof row.creates[3].path, "%s", field[8]);
      snprintf(row.creates[3].content_hash, sizeof row.creates[3].content_hash, "%s", field[9]);
      snprintf(row.creates[4].path, sizeof row.creates[4].path, "%s", field[10]);
      snprintf(row.creates[4].content_hash, sizeof row.creates[4].content_hash, "%s", field[11]);
      snprintf(row.creates[5].path, sizeof row.creates[5].path, "%s", field[12]);
      snprintf(row.creates[5].content_hash, sizeof row.creates[5].content_hash, "%s", field[13]);
      snprintf(row.creates[6].path, sizeof row.creates[6].path, "%s", field[14]);
      snprintf(row.creates[6].content_hash, sizeof row.creates[6].content_hash, "%s", field[15]);
      snprintf(row.creates[7].path, sizeof row.creates[7].path, "%s", field[16]);
      snprintf(row.creates[7].content_hash, sizeof row.creates[7].content_hash, "%s", field[17]);
      snprintf(row.creates[8].path, sizeof row.creates[8].path, "%s", field[18]);
      snprintf(row.creates[8].content_hash, sizeof row.creates[8].content_hash, "%s", field[19]);
      snprintf(row.creates[9].path, sizeof row.creates[9].path, "%s", field[20]);
      snprintf(row.creates[9].content_hash, sizeof row.creates[9].content_hash, "%s", field[21]);
      snprintf(row.creates[10].path, sizeof row.creates[10].path, "%s", field[22]);
      snprintf(row.creates[10].content_hash, sizeof row.creates[10].content_hash, "%s", field[23]);
      snprintf(row.creates[11].path, sizeof row.creates[11].path, "%s", field[24]);
      snprintf(row.creates[11].content_hash, sizeof row.creates[11].content_hash, "%s", field[25]);
      snprintf(row.creates[12].path, sizeof row.creates[12].path, "%s", field[26]);
      snprintf(row.creates[12].content_hash, sizeof row.creates[12].content_hash, "%s", field[27]);
      snprintf(row.creates[13].path, sizeof row.creates[13].path, "%s", field[28]);
      snprintf(row.creates[13].content_hash, sizeof row.creates[13].content_hash, "%s", field[29]);
      snprintf(row.creates[14].path, sizeof row.creates[14].path, "%s", field[30]);
      snprintf(row.creates[14].content_hash, sizeof row.creates[14].content_hash, "%s", field[31]);
      snprintf(row.creates[15].path, sizeof row.creates[15].path, "%s", field[32]);
      snprintf(row.creates[15].content_hash, sizeof row.creates[15].content_hash, "%s", field[33]);
      snprintf(row.creates[16].path, sizeof row.creates[16].path, "%s", field[34]);
      snprintf(row.creates[16].content_hash, sizeof row.creates[16].content_hash, "%s", field[35]);
      snprintf(row.creates[17].path, sizeof row.creates[17].path, "%s", field[36]);
      snprintf(row.creates[17].content_hash, sizeof row.creates[17].content_hash, "%s", field[37]);
      snprintf(row.creates[18].path, sizeof row.creates[18].path, "%s", field[38]);
      snprintf(row.creates[18].content_hash, sizeof row.creates[18].content_hash, "%s", field[39]);
      snprintf(row.creates[19].path, sizeof row.creates[19].path, "%s", field[40]);
      snprintf(row.creates[19].content_hash, sizeof row.creates[19].content_hash, "%s", field[41]);
      snprintf(row.creates[20].path, sizeof row.creates[20].path, "%s", field[42]);
      snprintf(row.creates[20].content_hash, sizeof row.creates[20].content_hash, "%s", field[43]);
      snprintf(row.creates[21].path, sizeof row.creates[21].path, "%s", field[44]);
      snprintf(row.creates[21].content_hash, sizeof row.creates[21].content_hash, "%s", field[45]);
      snprintf(row.creates[22].path, sizeof row.creates[22].path, "%s", field[46]);
      snprintf(row.creates[22].content_hash, sizeof row.creates[22].content_hash, "%s", field[47]);
      snprintf(row.creates[23].path, sizeof row.creates[23].path, "%s", field[48]);
      snprintf(row.creates[23].content_hash, sizeof row.creates[23].content_hash, "%s", field[49]);
      snprintf(row.creates[24].path, sizeof row.creates[24].path, "%s", field[50]);
      snprintf(row.creates[24].content_hash, sizeof row.creates[24].content_hash, "%s", field[51]);
      snprintf(row.creates[25].path, sizeof row.creates[25].path, "%s", field[52]);
      snprintf(row.creates[25].content_hash, sizeof row.creates[25].content_hash, "%s", field[53]);
      snprintf(row.creates[26].path, sizeof row.creates[26].path, "%s", field[54]);
      snprintf(row.creates[26].content_hash, sizeof row.creates[26].content_hash, "%s", field[55]);
      snprintf(row.creates[27].path, sizeof row.creates[27].path, "%s", field[56]);
      snprintf(row.creates[27].content_hash, sizeof row.creates[27].content_hash, "%s", field[57]);
      snprintf(row.creates[28].path, sizeof row.creates[28].path, "%s", field[58]);
      snprintf(row.creates[28].content_hash, sizeof row.creates[28].content_hash, "%s", field[59]);
      snprintf(row.creates[29].path, sizeof row.creates[29].path, "%s", field[60]);
      snprintf(row.creates[29].content_hash, sizeof row.creates[29].content_hash, "%s", field[61]);
      snprintf(row.creates[30].path, sizeof row.creates[30].path, "%s", field[62]);
      snprintf(row.creates[30].content_hash, sizeof row.creates[30].content_hash, "%s", field[63]);
      snprintf(row.creates[31].path, sizeof row.creates[31].path, "%s", field[64]);
      snprintf(row.creates[31].content_hash, sizeof row.creates[31].content_hash, "%s", field[65]);
      snprintf(row.creates[32].path, sizeof row.creates[32].path, "%s", field[66]);
      snprintf(row.creates[32].content_hash, sizeof row.creates[32].content_hash, "%s", field[67]);
      snprintf(row.creates[33].path, sizeof row.creates[33].path, "%s", field[68]);
      snprintf(row.creates[33].content_hash, sizeof row.creates[33].content_hash, "%s", field[69]);
      snprintf(row.creates[34].path, sizeof row.creates[34].path, "%s", field[70]);
      snprintf(row.creates[34].content_hash, sizeof row.creates[34].content_hash, "%s", field[71]);
      snprintf(row.creates[35].path, sizeof row.creates[35].path, "%s", field[72]);
      snprintf(row.creates[35].content_hash, sizeof row.creates[35].content_hash, "%s", field[73]);
      snprintf(row.creates[36].path, sizeof row.creates[36].path, "%s", field[74]);
      snprintf(row.creates[36].content_hash, sizeof row.creates[36].content_hash, "%s", field[75]);
      snprintf(row.creates[37].path, sizeof row.creates[37].path, "%s", field[76]);
      snprintf(row.creates[37].content_hash, sizeof row.creates[37].content_hash, "%s", field[77]);
      snprintf(row.creates[38].path, sizeof row.creates[38].path, "%s", field[78]);
      snprintf(row.creates[38].content_hash, sizeof row.creates[38].content_hash, "%s", field[79]);
      snprintf(row.creates[39].path, sizeof row.creates[39].path, "%s", field[80]);
      snprintf(row.creates[39].content_hash, sizeof row.creates[39].content_hash, "%s", field[81]);
      snprintf(row.creates[40].path, sizeof row.creates[40].path, "%s", field[82]);
      snprintf(row.creates[40].content_hash, sizeof row.creates[40].content_hash, "%s", field[83]);
      snprintf(row.creates[41].path, sizeof row.creates[41].path, "%s", field[84]);
      snprintf(row.creates[41].content_hash, sizeof row.creates[41].content_hash, "%s", field[85]);
      snprintf(row.creates[42].path, sizeof row.creates[42].path, "%s", field[86]);
      snprintf(row.creates[42].content_hash, sizeof row.creates[42].content_hash, "%s", field[87]);
      snprintf(row.creates[43].path, sizeof row.creates[43].path, "%s", field[88]);
      snprintf(row.creates[43].content_hash, sizeof row.creates[43].content_hash, "%s", field[89]);
      snprintf(row.creates[44].path, sizeof row.creates[44].path, "%s", field[90]);
      snprintf(row.creates[44].content_hash, sizeof row.creates[44].content_hash, "%s", field[91]);
      snprintf(row.creates[45].path, sizeof row.creates[45].path, "%s", field[92]);
      snprintf(row.creates[45].content_hash, sizeof row.creates[45].content_hash, "%s", field[93]);
      snprintf(row.creates[46].path, sizeof row.creates[46].path, "%s", field[94]);
      snprintf(row.creates[46].content_hash, sizeof row.creates[46].content_hash, "%s", field[95]);
      snprintf(row.creates[47].path, sizeof row.creates[47].path, "%s", field[96]);
      snprintf(row.creates[47].content_hash, sizeof row.creates[47].content_hash, "%s", field[97]);
      snprintf(row.creates[48].path, sizeof row.creates[48].path, "%s", field[98]);
      snprintf(row.creates[48].content_hash, sizeof row.creates[48].content_hash, "%s", field[99]);
      snprintf(row.creates[49].path, sizeof row.creates[49].path, "%s", field[100]);
      snprintf(row.creates[49].content_hash, sizeof row.creates[49].content_hash, "%s", field[101]);
      snprintf(row.creates[50].path, sizeof row.creates[50].path, "%s", field[102]);
      snprintf(row.creates[50].content_hash, sizeof row.creates[50].content_hash, "%s", field[103]);
      snprintf(row.creates[51].path, sizeof row.creates[51].path, "%s", field[104]);
      snprintf(row.creates[51].content_hash, sizeof row.creates[51].content_hash, "%s", field[105]);
      snprintf(row.creates[52].path, sizeof row.creates[52].path, "%s", field[106]);
      snprintf(row.creates[52].content_hash, sizeof row.creates[52].content_hash, "%s", field[107]);
      snprintf(row.creates[53].path, sizeof row.creates[53].path, "%s", field[108]);
      snprintf(row.creates[53].content_hash, sizeof row.creates[53].content_hash, "%s", field[109]);
      snprintf(row.creates[54].path, sizeof row.creates[54].path, "%s", field[110]);
      snprintf(row.creates[54].content_hash, sizeof row.creates[54].content_hash, "%s", field[111]);
      snprintf(row.creates[55].path, sizeof row.creates[55].path, "%s", field[112]);
      snprintf(row.creates[55].content_hash, sizeof row.creates[55].content_hash, "%s", field[113]);
      snprintf(row.creates[56].path, sizeof row.creates[56].path, "%s", field[114]);
      snprintf(row.creates[56].content_hash, sizeof row.creates[56].content_hash, "%s", field[115]);
      snprintf(row.creates[57].path, sizeof row.creates[57].path, "%s", field[116]);
      snprintf(row.creates[57].content_hash, sizeof row.creates[57].content_hash, "%s", field[117]);
      snprintf(row.creates[58].path, sizeof row.creates[58].path, "%s", field[118]);
      snprintf(row.creates[58].content_hash, sizeof row.creates[58].content_hash, "%s", field[119]);
      snprintf(row.creates[59].path, sizeof row.creates[59].path, "%s", field[120]);
      snprintf(row.creates[59].content_hash, sizeof row.creates[59].content_hash, "%s", field[121]);
      snprintf(row.creates[60].path, sizeof row.creates[60].path, "%s", field[122]);
      snprintf(row.creates[60].content_hash, sizeof row.creates[60].content_hash, "%s", field[123]);
      snprintf(row.creates[61].path, sizeof row.creates[61].path, "%s", field[124]);
      snprintf(row.creates[61].content_hash, sizeof row.creates[61].content_hash, "%s", field[125]);
      snprintf(row.creates[62].path, sizeof row.creates[62].path, "%s", field[126]);
      snprintf(row.creates[62].content_hash, sizeof row.creates[62].content_hash, "%s", field[127]);
      snprintf(row.creates[63].path, sizeof row.creates[63].path, "%s", field[128]);
      snprintf(row.creates[63].content_hash, sizeof row.creates[63].content_hash, "%s", field[129]);
      int member_130 = 0;
      if (parse_int(field[130], &member_130) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.create_count = member_130;
      memset(&row_db1_wfe_frozen_conflict_t, 0, sizeof row_db1_wfe_frozen_conflict_t);
      rc = db1_wfe_claim_frozen_creates(&row, &row_db1_wfe_frozen_conflict_t);
      row_slots[0] = row_db1_wfe_frozen_conflict_t.path;
      row_slots[1] = row_db1_wfe_frozen_conflict_t.existing_work_item;
      row_slots[2] = row_db1_wfe_frozen_conflict_t.conflicting_work_item;
      rows = row_slots;
      row_count = 3u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_CREATE_WORK_ITEM:
   {
      if (count != 12u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[3][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[5][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      double parsed10;
      if (parse_double(field[10], &parsed10) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed11;
      if (parse_int(field[11], &parsed11) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_create_work_item(field[0], field[1], field[2], field[3], field[4], field[5], field[6], field[7], field[8], field[9], parsed10, parsed11);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_LATEST_STAGE_RETRY_DETAIL:
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wfe_latest_stage_retry_detail(field[0], field[1], value, sizeof value);
      reads = 1;
      break;
   default:
      free(scratch);
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }
   free(scratch);

   /* The two conventions must not be flattened. A read returns FOUND(1),
      not-found(0) or error(-1); a write returns 0 or -1. Mapping a read's -1
      onto MISSING would report a broken store as "nothing recorded", and the
      caller would act on an absence that was never established. */
   uint32_t status;
   if (listed)
      /* A list answers with how many rows it found, so any count is success and
         only a negative return is a failure. Zero rows is an empty list, not a
         miss: the caller asked what was there and the answer was nothing. */
      status = (rc >= 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (found)
      status = (rc > 0) ? AIMEE_DB1_STATUS_OK
                        : (rc == 0 ? AIMEE_DB1_STATUS_MISSING : AIMEE_DB1_STATUS_FAILED);
   else if (rows)
      /* A row-returning domain usually answers 0 or -1: there is no
         found/not-found distinction to preserve, so neither is invented. */
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (reads)
   {
      if (rc < 0)
         status = AIMEE_DB1_STATUS_FAILED;
      else if (rc == 0 || !(text_owned ? text_owned : value)[0])
         status = AIMEE_DB1_STATUS_MISSING;
      else
         status = AIMEE_DB1_STATUS_OK;
   }
   else
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;

   {
      const char *held = text_owned ? text_owned : value;
      const char *one = (status == AIMEE_DB1_STATUS_OK) ? held : "";
      const char *const single[] = {one};
      const char *const *out_values = rows ? rows : (reads ? single : NULL);
      uint32_t out_count = rows ? row_count : (reads ? 1u : 0u);
      if (status != AIMEE_DB1_STATUS_OK && rows)
         out_count = 0u; /* nothing to report but the status */
      /* A reply that does not fit is a failure, not a success with nothing in
         it. write_reply refuses rather than truncating -- which is right -- but
         discarding that answer left the caller a well-formed frame carrying no
         rows, and a read cannot tell that from a row that is genuinely empty.
         Say it in the frame instead: a bare status needs eight bytes, so the
         second call fits wherever the first did not. */
      if (write_reply(response_body, response_capacity, response_len, status, out_values,
                      out_count) != status)
         write_reply(response_body, response_capacity, response_len, AIMEE_DB1_STATUS_FAILED,
                     NULL, 0u);
   }
   free(cells_owned);
   free(numeric_owned);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
