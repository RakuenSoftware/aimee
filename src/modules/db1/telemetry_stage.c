/* modules/db1/telemetry_stage.c: the telemetry stage handler.
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
#include "cost_fold.h"
#include "diagnose.h"
#include "eval.h"
#include "guardrail_events.h"
#include "interaction_events.h"
#include "token_audit.h"

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

aimee_module_status_t aimee_db1_stage_telemetry(const uint8_t *request_body, uint32_t request_len,
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
   db1_token_audit_session_split_t row_db1_token_audit_session_split_t;
   db1_token_audit_totals_t row_db1_token_audit_totals_t;
   db1_token_audit_spend_t row_db1_token_audit_spend_t;
   guardrail_event_counts_t row_guardrail_event_counts_t;
   diagnosis_t row_diagnosis_t;
   const char *row_slots[12];
   char row_text[12][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;

   switch (op)
   {
   case AIMEE_DB1_OP_TOKEN_AUDIT_INSERT:
   {
      if (count != 23u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_token_audit_row_t row;
      memset(&row, 0, sizeof row);
      row.session_id = field[0];
      row.delegation_id = field[1];
      row.project_name = field[2];
      row.tool_name = field[3];
      row.role = field[4];
      row.model = field[5];
      row.source = field[6];
      row.requested_model = field[7];
      row.stop_reason = field[8];
      row.usage_kind = field[9];
      int64_t member_10 = 0;
      if (parse_int64(field[10], &member_10) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.agent_log_id = member_10;
      row.request_id = field[11];
      row.idempotency_key = field[12];
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.attempt = member_13;
      row.principal = field[14];
      row.served_model = field[15];
      int member_16 = 0;
      if (parse_int(field[16], &member_16) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.duration_ms = member_16;
      row.metadata = field[17];
      int member_18 = 0;
      if (parse_int(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.prompt_tokens = member_18;
      int member_19 = 0;
      if (parse_int(field[19], &member_19) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.completion_tokens = member_19;
      int member_20 = 0;
      if (parse_int(field[20], &member_20) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cache_write_tokens = member_20;
      int member_21 = 0;
      if (parse_int(field[21], &member_21) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.cache_read_tokens = member_21;
      double member_22 = 0;
      if (parse_double(field[22], &member_22) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.estimated_cost_usd = member_22;
      rc = db1_token_audit_insert(&row);
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_ENSURE_IDEM_INDEX:
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_token_audit_ensure_idem_index();
      rc = 0;
      break;
   case AIMEE_DB1_OP_TOKEN_AUDIT_COST_FOR_DELEGATION:
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
      double produced = db1_token_audit_cost_for_delegation(field[0]);
      rc = 0;
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_COST_FOR_DELEGATION_EX:
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
      double scalar0 = 0;
      rc = db1_token_audit_cost_for_delegation_ex(field[0], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_SESSION_SPLIT:
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
      memset(&row_db1_token_audit_session_split_t, 0, sizeof row_db1_token_audit_session_split_t);
      rc = db1_token_audit_session_split(field[0], &row_db1_token_audit_session_split_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_token_audit_session_split_t.supervisor_calls);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_db1_token_audit_session_split_t.supervisor_prompt_tokens);
      snprintf(row_text[2], sizeof row_text[2], "%lld", (long long)row_db1_token_audit_session_split_t.supervisor_completion_tokens);
      snprintf(row_text[3], sizeof row_text[3], "%lld", (long long)row_db1_token_audit_session_split_t.supervisor_cache_write_tokens);
      snprintf(row_text[4], sizeof row_text[4], "%lld", (long long)row_db1_token_audit_session_split_t.supervisor_cache_read_tokens);
      snprintf(row_text[5], sizeof row_text[5], "%.17g", (double)row_db1_token_audit_session_split_t.supervisor_cost_usd);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_db1_token_audit_session_split_t.worker_calls);
      snprintf(row_text[7], sizeof row_text[7], "%lld", (long long)row_db1_token_audit_session_split_t.worker_prompt_tokens);
      snprintf(row_text[8], sizeof row_text[8], "%lld", (long long)row_db1_token_audit_session_split_t.worker_completion_tokens);
      snprintf(row_text[9], sizeof row_text[9], "%lld", (long long)row_db1_token_audit_session_split_t.worker_cache_write_tokens);
      snprintf(row_text[10], sizeof row_text[10], "%lld", (long long)row_db1_token_audit_session_split_t.worker_cache_read_tokens);
      snprintf(row_text[11], sizeof row_text[11], "%.17g", (double)row_db1_token_audit_session_split_t.worker_cost_usd);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_text[3];
      row_slots[4] = row_text[4];
      row_slots[5] = row_text[5];
      row_slots[6] = row_text[6];
      row_slots[7] = row_text[7];
      row_slots[8] = row_text[8];
      row_slots[9] = row_text[9];
      row_slots[10] = row_text[10];
      row_slots[11] = row_text[11];
      rows = row_slots;
      row_count = 12u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_TOTALS:
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
      memset(&row_db1_token_audit_totals_t, 0, sizeof row_db1_token_audit_totals_t);
      rc = db1_token_audit_totals(parsed0, &row_db1_token_audit_totals_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_token_audit_totals_t.total_calls);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_db1_token_audit_totals_t.prompt_tokens);
      snprintf(row_text[2], sizeof row_text[2], "%lld", (long long)row_db1_token_audit_totals_t.completion_tokens);
      snprintf(row_text[3], sizeof row_text[3], "%lld", (long long)row_db1_token_audit_totals_t.cache_write_tokens);
      snprintf(row_text[4], sizeof row_text[4], "%lld", (long long)row_db1_token_audit_totals_t.cache_read_tokens);
      snprintf(row_text[5], sizeof row_text[5], "%.17g", (double)row_db1_token_audit_totals_t.estimated_cost_usd);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_text[3];
      row_slots[4] = row_text[4];
      row_slots[5] = row_text[5];
      rows = row_slots;
      row_count = 6u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_SPEND_BREAKDOWN:
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
      memset(&row_db1_token_audit_spend_t, 0, sizeof row_db1_token_audit_spend_t);
      rc = db1_token_audit_spend_breakdown(parsed0, &row_db1_token_audit_spend_t);
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)row_db1_token_audit_spend_t.realized_cost_usd);
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)row_db1_token_audit_spend_t.estimated_cost_usd);
      snprintf(row_text[2], sizeof row_text[2], "%.17g", (double)row_db1_token_audit_spend_t.avoided_cost_usd);
      snprintf(row_text[3], sizeof row_text[3], "%.17g", (double)row_db1_token_audit_spend_t.partial_cost_usd);
      snprintf(row_text[4], sizeof row_text[4], "%.17g", (double)row_db1_token_audit_spend_t.spend_cost_usd);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_text[3];
      row_slots[4] = row_text[4];
      rows = row_slots;
      row_count = 5u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_BY_ROLE:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_token_audit_role_summary_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_token_audit_by_role(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 5u * sizeof *cells);
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
                     "%d", found[row].calls);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%lld", (long long)found[row].prompt_tokens);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%lld", (long long)found[row].completion_tokens);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            cells[row * 5u + 0u] = found[row].role;
            cells[row * 5u + 1u] = numbers[row * 4u + 0u];
            cells[row * 5u + 2u] = numbers[row * 4u + 1u];
            cells[row * 5u + 3u] = numbers[row * 4u + 2u];
            cells[row * 5u + 4u] = numbers[row * 4u + 3u];
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_BY_TOOL:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_token_audit_tool_summary_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_token_audit_by_tool(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 5u * sizeof *cells);
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
                     "%d", found[row].calls);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%lld", (long long)found[row].prompt_tokens);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%lld", (long long)found[row].completion_tokens);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            cells[row * 5u + 0u] = found[row].tool_name;
            cells[row * 5u + 1u] = numbers[row * 4u + 0u];
            cells[row * 5u + 2u] = numbers[row * 4u + 1u];
            cells[row * 5u + 3u] = numbers[row * 4u + 2u];
            cells[row * 5u + 4u] = numbers[row * 4u + 3u];
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_BY_MODEL:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_token_audit_model_summary_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_token_audit_by_model(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 5u * sizeof *cells);
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
                     "%d", found[row].calls);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%lld", (long long)found[row].prompt_tokens);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%lld", (long long)found[row].completion_tokens);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            cells[row * 5u + 0u] = found[row].model;
            cells[row * 5u + 1u] = numbers[row * 4u + 0u];
            cells[row * 5u + 2u] = numbers[row * 4u + 1u];
            cells[row * 5u + 3u] = numbers[row * 4u + 2u];
            cells[row * 5u + 4u] = numbers[row * 4u + 3u];
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_BY_SOURCE:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_token_audit_source_summary_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_token_audit_by_source(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 5u * sizeof *cells);
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
                     "%d", found[row].calls);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%lld", (long long)found[row].prompt_tokens);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%lld", (long long)found[row].completion_tokens);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            cells[row * 5u + 0u] = found[row].source;
            cells[row * 5u + 1u] = numbers[row * 4u + 0u];
            cells[row * 5u + 2u] = numbers[row * 4u + 1u];
            cells[row * 5u + 3u] = numbers[row * 4u + 2u];
            cells[row * 5u + 4u] = numbers[row * 4u + 3u];
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_TOKEN_AUDIT_LIST_DASHBOARD:
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
      db1_token_audit_dashboard_row_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_token_audit_list_dashboard(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 9u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 6u * sizeof *numbers);
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
            snprintf(numbers[row * 6u + 0u], 32,
                     "%lld", (long long)found[row].prompt_tokens);
            snprintf(numbers[row * 6u + 1u], 32,
                     "%lld", (long long)found[row].completion_tokens);
            snprintf(numbers[row * 6u + 2u], 32,
                     "%lld", (long long)found[row].cache_write_tokens);
            snprintf(numbers[row * 6u + 3u], 32,
                     "%lld", (long long)found[row].cache_read_tokens);
            snprintf(numbers[row * 6u + 4u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            snprintf(numbers[row * 6u + 5u], 32,
                     "%d", found[row].call_count);
            cells[row * 9u + 0u] = found[row].tool_name;
            cells[row * 9u + 1u] = found[row].role;
            cells[row * 9u + 2u] = numbers[row * 6u + 0u];
            cells[row * 9u + 3u] = numbers[row * 6u + 1u];
            cells[row * 9u + 4u] = numbers[row * 6u + 2u];
            cells[row * 9u + 5u] = numbers[row * 6u + 3u];
            cells[row * 9u + 6u] = numbers[row * 6u + 4u];
            cells[row * 9u + 7u] = numbers[row * 6u + 5u];
            cells[row * 9u + 8u] = found[row].last_seen;
         }
         rows = cells;
         row_count = produced * 9u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_INSIGHTS_BY_PLATFORM:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_insights_platform_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_insights_by_platform(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
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
                     "%d", found[row].session_count);
            cells[row * 2u + 0u] = found[row].platform;
            cells[row * 2u + 1u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 2u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_INSIGHTS_TOP_SESSIONS:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_insights_top_session_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_insights_top_sessions(parsed0, found, parsed1);
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
                     "%lld", (long long)found[row].prompt_tokens);
            snprintf(numbers[row * 3u + 1u], 32,
                     "%lld", (long long)found[row].completion_tokens);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            cells[row * 7u + 0u] = found[row].session_id;
            cells[row * 7u + 1u] = found[row].title;
            cells[row * 7u + 2u] = found[row].model;
            cells[row * 7u + 3u] = numbers[row * 3u + 0u];
            cells[row * 7u + 4u] = numbers[row * 3u + 1u];
            cells[row * 7u + 5u] = numbers[row * 3u + 2u];
            cells[row * 7u + 6u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_INSIGHTS_DELEGATES_BY_ROLE:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_insights_delegate_role_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_insights_delegates_by_role(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 3u * sizeof *cells);
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
                     "%d", found[row].total);
            snprintf(numbers[row * 2u + 1u], 32,
                     "%d", found[row].completed);
            cells[row * 3u + 0u] = found[row].role;
            cells[row * 3u + 1u] = numbers[row * 2u + 0u];
            cells[row * 3u + 2u] = numbers[row * 2u + 1u];
         }
         rows = cells;
         row_count = produced * 3u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_COST_FOLD_RECORD:
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
      double parsed2;
      if (parse_double(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_cost_fold_record(field[0], field[1], parsed2, field[3]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_COST_FOLD_TOTAL:
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
      double produced = db1_cost_fold_total(field[0]);
      rc = 0;
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_RECORD:
   {
      if (count != 5u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_interaction_event_record(field[0], field[1], field[2], field[3], field[4]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_LIST_UNREFLECTED:
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
      if (parsed1 <= 0 || parsed1 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      ie_event_row_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_interaction_event_list_unreflected(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 7u * sizeof *cells);
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
                     "%d", found[row].id);
            cells[row * 7u + 0u] = numbers[row * 1u + 0u];
            cells[row * 7u + 1u] = found[row].created_at;
            cells[row * 7u + 2u] = found[row].session_id;
            cells[row * 7u + 3u] = found[row].event_type;
            cells[row * 7u + 4u] = found[row].actor;
            cells[row * 7u + 5u] = found[row].payload;
            cells[row * 7u + 6u] = found[row].outcome;
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_LIST_FOR_SESSION:
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
      if (parsed1 <= 0 || parsed1 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      ie_event_row_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_interaction_event_list_for_session(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 7u * sizeof *cells);
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
                     "%d", found[row].id);
            cells[row * 7u + 0u] = numbers[row * 1u + 0u];
            cells[row * 7u + 1u] = found[row].created_at;
            cells[row * 7u + 2u] = found[row].session_id;
            cells[row * 7u + 3u] = found[row].event_type;
            cells[row * 7u + 4u] = found[row].actor;
            cells[row * 7u + 5u] = found[row].payload;
            cells[row * 7u + 6u] = found[row].outcome;
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_LIST_PROMOTION_FEED:
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
      ie_event_row_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_interaction_event_list_promotion_feed(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 7u * sizeof *cells);
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
                     "%d", found[row].id);
            cells[row * 7u + 0u] = numbers[row * 1u + 0u];
            cells[row * 7u + 1u] = found[row].created_at;
            cells[row * 7u + 2u] = found[row].session_id;
            cells[row * 7u + 3u] = found[row].event_type;
            cells[row * 7u + 4u] = found[row].actor;
            cells[row * 7u + 5u] = found[row].payload;
            cells[row * 7u + 6u] = found[row].outcome;
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_MARK_REFLECTED:
   {
      if (count > 512u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int repeated_rows = (int)(count - 0u);
      int *repeated_held = calloc((size_t)repeated_rows + 1u, sizeof *repeated_held);
      if (!repeated_held)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = repeated_held;
      for (int at = 0; at < repeated_rows; ++at)
      {
         if (parse_int(field[0 + at], &repeated_held[at]) != 0)
         {
            free(scratch);
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         }
      }
      int produced = db1_interaction_event_mark_reflected(repeated_held, repeated_rows);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_MARK_PROMOTED:
   {
      if (count > 512u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int repeated_rows = (int)(count - 0u);
      int *repeated_held = calloc((size_t)repeated_rows + 1u, sizeof *repeated_held);
      if (!repeated_held)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = repeated_held;
      for (int at = 0; at < repeated_rows; ++at)
      {
         if (parse_int(field[0 + at], &repeated_held[at]) != 0)
         {
            free(scratch);
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         }
      }
      int produced = db1_interaction_event_mark_promoted(repeated_held, repeated_rows);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_INTERACTION_EVENT_EVICT_IF_NEEDED:
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
      rc = db1_interaction_event_evict_if_needed(parsed0);
      break;
   }
   case AIMEE_DB1_OP_GUARDRAIL_EVENT_INSERT:
   {
      if (count != 12u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      guardrail_event_t row;
      memset(&row, 0, sizeof row);
      snprintf(row.session_id, sizeof row.session_id, "%s", field[0]);
      snprintf(row.tool_name, sizeof row.tool_name, "%s", field[1]);
      double member_2 = 0;
      if (parse_double(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.overall_risk = member_2;
      double member_3 = 0;
      if (parse_double(field[3], &member_3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.action_risk = member_3;
      double member_4 = 0;
      if (parse_double(field[4], &member_4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.diff_risk = member_4;
      double member_5 = 0;
      if (parse_double(field[5], &member_5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.drift_risk = member_5;
      double member_6 = 0;
      if (parse_double(field[6], &member_6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.antipattern_similarity = member_6;
      snprintf(row.recommendation, sizeof row.recommendation, "%s", field[7]);
      snprintf(row.labels, sizeof row.labels, "%s", field[8]);
      snprintf(row.final_action, sizeof row.final_action, "%s", field[9]);
      snprintf(row.explanation, sizeof row.explanation, "%s", field[10]);
      int member_11 = 0;
      if (parse_int(field[11], &member_11) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.dry_run = member_11;
      rc = db1_guardrail_event_insert(&row);
      break;
   }
   case AIMEE_DB1_OP_GUARDRAIL_EVENT_COUNTS_7D:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_guardrail_event_counts_t, 0, sizeof row_guardrail_event_counts_t);
      rc = db1_guardrail_event_counts_7d(&row_guardrail_event_counts_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_guardrail_event_counts_t.dry_run);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_guardrail_event_counts_t.warn);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_guardrail_event_counts_t.prompt);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_guardrail_event_counts_t.block);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_text[3];
      rows = row_slots;
      row_count = 4u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_GUARDRAIL_EVENT_SESSION_ADVISORY_COUNT:
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
      rc = db1_guardrail_event_session_advisory_count(field[0], &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_GUARDRAIL_EVENT_LIST:
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
      if (parsed0 <= 0 || parsed0 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      guardrail_event_row_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      int produced_rows = 0;
      rc = db1_guardrail_event_list(parsed0, parsed1, found, &produced_rows);
      rc = (rc == 0) ? produced_rows : -1;
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 9u * sizeof *cells);
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
                     "%.17g", (double)found[row].overall_risk);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%d", found[row].dry_run);
            cells[row * 9u + 0u] = numbers[row * 3u + 0u];
            cells[row * 9u + 1u] = found[row].recorded_at;
            cells[row * 9u + 2u] = found[row].session_id;
            cells[row * 9u + 3u] = found[row].tool_name;
            cells[row * 9u + 4u] = numbers[row * 3u + 1u];
            cells[row * 9u + 5u] = found[row].labels;
            cells[row * 9u + 6u] = found[row].final_action;
            cells[row * 9u + 7u] = found[row].explanation;
            cells[row * 9u + 8u] = numbers[row * 3u + 2u];
         }
         rows = cells;
         row_count = produced * 9u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EVAL_RESULT_INSERT:
   {
      if (count != 19u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_eval_result_row_t row;
      memset(&row, 0, sizeof row);
      row.suite = field[0];
      row.task_name = field[1];
      row.agent_name = field[2];
      row.ablation = field[3];
      int member_4 = 0;
      if (parse_int(field[4], &member_4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.success = member_4;
      int member_5 = 0;
      if (parse_int(field[5], &member_5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.turns = member_5;
      int member_6 = 0;
      if (parse_int(field[6], &member_6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tool_calls = member_6;
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tool_call_failures = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.rescue_recoveries = member_8;
      int member_9 = 0;
      if (parse_int(field[9], &member_9) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.prompt_tokens = member_9;
      int member_10 = 0;
      if (parse_int(field[10], &member_10) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.completion_tokens = member_10;
      int member_11 = 0;
      if (parse_int(field[11], &member_11) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.latency_ms = member_11;
      row.response = field[12];
      row.error = field[13];
      row.dataset_hash = field[14];
      row.target_hash = field[15];
      row.harness_version = field[16];
      row.hardware_profile = field[17];
      int member_18 = 0;
      if (parse_int(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.seed = member_18;
      rc = db1_eval_result_insert(&row);
      break;
   }
   case AIMEE_DB1_OP_EVAL_FAILED_TASKS_RECENT:
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
      db1_eval_failed_task_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_eval_failed_tasks_recent(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 2u * sizeof *cells);
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
            cells[row * 2u + 0u] = found[row].task_name;
            cells[row * 2u + 1u] = found[row].error;
         }
         rows = cells;
         row_count = produced * 2u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EVAL_PASSED_TASKS_RECENT:
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
      db1_eval_passed_task_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_eval_passed_tasks_recent(found, parsed0);
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
            cells[row * 1u + 0u] = found[row].task_name;
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_EVAL_RESULTS_LIST:
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
      if (parsed1 <= 0 || parsed1 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_eval_display_row_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_eval_results_list(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 11u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 6u * sizeof *numbers);
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
            snprintf(numbers[row * 6u + 0u], 32,
                     "%d", found[row].success);
            snprintf(numbers[row * 6u + 1u], 32,
                     "%d", found[row].turns);
            snprintf(numbers[row * 6u + 2u], 32,
                     "%d", found[row].tool_calls);
            snprintf(numbers[row * 6u + 3u], 32,
                     "%d", found[row].tool_call_failures);
            snprintf(numbers[row * 6u + 4u], 32,
                     "%d", found[row].rescue_recoveries);
            snprintf(numbers[row * 6u + 5u], 32,
                     "%d", found[row].latency_ms);
            cells[row * 11u + 0u] = found[row].suite;
            cells[row * 11u + 1u] = found[row].task_name;
            cells[row * 11u + 2u] = found[row].agent_name;
            cells[row * 11u + 3u] = found[row].ablation;
            cells[row * 11u + 4u] = numbers[row * 6u + 0u];
            cells[row * 11u + 5u] = numbers[row * 6u + 1u];
            cells[row * 11u + 6u] = numbers[row * 6u + 2u];
            cells[row * 11u + 7u] = numbers[row * 6u + 3u];
            cells[row * 11u + 8u] = numbers[row * 6u + 4u];
            cells[row * 11u + 9u] = numbers[row * 6u + 5u];
            cells[row * 11u + 10u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 11u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_START:
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
      int produced = db1_diagnose_start(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_ADD_OBSERVATION:
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
      int produced = db1_diagnose_add_observation(parsed0, field[1], field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_ADD_HYPOTHESIS:
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
      int produced = db1_diagnose_add_hypothesis(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_ADD_EVIDENCE:
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
      if (!field[5][0])
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
      int parsed5;
      if (parse_int(field[5], &parsed5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_diagnose_add_evidence(parsed0, parsed1, field[2], field[3], field[4], parsed5);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_ADD_PROBE:
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
      int produced = db1_diagnose_add_probe(parsed0, parsed1, field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_GET:
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
      memset(&row_diagnosis_t, 0, sizeof row_diagnosis_t);
      rc = db1_diagnose_get(parsed0, &row_diagnosis_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_diagnosis_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)row_diagnosis_t.confidence);
      row_slots[0] = row_text[0];
      row_slots[1] = row_diagnosis_t.symptom;
      row_slots[2] = row_diagnosis_t.status;
      row_slots[3] = row_diagnosis_t.conclusion;
      row_slots[4] = row_text[1];
      row_slots[5] = row_diagnosis_t.created_at;
      row_slots[6] = row_diagnosis_t.updated_at;
      rows = row_slots;
      row_count = 7u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_LIST:
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
      diagnosis_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_diagnose_list(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 7u * sizeof *cells);
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
                     "%.17g", (double)found[row].confidence);
            cells[row * 7u + 0u] = numbers[row * 2u + 0u];
            cells[row * 7u + 1u] = found[row].symptom;
            cells[row * 7u + 2u] = found[row].status;
            cells[row * 7u + 3u] = found[row].conclusion;
            cells[row * 7u + 4u] = numbers[row * 2u + 1u];
            cells[row * 7u + 5u] = found[row].created_at;
            cells[row * 7u + 6u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 7u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_LIST_ITEMS:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      diagnosis_item_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_diagnose_list_items(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%d", found[row].id);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%d", found[row].diagnosis_id);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%d", found[row].parent_id);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%d", found[row].evidence_rank);
            cells[row * 8u + 0u] = numbers[row * 4u + 0u];
            cells[row * 8u + 1u] = numbers[row * 4u + 1u];
            cells[row * 8u + 2u] = found[row].kind;
            cells[row * 8u + 3u] = numbers[row * 4u + 2u];
            cells[row * 8u + 4u] = found[row].content;
            cells[row * 8u + 5u] = found[row].source;
            cells[row * 8u + 6u] = numbers[row * 4u + 3u];
            cells[row * 8u + 7u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_LIST_HYPOTHESES:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      diagnosis_item_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_diagnose_list_hypotheses(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%d", found[row].id);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%d", found[row].diagnosis_id);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%d", found[row].parent_id);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%d", found[row].evidence_rank);
            cells[row * 8u + 0u] = numbers[row * 4u + 0u];
            cells[row * 8u + 1u] = numbers[row * 4u + 1u];
            cells[row * 8u + 2u] = found[row].kind;
            cells[row * 8u + 3u] = numbers[row * 4u + 2u];
            cells[row * 8u + 4u] = found[row].content;
            cells[row * 8u + 5u] = found[row].source;
            cells[row * 8u + 6u] = numbers[row * 4u + 3u];
            cells[row * 8u + 7u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_RANK_HYPOTHESES:
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
      if (parsed1 <= 0 || parsed1 > 128)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      diagnosis_ranking_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_diagnose_rank_hypotheses(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 13u * sizeof *cells);
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
                     "%d", found[row].hypothesis.id);
            snprintf(numbers[row * 9u + 1u], 32,
                     "%d", found[row].hypothesis.diagnosis_id);
            snprintf(numbers[row * 9u + 2u], 32,
                     "%d", found[row].hypothesis.parent_id);
            snprintf(numbers[row * 9u + 3u], 32,
                     "%d", found[row].hypothesis.evidence_rank);
            snprintf(numbers[row * 9u + 4u], 32,
                     "%d", found[row].evidence_for_count);
            snprintf(numbers[row * 9u + 5u], 32,
                     "%d", found[row].evidence_against_count);
            snprintf(numbers[row * 9u + 6u], 32,
                     "%d", found[row].strongest_for_rank);
            snprintf(numbers[row * 9u + 7u], 32,
                     "%d", found[row].strongest_against_rank);
            snprintf(numbers[row * 9u + 8u], 32,
                     "%.17g", (double)found[row].confidence);
            cells[row * 13u + 0u] = numbers[row * 9u + 0u];
            cells[row * 13u + 1u] = numbers[row * 9u + 1u];
            cells[row * 13u + 2u] = found[row].hypothesis.kind;
            cells[row * 13u + 3u] = numbers[row * 9u + 2u];
            cells[row * 13u + 4u] = found[row].hypothesis.content;
            cells[row * 13u + 5u] = found[row].hypothesis.source;
            cells[row * 13u + 6u] = numbers[row * 9u + 3u];
            cells[row * 13u + 7u] = found[row].hypothesis.created_at;
            cells[row * 13u + 8u] = numbers[row * 9u + 4u];
            cells[row * 13u + 9u] = numbers[row * 9u + 5u];
            cells[row * 13u + 10u] = numbers[row * 9u + 6u];
            cells[row * 13u + 11u] = numbers[row * 9u + 7u];
            cells[row * 13u + 12u] = numbers[row * 9u + 8u];
         }
         rows = cells;
         row_count = produced * 13u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_CONCLUDE:
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
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
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
      rc = db1_diagnose_conclude(parsed0, field[1], parsed2);
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_ABANDON:
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
      rc = db1_diagnose_abandon(parsed0);
      break;
   }
   case AIMEE_DB1_OP_DIAGNOSE_SUGGEST_PROBES:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      diagnosis_probe_suggestion_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_diagnose_suggest_probes(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 3u * sizeof *cells);
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
                     "%d", found[row].hypothesis_a_id);
            snprintf(numbers[row * 2u + 1u], 32,
                     "%d", found[row].hypothesis_b_id);
            cells[row * 3u + 0u] = numbers[row * 2u + 0u];
            cells[row * 3u + 1u] = numbers[row * 2u + 1u];
            cells[row * 3u + 2u] = found[row].suggestion;
         }
         rows = cells;
         row_count = produced * 3u;
      }
      listed = 1;
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
      write_reply(response_body, response_capacity, response_len, status, out_values, out_count);
   }
   free(cells_owned);
   free(numeric_owned);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
