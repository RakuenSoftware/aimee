/* modules/db1/workflow_stage.c: the workflow stage handler.
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
#include "execution_plans.h"
#include "execution_trace.h"
#include "pipelines.h"
#include "roadmap_runtime.h"
#include "wfe_binding.h"

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

aimee_module_status_t aimee_db1_stage_workflow(const uint8_t *request_body, uint32_t request_len,
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
   db1_execution_trace_detail_t row_db1_execution_trace_detail_t;
   db1_pipeline_t row_db1_pipeline_t;
   rdm_dispatch_t row_rdm_dispatch_t;
   rdm_unit_dispatch_t row_rdm_unit_dispatch_t;
   plan_t row_plan_t;
   db1_step_evidence_latest_t row_db1_step_evidence_latest_t;
   const char *row_slots[549];
   char row_text[386][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;
   /* Text scalars are written by the domain and read after the switch
      closes, so their storage cannot live in a case block. One
      allocation holds all of an operation's values end to end, and one
      free returns it. */
   char *scalar_owned = NULL;

   switch (op)
   {
   case AIMEE_DB1_OP_EXECUTION_TRACE_INSERT:
   {
      if (count != 9u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_execution_trace_insert_row_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.plan_id = member_0;
      row.session_id = field[1];
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.turn = member_2;
      row.direction = field[3];
      row.content = field[4];
      row.tool_name = field[5];
      row.tool_args = field[6];
      row.tool_result = field[7];
      row.context_hash = field[8];
      rc = db1_execution_trace_insert(&row);
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_TRACE_COUNT_FOR_SESSION:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_execution_trace_count_for_session(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_TRACE_LIST_RECENT:
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
      if (parsed0 <= 0 || parsed0 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_execution_trace_recent_row_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_execution_trace_list_recent(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 5u * sizeof *cells);
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
                     "%d", found[row].id);
            snprintf(numbers[row * 2u + 1u], 32,
                     "%d", found[row].turn);
            cells[row * 5u + 0u] = numbers[row * 2u + 0u];
            cells[row * 5u + 1u] = numbers[row * 2u + 1u];
            cells[row * 5u + 2u] = found[row].direction;
            cells[row * 5u + 3u] = found[row].tool_name;
            cells[row * 5u + 4u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_TRACE_GET:
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
      memset(&row_db1_execution_trace_detail_t, 0, sizeof row_db1_execution_trace_detail_t);
      rc = db1_execution_trace_get(parsed0, &row_db1_execution_trace_detail_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_execution_trace_detail_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_execution_trace_detail_t.plan_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_execution_trace_detail_t.turn);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_db1_execution_trace_detail_t.direction;
      row_slots[4] = row_db1_execution_trace_detail_t.content;
      row_slots[5] = row_db1_execution_trace_detail_t.tool_name;
      row_slots[6] = row_db1_execution_trace_detail_t.tool_args;
      row_slots[7] = row_db1_execution_trace_detail_t.tool_result;
      row_slots[8] = row_db1_execution_trace_detail_t.context_hash;
      row_slots[9] = row_db1_execution_trace_detail_t.created_at;
      rows = row_slots;
      row_count = 10u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_TRACE_LIST_TOOL_CALLS:
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
      if (parsed0 <= 0 || parsed0 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_execution_trace_tool_call_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_execution_trace_list_tool_calls(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 5u * sizeof *cells);
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
                     "%d", found[row].turn);
            cells[row * 5u + 0u] = numbers[row * 1u + 0u];
            cells[row * 5u + 1u] = found[row].direction;
            cells[row * 5u + 2u] = found[row].tool_name;
            cells[row * 5u + 3u] = found[row].tool_args;
            cells[row * 5u + 4u] = found[row].tool_result;
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_TRACE_LIST_AFTER_ID:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
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
      if (parsed1 <= 0 || parsed1 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_execution_trace_mining_row_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_execution_trace_list_after_id(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 7u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 3u * sizeof *numbers);
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
            snprintf(numbers[row * 3u + 0u], 32,
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 3u + 1u], 32,
                     "%d", found[row].plan_id);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%d", found[row].turn);
            cells[row * 7u + 0u] = numbers[row * 3u + 0u];
            cells[row * 7u + 1u] = numbers[row * 3u + 1u];
            cells[row * 7u + 2u] = numbers[row * 3u + 2u];
            cells[row * 7u + 3u] = found[row].direction;
            cells[row * 7u + 4u] = found[row].tool_name;
            cells[row * 7u + 5u] = found[row].tool_args;
            cells[row * 7u + 6u] = found[row].tool_result;
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_BIND:
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
      int produced = db1_wfe_bind(field[0], field[1], field[2]);
      rc = 0;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WFE_BINDING_GET:
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
      scalar_owned = calloc(1u, DB1_WFE_WORK_ITEM_ID_LEN + DB1_WFE_STAGE_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      char *scalar1 = scalar_owned + DB1_WFE_WORK_ITEM_ID_LEN;
      rc = db1_wfe_binding_get(field[0], scalar0, (size_t)DB1_WFE_WORK_ITEM_ID_LEN, scalar1, (size_t)DB1_WFE_STAGE_LEN);
      row_slots[0] = scalar0;
      row_slots[1] = scalar1;
      rows = row_slots;
      row_count = 2u;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_UNBIND:
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
      rc = db1_wfe_unbind(field[0]);
      break;
   case AIMEE_DB1_OP_WFE_LEASE_RENEW:
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
      rc = db1_wfe_lease_renew(field[0], parsed1);
      break;
   }
   case AIMEE_DB1_OP_WFE_LEASE_EXPIRY_GET:
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
      scalar_owned = calloc(1u, DB1_WFE_EXPIRY_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      rc = db1_wfe_lease_expiry_get(field[0], scalar0, (size_t)DB1_WFE_EXPIRY_LEN);
      row_slots[0] = scalar0;
      rows = row_slots;
      row_count = 1u;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_WFE_LEASE_STALE_WORK_ITEMS:
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
      if (parsed0 <= 0 || parsed0 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_WFE_WORK_ITEM_ID_LEN] = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wfe_lease_stale_work_items(found, parsed0);
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
   case AIMEE_DB1_OP_WFE_LEASE_RECLAIM_STALE:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_wfe_lease_reclaim_stale();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_CREATE:
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
      int scalar0 = 0;
      rc = db1_pipeline_create(field[0], field[1], field[2], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_GET:
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
      memset(&row_db1_pipeline_t, 0, sizeof row_db1_pipeline_t);
      rc = db1_pipeline_get(parsed0, &row_db1_pipeline_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_pipeline_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_pipeline_t.phase_attempts);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_pipeline_t.plan_id);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_db1_pipeline_t.job_id);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_db1_pipeline_t.clarify_session_id);
      row_slots[0] = row_text[0];
      row_slots[1] = row_db1_pipeline_t.task;
      row_slots[2] = row_db1_pipeline_t.status;
      row_slots[3] = row_db1_pipeline_t.current_phase;
      row_slots[4] = row_db1_pipeline_t.request_classification;
      row_slots[5] = row_db1_pipeline_t.plan_depth;
      row_slots[6] = row_text[1];
      row_slots[7] = row_text[2];
      row_slots[8] = row_text[3];
      row_slots[9] = row_text[4];
      row_slots[10] = row_db1_pipeline_t.created_at;
      row_slots[11] = row_db1_pipeline_t.updated_at;
      rows = row_slots;
      row_count = 12u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_UPDATE:
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
      int parsed4;
      if (parse_int(field[4], &parsed4) != 0)
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
      int parsed8;
      if (parse_int(field[8], &parsed8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_pipeline_update(parsed0, field[1], field[2], parsed3, parsed4, parsed5, field[6], field[7], parsed8);
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_LINK_PLAN:
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
      rc = db1_pipeline_link_plan(parsed0, parsed1);
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_LINK_JOB:
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
      rc = db1_pipeline_link_job(parsed0, parsed1);
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_CANCEL:
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
      rc = db1_pipeline_cancel(parsed0);
      break;
   }
   case AIMEE_DB1_OP_PIPELINE_LIST_ACTIVE:
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
      if (parsed0 <= 0 || parsed0 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_pipeline_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_pipeline_list_active(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 12u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 5u * sizeof *numbers);
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
            snprintf(numbers[row * 5u + 0u], 32,
                     "%d", found[row].id);
            snprintf(numbers[row * 5u + 1u], 32,
                     "%d", found[row].phase_attempts);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%d", found[row].plan_id);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%d", found[row].job_id);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].clarify_session_id);
            cells[row * 12u + 0u] = numbers[row * 5u + 0u];
            cells[row * 12u + 1u] = found[row].task;
            cells[row * 12u + 2u] = found[row].status;
            cells[row * 12u + 3u] = found[row].current_phase;
            cells[row * 12u + 4u] = found[row].request_classification;
            cells[row * 12u + 5u] = found[row].plan_depth;
            cells[row * 12u + 6u] = numbers[row * 5u + 1u];
            cells[row * 12u + 7u] = numbers[row * 5u + 2u];
            cells[row * 12u + 8u] = numbers[row * 5u + 3u];
            cells[row * 12u + 9u] = numbers[row * 5u + 4u];
            cells[row * 12u + 10u] = found[row].created_at;
            cells[row * 12u + 11u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 12u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_ROADMAP_DISPATCH_UPSERT:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
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
      rc = db1_roadmap_dispatch_upsert(field[0], field[1], parsed2, parsed3);
      break;
   }
   case AIMEE_DB1_OP_ROADMAP_DISPATCH_GET:
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
      memset(&row_rdm_dispatch_t, 0, sizeof row_rdm_dispatch_t);
      rc = db1_roadmap_dispatch_get(field[0], &row_rdm_dispatch_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rdm_dispatch_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rdm_dispatch_t.require_slice_discussion);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rdm_dispatch_t.budget_ceiling_tokens);
      row_slots[0] = row_text[0];
      row_slots[1] = row_rdm_dispatch_t.roadmap_id;
      row_slots[2] = row_rdm_dispatch_t.status;
      row_slots[3] = row_rdm_dispatch_t.phase;
      row_slots[4] = row_rdm_dispatch_t.token_profile;
      row_slots[5] = row_text[1];
      row_slots[6] = row_text[2];
      row_slots[7] = row_rdm_dispatch_t.exit_reason;
      row_slots[8] = row_rdm_dispatch_t.created_at;
      row_slots[9] = row_rdm_dispatch_t.updated_at;
      rows = row_slots;
      row_count = 10u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROADMAP_DISPATCH_SET_STATUS:
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
      rc = db1_roadmap_dispatch_set_status(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_ROADMAP_DISPATCH_SET_PHASE:
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
      rc = db1_roadmap_dispatch_set_phase(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_ENSURE:
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
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_roadmap_unit_ensure(field[0], field[1], field[2], field[3]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_GET:
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
      memset(&row_rdm_unit_dispatch_t, 0, sizeof row_rdm_unit_dispatch_t);
      rc = db1_roadmap_unit_get(field[0], field[1], &row_rdm_unit_dispatch_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_rdm_unit_dispatch_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_rdm_unit_dispatch_t.verify_attempts);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_rdm_unit_dispatch_t.dispatch_attempts);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_rdm_unit_dispatch_t.coord_job_id);
      row_slots[0] = row_text[0];
      row_slots[1] = row_rdm_unit_dispatch_t.roadmap_id;
      row_slots[2] = row_rdm_unit_dispatch_t.unit_id;
      row_slots[3] = row_rdm_unit_dispatch_t.level;
      row_slots[4] = row_rdm_unit_dispatch_t.state;
      row_slots[5] = row_rdm_unit_dispatch_t.tool_policy_mode;
      row_slots[6] = row_rdm_unit_dispatch_t.claimed_by;
      row_slots[7] = row_rdm_unit_dispatch_t.claimed_at;
      row_slots[8] = row_rdm_unit_dispatch_t.heartbeat_at;
      row_slots[9] = row_text[1];
      row_slots[10] = row_text[2];
      row_slots[11] = row_rdm_unit_dispatch_t.worktree_path;
      row_slots[12] = row_text[3];
      row_slots[13] = row_rdm_unit_dispatch_t.result;
      row_slots[14] = row_rdm_unit_dispatch_t.error;
      row_slots[15] = row_rdm_unit_dispatch_t.created_at;
      row_slots[16] = row_rdm_unit_dispatch_t.updated_at;
      rows = row_slots;
      row_count = 17u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ROADMAP_UNIT_SET_STATE:
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
      rc = db1_roadmap_unit_set_state(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_CLAIM:
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
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_roadmap_unit_claim(field[0], field[1], field[2], field[3]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_HEARTBEAT:
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
      rc = db1_roadmap_unit_heartbeat(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_FINISH:
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
      rc = db1_roadmap_unit_finish(field[0], field[1], field[2], field[3], field[4]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_SET_COORD_JOB:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_roadmap_unit_set_coord_job(field[0], field[1], parsed2);
      break;
   }
   case AIMEE_DB1_OP_ROADMAP_UNIT_INCREMENT_VERIFY_ATTEMPTS:
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
      rc = db1_roadmap_unit_increment_verify_attempts(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_ROADMAP_UNIT_SELECT_NEXT:
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
      rc = db1_roadmap_unit_select_next(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_EXECUTION_PLAN_CREATE:
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
      int produced = db1_execution_plan_create(field[0], field[1], field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_GET:
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
      memset(&row_plan_t, 0, sizeof row_plan_t);
      rc = db1_execution_plan_get(parsed0, &row_plan_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_plan_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_plan_t.steps[0].id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_plan_t.steps[0].depends_on[0]);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_plan_t.steps[0].depends_on[1]);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_plan_t.steps[0].depends_on[2]);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_plan_t.steps[0].depends_on[3]);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_plan_t.steps[0].depends_on[4]);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_plan_t.steps[0].depends_on[5]);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_plan_t.steps[0].depends_on[6]);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_plan_t.steps[0].depends_on[7]);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_plan_t.steps[0].dep_count);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_plan_t.steps[0].status);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_plan_t.steps[0].wave);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_plan_t.steps[1].id);
      snprintf(row_text[14], sizeof row_text[14], "%d", row_plan_t.steps[1].depends_on[0]);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_plan_t.steps[1].depends_on[1]);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_plan_t.steps[1].depends_on[2]);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_plan_t.steps[1].depends_on[3]);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_plan_t.steps[1].depends_on[4]);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_plan_t.steps[1].depends_on[5]);
      snprintf(row_text[20], sizeof row_text[20], "%d", row_plan_t.steps[1].depends_on[6]);
      snprintf(row_text[21], sizeof row_text[21], "%d", row_plan_t.steps[1].depends_on[7]);
      snprintf(row_text[22], sizeof row_text[22], "%d", row_plan_t.steps[1].dep_count);
      snprintf(row_text[23], sizeof row_text[23], "%d", row_plan_t.steps[1].status);
      snprintf(row_text[24], sizeof row_text[24], "%d", row_plan_t.steps[1].wave);
      snprintf(row_text[25], sizeof row_text[25], "%d", row_plan_t.steps[2].id);
      snprintf(row_text[26], sizeof row_text[26], "%d", row_plan_t.steps[2].depends_on[0]);
      snprintf(row_text[27], sizeof row_text[27], "%d", row_plan_t.steps[2].depends_on[1]);
      snprintf(row_text[28], sizeof row_text[28], "%d", row_plan_t.steps[2].depends_on[2]);
      snprintf(row_text[29], sizeof row_text[29], "%d", row_plan_t.steps[2].depends_on[3]);
      snprintf(row_text[30], sizeof row_text[30], "%d", row_plan_t.steps[2].depends_on[4]);
      snprintf(row_text[31], sizeof row_text[31], "%d", row_plan_t.steps[2].depends_on[5]);
      snprintf(row_text[32], sizeof row_text[32], "%d", row_plan_t.steps[2].depends_on[6]);
      snprintf(row_text[33], sizeof row_text[33], "%d", row_plan_t.steps[2].depends_on[7]);
      snprintf(row_text[34], sizeof row_text[34], "%d", row_plan_t.steps[2].dep_count);
      snprintf(row_text[35], sizeof row_text[35], "%d", row_plan_t.steps[2].status);
      snprintf(row_text[36], sizeof row_text[36], "%d", row_plan_t.steps[2].wave);
      snprintf(row_text[37], sizeof row_text[37], "%d", row_plan_t.steps[3].id);
      snprintf(row_text[38], sizeof row_text[38], "%d", row_plan_t.steps[3].depends_on[0]);
      snprintf(row_text[39], sizeof row_text[39], "%d", row_plan_t.steps[3].depends_on[1]);
      snprintf(row_text[40], sizeof row_text[40], "%d", row_plan_t.steps[3].depends_on[2]);
      snprintf(row_text[41], sizeof row_text[41], "%d", row_plan_t.steps[3].depends_on[3]);
      snprintf(row_text[42], sizeof row_text[42], "%d", row_plan_t.steps[3].depends_on[4]);
      snprintf(row_text[43], sizeof row_text[43], "%d", row_plan_t.steps[3].depends_on[5]);
      snprintf(row_text[44], sizeof row_text[44], "%d", row_plan_t.steps[3].depends_on[6]);
      snprintf(row_text[45], sizeof row_text[45], "%d", row_plan_t.steps[3].depends_on[7]);
      snprintf(row_text[46], sizeof row_text[46], "%d", row_plan_t.steps[3].dep_count);
      snprintf(row_text[47], sizeof row_text[47], "%d", row_plan_t.steps[3].status);
      snprintf(row_text[48], sizeof row_text[48], "%d", row_plan_t.steps[3].wave);
      snprintf(row_text[49], sizeof row_text[49], "%d", row_plan_t.steps[4].id);
      snprintf(row_text[50], sizeof row_text[50], "%d", row_plan_t.steps[4].depends_on[0]);
      snprintf(row_text[51], sizeof row_text[51], "%d", row_plan_t.steps[4].depends_on[1]);
      snprintf(row_text[52], sizeof row_text[52], "%d", row_plan_t.steps[4].depends_on[2]);
      snprintf(row_text[53], sizeof row_text[53], "%d", row_plan_t.steps[4].depends_on[3]);
      snprintf(row_text[54], sizeof row_text[54], "%d", row_plan_t.steps[4].depends_on[4]);
      snprintf(row_text[55], sizeof row_text[55], "%d", row_plan_t.steps[4].depends_on[5]);
      snprintf(row_text[56], sizeof row_text[56], "%d", row_plan_t.steps[4].depends_on[6]);
      snprintf(row_text[57], sizeof row_text[57], "%d", row_plan_t.steps[4].depends_on[7]);
      snprintf(row_text[58], sizeof row_text[58], "%d", row_plan_t.steps[4].dep_count);
      snprintf(row_text[59], sizeof row_text[59], "%d", row_plan_t.steps[4].status);
      snprintf(row_text[60], sizeof row_text[60], "%d", row_plan_t.steps[4].wave);
      snprintf(row_text[61], sizeof row_text[61], "%d", row_plan_t.steps[5].id);
      snprintf(row_text[62], sizeof row_text[62], "%d", row_plan_t.steps[5].depends_on[0]);
      snprintf(row_text[63], sizeof row_text[63], "%d", row_plan_t.steps[5].depends_on[1]);
      snprintf(row_text[64], sizeof row_text[64], "%d", row_plan_t.steps[5].depends_on[2]);
      snprintf(row_text[65], sizeof row_text[65], "%d", row_plan_t.steps[5].depends_on[3]);
      snprintf(row_text[66], sizeof row_text[66], "%d", row_plan_t.steps[5].depends_on[4]);
      snprintf(row_text[67], sizeof row_text[67], "%d", row_plan_t.steps[5].depends_on[5]);
      snprintf(row_text[68], sizeof row_text[68], "%d", row_plan_t.steps[5].depends_on[6]);
      snprintf(row_text[69], sizeof row_text[69], "%d", row_plan_t.steps[5].depends_on[7]);
      snprintf(row_text[70], sizeof row_text[70], "%d", row_plan_t.steps[5].dep_count);
      snprintf(row_text[71], sizeof row_text[71], "%d", row_plan_t.steps[5].status);
      snprintf(row_text[72], sizeof row_text[72], "%d", row_plan_t.steps[5].wave);
      snprintf(row_text[73], sizeof row_text[73], "%d", row_plan_t.steps[6].id);
      snprintf(row_text[74], sizeof row_text[74], "%d", row_plan_t.steps[6].depends_on[0]);
      snprintf(row_text[75], sizeof row_text[75], "%d", row_plan_t.steps[6].depends_on[1]);
      snprintf(row_text[76], sizeof row_text[76], "%d", row_plan_t.steps[6].depends_on[2]);
      snprintf(row_text[77], sizeof row_text[77], "%d", row_plan_t.steps[6].depends_on[3]);
      snprintf(row_text[78], sizeof row_text[78], "%d", row_plan_t.steps[6].depends_on[4]);
      snprintf(row_text[79], sizeof row_text[79], "%d", row_plan_t.steps[6].depends_on[5]);
      snprintf(row_text[80], sizeof row_text[80], "%d", row_plan_t.steps[6].depends_on[6]);
      snprintf(row_text[81], sizeof row_text[81], "%d", row_plan_t.steps[6].depends_on[7]);
      snprintf(row_text[82], sizeof row_text[82], "%d", row_plan_t.steps[6].dep_count);
      snprintf(row_text[83], sizeof row_text[83], "%d", row_plan_t.steps[6].status);
      snprintf(row_text[84], sizeof row_text[84], "%d", row_plan_t.steps[6].wave);
      snprintf(row_text[85], sizeof row_text[85], "%d", row_plan_t.steps[7].id);
      snprintf(row_text[86], sizeof row_text[86], "%d", row_plan_t.steps[7].depends_on[0]);
      snprintf(row_text[87], sizeof row_text[87], "%d", row_plan_t.steps[7].depends_on[1]);
      snprintf(row_text[88], sizeof row_text[88], "%d", row_plan_t.steps[7].depends_on[2]);
      snprintf(row_text[89], sizeof row_text[89], "%d", row_plan_t.steps[7].depends_on[3]);
      snprintf(row_text[90], sizeof row_text[90], "%d", row_plan_t.steps[7].depends_on[4]);
      snprintf(row_text[91], sizeof row_text[91], "%d", row_plan_t.steps[7].depends_on[5]);
      snprintf(row_text[92], sizeof row_text[92], "%d", row_plan_t.steps[7].depends_on[6]);
      snprintf(row_text[93], sizeof row_text[93], "%d", row_plan_t.steps[7].depends_on[7]);
      snprintf(row_text[94], sizeof row_text[94], "%d", row_plan_t.steps[7].dep_count);
      snprintf(row_text[95], sizeof row_text[95], "%d", row_plan_t.steps[7].status);
      snprintf(row_text[96], sizeof row_text[96], "%d", row_plan_t.steps[7].wave);
      snprintf(row_text[97], sizeof row_text[97], "%d", row_plan_t.steps[8].id);
      snprintf(row_text[98], sizeof row_text[98], "%d", row_plan_t.steps[8].depends_on[0]);
      snprintf(row_text[99], sizeof row_text[99], "%d", row_plan_t.steps[8].depends_on[1]);
      snprintf(row_text[100], sizeof row_text[100], "%d", row_plan_t.steps[8].depends_on[2]);
      snprintf(row_text[101], sizeof row_text[101], "%d", row_plan_t.steps[8].depends_on[3]);
      snprintf(row_text[102], sizeof row_text[102], "%d", row_plan_t.steps[8].depends_on[4]);
      snprintf(row_text[103], sizeof row_text[103], "%d", row_plan_t.steps[8].depends_on[5]);
      snprintf(row_text[104], sizeof row_text[104], "%d", row_plan_t.steps[8].depends_on[6]);
      snprintf(row_text[105], sizeof row_text[105], "%d", row_plan_t.steps[8].depends_on[7]);
      snprintf(row_text[106], sizeof row_text[106], "%d", row_plan_t.steps[8].dep_count);
      snprintf(row_text[107], sizeof row_text[107], "%d", row_plan_t.steps[8].status);
      snprintf(row_text[108], sizeof row_text[108], "%d", row_plan_t.steps[8].wave);
      snprintf(row_text[109], sizeof row_text[109], "%d", row_plan_t.steps[9].id);
      snprintf(row_text[110], sizeof row_text[110], "%d", row_plan_t.steps[9].depends_on[0]);
      snprintf(row_text[111], sizeof row_text[111], "%d", row_plan_t.steps[9].depends_on[1]);
      snprintf(row_text[112], sizeof row_text[112], "%d", row_plan_t.steps[9].depends_on[2]);
      snprintf(row_text[113], sizeof row_text[113], "%d", row_plan_t.steps[9].depends_on[3]);
      snprintf(row_text[114], sizeof row_text[114], "%d", row_plan_t.steps[9].depends_on[4]);
      snprintf(row_text[115], sizeof row_text[115], "%d", row_plan_t.steps[9].depends_on[5]);
      snprintf(row_text[116], sizeof row_text[116], "%d", row_plan_t.steps[9].depends_on[6]);
      snprintf(row_text[117], sizeof row_text[117], "%d", row_plan_t.steps[9].depends_on[7]);
      snprintf(row_text[118], sizeof row_text[118], "%d", row_plan_t.steps[9].dep_count);
      snprintf(row_text[119], sizeof row_text[119], "%d", row_plan_t.steps[9].status);
      snprintf(row_text[120], sizeof row_text[120], "%d", row_plan_t.steps[9].wave);
      snprintf(row_text[121], sizeof row_text[121], "%d", row_plan_t.steps[10].id);
      snprintf(row_text[122], sizeof row_text[122], "%d", row_plan_t.steps[10].depends_on[0]);
      snprintf(row_text[123], sizeof row_text[123], "%d", row_plan_t.steps[10].depends_on[1]);
      snprintf(row_text[124], sizeof row_text[124], "%d", row_plan_t.steps[10].depends_on[2]);
      snprintf(row_text[125], sizeof row_text[125], "%d", row_plan_t.steps[10].depends_on[3]);
      snprintf(row_text[126], sizeof row_text[126], "%d", row_plan_t.steps[10].depends_on[4]);
      snprintf(row_text[127], sizeof row_text[127], "%d", row_plan_t.steps[10].depends_on[5]);
      snprintf(row_text[128], sizeof row_text[128], "%d", row_plan_t.steps[10].depends_on[6]);
      snprintf(row_text[129], sizeof row_text[129], "%d", row_plan_t.steps[10].depends_on[7]);
      snprintf(row_text[130], sizeof row_text[130], "%d", row_plan_t.steps[10].dep_count);
      snprintf(row_text[131], sizeof row_text[131], "%d", row_plan_t.steps[10].status);
      snprintf(row_text[132], sizeof row_text[132], "%d", row_plan_t.steps[10].wave);
      snprintf(row_text[133], sizeof row_text[133], "%d", row_plan_t.steps[11].id);
      snprintf(row_text[134], sizeof row_text[134], "%d", row_plan_t.steps[11].depends_on[0]);
      snprintf(row_text[135], sizeof row_text[135], "%d", row_plan_t.steps[11].depends_on[1]);
      snprintf(row_text[136], sizeof row_text[136], "%d", row_plan_t.steps[11].depends_on[2]);
      snprintf(row_text[137], sizeof row_text[137], "%d", row_plan_t.steps[11].depends_on[3]);
      snprintf(row_text[138], sizeof row_text[138], "%d", row_plan_t.steps[11].depends_on[4]);
      snprintf(row_text[139], sizeof row_text[139], "%d", row_plan_t.steps[11].depends_on[5]);
      snprintf(row_text[140], sizeof row_text[140], "%d", row_plan_t.steps[11].depends_on[6]);
      snprintf(row_text[141], sizeof row_text[141], "%d", row_plan_t.steps[11].depends_on[7]);
      snprintf(row_text[142], sizeof row_text[142], "%d", row_plan_t.steps[11].dep_count);
      snprintf(row_text[143], sizeof row_text[143], "%d", row_plan_t.steps[11].status);
      snprintf(row_text[144], sizeof row_text[144], "%d", row_plan_t.steps[11].wave);
      snprintf(row_text[145], sizeof row_text[145], "%d", row_plan_t.steps[12].id);
      snprintf(row_text[146], sizeof row_text[146], "%d", row_plan_t.steps[12].depends_on[0]);
      snprintf(row_text[147], sizeof row_text[147], "%d", row_plan_t.steps[12].depends_on[1]);
      snprintf(row_text[148], sizeof row_text[148], "%d", row_plan_t.steps[12].depends_on[2]);
      snprintf(row_text[149], sizeof row_text[149], "%d", row_plan_t.steps[12].depends_on[3]);
      snprintf(row_text[150], sizeof row_text[150], "%d", row_plan_t.steps[12].depends_on[4]);
      snprintf(row_text[151], sizeof row_text[151], "%d", row_plan_t.steps[12].depends_on[5]);
      snprintf(row_text[152], sizeof row_text[152], "%d", row_plan_t.steps[12].depends_on[6]);
      snprintf(row_text[153], sizeof row_text[153], "%d", row_plan_t.steps[12].depends_on[7]);
      snprintf(row_text[154], sizeof row_text[154], "%d", row_plan_t.steps[12].dep_count);
      snprintf(row_text[155], sizeof row_text[155], "%d", row_plan_t.steps[12].status);
      snprintf(row_text[156], sizeof row_text[156], "%d", row_plan_t.steps[12].wave);
      snprintf(row_text[157], sizeof row_text[157], "%d", row_plan_t.steps[13].id);
      snprintf(row_text[158], sizeof row_text[158], "%d", row_plan_t.steps[13].depends_on[0]);
      snprintf(row_text[159], sizeof row_text[159], "%d", row_plan_t.steps[13].depends_on[1]);
      snprintf(row_text[160], sizeof row_text[160], "%d", row_plan_t.steps[13].depends_on[2]);
      snprintf(row_text[161], sizeof row_text[161], "%d", row_plan_t.steps[13].depends_on[3]);
      snprintf(row_text[162], sizeof row_text[162], "%d", row_plan_t.steps[13].depends_on[4]);
      snprintf(row_text[163], sizeof row_text[163], "%d", row_plan_t.steps[13].depends_on[5]);
      snprintf(row_text[164], sizeof row_text[164], "%d", row_plan_t.steps[13].depends_on[6]);
      snprintf(row_text[165], sizeof row_text[165], "%d", row_plan_t.steps[13].depends_on[7]);
      snprintf(row_text[166], sizeof row_text[166], "%d", row_plan_t.steps[13].dep_count);
      snprintf(row_text[167], sizeof row_text[167], "%d", row_plan_t.steps[13].status);
      snprintf(row_text[168], sizeof row_text[168], "%d", row_plan_t.steps[13].wave);
      snprintf(row_text[169], sizeof row_text[169], "%d", row_plan_t.steps[14].id);
      snprintf(row_text[170], sizeof row_text[170], "%d", row_plan_t.steps[14].depends_on[0]);
      snprintf(row_text[171], sizeof row_text[171], "%d", row_plan_t.steps[14].depends_on[1]);
      snprintf(row_text[172], sizeof row_text[172], "%d", row_plan_t.steps[14].depends_on[2]);
      snprintf(row_text[173], sizeof row_text[173], "%d", row_plan_t.steps[14].depends_on[3]);
      snprintf(row_text[174], sizeof row_text[174], "%d", row_plan_t.steps[14].depends_on[4]);
      snprintf(row_text[175], sizeof row_text[175], "%d", row_plan_t.steps[14].depends_on[5]);
      snprintf(row_text[176], sizeof row_text[176], "%d", row_plan_t.steps[14].depends_on[6]);
      snprintf(row_text[177], sizeof row_text[177], "%d", row_plan_t.steps[14].depends_on[7]);
      snprintf(row_text[178], sizeof row_text[178], "%d", row_plan_t.steps[14].dep_count);
      snprintf(row_text[179], sizeof row_text[179], "%d", row_plan_t.steps[14].status);
      snprintf(row_text[180], sizeof row_text[180], "%d", row_plan_t.steps[14].wave);
      snprintf(row_text[181], sizeof row_text[181], "%d", row_plan_t.steps[15].id);
      snprintf(row_text[182], sizeof row_text[182], "%d", row_plan_t.steps[15].depends_on[0]);
      snprintf(row_text[183], sizeof row_text[183], "%d", row_plan_t.steps[15].depends_on[1]);
      snprintf(row_text[184], sizeof row_text[184], "%d", row_plan_t.steps[15].depends_on[2]);
      snprintf(row_text[185], sizeof row_text[185], "%d", row_plan_t.steps[15].depends_on[3]);
      snprintf(row_text[186], sizeof row_text[186], "%d", row_plan_t.steps[15].depends_on[4]);
      snprintf(row_text[187], sizeof row_text[187], "%d", row_plan_t.steps[15].depends_on[5]);
      snprintf(row_text[188], sizeof row_text[188], "%d", row_plan_t.steps[15].depends_on[6]);
      snprintf(row_text[189], sizeof row_text[189], "%d", row_plan_t.steps[15].depends_on[7]);
      snprintf(row_text[190], sizeof row_text[190], "%d", row_plan_t.steps[15].dep_count);
      snprintf(row_text[191], sizeof row_text[191], "%d", row_plan_t.steps[15].status);
      snprintf(row_text[192], sizeof row_text[192], "%d", row_plan_t.steps[15].wave);
      snprintf(row_text[193], sizeof row_text[193], "%d", row_plan_t.steps[16].id);
      snprintf(row_text[194], sizeof row_text[194], "%d", row_plan_t.steps[16].depends_on[0]);
      snprintf(row_text[195], sizeof row_text[195], "%d", row_plan_t.steps[16].depends_on[1]);
      snprintf(row_text[196], sizeof row_text[196], "%d", row_plan_t.steps[16].depends_on[2]);
      snprintf(row_text[197], sizeof row_text[197], "%d", row_plan_t.steps[16].depends_on[3]);
      snprintf(row_text[198], sizeof row_text[198], "%d", row_plan_t.steps[16].depends_on[4]);
      snprintf(row_text[199], sizeof row_text[199], "%d", row_plan_t.steps[16].depends_on[5]);
      snprintf(row_text[200], sizeof row_text[200], "%d", row_plan_t.steps[16].depends_on[6]);
      snprintf(row_text[201], sizeof row_text[201], "%d", row_plan_t.steps[16].depends_on[7]);
      snprintf(row_text[202], sizeof row_text[202], "%d", row_plan_t.steps[16].dep_count);
      snprintf(row_text[203], sizeof row_text[203], "%d", row_plan_t.steps[16].status);
      snprintf(row_text[204], sizeof row_text[204], "%d", row_plan_t.steps[16].wave);
      snprintf(row_text[205], sizeof row_text[205], "%d", row_plan_t.steps[17].id);
      snprintf(row_text[206], sizeof row_text[206], "%d", row_plan_t.steps[17].depends_on[0]);
      snprintf(row_text[207], sizeof row_text[207], "%d", row_plan_t.steps[17].depends_on[1]);
      snprintf(row_text[208], sizeof row_text[208], "%d", row_plan_t.steps[17].depends_on[2]);
      snprintf(row_text[209], sizeof row_text[209], "%d", row_plan_t.steps[17].depends_on[3]);
      snprintf(row_text[210], sizeof row_text[210], "%d", row_plan_t.steps[17].depends_on[4]);
      snprintf(row_text[211], sizeof row_text[211], "%d", row_plan_t.steps[17].depends_on[5]);
      snprintf(row_text[212], sizeof row_text[212], "%d", row_plan_t.steps[17].depends_on[6]);
      snprintf(row_text[213], sizeof row_text[213], "%d", row_plan_t.steps[17].depends_on[7]);
      snprintf(row_text[214], sizeof row_text[214], "%d", row_plan_t.steps[17].dep_count);
      snprintf(row_text[215], sizeof row_text[215], "%d", row_plan_t.steps[17].status);
      snprintf(row_text[216], sizeof row_text[216], "%d", row_plan_t.steps[17].wave);
      snprintf(row_text[217], sizeof row_text[217], "%d", row_plan_t.steps[18].id);
      snprintf(row_text[218], sizeof row_text[218], "%d", row_plan_t.steps[18].depends_on[0]);
      snprintf(row_text[219], sizeof row_text[219], "%d", row_plan_t.steps[18].depends_on[1]);
      snprintf(row_text[220], sizeof row_text[220], "%d", row_plan_t.steps[18].depends_on[2]);
      snprintf(row_text[221], sizeof row_text[221], "%d", row_plan_t.steps[18].depends_on[3]);
      snprintf(row_text[222], sizeof row_text[222], "%d", row_plan_t.steps[18].depends_on[4]);
      snprintf(row_text[223], sizeof row_text[223], "%d", row_plan_t.steps[18].depends_on[5]);
      snprintf(row_text[224], sizeof row_text[224], "%d", row_plan_t.steps[18].depends_on[6]);
      snprintf(row_text[225], sizeof row_text[225], "%d", row_plan_t.steps[18].depends_on[7]);
      snprintf(row_text[226], sizeof row_text[226], "%d", row_plan_t.steps[18].dep_count);
      snprintf(row_text[227], sizeof row_text[227], "%d", row_plan_t.steps[18].status);
      snprintf(row_text[228], sizeof row_text[228], "%d", row_plan_t.steps[18].wave);
      snprintf(row_text[229], sizeof row_text[229], "%d", row_plan_t.steps[19].id);
      snprintf(row_text[230], sizeof row_text[230], "%d", row_plan_t.steps[19].depends_on[0]);
      snprintf(row_text[231], sizeof row_text[231], "%d", row_plan_t.steps[19].depends_on[1]);
      snprintf(row_text[232], sizeof row_text[232], "%d", row_plan_t.steps[19].depends_on[2]);
      snprintf(row_text[233], sizeof row_text[233], "%d", row_plan_t.steps[19].depends_on[3]);
      snprintf(row_text[234], sizeof row_text[234], "%d", row_plan_t.steps[19].depends_on[4]);
      snprintf(row_text[235], sizeof row_text[235], "%d", row_plan_t.steps[19].depends_on[5]);
      snprintf(row_text[236], sizeof row_text[236], "%d", row_plan_t.steps[19].depends_on[6]);
      snprintf(row_text[237], sizeof row_text[237], "%d", row_plan_t.steps[19].depends_on[7]);
      snprintf(row_text[238], sizeof row_text[238], "%d", row_plan_t.steps[19].dep_count);
      snprintf(row_text[239], sizeof row_text[239], "%d", row_plan_t.steps[19].status);
      snprintf(row_text[240], sizeof row_text[240], "%d", row_plan_t.steps[19].wave);
      snprintf(row_text[241], sizeof row_text[241], "%d", row_plan_t.steps[20].id);
      snprintf(row_text[242], sizeof row_text[242], "%d", row_plan_t.steps[20].depends_on[0]);
      snprintf(row_text[243], sizeof row_text[243], "%d", row_plan_t.steps[20].depends_on[1]);
      snprintf(row_text[244], sizeof row_text[244], "%d", row_plan_t.steps[20].depends_on[2]);
      snprintf(row_text[245], sizeof row_text[245], "%d", row_plan_t.steps[20].depends_on[3]);
      snprintf(row_text[246], sizeof row_text[246], "%d", row_plan_t.steps[20].depends_on[4]);
      snprintf(row_text[247], sizeof row_text[247], "%d", row_plan_t.steps[20].depends_on[5]);
      snprintf(row_text[248], sizeof row_text[248], "%d", row_plan_t.steps[20].depends_on[6]);
      snprintf(row_text[249], sizeof row_text[249], "%d", row_plan_t.steps[20].depends_on[7]);
      snprintf(row_text[250], sizeof row_text[250], "%d", row_plan_t.steps[20].dep_count);
      snprintf(row_text[251], sizeof row_text[251], "%d", row_plan_t.steps[20].status);
      snprintf(row_text[252], sizeof row_text[252], "%d", row_plan_t.steps[20].wave);
      snprintf(row_text[253], sizeof row_text[253], "%d", row_plan_t.steps[21].id);
      snprintf(row_text[254], sizeof row_text[254], "%d", row_plan_t.steps[21].depends_on[0]);
      snprintf(row_text[255], sizeof row_text[255], "%d", row_plan_t.steps[21].depends_on[1]);
      snprintf(row_text[256], sizeof row_text[256], "%d", row_plan_t.steps[21].depends_on[2]);
      snprintf(row_text[257], sizeof row_text[257], "%d", row_plan_t.steps[21].depends_on[3]);
      snprintf(row_text[258], sizeof row_text[258], "%d", row_plan_t.steps[21].depends_on[4]);
      snprintf(row_text[259], sizeof row_text[259], "%d", row_plan_t.steps[21].depends_on[5]);
      snprintf(row_text[260], sizeof row_text[260], "%d", row_plan_t.steps[21].depends_on[6]);
      snprintf(row_text[261], sizeof row_text[261], "%d", row_plan_t.steps[21].depends_on[7]);
      snprintf(row_text[262], sizeof row_text[262], "%d", row_plan_t.steps[21].dep_count);
      snprintf(row_text[263], sizeof row_text[263], "%d", row_plan_t.steps[21].status);
      snprintf(row_text[264], sizeof row_text[264], "%d", row_plan_t.steps[21].wave);
      snprintf(row_text[265], sizeof row_text[265], "%d", row_plan_t.steps[22].id);
      snprintf(row_text[266], sizeof row_text[266], "%d", row_plan_t.steps[22].depends_on[0]);
      snprintf(row_text[267], sizeof row_text[267], "%d", row_plan_t.steps[22].depends_on[1]);
      snprintf(row_text[268], sizeof row_text[268], "%d", row_plan_t.steps[22].depends_on[2]);
      snprintf(row_text[269], sizeof row_text[269], "%d", row_plan_t.steps[22].depends_on[3]);
      snprintf(row_text[270], sizeof row_text[270], "%d", row_plan_t.steps[22].depends_on[4]);
      snprintf(row_text[271], sizeof row_text[271], "%d", row_plan_t.steps[22].depends_on[5]);
      snprintf(row_text[272], sizeof row_text[272], "%d", row_plan_t.steps[22].depends_on[6]);
      snprintf(row_text[273], sizeof row_text[273], "%d", row_plan_t.steps[22].depends_on[7]);
      snprintf(row_text[274], sizeof row_text[274], "%d", row_plan_t.steps[22].dep_count);
      snprintf(row_text[275], sizeof row_text[275], "%d", row_plan_t.steps[22].status);
      snprintf(row_text[276], sizeof row_text[276], "%d", row_plan_t.steps[22].wave);
      snprintf(row_text[277], sizeof row_text[277], "%d", row_plan_t.steps[23].id);
      snprintf(row_text[278], sizeof row_text[278], "%d", row_plan_t.steps[23].depends_on[0]);
      snprintf(row_text[279], sizeof row_text[279], "%d", row_plan_t.steps[23].depends_on[1]);
      snprintf(row_text[280], sizeof row_text[280], "%d", row_plan_t.steps[23].depends_on[2]);
      snprintf(row_text[281], sizeof row_text[281], "%d", row_plan_t.steps[23].depends_on[3]);
      snprintf(row_text[282], sizeof row_text[282], "%d", row_plan_t.steps[23].depends_on[4]);
      snprintf(row_text[283], sizeof row_text[283], "%d", row_plan_t.steps[23].depends_on[5]);
      snprintf(row_text[284], sizeof row_text[284], "%d", row_plan_t.steps[23].depends_on[6]);
      snprintf(row_text[285], sizeof row_text[285], "%d", row_plan_t.steps[23].depends_on[7]);
      snprintf(row_text[286], sizeof row_text[286], "%d", row_plan_t.steps[23].dep_count);
      snprintf(row_text[287], sizeof row_text[287], "%d", row_plan_t.steps[23].status);
      snprintf(row_text[288], sizeof row_text[288], "%d", row_plan_t.steps[23].wave);
      snprintf(row_text[289], sizeof row_text[289], "%d", row_plan_t.steps[24].id);
      snprintf(row_text[290], sizeof row_text[290], "%d", row_plan_t.steps[24].depends_on[0]);
      snprintf(row_text[291], sizeof row_text[291], "%d", row_plan_t.steps[24].depends_on[1]);
      snprintf(row_text[292], sizeof row_text[292], "%d", row_plan_t.steps[24].depends_on[2]);
      snprintf(row_text[293], sizeof row_text[293], "%d", row_plan_t.steps[24].depends_on[3]);
      snprintf(row_text[294], sizeof row_text[294], "%d", row_plan_t.steps[24].depends_on[4]);
      snprintf(row_text[295], sizeof row_text[295], "%d", row_plan_t.steps[24].depends_on[5]);
      snprintf(row_text[296], sizeof row_text[296], "%d", row_plan_t.steps[24].depends_on[6]);
      snprintf(row_text[297], sizeof row_text[297], "%d", row_plan_t.steps[24].depends_on[7]);
      snprintf(row_text[298], sizeof row_text[298], "%d", row_plan_t.steps[24].dep_count);
      snprintf(row_text[299], sizeof row_text[299], "%d", row_plan_t.steps[24].status);
      snprintf(row_text[300], sizeof row_text[300], "%d", row_plan_t.steps[24].wave);
      snprintf(row_text[301], sizeof row_text[301], "%d", row_plan_t.steps[25].id);
      snprintf(row_text[302], sizeof row_text[302], "%d", row_plan_t.steps[25].depends_on[0]);
      snprintf(row_text[303], sizeof row_text[303], "%d", row_plan_t.steps[25].depends_on[1]);
      snprintf(row_text[304], sizeof row_text[304], "%d", row_plan_t.steps[25].depends_on[2]);
      snprintf(row_text[305], sizeof row_text[305], "%d", row_plan_t.steps[25].depends_on[3]);
      snprintf(row_text[306], sizeof row_text[306], "%d", row_plan_t.steps[25].depends_on[4]);
      snprintf(row_text[307], sizeof row_text[307], "%d", row_plan_t.steps[25].depends_on[5]);
      snprintf(row_text[308], sizeof row_text[308], "%d", row_plan_t.steps[25].depends_on[6]);
      snprintf(row_text[309], sizeof row_text[309], "%d", row_plan_t.steps[25].depends_on[7]);
      snprintf(row_text[310], sizeof row_text[310], "%d", row_plan_t.steps[25].dep_count);
      snprintf(row_text[311], sizeof row_text[311], "%d", row_plan_t.steps[25].status);
      snprintf(row_text[312], sizeof row_text[312], "%d", row_plan_t.steps[25].wave);
      snprintf(row_text[313], sizeof row_text[313], "%d", row_plan_t.steps[26].id);
      snprintf(row_text[314], sizeof row_text[314], "%d", row_plan_t.steps[26].depends_on[0]);
      snprintf(row_text[315], sizeof row_text[315], "%d", row_plan_t.steps[26].depends_on[1]);
      snprintf(row_text[316], sizeof row_text[316], "%d", row_plan_t.steps[26].depends_on[2]);
      snprintf(row_text[317], sizeof row_text[317], "%d", row_plan_t.steps[26].depends_on[3]);
      snprintf(row_text[318], sizeof row_text[318], "%d", row_plan_t.steps[26].depends_on[4]);
      snprintf(row_text[319], sizeof row_text[319], "%d", row_plan_t.steps[26].depends_on[5]);
      snprintf(row_text[320], sizeof row_text[320], "%d", row_plan_t.steps[26].depends_on[6]);
      snprintf(row_text[321], sizeof row_text[321], "%d", row_plan_t.steps[26].depends_on[7]);
      snprintf(row_text[322], sizeof row_text[322], "%d", row_plan_t.steps[26].dep_count);
      snprintf(row_text[323], sizeof row_text[323], "%d", row_plan_t.steps[26].status);
      snprintf(row_text[324], sizeof row_text[324], "%d", row_plan_t.steps[26].wave);
      snprintf(row_text[325], sizeof row_text[325], "%d", row_plan_t.steps[27].id);
      snprintf(row_text[326], sizeof row_text[326], "%d", row_plan_t.steps[27].depends_on[0]);
      snprintf(row_text[327], sizeof row_text[327], "%d", row_plan_t.steps[27].depends_on[1]);
      snprintf(row_text[328], sizeof row_text[328], "%d", row_plan_t.steps[27].depends_on[2]);
      snprintf(row_text[329], sizeof row_text[329], "%d", row_plan_t.steps[27].depends_on[3]);
      snprintf(row_text[330], sizeof row_text[330], "%d", row_plan_t.steps[27].depends_on[4]);
      snprintf(row_text[331], sizeof row_text[331], "%d", row_plan_t.steps[27].depends_on[5]);
      snprintf(row_text[332], sizeof row_text[332], "%d", row_plan_t.steps[27].depends_on[6]);
      snprintf(row_text[333], sizeof row_text[333], "%d", row_plan_t.steps[27].depends_on[7]);
      snprintf(row_text[334], sizeof row_text[334], "%d", row_plan_t.steps[27].dep_count);
      snprintf(row_text[335], sizeof row_text[335], "%d", row_plan_t.steps[27].status);
      snprintf(row_text[336], sizeof row_text[336], "%d", row_plan_t.steps[27].wave);
      snprintf(row_text[337], sizeof row_text[337], "%d", row_plan_t.steps[28].id);
      snprintf(row_text[338], sizeof row_text[338], "%d", row_plan_t.steps[28].depends_on[0]);
      snprintf(row_text[339], sizeof row_text[339], "%d", row_plan_t.steps[28].depends_on[1]);
      snprintf(row_text[340], sizeof row_text[340], "%d", row_plan_t.steps[28].depends_on[2]);
      snprintf(row_text[341], sizeof row_text[341], "%d", row_plan_t.steps[28].depends_on[3]);
      snprintf(row_text[342], sizeof row_text[342], "%d", row_plan_t.steps[28].depends_on[4]);
      snprintf(row_text[343], sizeof row_text[343], "%d", row_plan_t.steps[28].depends_on[5]);
      snprintf(row_text[344], sizeof row_text[344], "%d", row_plan_t.steps[28].depends_on[6]);
      snprintf(row_text[345], sizeof row_text[345], "%d", row_plan_t.steps[28].depends_on[7]);
      snprintf(row_text[346], sizeof row_text[346], "%d", row_plan_t.steps[28].dep_count);
      snprintf(row_text[347], sizeof row_text[347], "%d", row_plan_t.steps[28].status);
      snprintf(row_text[348], sizeof row_text[348], "%d", row_plan_t.steps[28].wave);
      snprintf(row_text[349], sizeof row_text[349], "%d", row_plan_t.steps[29].id);
      snprintf(row_text[350], sizeof row_text[350], "%d", row_plan_t.steps[29].depends_on[0]);
      snprintf(row_text[351], sizeof row_text[351], "%d", row_plan_t.steps[29].depends_on[1]);
      snprintf(row_text[352], sizeof row_text[352], "%d", row_plan_t.steps[29].depends_on[2]);
      snprintf(row_text[353], sizeof row_text[353], "%d", row_plan_t.steps[29].depends_on[3]);
      snprintf(row_text[354], sizeof row_text[354], "%d", row_plan_t.steps[29].depends_on[4]);
      snprintf(row_text[355], sizeof row_text[355], "%d", row_plan_t.steps[29].depends_on[5]);
      snprintf(row_text[356], sizeof row_text[356], "%d", row_plan_t.steps[29].depends_on[6]);
      snprintf(row_text[357], sizeof row_text[357], "%d", row_plan_t.steps[29].depends_on[7]);
      snprintf(row_text[358], sizeof row_text[358], "%d", row_plan_t.steps[29].dep_count);
      snprintf(row_text[359], sizeof row_text[359], "%d", row_plan_t.steps[29].status);
      snprintf(row_text[360], sizeof row_text[360], "%d", row_plan_t.steps[29].wave);
      snprintf(row_text[361], sizeof row_text[361], "%d", row_plan_t.steps[30].id);
      snprintf(row_text[362], sizeof row_text[362], "%d", row_plan_t.steps[30].depends_on[0]);
      snprintf(row_text[363], sizeof row_text[363], "%d", row_plan_t.steps[30].depends_on[1]);
      snprintf(row_text[364], sizeof row_text[364], "%d", row_plan_t.steps[30].depends_on[2]);
      snprintf(row_text[365], sizeof row_text[365], "%d", row_plan_t.steps[30].depends_on[3]);
      snprintf(row_text[366], sizeof row_text[366], "%d", row_plan_t.steps[30].depends_on[4]);
      snprintf(row_text[367], sizeof row_text[367], "%d", row_plan_t.steps[30].depends_on[5]);
      snprintf(row_text[368], sizeof row_text[368], "%d", row_plan_t.steps[30].depends_on[6]);
      snprintf(row_text[369], sizeof row_text[369], "%d", row_plan_t.steps[30].depends_on[7]);
      snprintf(row_text[370], sizeof row_text[370], "%d", row_plan_t.steps[30].dep_count);
      snprintf(row_text[371], sizeof row_text[371], "%d", row_plan_t.steps[30].status);
      snprintf(row_text[372], sizeof row_text[372], "%d", row_plan_t.steps[30].wave);
      snprintf(row_text[373], sizeof row_text[373], "%d", row_plan_t.steps[31].id);
      snprintf(row_text[374], sizeof row_text[374], "%d", row_plan_t.steps[31].depends_on[0]);
      snprintf(row_text[375], sizeof row_text[375], "%d", row_plan_t.steps[31].depends_on[1]);
      snprintf(row_text[376], sizeof row_text[376], "%d", row_plan_t.steps[31].depends_on[2]);
      snprintf(row_text[377], sizeof row_text[377], "%d", row_plan_t.steps[31].depends_on[3]);
      snprintf(row_text[378], sizeof row_text[378], "%d", row_plan_t.steps[31].depends_on[4]);
      snprintf(row_text[379], sizeof row_text[379], "%d", row_plan_t.steps[31].depends_on[5]);
      snprintf(row_text[380], sizeof row_text[380], "%d", row_plan_t.steps[31].depends_on[6]);
      snprintf(row_text[381], sizeof row_text[381], "%d", row_plan_t.steps[31].depends_on[7]);
      snprintf(row_text[382], sizeof row_text[382], "%d", row_plan_t.steps[31].dep_count);
      snprintf(row_text[383], sizeof row_text[383], "%d", row_plan_t.steps[31].status);
      snprintf(row_text[384], sizeof row_text[384], "%d", row_plan_t.steps[31].wave);
      snprintf(row_text[385], sizeof row_text[385], "%d", row_plan_t.step_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_plan_t.agent_name;
      row_slots[2] = row_plan_t.task;
      row_slots[3] = row_plan_t.status;
      row_slots[4] = row_text[1];
      row_slots[5] = row_plan_t.steps[0].action;
      row_slots[6] = row_plan_t.steps[0].precondition;
      row_slots[7] = row_plan_t.steps[0].success_predicate;
      row_slots[8] = row_plan_t.steps[0].rollback;
      row_slots[9] = row_text[2];
      row_slots[10] = row_text[3];
      row_slots[11] = row_text[4];
      row_slots[12] = row_text[5];
      row_slots[13] = row_text[6];
      row_slots[14] = row_text[7];
      row_slots[15] = row_text[8];
      row_slots[16] = row_text[9];
      row_slots[17] = row_text[10];
      row_slots[18] = row_text[11];
      row_slots[19] = row_plan_t.steps[0].output;
      row_slots[20] = row_text[12];
      row_slots[21] = row_text[13];
      row_slots[22] = row_plan_t.steps[1].action;
      row_slots[23] = row_plan_t.steps[1].precondition;
      row_slots[24] = row_plan_t.steps[1].success_predicate;
      row_slots[25] = row_plan_t.steps[1].rollback;
      row_slots[26] = row_text[14];
      row_slots[27] = row_text[15];
      row_slots[28] = row_text[16];
      row_slots[29] = row_text[17];
      row_slots[30] = row_text[18];
      row_slots[31] = row_text[19];
      row_slots[32] = row_text[20];
      row_slots[33] = row_text[21];
      row_slots[34] = row_text[22];
      row_slots[35] = row_text[23];
      row_slots[36] = row_plan_t.steps[1].output;
      row_slots[37] = row_text[24];
      row_slots[38] = row_text[25];
      row_slots[39] = row_plan_t.steps[2].action;
      row_slots[40] = row_plan_t.steps[2].precondition;
      row_slots[41] = row_plan_t.steps[2].success_predicate;
      row_slots[42] = row_plan_t.steps[2].rollback;
      row_slots[43] = row_text[26];
      row_slots[44] = row_text[27];
      row_slots[45] = row_text[28];
      row_slots[46] = row_text[29];
      row_slots[47] = row_text[30];
      row_slots[48] = row_text[31];
      row_slots[49] = row_text[32];
      row_slots[50] = row_text[33];
      row_slots[51] = row_text[34];
      row_slots[52] = row_text[35];
      row_slots[53] = row_plan_t.steps[2].output;
      row_slots[54] = row_text[36];
      row_slots[55] = row_text[37];
      row_slots[56] = row_plan_t.steps[3].action;
      row_slots[57] = row_plan_t.steps[3].precondition;
      row_slots[58] = row_plan_t.steps[3].success_predicate;
      row_slots[59] = row_plan_t.steps[3].rollback;
      row_slots[60] = row_text[38];
      row_slots[61] = row_text[39];
      row_slots[62] = row_text[40];
      row_slots[63] = row_text[41];
      row_slots[64] = row_text[42];
      row_slots[65] = row_text[43];
      row_slots[66] = row_text[44];
      row_slots[67] = row_text[45];
      row_slots[68] = row_text[46];
      row_slots[69] = row_text[47];
      row_slots[70] = row_plan_t.steps[3].output;
      row_slots[71] = row_text[48];
      row_slots[72] = row_text[49];
      row_slots[73] = row_plan_t.steps[4].action;
      row_slots[74] = row_plan_t.steps[4].precondition;
      row_slots[75] = row_plan_t.steps[4].success_predicate;
      row_slots[76] = row_plan_t.steps[4].rollback;
      row_slots[77] = row_text[50];
      row_slots[78] = row_text[51];
      row_slots[79] = row_text[52];
      row_slots[80] = row_text[53];
      row_slots[81] = row_text[54];
      row_slots[82] = row_text[55];
      row_slots[83] = row_text[56];
      row_slots[84] = row_text[57];
      row_slots[85] = row_text[58];
      row_slots[86] = row_text[59];
      row_slots[87] = row_plan_t.steps[4].output;
      row_slots[88] = row_text[60];
      row_slots[89] = row_text[61];
      row_slots[90] = row_plan_t.steps[5].action;
      row_slots[91] = row_plan_t.steps[5].precondition;
      row_slots[92] = row_plan_t.steps[5].success_predicate;
      row_slots[93] = row_plan_t.steps[5].rollback;
      row_slots[94] = row_text[62];
      row_slots[95] = row_text[63];
      row_slots[96] = row_text[64];
      row_slots[97] = row_text[65];
      row_slots[98] = row_text[66];
      row_slots[99] = row_text[67];
      row_slots[100] = row_text[68];
      row_slots[101] = row_text[69];
      row_slots[102] = row_text[70];
      row_slots[103] = row_text[71];
      row_slots[104] = row_plan_t.steps[5].output;
      row_slots[105] = row_text[72];
      row_slots[106] = row_text[73];
      row_slots[107] = row_plan_t.steps[6].action;
      row_slots[108] = row_plan_t.steps[6].precondition;
      row_slots[109] = row_plan_t.steps[6].success_predicate;
      row_slots[110] = row_plan_t.steps[6].rollback;
      row_slots[111] = row_text[74];
      row_slots[112] = row_text[75];
      row_slots[113] = row_text[76];
      row_slots[114] = row_text[77];
      row_slots[115] = row_text[78];
      row_slots[116] = row_text[79];
      row_slots[117] = row_text[80];
      row_slots[118] = row_text[81];
      row_slots[119] = row_text[82];
      row_slots[120] = row_text[83];
      row_slots[121] = row_plan_t.steps[6].output;
      row_slots[122] = row_text[84];
      row_slots[123] = row_text[85];
      row_slots[124] = row_plan_t.steps[7].action;
      row_slots[125] = row_plan_t.steps[7].precondition;
      row_slots[126] = row_plan_t.steps[7].success_predicate;
      row_slots[127] = row_plan_t.steps[7].rollback;
      row_slots[128] = row_text[86];
      row_slots[129] = row_text[87];
      row_slots[130] = row_text[88];
      row_slots[131] = row_text[89];
      row_slots[132] = row_text[90];
      row_slots[133] = row_text[91];
      row_slots[134] = row_text[92];
      row_slots[135] = row_text[93];
      row_slots[136] = row_text[94];
      row_slots[137] = row_text[95];
      row_slots[138] = row_plan_t.steps[7].output;
      row_slots[139] = row_text[96];
      row_slots[140] = row_text[97];
      row_slots[141] = row_plan_t.steps[8].action;
      row_slots[142] = row_plan_t.steps[8].precondition;
      row_slots[143] = row_plan_t.steps[8].success_predicate;
      row_slots[144] = row_plan_t.steps[8].rollback;
      row_slots[145] = row_text[98];
      row_slots[146] = row_text[99];
      row_slots[147] = row_text[100];
      row_slots[148] = row_text[101];
      row_slots[149] = row_text[102];
      row_slots[150] = row_text[103];
      row_slots[151] = row_text[104];
      row_slots[152] = row_text[105];
      row_slots[153] = row_text[106];
      row_slots[154] = row_text[107];
      row_slots[155] = row_plan_t.steps[8].output;
      row_slots[156] = row_text[108];
      row_slots[157] = row_text[109];
      row_slots[158] = row_plan_t.steps[9].action;
      row_slots[159] = row_plan_t.steps[9].precondition;
      row_slots[160] = row_plan_t.steps[9].success_predicate;
      row_slots[161] = row_plan_t.steps[9].rollback;
      row_slots[162] = row_text[110];
      row_slots[163] = row_text[111];
      row_slots[164] = row_text[112];
      row_slots[165] = row_text[113];
      row_slots[166] = row_text[114];
      row_slots[167] = row_text[115];
      row_slots[168] = row_text[116];
      row_slots[169] = row_text[117];
      row_slots[170] = row_text[118];
      row_slots[171] = row_text[119];
      row_slots[172] = row_plan_t.steps[9].output;
      row_slots[173] = row_text[120];
      row_slots[174] = row_text[121];
      row_slots[175] = row_plan_t.steps[10].action;
      row_slots[176] = row_plan_t.steps[10].precondition;
      row_slots[177] = row_plan_t.steps[10].success_predicate;
      row_slots[178] = row_plan_t.steps[10].rollback;
      row_slots[179] = row_text[122];
      row_slots[180] = row_text[123];
      row_slots[181] = row_text[124];
      row_slots[182] = row_text[125];
      row_slots[183] = row_text[126];
      row_slots[184] = row_text[127];
      row_slots[185] = row_text[128];
      row_slots[186] = row_text[129];
      row_slots[187] = row_text[130];
      row_slots[188] = row_text[131];
      row_slots[189] = row_plan_t.steps[10].output;
      row_slots[190] = row_text[132];
      row_slots[191] = row_text[133];
      row_slots[192] = row_plan_t.steps[11].action;
      row_slots[193] = row_plan_t.steps[11].precondition;
      row_slots[194] = row_plan_t.steps[11].success_predicate;
      row_slots[195] = row_plan_t.steps[11].rollback;
      row_slots[196] = row_text[134];
      row_slots[197] = row_text[135];
      row_slots[198] = row_text[136];
      row_slots[199] = row_text[137];
      row_slots[200] = row_text[138];
      row_slots[201] = row_text[139];
      row_slots[202] = row_text[140];
      row_slots[203] = row_text[141];
      row_slots[204] = row_text[142];
      row_slots[205] = row_text[143];
      row_slots[206] = row_plan_t.steps[11].output;
      row_slots[207] = row_text[144];
      row_slots[208] = row_text[145];
      row_slots[209] = row_plan_t.steps[12].action;
      row_slots[210] = row_plan_t.steps[12].precondition;
      row_slots[211] = row_plan_t.steps[12].success_predicate;
      row_slots[212] = row_plan_t.steps[12].rollback;
      row_slots[213] = row_text[146];
      row_slots[214] = row_text[147];
      row_slots[215] = row_text[148];
      row_slots[216] = row_text[149];
      row_slots[217] = row_text[150];
      row_slots[218] = row_text[151];
      row_slots[219] = row_text[152];
      row_slots[220] = row_text[153];
      row_slots[221] = row_text[154];
      row_slots[222] = row_text[155];
      row_slots[223] = row_plan_t.steps[12].output;
      row_slots[224] = row_text[156];
      row_slots[225] = row_text[157];
      row_slots[226] = row_plan_t.steps[13].action;
      row_slots[227] = row_plan_t.steps[13].precondition;
      row_slots[228] = row_plan_t.steps[13].success_predicate;
      row_slots[229] = row_plan_t.steps[13].rollback;
      row_slots[230] = row_text[158];
      row_slots[231] = row_text[159];
      row_slots[232] = row_text[160];
      row_slots[233] = row_text[161];
      row_slots[234] = row_text[162];
      row_slots[235] = row_text[163];
      row_slots[236] = row_text[164];
      row_slots[237] = row_text[165];
      row_slots[238] = row_text[166];
      row_slots[239] = row_text[167];
      row_slots[240] = row_plan_t.steps[13].output;
      row_slots[241] = row_text[168];
      row_slots[242] = row_text[169];
      row_slots[243] = row_plan_t.steps[14].action;
      row_slots[244] = row_plan_t.steps[14].precondition;
      row_slots[245] = row_plan_t.steps[14].success_predicate;
      row_slots[246] = row_plan_t.steps[14].rollback;
      row_slots[247] = row_text[170];
      row_slots[248] = row_text[171];
      row_slots[249] = row_text[172];
      row_slots[250] = row_text[173];
      row_slots[251] = row_text[174];
      row_slots[252] = row_text[175];
      row_slots[253] = row_text[176];
      row_slots[254] = row_text[177];
      row_slots[255] = row_text[178];
      row_slots[256] = row_text[179];
      row_slots[257] = row_plan_t.steps[14].output;
      row_slots[258] = row_text[180];
      row_slots[259] = row_text[181];
      row_slots[260] = row_plan_t.steps[15].action;
      row_slots[261] = row_plan_t.steps[15].precondition;
      row_slots[262] = row_plan_t.steps[15].success_predicate;
      row_slots[263] = row_plan_t.steps[15].rollback;
      row_slots[264] = row_text[182];
      row_slots[265] = row_text[183];
      row_slots[266] = row_text[184];
      row_slots[267] = row_text[185];
      row_slots[268] = row_text[186];
      row_slots[269] = row_text[187];
      row_slots[270] = row_text[188];
      row_slots[271] = row_text[189];
      row_slots[272] = row_text[190];
      row_slots[273] = row_text[191];
      row_slots[274] = row_plan_t.steps[15].output;
      row_slots[275] = row_text[192];
      row_slots[276] = row_text[193];
      row_slots[277] = row_plan_t.steps[16].action;
      row_slots[278] = row_plan_t.steps[16].precondition;
      row_slots[279] = row_plan_t.steps[16].success_predicate;
      row_slots[280] = row_plan_t.steps[16].rollback;
      row_slots[281] = row_text[194];
      row_slots[282] = row_text[195];
      row_slots[283] = row_text[196];
      row_slots[284] = row_text[197];
      row_slots[285] = row_text[198];
      row_slots[286] = row_text[199];
      row_slots[287] = row_text[200];
      row_slots[288] = row_text[201];
      row_slots[289] = row_text[202];
      row_slots[290] = row_text[203];
      row_slots[291] = row_plan_t.steps[16].output;
      row_slots[292] = row_text[204];
      row_slots[293] = row_text[205];
      row_slots[294] = row_plan_t.steps[17].action;
      row_slots[295] = row_plan_t.steps[17].precondition;
      row_slots[296] = row_plan_t.steps[17].success_predicate;
      row_slots[297] = row_plan_t.steps[17].rollback;
      row_slots[298] = row_text[206];
      row_slots[299] = row_text[207];
      row_slots[300] = row_text[208];
      row_slots[301] = row_text[209];
      row_slots[302] = row_text[210];
      row_slots[303] = row_text[211];
      row_slots[304] = row_text[212];
      row_slots[305] = row_text[213];
      row_slots[306] = row_text[214];
      row_slots[307] = row_text[215];
      row_slots[308] = row_plan_t.steps[17].output;
      row_slots[309] = row_text[216];
      row_slots[310] = row_text[217];
      row_slots[311] = row_plan_t.steps[18].action;
      row_slots[312] = row_plan_t.steps[18].precondition;
      row_slots[313] = row_plan_t.steps[18].success_predicate;
      row_slots[314] = row_plan_t.steps[18].rollback;
      row_slots[315] = row_text[218];
      row_slots[316] = row_text[219];
      row_slots[317] = row_text[220];
      row_slots[318] = row_text[221];
      row_slots[319] = row_text[222];
      row_slots[320] = row_text[223];
      row_slots[321] = row_text[224];
      row_slots[322] = row_text[225];
      row_slots[323] = row_text[226];
      row_slots[324] = row_text[227];
      row_slots[325] = row_plan_t.steps[18].output;
      row_slots[326] = row_text[228];
      row_slots[327] = row_text[229];
      row_slots[328] = row_plan_t.steps[19].action;
      row_slots[329] = row_plan_t.steps[19].precondition;
      row_slots[330] = row_plan_t.steps[19].success_predicate;
      row_slots[331] = row_plan_t.steps[19].rollback;
      row_slots[332] = row_text[230];
      row_slots[333] = row_text[231];
      row_slots[334] = row_text[232];
      row_slots[335] = row_text[233];
      row_slots[336] = row_text[234];
      row_slots[337] = row_text[235];
      row_slots[338] = row_text[236];
      row_slots[339] = row_text[237];
      row_slots[340] = row_text[238];
      row_slots[341] = row_text[239];
      row_slots[342] = row_plan_t.steps[19].output;
      row_slots[343] = row_text[240];
      row_slots[344] = row_text[241];
      row_slots[345] = row_plan_t.steps[20].action;
      row_slots[346] = row_plan_t.steps[20].precondition;
      row_slots[347] = row_plan_t.steps[20].success_predicate;
      row_slots[348] = row_plan_t.steps[20].rollback;
      row_slots[349] = row_text[242];
      row_slots[350] = row_text[243];
      row_slots[351] = row_text[244];
      row_slots[352] = row_text[245];
      row_slots[353] = row_text[246];
      row_slots[354] = row_text[247];
      row_slots[355] = row_text[248];
      row_slots[356] = row_text[249];
      row_slots[357] = row_text[250];
      row_slots[358] = row_text[251];
      row_slots[359] = row_plan_t.steps[20].output;
      row_slots[360] = row_text[252];
      row_slots[361] = row_text[253];
      row_slots[362] = row_plan_t.steps[21].action;
      row_slots[363] = row_plan_t.steps[21].precondition;
      row_slots[364] = row_plan_t.steps[21].success_predicate;
      row_slots[365] = row_plan_t.steps[21].rollback;
      row_slots[366] = row_text[254];
      row_slots[367] = row_text[255];
      row_slots[368] = row_text[256];
      row_slots[369] = row_text[257];
      row_slots[370] = row_text[258];
      row_slots[371] = row_text[259];
      row_slots[372] = row_text[260];
      row_slots[373] = row_text[261];
      row_slots[374] = row_text[262];
      row_slots[375] = row_text[263];
      row_slots[376] = row_plan_t.steps[21].output;
      row_slots[377] = row_text[264];
      row_slots[378] = row_text[265];
      row_slots[379] = row_plan_t.steps[22].action;
      row_slots[380] = row_plan_t.steps[22].precondition;
      row_slots[381] = row_plan_t.steps[22].success_predicate;
      row_slots[382] = row_plan_t.steps[22].rollback;
      row_slots[383] = row_text[266];
      row_slots[384] = row_text[267];
      row_slots[385] = row_text[268];
      row_slots[386] = row_text[269];
      row_slots[387] = row_text[270];
      row_slots[388] = row_text[271];
      row_slots[389] = row_text[272];
      row_slots[390] = row_text[273];
      row_slots[391] = row_text[274];
      row_slots[392] = row_text[275];
      row_slots[393] = row_plan_t.steps[22].output;
      row_slots[394] = row_text[276];
      row_slots[395] = row_text[277];
      row_slots[396] = row_plan_t.steps[23].action;
      row_slots[397] = row_plan_t.steps[23].precondition;
      row_slots[398] = row_plan_t.steps[23].success_predicate;
      row_slots[399] = row_plan_t.steps[23].rollback;
      row_slots[400] = row_text[278];
      row_slots[401] = row_text[279];
      row_slots[402] = row_text[280];
      row_slots[403] = row_text[281];
      row_slots[404] = row_text[282];
      row_slots[405] = row_text[283];
      row_slots[406] = row_text[284];
      row_slots[407] = row_text[285];
      row_slots[408] = row_text[286];
      row_slots[409] = row_text[287];
      row_slots[410] = row_plan_t.steps[23].output;
      row_slots[411] = row_text[288];
      row_slots[412] = row_text[289];
      row_slots[413] = row_plan_t.steps[24].action;
      row_slots[414] = row_plan_t.steps[24].precondition;
      row_slots[415] = row_plan_t.steps[24].success_predicate;
      row_slots[416] = row_plan_t.steps[24].rollback;
      row_slots[417] = row_text[290];
      row_slots[418] = row_text[291];
      row_slots[419] = row_text[292];
      row_slots[420] = row_text[293];
      row_slots[421] = row_text[294];
      row_slots[422] = row_text[295];
      row_slots[423] = row_text[296];
      row_slots[424] = row_text[297];
      row_slots[425] = row_text[298];
      row_slots[426] = row_text[299];
      row_slots[427] = row_plan_t.steps[24].output;
      row_slots[428] = row_text[300];
      row_slots[429] = row_text[301];
      row_slots[430] = row_plan_t.steps[25].action;
      row_slots[431] = row_plan_t.steps[25].precondition;
      row_slots[432] = row_plan_t.steps[25].success_predicate;
      row_slots[433] = row_plan_t.steps[25].rollback;
      row_slots[434] = row_text[302];
      row_slots[435] = row_text[303];
      row_slots[436] = row_text[304];
      row_slots[437] = row_text[305];
      row_slots[438] = row_text[306];
      row_slots[439] = row_text[307];
      row_slots[440] = row_text[308];
      row_slots[441] = row_text[309];
      row_slots[442] = row_text[310];
      row_slots[443] = row_text[311];
      row_slots[444] = row_plan_t.steps[25].output;
      row_slots[445] = row_text[312];
      row_slots[446] = row_text[313];
      row_slots[447] = row_plan_t.steps[26].action;
      row_slots[448] = row_plan_t.steps[26].precondition;
      row_slots[449] = row_plan_t.steps[26].success_predicate;
      row_slots[450] = row_plan_t.steps[26].rollback;
      row_slots[451] = row_text[314];
      row_slots[452] = row_text[315];
      row_slots[453] = row_text[316];
      row_slots[454] = row_text[317];
      row_slots[455] = row_text[318];
      row_slots[456] = row_text[319];
      row_slots[457] = row_text[320];
      row_slots[458] = row_text[321];
      row_slots[459] = row_text[322];
      row_slots[460] = row_text[323];
      row_slots[461] = row_plan_t.steps[26].output;
      row_slots[462] = row_text[324];
      row_slots[463] = row_text[325];
      row_slots[464] = row_plan_t.steps[27].action;
      row_slots[465] = row_plan_t.steps[27].precondition;
      row_slots[466] = row_plan_t.steps[27].success_predicate;
      row_slots[467] = row_plan_t.steps[27].rollback;
      row_slots[468] = row_text[326];
      row_slots[469] = row_text[327];
      row_slots[470] = row_text[328];
      row_slots[471] = row_text[329];
      row_slots[472] = row_text[330];
      row_slots[473] = row_text[331];
      row_slots[474] = row_text[332];
      row_slots[475] = row_text[333];
      row_slots[476] = row_text[334];
      row_slots[477] = row_text[335];
      row_slots[478] = row_plan_t.steps[27].output;
      row_slots[479] = row_text[336];
      row_slots[480] = row_text[337];
      row_slots[481] = row_plan_t.steps[28].action;
      row_slots[482] = row_plan_t.steps[28].precondition;
      row_slots[483] = row_plan_t.steps[28].success_predicate;
      row_slots[484] = row_plan_t.steps[28].rollback;
      row_slots[485] = row_text[338];
      row_slots[486] = row_text[339];
      row_slots[487] = row_text[340];
      row_slots[488] = row_text[341];
      row_slots[489] = row_text[342];
      row_slots[490] = row_text[343];
      row_slots[491] = row_text[344];
      row_slots[492] = row_text[345];
      row_slots[493] = row_text[346];
      row_slots[494] = row_text[347];
      row_slots[495] = row_plan_t.steps[28].output;
      row_slots[496] = row_text[348];
      row_slots[497] = row_text[349];
      row_slots[498] = row_plan_t.steps[29].action;
      row_slots[499] = row_plan_t.steps[29].precondition;
      row_slots[500] = row_plan_t.steps[29].success_predicate;
      row_slots[501] = row_plan_t.steps[29].rollback;
      row_slots[502] = row_text[350];
      row_slots[503] = row_text[351];
      row_slots[504] = row_text[352];
      row_slots[505] = row_text[353];
      row_slots[506] = row_text[354];
      row_slots[507] = row_text[355];
      row_slots[508] = row_text[356];
      row_slots[509] = row_text[357];
      row_slots[510] = row_text[358];
      row_slots[511] = row_text[359];
      row_slots[512] = row_plan_t.steps[29].output;
      row_slots[513] = row_text[360];
      row_slots[514] = row_text[361];
      row_slots[515] = row_plan_t.steps[30].action;
      row_slots[516] = row_plan_t.steps[30].precondition;
      row_slots[517] = row_plan_t.steps[30].success_predicate;
      row_slots[518] = row_plan_t.steps[30].rollback;
      row_slots[519] = row_text[362];
      row_slots[520] = row_text[363];
      row_slots[521] = row_text[364];
      row_slots[522] = row_text[365];
      row_slots[523] = row_text[366];
      row_slots[524] = row_text[367];
      row_slots[525] = row_text[368];
      row_slots[526] = row_text[369];
      row_slots[527] = row_text[370];
      row_slots[528] = row_text[371];
      row_slots[529] = row_plan_t.steps[30].output;
      row_slots[530] = row_text[372];
      row_slots[531] = row_text[373];
      row_slots[532] = row_plan_t.steps[31].action;
      row_slots[533] = row_plan_t.steps[31].precondition;
      row_slots[534] = row_plan_t.steps[31].success_predicate;
      row_slots[535] = row_plan_t.steps[31].rollback;
      row_slots[536] = row_text[374];
      row_slots[537] = row_text[375];
      row_slots[538] = row_text[376];
      row_slots[539] = row_text[377];
      row_slots[540] = row_text[378];
      row_slots[541] = row_text[379];
      row_slots[542] = row_text[380];
      row_slots[543] = row_text[381];
      row_slots[544] = row_text[382];
      row_slots[545] = row_text[383];
      row_slots[546] = row_plan_t.steps[31].output;
      row_slots[547] = row_text[384];
      row_slots[548] = row_text[385];
      rows = row_slots;
      row_count = 549u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_LIST_IDS:
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
      if (parsed0 <= 0 || parsed0 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_execution_plan_list_ids(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
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
                     "%d", found[row]);
            cells[row * 1u + 0u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_EXISTS:
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
      rc = db1_execution_plan_exists(parsed0);
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_COUNT_STEPS:
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
      int produced = db1_execution_plan_count_steps(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_LIST_RUNNING_IDS:
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
      if (parsed0 <= 0 || parsed0 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_execution_plan_list_running_ids(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
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
                     "%d", found[row]);
            cells[row * 1u + 0u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_LIST_RECENT_SUMMARIES:
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
      if (parsed0 <= 0 || parsed0 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_execution_plan_summary_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_execution_plan_list_recent_summaries(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 7u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 3u * sizeof *numbers);
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
            snprintf(numbers[row * 3u + 0u], 32,
                     "%d", found[row].id);
            snprintf(numbers[row * 3u + 1u], 32,
                     "%d", found[row].total_steps);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%d", found[row].done_steps);
            cells[row * 7u + 0u] = numbers[row * 3u + 0u];
            cells[row * 7u + 1u] = found[row].agent_name;
            cells[row * 7u + 2u] = found[row].task;
            cells[row * 7u + 3u] = found[row].status;
            cells[row * 7u + 4u] = found[row].created_at;
            cells[row * 7u + 5u] = numbers[row * 3u + 1u];
            cells[row * 7u + 6u] = numbers[row * 3u + 2u];
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_SET_STATUS:
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
      int produced = db1_execution_plan_set_status(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_CANCEL_BY_ID:
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
      int produced = db1_execution_plan_cancel_by_id(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_EXECUTION_PLAN_CANCEL_STALE:
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
      int produced = db1_execution_plan_cancel_stale(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PLAN_STEP_SET_STATUS:
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
      int produced = db1_plan_step_set_status(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PLAN_STEP_SET_STATUS_OUTPUT:
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
      int produced = db1_plan_step_set_status_output(parsed0, field[1], field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PLAN_STEP_CANCEL_ACTIVE_FOR_PLAN:
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
      int produced = db1_plan_step_cancel_active_for_plan(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PLAN_STEP_CANCEL_ORPHANS:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_plan_step_cancel_orphans();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_STEP_EVIDENCE_INSERT:
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
      int parsed4;
      if (parse_int(field[4], &parsed4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_step_evidence_insert(parsed0, parsed1, field[2], field[3], parsed4, field[5]);
      break;
   }
   case AIMEE_DB1_OP_STEP_EVIDENCE_GET_LATEST:
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
      memset(&row_db1_step_evidence_latest_t, 0, sizeof row_db1_step_evidence_latest_t);
      rc = db1_step_evidence_get_latest(parsed0, &row_db1_step_evidence_latest_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_step_evidence_latest_t.passed);
      row_slots[0] = row_db1_step_evidence_latest_t.strength;
      row_slots[1] = row_text[0];
      row_slots[2] = row_db1_step_evidence_latest_t.kind;
      row_slots[3] = row_db1_step_evidence_latest_t.created_at;
      rows = row_slots;
      row_count = 4u;
      reads = 1;
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
   free(scalar_owned);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
