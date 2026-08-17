/* modules/db1/agent_work_stage.c: the agent work stage handler.
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
#include "agent_log.h"
#include "cognify_jobs.h"

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

aimee_module_status_t aimee_db1_stage_agent_work(const uint8_t *request_body, uint32_t request_len,
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
   db1_cognify_job_stats_t row_db1_cognify_job_stats_t;
   db1_cognify_job_t row_db1_cognify_job_t;
   db1_agent_log_hud_t row_db1_agent_log_hud_t;
   db1_agent_log_stats_t row_db1_agent_log_stats_t;
   const char *row_slots[10];
   char row_text[10][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;

   switch (op)
   {
   case AIMEE_DB1_OP_COGNIFY_ENQUEUE:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_cognify_job_enqueue(parsed0);
      break;
   }
   case AIMEE_DB1_OP_COGNIFY_STATUS:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_cognify_job_stats_t, 0, sizeof row_db1_cognify_job_stats_t);
      rc = db1_cognify_job_status(&row_db1_cognify_job_stats_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_cognify_job_stats_t.pending);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_cognify_job_stats_t.running);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_cognify_job_stats_t.done);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_db1_cognify_job_stats_t.failed);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_db1_cognify_job_stats_t.total);
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
   case AIMEE_DB1_OP_COGNIFY_CLAIM_NEXT:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_cognify_job_t, 0, sizeof row_db1_cognify_job_t);
      rc = db1_cognify_job_claim_next(&row_db1_cognify_job_t);
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)row_db1_cognify_job_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_db1_cognify_job_t.memory_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_cognify_job_t.attempts);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_db1_cognify_job_t.max_attempts);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_text[3];
      row_slots[4] = row_db1_cognify_job_t.kind;
      row_slots[5] = row_db1_cognify_job_t.status;
      row_slots[6] = row_db1_cognify_job_t.claimed_by;
      row_slots[7] = row_db1_cognify_job_t.claimed_at;
      row_slots[8] = row_db1_cognify_job_t.last_error;
      rows = row_slots;
      row_count = 9u;
      found = 1;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_COGNIFY_MARK:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_cognify_job_mark(parsed0, field[1], field[2]);
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_INSERT:
   {
      if (count != 11u)
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
      if (!field[5][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[7][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[8][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[9][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_insert_row_t row;
      memset(&row, 0, sizeof row);
      row.agent_name = field[0];
      row.role = field[1];
      if (parse_int(field[2], &row.prompt_tokens) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[3], &row.completion_tokens) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[4], &row.latency_ms) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[5], &row.success) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.error = field[6];
      if (parse_int(field[7], &row.turns) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[8], &row.tool_calls) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[9], &row.confidence) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.session_id = field[10];
      int64_t produced = db1_agent_log_insert(&row);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_LIST_RECENT:
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
      if (parsed0 <= 0 || parsed0 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_display_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_list_recent(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 13u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 8u * sizeof *numbers);
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
            snprintf(numbers[row * 8u + 0u], 32,
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 8u + 1u], 32,
                     "%d", found[row].prompt_tokens);
            snprintf(numbers[row * 8u + 2u], 32,
                     "%d", found[row].completion_tokens);
            snprintf(numbers[row * 8u + 3u], 32,
                     "%d", found[row].latency_ms);
            snprintf(numbers[row * 8u + 4u], 32,
                     "%d", found[row].success);
            snprintf(numbers[row * 8u + 5u], 32,
                     "%d", found[row].turns);
            snprintf(numbers[row * 8u + 6u], 32,
                     "%d", found[row].tool_calls);
            snprintf(numbers[row * 8u + 7u], 32,
                     "%d", found[row].confidence);
            cells[row * 13u + 0u] = numbers[row * 8u + 0u];
            cells[row * 13u + 1u] = found[row].agent_name;
            cells[row * 13u + 2u] = found[row].role;
            cells[row * 13u + 3u] = numbers[row * 8u + 1u];
            cells[row * 13u + 4u] = numbers[row * 8u + 2u];
            cells[row * 13u + 5u] = numbers[row * 8u + 3u];
            cells[row * 13u + 6u] = numbers[row * 8u + 4u];
            cells[row * 13u + 7u] = numbers[row * 8u + 5u];
            cells[row * 13u + 8u] = numbers[row * 8u + 6u];
            cells[row * 13u + 9u] = numbers[row * 8u + 7u];
            cells[row * 13u + 10u] = found[row].session_id;
            cells[row * 13u + 11u] = found[row].created_at;
            cells[row * 13u + 12u] = found[row].error;
         }
         rows = cells;
         row_count = produced * 13u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_LIST_BY_SESSION:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_display_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_list_by_session(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 13u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 8u * sizeof *numbers);
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
            snprintf(numbers[row * 8u + 0u], 32,
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 8u + 1u], 32,
                     "%d", found[row].prompt_tokens);
            snprintf(numbers[row * 8u + 2u], 32,
                     "%d", found[row].completion_tokens);
            snprintf(numbers[row * 8u + 3u], 32,
                     "%d", found[row].latency_ms);
            snprintf(numbers[row * 8u + 4u], 32,
                     "%d", found[row].success);
            snprintf(numbers[row * 8u + 5u], 32,
                     "%d", found[row].turns);
            snprintf(numbers[row * 8u + 6u], 32,
                     "%d", found[row].tool_calls);
            snprintf(numbers[row * 8u + 7u], 32,
                     "%d", found[row].confidence);
            cells[row * 13u + 0u] = numbers[row * 8u + 0u];
            cells[row * 13u + 1u] = found[row].agent_name;
            cells[row * 13u + 2u] = found[row].role;
            cells[row * 13u + 3u] = numbers[row * 8u + 1u];
            cells[row * 13u + 4u] = numbers[row * 8u + 2u];
            cells[row * 13u + 5u] = numbers[row * 8u + 3u];
            cells[row * 13u + 6u] = numbers[row * 8u + 4u];
            cells[row * 13u + 7u] = numbers[row * 8u + 5u];
            cells[row * 13u + 8u] = numbers[row * 8u + 6u];
            cells[row * 13u + 9u] = numbers[row * 8u + 7u];
            cells[row * 13u + 10u] = found[row].session_id;
            cells[row * 13u + 11u] = found[row].created_at;
            cells[row * 13u + 12u] = found[row].error;
         }
         rows = cells;
         row_count = produced * 13u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_SEARCH_SESSIONS_BY_ROLE:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_AL_SESSION_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_search_session_ids_by_role(field[0], found, parsed1);
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
   case AIMEE_DB1_OP_AGENT_LOG_COUNT_PER_ROLE:
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
      db1_agent_log_role_count_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_count_per_role(field[0], found, parsed1);
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
                     "%d", found[row].count);
            cells[row * 2u + 0u] = found[row].role;
            cells[row * 2u + 1u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 2u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_FAILURES_SINCE:
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
      if (parsed0 <= 0 || parsed0 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_failure_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_failures_since_seconds(parsed0, parsed1, found);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 3u * sizeof *cells);
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
            cells[row * 3u + 0u] = found[row].role;
            cells[row * 3u + 1u] = found[row].error;
            cells[row * 3u + 2u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 3u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_LIST_RECENT_ERRORS:
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
      db1_agent_log_recent_error_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_list_recent_errors(parsed0, found, parsed1);
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
            cells[row * 1u + 0u] = found[row].error;
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_DELEGATION_PATTERNS:
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
      if (parsed2 <= 0 || parsed2 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_delegation_pattern_t *found = calloc((size_t)parsed2, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_list_delegation_patterns(parsed0, parsed1, found, parsed2);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed2)
                                 ? (uint32_t)rc : (uint32_t)parsed2;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%d", found[row].wins);
            snprintf(numbers[row * 5u + 1u], 32,
                     "%d", found[row].fails);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%d", found[row].total);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%d", found[row].avg_turns);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].avg_tools);
            cells[row * 8u + 0u] = found[row].role;
            cells[row * 8u + 1u] = found[row].agent_name;
            cells[row * 8u + 2u] = numbers[row * 5u + 0u];
            cells[row * 8u + 3u] = numbers[row * 5u + 1u];
            cells[row * 8u + 4u] = numbers[row * 5u + 2u];
            cells[row * 8u + 5u] = numbers[row * 5u + 3u];
            cells[row * 8u + 6u] = numbers[row * 5u + 4u];
            cells[row * 8u + 7u] = found[row].recent_error;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_FAILURE_SEEDS:
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
      if (parsed2 <= 0 || parsed2 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_failure_episode_seed_t *found = calloc((size_t)parsed2, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_list_failure_episode_seeds(parsed0, parsed1, found, parsed2);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed2)
                                 ? (uint32_t)rc : (uint32_t)parsed2;
         const char **cells = malloc((size_t)produced * 4u * sizeof *cells);
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
                     "%d", found[row].fails);
            cells[row * 4u + 0u] = found[row].role;
            cells[row * 4u + 1u] = found[row].agent_name;
            cells[row * 4u + 2u] = numbers[row * 1u + 0u];
            cells[row * 4u + 3u] = found[row].errors;
         }
         rows = cells;
         row_count = produced * 4u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_METRICS_BY_ROLE:
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
      if (parsed0 <= 0 || parsed0 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_metric_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_metrics_by_role(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 7u * sizeof *numbers);
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
            snprintf(numbers[row * 7u + 0u], 32,
                     "%d", found[row].total);
            snprintf(numbers[row * 7u + 1u], 32,
                     "%d", found[row].successes);
            snprintf(numbers[row * 7u + 2u], 32,
                     "%d", found[row].avg_latency_ms);
            snprintf(numbers[row * 7u + 3u], 32,
                     "%lld", (long long)found[row].tokens);
            snprintf(numbers[row * 7u + 4u], 32,
                     "%lld", (long long)found[row].cache_write_tokens);
            snprintf(numbers[row * 7u + 5u], 32,
                     "%lld", (long long)found[row].cache_read_tokens);
            snprintf(numbers[row * 7u + 6u], 32,
                     "%.17g", (double)found[row].estimated_cost_usd);
            cells[row * 8u + 0u] = found[row].role;
            cells[row * 8u + 1u] = numbers[row * 7u + 0u];
            cells[row * 8u + 2u] = numbers[row * 7u + 1u];
            cells[row * 8u + 3u] = numbers[row * 7u + 2u];
            cells[row * 8u + 4u] = numbers[row * 7u + 3u];
            cells[row * 8u + 5u] = numbers[row * 7u + 4u];
            cells[row * 8u + 6u] = numbers[row * 7u + 5u];
            cells[row * 8u + 7u] = numbers[row * 7u + 6u];
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_AGENT_STATS:
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
      db1_agent_log_agent_stats_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_agent_stats(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 9u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 8u * sizeof *numbers);
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
            snprintf(numbers[row * 8u + 0u], 32,
                     "%d", found[row].total_calls);
            snprintf(numbers[row * 8u + 1u], 32,
                     "%d", found[row].total_prompt_tokens);
            snprintf(numbers[row * 8u + 2u], 32,
                     "%d", found[row].total_completion_tokens);
            snprintf(numbers[row * 8u + 3u], 32,
                     "%d", found[row].avg_latency_ms);
            snprintf(numbers[row * 8u + 4u], 32,
                     "%.17g", (double)found[row].success_rate);
            snprintf(numbers[row * 8u + 5u], 32,
                     "%lld", (long long)found[row].total_cache_write_tokens);
            snprintf(numbers[row * 8u + 6u], 32,
                     "%lld", (long long)found[row].total_cache_read_tokens);
            snprintf(numbers[row * 8u + 7u], 32,
                     "%.17g", (double)found[row].total_estimated_cost_usd);
            cells[row * 9u + 0u] = found[row].agent_name;
            cells[row * 9u + 1u] = numbers[row * 8u + 0u];
            cells[row * 9u + 2u] = numbers[row * 8u + 1u];
            cells[row * 9u + 3u] = numbers[row * 8u + 2u];
            cells[row * 9u + 4u] = numbers[row * 8u + 3u];
            cells[row * 9u + 5u] = numbers[row * 8u + 4u];
            cells[row * 9u + 6u] = numbers[row * 8u + 5u];
            cells[row * 9u + 7u] = numbers[row * 8u + 6u];
            cells[row * 9u + 8u] = numbers[row * 8u + 7u];
         }
         rows = cells;
         row_count = produced * 9u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_HUD_SUMMARY:
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
      memset(&row_db1_agent_log_hud_t, 0, sizeof row_db1_agent_log_hud_t);
      rc = db1_agent_log_hud_summary(&row_db1_agent_log_hud_t, parsed0);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_agent_log_hud_t.total_calls);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_agent_log_hud_t.successful_calls);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_agent_log_hud_t.failed_calls);
      snprintf(row_text[3], sizeof row_text[3], "%lld", (long long)row_db1_agent_log_hud_t.total_prompt_tokens);
      snprintf(row_text[4], sizeof row_text[4], "%lld", (long long)row_db1_agent_log_hud_t.total_completion_tokens);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_db1_agent_log_hud_t.total_turns);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_db1_agent_log_hud_t.total_tool_calls);
      snprintf(row_text[7], sizeof row_text[7], "%.17g", (double)row_db1_agent_log_hud_t.avg_latency_ms);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_db1_agent_log_hud_t.recent_calls);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_db1_agent_log_hud_t.recent_successes);
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
      rows = row_slots;
      row_count = 10u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_SESSION_OUTCOME:
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
      rc = db1_agent_log_session_outcome(field[0], &scalar0, &scalar1);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%d", scalar1);
      row_slots[1] = row_text[1];
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_PROMETHEUS:
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
      if (parsed0 <= 0 || parsed0 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_log_prometheus_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_prometheus(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%d", found[row].total);
            snprintf(numbers[row * 6u + 1u], 32,
                     "%d", found[row].successes);
            snprintf(numbers[row * 6u + 2u], 32,
                     "%d", found[row].prompt_tokens);
            snprintf(numbers[row * 6u + 3u], 32,
                     "%d", found[row].completion_tokens);
            snprintf(numbers[row * 6u + 4u], 32,
                     "%d", found[row].avg_latency_ms);
            snprintf(numbers[row * 6u + 5u], 32,
                     "%d", found[row].tool_calls);
            cells[row * 8u + 0u] = found[row].agent_name;
            cells[row * 8u + 1u] = found[row].role;
            cells[row * 8u + 2u] = numbers[row * 6u + 0u];
            cells[row * 8u + 3u] = numbers[row * 6u + 1u];
            cells[row * 8u + 4u] = numbers[row * 6u + 2u];
            cells[row * 8u + 5u] = numbers[row * 6u + 3u];
            cells[row * 8u + 6u] = numbers[row * 6u + 4u];
            cells[row * 8u + 7u] = numbers[row * 6u + 5u];
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_STATS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_agent_log_stats_t, 0, sizeof row_db1_agent_log_stats_t);
      rc = db1_agent_log_stats(field[0], &row_db1_agent_log_stats_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_agent_log_stats_t.total);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_db1_agent_log_stats_t.turns);
      snprintf(row_text[2], sizeof row_text[2], "%lld", (long long)row_db1_agent_log_stats_t.tool_calls);
      snprintf(row_text[3], sizeof row_text[3], "%lld", (long long)row_db1_agent_log_stats_t.prompt_tokens);
      snprintf(row_text[4], sizeof row_text[4], "%lld", (long long)row_db1_agent_log_stats_t.completion_tokens);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_db1_agent_log_stats_t.successes);
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
