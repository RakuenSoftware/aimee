/* modules/db1/roundtable_stage.c: the roundtable stage handler.
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
#include "roundtable_pipeline.h"

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

aimee_module_status_t aimee_db1_stage_roundtable(const uint8_t *request_body, uint32_t request_len,
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
   rtp_run_t row_rtp_run_t;
   rtp_pass_t row_rtp_pass_t;
   rtp_group_agg_t row_rtp_group_agg_t;
   rtp_attempt_t row_rtp_attempt_t;
   rtp_gate_t row_rtp_gate_t;
   const char *row_slots[36];
   char row_text[26][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;

   switch (op)
   {
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_CREATE:
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
      int scalar0 = 0;
      rc = db1_roundtable_run_create(field[0], field[1], field[2], field[3], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_GET:
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
      memset(&row_rtp_run_t, 0, sizeof row_rtp_run_t);
      rc = db1_roundtable_run_get(parsed0, &row_rtp_run_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_run_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_run_t.schema_version);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_run_t.proposal_pr_number);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_run_t.impl_pr_number);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_run_t.cost_version);
      snprintf(row_text[5], sizeof row_text[5], "%.17g", (double)row_rtp_run_t.proposal_phase_cost_usd);
      snprintf(row_text[6], sizeof row_text[6], "%.17g", (double)row_rtp_run_t.impl_phase_cost_usd);
      snprintf(row_text[7], sizeof row_text[7], "%.17g", (double)row_rtp_run_t.total_cost_usd);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_rtp_run_t.accepted_question_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_rtp_run_t.idea;
      row_slots[2] = row_rtp_run_t.state;
      row_slots[3] = row_rtp_run_t.phase;
      row_slots[4] = row_rtp_run_t.admission_class;
      row_slots[5] = row_text[1];
      row_slots[6] = row_rtp_run_t.done_bar;
      row_slots[7] = row_rtp_run_t.brief;
      row_slots[8] = row_rtp_run_t.gate_digest;
      row_slots[9] = row_rtp_run_t.proposal_ref;
      row_slots[10] = row_rtp_run_t.proposal_origin_hash;
      row_slots[11] = row_rtp_run_t.diff_ref;
      row_slots[12] = row_rtp_run_t.diff_origin_hash;
      row_slots[13] = row_rtp_run_t.chunk_index_ref;
      row_slots[14] = row_rtp_run_t.repo_root;
      row_slots[15] = row_rtp_run_t.remote;
      row_slots[16] = row_rtp_run_t.base_branch;
      row_slots[17] = row_rtp_run_t.head_branch;
      row_slots[18] = row_rtp_run_t.workspace_id;
      row_slots[19] = row_rtp_run_t.workspace_provider;
      row_slots[20] = row_rtp_run_t.worktree_path;
      row_slots[21] = row_rtp_run_t.head_sha;
      row_slots[22] = row_rtp_run_t.base_sha;
      row_slots[23] = row_text[2];
      row_slots[24] = row_rtp_run_t.proposal_pr_url;
      row_slots[25] = row_text[3];
      row_slots[26] = row_rtp_run_t.impl_pr_url;
      row_slots[27] = row_rtp_run_t.cost_scope;
      row_slots[28] = row_rtp_run_t.cost_source;
      row_slots[29] = row_text[4];
      row_slots[30] = row_text[5];
      row_slots[31] = row_text[6];
      row_slots[32] = row_text[7];
      row_slots[33] = row_text[8];
      row_slots[34] = row_rtp_run_t.created_at;
      row_slots[35] = row_rtp_run_t.updated_at;
      rows = row_slots;
      row_count = 36u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_UPDATE:
   {
      if (count != 36u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rtp_run_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      snprintf(row.idea, sizeof row.idea, "%s", field[1]);
      snprintf(row.state, sizeof row.state, "%s", field[2]);
      snprintf(row.phase, sizeof row.phase, "%s", field[3]);
      snprintf(row.admission_class, sizeof row.admission_class, "%s", field[4]);
      int member_5 = 0;
      if (parse_int(field[5], &member_5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.schema_version = member_5;
      snprintf(row.done_bar, sizeof row.done_bar, "%s", field[6]);
      snprintf(row.brief, sizeof row.brief, "%s", field[7]);
      snprintf(row.gate_digest, sizeof row.gate_digest, "%s", field[8]);
      snprintf(row.proposal_ref, sizeof row.proposal_ref, "%s", field[9]);
      snprintf(row.proposal_origin_hash, sizeof row.proposal_origin_hash, "%s", field[10]);
      snprintf(row.diff_ref, sizeof row.diff_ref, "%s", field[11]);
      snprintf(row.diff_origin_hash, sizeof row.diff_origin_hash, "%s", field[12]);
      snprintf(row.chunk_index_ref, sizeof row.chunk_index_ref, "%s", field[13]);
      snprintf(row.repo_root, sizeof row.repo_root, "%s", field[14]);
      snprintf(row.remote, sizeof row.remote, "%s", field[15]);
      snprintf(row.base_branch, sizeof row.base_branch, "%s", field[16]);
      snprintf(row.head_branch, sizeof row.head_branch, "%s", field[17]);
      snprintf(row.workspace_id, sizeof row.workspace_id, "%s", field[18]);
      snprintf(row.workspace_provider, sizeof row.workspace_provider, "%s", field[19]);
      snprintf(row.worktree_path, sizeof row.worktree_path, "%s", field[20]);
      snprintf(row.head_sha, sizeof row.head_sha, "%s", field[21]);
      snprintf(row.base_sha, sizeof row.base_sha, "%s", field[22]);
      int member_23 = 0;
      if (parse_int(field[23], &member_23) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.proposal_pr_number = member_23;
      snprintf(row.proposal_pr_url, sizeof row.proposal_pr_url, "%s", field[24]);
      int member_25 = 0;
      if (parse_int(field[25], &member_25) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.impl_pr_number = member_25;
      snprintf(row.impl_pr_url, sizeof row.impl_pr_url, "%s", field[26]);
      snprintf(row.cost_scope, sizeof row.cost_scope, "%s", field[27]);
      snprintf(row.cost_source, sizeof row.cost_source, "%s", field[28]);
      int member_29 = 0;
      if (parse_int(field[29], &member_29) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cost_version = member_29;
      double member_30 = 0;
      if (parse_double(field[30], &member_30) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.proposal_phase_cost_usd = member_30;
      double member_31 = 0;
      if (parse_double(field[31], &member_31) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.impl_phase_cost_usd = member_31;
      double member_32 = 0;
      if (parse_double(field[32], &member_32) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.total_cost_usd = member_32;
      int member_33 = 0;
      if (parse_int(field[33], &member_33) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.accepted_question_count = member_33;
      snprintf(row.created_at, sizeof row.created_at, "%s", field[34]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[35]);
      rc = db1_roundtable_run_update(&row);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_SET_STATE:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_roundtable_run_set_state(parsed0, field[1], field[2]);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_CAS_STATE:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_roundtable_run_cas_state(parsed0, field[1], field[2]);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_LIST:
   {
      if (count != 2u)
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rtp_run_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_roundtable_run_list(field[0][0] ? field[0] : NULL, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 36u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 9u * sizeof *numbers);
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
            snprintf(numbers[row * 9u + 0u], 32,
                     "%d", found[row].id);
            snprintf(numbers[row * 9u + 1u], 32,
                     "%d", found[row].schema_version);
            snprintf(numbers[row * 9u + 2u], 32,
                     "%d", found[row].proposal_pr_number);
            snprintf(numbers[row * 9u + 3u], 32,
                     "%d", found[row].impl_pr_number);
            snprintf(numbers[row * 9u + 4u], 32,
                     "%d", found[row].cost_version);
            snprintf(numbers[row * 9u + 5u], 32,
                     "%.17g", (double)found[row].proposal_phase_cost_usd);
            snprintf(numbers[row * 9u + 6u], 32,
                     "%.17g", (double)found[row].impl_phase_cost_usd);
            snprintf(numbers[row * 9u + 7u], 32,
                     "%.17g", (double)found[row].total_cost_usd);
            snprintf(numbers[row * 9u + 8u], 32,
                     "%d", found[row].accepted_question_count);
            cells[row * 36u + 0u] = numbers[row * 9u + 0u];
            cells[row * 36u + 1u] = found[row].idea;
            cells[row * 36u + 2u] = found[row].state;
            cells[row * 36u + 3u] = found[row].phase;
            cells[row * 36u + 4u] = found[row].admission_class;
            cells[row * 36u + 5u] = numbers[row * 9u + 1u];
            cells[row * 36u + 6u] = found[row].done_bar;
            cells[row * 36u + 7u] = found[row].brief;
            cells[row * 36u + 8u] = found[row].gate_digest;
            cells[row * 36u + 9u] = found[row].proposal_ref;
            cells[row * 36u + 10u] = found[row].proposal_origin_hash;
            cells[row * 36u + 11u] = found[row].diff_ref;
            cells[row * 36u + 12u] = found[row].diff_origin_hash;
            cells[row * 36u + 13u] = found[row].chunk_index_ref;
            cells[row * 36u + 14u] = found[row].repo_root;
            cells[row * 36u + 15u] = found[row].remote;
            cells[row * 36u + 16u] = found[row].base_branch;
            cells[row * 36u + 17u] = found[row].head_branch;
            cells[row * 36u + 18u] = found[row].workspace_id;
            cells[row * 36u + 19u] = found[row].workspace_provider;
            cells[row * 36u + 20u] = found[row].worktree_path;
            cells[row * 36u + 21u] = found[row].head_sha;
            cells[row * 36u + 22u] = found[row].base_sha;
            cells[row * 36u + 23u] = numbers[row * 9u + 2u];
            cells[row * 36u + 24u] = found[row].proposal_pr_url;
            cells[row * 36u + 25u] = numbers[row * 9u + 3u];
            cells[row * 36u + 26u] = found[row].impl_pr_url;
            cells[row * 36u + 27u] = found[row].cost_scope;
            cells[row * 36u + 28u] = found[row].cost_source;
            cells[row * 36u + 29u] = numbers[row * 9u + 4u];
            cells[row * 36u + 30u] = numbers[row * 9u + 5u];
            cells[row * 36u + 31u] = numbers[row * 9u + 6u];
            cells[row * 36u + 32u] = numbers[row * 9u + 7u];
            cells[row * 36u + 33u] = numbers[row * 9u + 8u];
            cells[row * 36u + 34u] = found[row].created_at;
            cells[row * 36u + 35u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 36u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_COUNT_ACTIVE:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_roundtable_run_count_active();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_RUN_BRANCH_OWNER:
   {
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_roundtable_run_branch_owner(field[0], field[1], parsed2);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_CREATE:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed3;
      if (parse_int(field[3], &parsed3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int scalar0 = 0;
      rc = db1_roundtable_pass_create(parsed0, field[1], field[2], parsed3, field[4], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_GET:
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
      memset(&row_rtp_pass_t, 0, sizeof row_rtp_pass_t);
      rc = db1_roundtable_pass_get(parsed0, &row_rtp_pass_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_pass_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_pass_t.pipeline_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_pass_t.pass_no);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_pass_t.converged);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_pass_t.envelope_valid);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_rtp_pass_t.blocking_count);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_rtp_pass_t.suggestion_count);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_rtp_pass_t.nit_count);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_rtp_pass_t.open_questions);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_rtp_pass_t.coverage_gaps);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_rtp_pass_t.items_round);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_rtp_pass_t.artifact_round);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_rtp_pass_t.best_round);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_rtp_pass_t.rounds_run);
      snprintf(row_text[14], sizeof row_text[14], "%.17g", (double)row_rtp_pass_t.cost_usd);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_rtp_pass_t.is_chunked);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_rtp_pass_t.chunk_total);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_rtp_pass_t.chunk_done);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_rtp_pass_t.synthesis_done);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_rtp_pass_t.chunk_group);
      snprintf(row_text[20], sizeof row_text[20], "%d", row_rtp_pass_t.chunk_index);
      snprintf(row_text[21], sizeof row_text[21], "%d", row_rtp_pass_t.answered_count);
      snprintf(row_text[22], sizeof row_text[22], "%d", row_rtp_pass_t.chunk_offset);
      snprintf(row_text[23], sizeof row_text[23], "%d", row_rtp_pass_t.chunk_len);
      snprintf(row_text[24], sizeof row_text[24], "%d", row_rtp_pass_t.chunk_omitted);
      snprintf(row_text[25], sizeof row_text[25], "%d", row_rtp_pass_t.chunk_over_budget);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_rtp_pass_t.phase;
      row_slots[3] = row_rtp_pass_t.mode;
      row_slots[4] = row_text[2];
      row_slots[5] = row_rtp_pass_t.status;
      row_slots[6] = row_rtp_pass_t.artifact_hash;
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_text[5];
      row_slots[10] = row_text[6];
      row_slots[11] = row_text[7];
      row_slots[12] = row_text[8];
      row_slots[13] = row_text[9];
      row_slots[14] = row_text[10];
      row_slots[15] = row_text[11];
      row_slots[16] = row_text[12];
      row_slots[17] = row_text[13];
      row_slots[18] = row_text[14];
      row_slots[19] = row_rtp_pass_t.result_hash;
      row_slots[20] = row_text[15];
      row_slots[21] = row_text[16];
      row_slots[22] = row_text[17];
      row_slots[23] = row_text[18];
      row_slots[24] = row_text[19];
      row_slots[25] = row_text[20];
      row_slots[26] = row_text[21];
      row_slots[27] = row_text[22];
      row_slots[28] = row_text[23];
      row_slots[29] = row_text[24];
      row_slots[30] = row_text[25];
      row_slots[31] = row_rtp_pass_t.created_at;
      row_slots[32] = row_rtp_pass_t.updated_at;
      rows = row_slots;
      row_count = 33u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_UPDATE:
   {
      if (count != 33u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rtp_pass_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      int member_1 = 0;
      if (parse_int(field[1], &member_1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.pipeline_id = member_1;
      snprintf(row.phase, sizeof row.phase, "%s", field[2]);
      snprintf(row.mode, sizeof row.mode, "%s", field[3]);
      int member_4 = 0;
      if (parse_int(field[4], &member_4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.pass_no = member_4;
      snprintf(row.status, sizeof row.status, "%s", field[5]);
      snprintf(row.artifact_hash, sizeof row.artifact_hash, "%s", field[6]);
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.converged = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.envelope_valid = member_8;
      int member_9 = 0;
      if (parse_int(field[9], &member_9) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.blocking_count = member_9;
      int member_10 = 0;
      if (parse_int(field[10], &member_10) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.suggestion_count = member_10;
      int member_11 = 0;
      if (parse_int(field[11], &member_11) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.nit_count = member_11;
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.open_questions = member_12;
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.coverage_gaps = member_13;
      int member_14 = 0;
      if (parse_int(field[14], &member_14) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.items_round = member_14;
      int member_15 = 0;
      if (parse_int(field[15], &member_15) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.artifact_round = member_15;
      int member_16 = 0;
      if (parse_int(field[16], &member_16) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.best_round = member_16;
      int member_17 = 0;
      if (parse_int(field[17], &member_17) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.rounds_run = member_17;
      double member_18 = 0;
      if (parse_double(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cost_usd = member_18;
      snprintf(row.result_hash, sizeof row.result_hash, "%s", field[19]);
      int member_20 = 0;
      if (parse_int(field[20], &member_20) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.is_chunked = member_20;
      int member_21 = 0;
      if (parse_int(field[21], &member_21) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_total = member_21;
      int member_22 = 0;
      if (parse_int(field[22], &member_22) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_done = member_22;
      int member_23 = 0;
      if (parse_int(field[23], &member_23) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.synthesis_done = member_23;
      int member_24 = 0;
      if (parse_int(field[24], &member_24) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_group = member_24;
      int member_25 = 0;
      if (parse_int(field[25], &member_25) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_index = member_25;
      int member_26 = 0;
      if (parse_int(field[26], &member_26) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.answered_count = member_26;
      int member_27 = 0;
      if (parse_int(field[27], &member_27) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_offset = member_27;
      int member_28 = 0;
      if (parse_int(field[28], &member_28) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_len = member_28;
      int member_29 = 0;
      if (parse_int(field[29], &member_29) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_omitted = member_29;
      int member_30 = 0;
      if (parse_int(field[30], &member_30) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.chunk_over_budget = member_30;
      snprintf(row.created_at, sizeof row.created_at, "%s", field[31]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[32]);
      rc = db1_roundtable_pass_update(&row);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_LATEST:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_rtp_pass_t, 0, sizeof row_rtp_pass_t);
      rc = db1_roundtable_pass_latest(parsed0, field[1], &row_rtp_pass_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_pass_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_pass_t.pipeline_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_pass_t.pass_no);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_pass_t.converged);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_pass_t.envelope_valid);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_rtp_pass_t.blocking_count);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_rtp_pass_t.suggestion_count);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_rtp_pass_t.nit_count);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_rtp_pass_t.open_questions);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_rtp_pass_t.coverage_gaps);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_rtp_pass_t.items_round);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_rtp_pass_t.artifact_round);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_rtp_pass_t.best_round);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_rtp_pass_t.rounds_run);
      snprintf(row_text[14], sizeof row_text[14], "%.17g", (double)row_rtp_pass_t.cost_usd);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_rtp_pass_t.is_chunked);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_rtp_pass_t.chunk_total);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_rtp_pass_t.chunk_done);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_rtp_pass_t.synthesis_done);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_rtp_pass_t.chunk_group);
      snprintf(row_text[20], sizeof row_text[20], "%d", row_rtp_pass_t.chunk_index);
      snprintf(row_text[21], sizeof row_text[21], "%d", row_rtp_pass_t.answered_count);
      snprintf(row_text[22], sizeof row_text[22], "%d", row_rtp_pass_t.chunk_offset);
      snprintf(row_text[23], sizeof row_text[23], "%d", row_rtp_pass_t.chunk_len);
      snprintf(row_text[24], sizeof row_text[24], "%d", row_rtp_pass_t.chunk_omitted);
      snprintf(row_text[25], sizeof row_text[25], "%d", row_rtp_pass_t.chunk_over_budget);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_rtp_pass_t.phase;
      row_slots[3] = row_rtp_pass_t.mode;
      row_slots[4] = row_text[2];
      row_slots[5] = row_rtp_pass_t.status;
      row_slots[6] = row_rtp_pass_t.artifact_hash;
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_text[5];
      row_slots[10] = row_text[6];
      row_slots[11] = row_text[7];
      row_slots[12] = row_text[8];
      row_slots[13] = row_text[9];
      row_slots[14] = row_text[10];
      row_slots[15] = row_text[11];
      row_slots[16] = row_text[12];
      row_slots[17] = row_text[13];
      row_slots[18] = row_text[14];
      row_slots[19] = row_rtp_pass_t.result_hash;
      row_slots[20] = row_text[15];
      row_slots[21] = row_text[16];
      row_slots[22] = row_text[17];
      row_slots[23] = row_text[18];
      row_slots[24] = row_text[19];
      row_slots[25] = row_text[20];
      row_slots[26] = row_text[21];
      row_slots[27] = row_text[22];
      row_slots[28] = row_text[23];
      row_slots[29] = row_text[24];
      row_slots[30] = row_text[25];
      row_slots[31] = row_rtp_pass_t.created_at;
      row_slots[32] = row_rtp_pass_t.updated_at;
      rows = row_slots;
      row_count = 33u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_MAX_NO:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_roundtable_pass_max_no(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_MAX_GROUP:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_roundtable_pass_max_group(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_PASS_GROUP_AGG:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_rtp_group_agg_t, 0, sizeof row_rtp_group_agg_t);
      rc = db1_roundtable_pass_group_agg(parsed0, field[1], parsed2, &row_rtp_group_agg_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_group_agg_t.total);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_group_agg_t.done);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_group_agg_t.invalid);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_group_agg_t.synthesis_present);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_group_agg_t.synthesis_done);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_rtp_group_agg_t.blocking_count);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_rtp_group_agg_t.suggestion_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_text[3];
      row_slots[4] = row_text[4];
      row_slots[5] = row_text[5];
      row_slots[6] = row_text[6];
      rows = row_slots;
      row_count = 7u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_CREATE:
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
      int scalar0 = 0;
      rc = db1_roundtable_attempt_create(parsed0, parsed1, field[2], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_GET_BY_RUN:
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
      memset(&row_rtp_attempt_t, 0, sizeof row_rtp_attempt_t);
      rc = db1_roundtable_attempt_get_by_run(field[0], &row_rtp_attempt_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_attempt_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_attempt_t.pass_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_attempt_t.attempt_no);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_attempt_t.is_current);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_attempt_t.envelope_valid);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_rtp_attempt_t.items_truncated);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_rtp_attempt_t.truncated);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_rtp_attempt_t.degraded);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_rtp_attempt_t.cost_capped);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_rtp_attempt_t.deadline_hit);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_rtp_attempt_t.cancelled);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_rtp_attempt_t.lost_result);
      snprintf(row_text[12], sizeof row_text[12], "%.17g", (double)row_rtp_attempt_t.cost_usd);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_rtp_attempt_t.cost_known);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_rtp_attempt_t.run_id;
      row_slots[4] = row_text[3];
      row_slots[5] = row_rtp_attempt_t.capture_status;
      row_slots[6] = row_rtp_attempt_t.terminal_status;
      row_slots[7] = row_rtp_attempt_t.parse_status;
      row_slots[8] = row_text[4];
      row_slots[9] = row_text[5];
      row_slots[10] = row_text[6];
      row_slots[11] = row_text[7];
      row_slots[12] = row_text[8];
      row_slots[13] = row_text[9];
      row_slots[14] = row_text[10];
      row_slots[15] = row_text[11];
      row_slots[16] = row_rtp_attempt_t.result_hash;
      row_slots[17] = row_rtp_attempt_t.result_snapshot;
      row_slots[18] = row_text[12];
      row_slots[19] = row_text[13];
      row_slots[20] = row_rtp_attempt_t.submitted_at;
      row_slots[21] = row_rtp_attempt_t.terminal_at;
      rows = row_slots;
      row_count = 22u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_CURRENT:
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
      memset(&row_rtp_attempt_t, 0, sizeof row_rtp_attempt_t);
      rc = db1_roundtable_attempt_current(parsed0, &row_rtp_attempt_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_attempt_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_attempt_t.pass_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_attempt_t.attempt_no);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_attempt_t.is_current);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_attempt_t.envelope_valid);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_rtp_attempt_t.items_truncated);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_rtp_attempt_t.truncated);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_rtp_attempt_t.degraded);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_rtp_attempt_t.cost_capped);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_rtp_attempt_t.deadline_hit);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_rtp_attempt_t.cancelled);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_rtp_attempt_t.lost_result);
      snprintf(row_text[12], sizeof row_text[12], "%.17g", (double)row_rtp_attempt_t.cost_usd);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_rtp_attempt_t.cost_known);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_rtp_attempt_t.run_id;
      row_slots[4] = row_text[3];
      row_slots[5] = row_rtp_attempt_t.capture_status;
      row_slots[6] = row_rtp_attempt_t.terminal_status;
      row_slots[7] = row_rtp_attempt_t.parse_status;
      row_slots[8] = row_text[4];
      row_slots[9] = row_text[5];
      row_slots[10] = row_text[6];
      row_slots[11] = row_text[7];
      row_slots[12] = row_text[8];
      row_slots[13] = row_text[9];
      row_slots[14] = row_text[10];
      row_slots[15] = row_text[11];
      row_slots[16] = row_rtp_attempt_t.result_hash;
      row_slots[17] = row_rtp_attempt_t.result_snapshot;
      row_slots[18] = row_text[12];
      row_slots[19] = row_text[13];
      row_slots[20] = row_rtp_attempt_t.submitted_at;
      row_slots[21] = row_rtp_attempt_t.terminal_at;
      rows = row_slots;
      row_count = 22u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_UPDATE:
   {
      if (count != 22u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rtp_attempt_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      int member_1 = 0;
      if (parse_int(field[1], &member_1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.pass_id = member_1;
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.attempt_no = member_2;
      snprintf(row.run_id, sizeof row.run_id, "%s", field[3]);
      int member_4 = 0;
      if (parse_int(field[4], &member_4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.is_current = member_4;
      snprintf(row.capture_status, sizeof row.capture_status, "%s", field[5]);
      snprintf(row.terminal_status, sizeof row.terminal_status, "%s", field[6]);
      snprintf(row.parse_status, sizeof row.parse_status, "%s", field[7]);
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.envelope_valid = member_8;
      int member_9 = 0;
      if (parse_int(field[9], &member_9) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.items_truncated = member_9;
      int member_10 = 0;
      if (parse_int(field[10], &member_10) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.truncated = member_10;
      int member_11 = 0;
      if (parse_int(field[11], &member_11) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.degraded = member_11;
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cost_capped = member_12;
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.deadline_hit = member_13;
      int member_14 = 0;
      if (parse_int(field[14], &member_14) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cancelled = member_14;
      int member_15 = 0;
      if (parse_int(field[15], &member_15) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.lost_result = member_15;
      snprintf(row.result_hash, sizeof row.result_hash, "%s", field[16]);
      snprintf(row.result_snapshot, sizeof row.result_snapshot, "%s", field[17]);
      double member_18 = 0;
      if (parse_double(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cost_usd = member_18;
      int member_19 = 0;
      if (parse_int(field[19], &member_19) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cost_known = member_19;
      snprintf(row.submitted_at, sizeof row.submitted_at, "%s", field[20]);
      snprintf(row.terminal_at, sizeof row.terminal_at, "%s", field[21]);
      rc = db1_roundtable_attempt_update(&row);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_MAX_NO:
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
      int produced = db1_roundtable_attempt_max_no(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_SUPERSEDE_OTHERS:
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
      rc = db1_roundtable_attempt_supersede_others(parsed0, parsed1);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_GATE_CREATE:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int scalar0 = 0;
      rc = db1_roundtable_gate_create(parsed0, parsed1, parsed2, field[3], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_GATE_GET:
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
      memset(&row_rtp_gate_t, 0, sizeof row_rtp_gate_t);
      rc = db1_roundtable_gate_get(parsed0, parsed1, &row_rtp_gate_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rtp_gate_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rtp_gate_t.pipeline_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rtp_gate_t.gate_no);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rtp_gate_t.pr_number);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_rtp_gate_t.merge_exit_code);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_rtp_gate_t.verdict;
      row_slots[4] = row_rtp_gate_t.reason;
      row_slots[5] = row_rtp_gate_t.actor;
      row_slots[6] = row_text[3];
      row_slots[7] = row_rtp_gate_t.expected_head_sha;
      row_slots[8] = row_rtp_gate_t.merge_sha;
      row_slots[9] = row_rtp_gate_t.merge_executor;
      row_slots[10] = row_rtp_gate_t.merge_command;
      row_slots[11] = row_rtp_gate_t.merge_output;
      row_slots[12] = row_text[4];
      row_slots[13] = row_rtp_gate_t.resolved_at;
      row_slots[14] = row_rtp_gate_t.created_at;
      rows = row_slots;
      row_count = 15u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_GATE_UPDATE:
   {
      if (count != 15u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rtp_gate_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      int member_1 = 0;
      if (parse_int(field[1], &member_1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.pipeline_id = member_1;
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.gate_no = member_2;
      snprintf(row.verdict, sizeof row.verdict, "%s", field[3]);
      snprintf(row.reason, sizeof row.reason, "%s", field[4]);
      snprintf(row.actor, sizeof row.actor, "%s", field[5]);
      int member_6 = 0;
      if (parse_int(field[6], &member_6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.pr_number = member_6;
      snprintf(row.expected_head_sha, sizeof row.expected_head_sha, "%s", field[7]);
      snprintf(row.merge_sha, sizeof row.merge_sha, "%s", field[8]);
      snprintf(row.merge_executor, sizeof row.merge_executor, "%s", field[9]);
      snprintf(row.merge_command, sizeof row.merge_command, "%s", field[10]);
      snprintf(row.merge_output, sizeof row.merge_output, "%s", field[11]);
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.merge_exit_code = member_12;
      snprintf(row.resolved_at, sizeof row.resolved_at, "%s", field[13]);
      snprintf(row.created_at, sizeof row.created_at, "%s", field[14]);
      rc = db1_roundtable_gate_update(&row);
      break;
   }
   case AIMEE_DB1_OP_ROUNDTABLE_GATE_AGE_EXCEEDS_HOURS:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_roundtable_gate_age_exceeds_hours(parsed0, parsed1, parsed2);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
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
