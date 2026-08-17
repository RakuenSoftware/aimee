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
#include "coord_jobs.h"
#include "db1_cron_jobs.h"
#include "db1_trigger.h"

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
   db1_trigger_run_t row_db1_trigger_run_t;
   db1_coord_task_t row_db1_coord_task_t;
   db1_coord_job_t row_db1_coord_job_t;
   cron_job_t row_cron_job_t;
   const char *row_slots[22];
   char row_text[10][32];
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
      reads = 1;
      found = 1;
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
   case AIMEE_DB1_OP_TRIGGER_INSERT:
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
      rc = db1_trigger_insert(field[0], field[1], field[2], field[3], field[4], field[5]);
      break;
   case AIMEE_DB1_OP_TRIGGER_STATUS_SET:
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
      rc = db1_trigger_status_set(field[0], field[1], field[2], field[3]);
      break;
   case AIMEE_DB1_OP_TRIGGER_GET:
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
      memset(&row_db1_trigger_run_t, 0, sizeof row_db1_trigger_run_t);
      rc = db1_trigger_get(field[0], &row_db1_trigger_run_t);
      row_slots[0] = row_db1_trigger_run_t.id;
      row_slots[1] = row_db1_trigger_run_t.source;
      row_slots[2] = row_db1_trigger_run_t.event;
      row_slots[3] = row_db1_trigger_run_t.task;
      row_slots[4] = row_db1_trigger_run_t.workspace;
      row_slots[5] = row_db1_trigger_run_t.metadata;
      row_slots[6] = row_db1_trigger_run_t.pipeline_id;
      row_slots[7] = row_db1_trigger_run_t.status;
      row_slots[8] = row_db1_trigger_run_t.queued_at;
      row_slots[9] = row_db1_trigger_run_t.started_at;
      row_slots[10] = row_db1_trigger_run_t.finished_at;
      row_slots[11] = row_db1_trigger_run_t.error;
      rows = row_slots;
      row_count = 12u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_TRIGGER_LIST_JSON:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char *produced = db1_trigger_list_json(field[0]);
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_CREATE:
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
      int64_t produced = db1_coord_job_create(parsed0, parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_ADD:
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
      int64_t produced = db1_coord_job_add_task(parsed0, parsed1, field[2], field[3], field[4], field[5], field[6]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_CLAIM_NEXT:
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
      memset(&row_db1_coord_task_t, 0, sizeof row_db1_coord_task_t);
      rc = db1_coord_job_claim_next(parsed0, field[1], &row_db1_coord_task_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_coord_task_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_coord_task_t.job_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_coord_task_t.step_id);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_db1_coord_task_t.preempt_requeues);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_text[2];
      row_slots[3] = row_db1_coord_task_t.status;
      row_slots[4] = row_db1_coord_task_t.claimed_by;
      row_slots[5] = row_db1_coord_task_t.claimed_at;
      row_slots[6] = row_db1_coord_task_t.files;
      row_slots[7] = row_db1_coord_task_t.result;
      row_slots[8] = row_db1_coord_task_t.error;
      row_slots[9] = row_text[3];
      row_slots[10] = row_db1_coord_task_t.created_at;
      rows = row_slots;
      row_count = 11u;
      reads = 1;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_COMPLETE:
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
      rc = db1_coord_job_complete_task(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_FAIL:
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
      rc = db1_coord_job_fail_task(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_COMPLETE_OWNED:
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
      rc = db1_coord_job_complete_task_owned(parsed0, field[1], field[2]);
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_FAIL_OWNED:
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
      rc = db1_coord_job_fail_task_owned(parsed0, field[1], field[2]);
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_RELEASE:
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
      rc = db1_coord_job_release_task(parsed0);
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_RELEASE_BOUNDED:
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
      rc = db1_coord_job_release_task_bounded(parsed0, parsed1);
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_RELEASE_BOUNDED_OWNED:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_coord_job_release_task_bounded_owned(parsed0, field[1], parsed2);
      break;
   }
   case AIMEE_DB1_OP_COORD_OWNER_RECOVER:
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
      int scalar0 = 0;
      int scalar1 = 0;
      rc = db1_coord_job_recover_owner(field[0], parsed1, &scalar0, &scalar1);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%d", scalar1);
      row_slots[1] = row_text[1];
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_GET:
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
      memset(&row_db1_coord_job_t, 0, sizeof row_db1_coord_job_t);
      rc = db1_coord_job_get(parsed0, &row_db1_coord_job_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_coord_job_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_coord_job_t.plan_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_coord_job_t.max_concurrent);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_db1_coord_job_t.total_tasks);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_db1_coord_job_t.done_tasks);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_db1_coord_job_t.failed_tasks);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_db1_coord_job_t.running_tasks);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_db1_coord_job_t.status;
      row_slots[3] = row_text[2];
      row_slots[4] = row_db1_coord_job_t.created_at;
      row_slots[5] = row_db1_coord_job_t.updated_at;
      row_slots[6] = row_text[3];
      row_slots[7] = row_text[4];
      row_slots[8] = row_text[5];
      row_slots[9] = row_text[6];
      rows = row_slots;
      row_count = 10u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_COORD_TASK_LIST:
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
      db1_coord_task_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_coord_job_list_tasks(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 11u * sizeof *cells);
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
                     "%d", found[row].job_id);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%d", found[row].step_id);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%d", found[row].preempt_requeues);
            cells[row * 11u + 0u] = numbers[row * 4u + 0u];
            cells[row * 11u + 1u] = numbers[row * 4u + 1u];
            cells[row * 11u + 2u] = numbers[row * 4u + 2u];
            cells[row * 11u + 3u] = found[row].status;
            cells[row * 11u + 4u] = found[row].claimed_by;
            cells[row * 11u + 5u] = found[row].claimed_at;
            cells[row * 11u + 6u] = found[row].files;
            cells[row * 11u + 7u] = found[row].result;
            cells[row * 11u + 8u] = found[row].error;
            cells[row * 11u + 9u] = numbers[row * 4u + 3u];
            cells[row * 11u + 10u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 11u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_CANCEL:
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
      rc = db1_coord_job_cancel(parsed0);
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_REFRESH_STATUS:
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
      rc = db1_coord_job_refresh_status(parsed0);
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_FILE_CONFLICT:
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
      rc = db1_coord_job_has_file_conflict(parsed0, field[1]);
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_LIST_RECENT:
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
      db1_coord_job_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_coord_job_list_recent(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 10u * sizeof *cells);
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
                     "%d", found[row].id);
            snprintf(numbers[row * 7u + 1u], 32,
                     "%d", found[row].plan_id);
            snprintf(numbers[row * 7u + 2u], 32,
                     "%d", found[row].max_concurrent);
            snprintf(numbers[row * 7u + 3u], 32,
                     "%d", found[row].total_tasks);
            snprintf(numbers[row * 7u + 4u], 32,
                     "%d", found[row].done_tasks);
            snprintf(numbers[row * 7u + 5u], 32,
                     "%d", found[row].failed_tasks);
            snprintf(numbers[row * 7u + 6u], 32,
                     "%d", found[row].running_tasks);
            cells[row * 10u + 0u] = numbers[row * 7u + 0u];
            cells[row * 10u + 1u] = numbers[row * 7u + 1u];
            cells[row * 10u + 2u] = found[row].status;
            cells[row * 10u + 3u] = numbers[row * 7u + 2u];
            cells[row * 10u + 4u] = found[row].created_at;
            cells[row * 10u + 5u] = found[row].updated_at;
            cells[row * 10u + 6u] = numbers[row * 7u + 3u];
            cells[row * 10u + 7u] = numbers[row * 7u + 4u];
            cells[row * 10u + 8u] = numbers[row * 7u + 5u];
            cells[row * 10u + 9u] = numbers[row * 7u + 6u];
         }
         rows = cells;
         row_count = produced * 10u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_COORD_JOB_LIST_ACTIVE_IDS:
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
      int *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_coord_job_list_active_ids(found, parsed0);
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
   case AIMEE_DB1_OP_COORD_TASK_GET_DISPATCH:
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
      scalar_owned = calloc(1u, DB1_COORD_ROLE_LEN + DB1_COORD_PROMPT_LEN + DB1_COORD_FILES_LEN + DB1_COORD_CWD_LEN + DB1_COORD_ROLE_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      char *scalar1 = scalar_owned + DB1_COORD_ROLE_LEN;
      char *scalar2 = scalar_owned + DB1_COORD_ROLE_LEN + DB1_COORD_PROMPT_LEN;
      char *scalar3 = scalar_owned + DB1_COORD_ROLE_LEN + DB1_COORD_PROMPT_LEN + DB1_COORD_FILES_LEN;
      char *scalar4 = scalar_owned + DB1_COORD_ROLE_LEN + DB1_COORD_PROMPT_LEN + DB1_COORD_FILES_LEN + DB1_COORD_CWD_LEN;
      rc = db1_coord_task_get_dispatch(parsed0, scalar0, (size_t)DB1_COORD_ROLE_LEN, scalar1, (size_t)DB1_COORD_PROMPT_LEN, scalar2, (size_t)DB1_COORD_FILES_LEN, scalar3, (size_t)DB1_COORD_CWD_LEN, scalar4, (size_t)DB1_COORD_ROLE_LEN);
      row_slots[0] = scalar0;
      row_slots[1] = scalar1;
      row_slots[2] = scalar2;
      row_slots[3] = scalar3;
      row_slots[4] = scalar4;
      rows = row_slots;
      row_count = 5u;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_UPSERT:
   {
      if (count != 22u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      cron_job_t row;
      memset(&row, 0, sizeof row);
      snprintf(row.id, sizeof row.id, "%s", field[0]);
      snprintf(row.schedule, sizeof row.schedule, "%s", field[1]);
      snprintf(row.mode, sizeof row.mode, "%s", field[2]);
      snprintf(row.script, sizeof row.script, "%s", field[3]);
      snprintf(row.prompt, sizeof row.prompt, "%s", field[4]);
      snprintf(row.workdir, sizeof row.workdir, "%s", field[5]);
      snprintf(row.context_from, sizeof row.context_from, "%s", field[6]);
      snprintf(row.when_context_contains, sizeof row.when_context_contains, "%s", field[7]);
      snprintf(row.skills[0], sizeof row.skills[0], "%s", field[8]);
      snprintf(row.skills[1], sizeof row.skills[1], "%s", field[9]);
      snprintf(row.skills[2], sizeof row.skills[2], "%s", field[10]);
      snprintf(row.skills[3], sizeof row.skills[3], "%s", field[11]);
      snprintf(row.skills[4], sizeof row.skills[4], "%s", field[12]);
      snprintf(row.skills[5], sizeof row.skills[5], "%s", field[13]);
      snprintf(row.skills[6], sizeof row.skills[6], "%s", field[14]);
      snprintf(row.skills[7], sizeof row.skills[7], "%s", field[15]);
      if (parse_int(field[16], &row.skill_count) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      snprintf(row.deliver_target, sizeof row.deliver_target, "%s", field[17]);
      if (parse_int(field[18], &row.deliver_only_if_changed) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[19], &row.deliver_first_run_silent) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[20], &row.pre_wake_gate) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[21], &row.enabled) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_cron_job_upsert(&row);
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_GET:
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
      memset(&row_cron_job_t, 0, sizeof row_cron_job_t);
      rc = db1_cron_job_get(field[0], &row_cron_job_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_cron_job_t.skill_count);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_cron_job_t.deliver_only_if_changed);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_cron_job_t.deliver_first_run_silent);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_cron_job_t.pre_wake_gate);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_cron_job_t.enabled);
      row_slots[0] = row_cron_job_t.id;
      row_slots[1] = row_cron_job_t.schedule;
      row_slots[2] = row_cron_job_t.mode;
      row_slots[3] = row_cron_job_t.script;
      row_slots[4] = row_cron_job_t.prompt;
      row_slots[5] = row_cron_job_t.workdir;
      row_slots[6] = row_cron_job_t.context_from;
      row_slots[7] = row_cron_job_t.when_context_contains;
      row_slots[8] = row_cron_job_t.skills[0];
      row_slots[9] = row_cron_job_t.skills[1];
      row_slots[10] = row_cron_job_t.skills[2];
      row_slots[11] = row_cron_job_t.skills[3];
      row_slots[12] = row_cron_job_t.skills[4];
      row_slots[13] = row_cron_job_t.skills[5];
      row_slots[14] = row_cron_job_t.skills[6];
      row_slots[15] = row_cron_job_t.skills[7];
      row_slots[16] = row_text[0];
      row_slots[17] = row_cron_job_t.deliver_target;
      row_slots[18] = row_text[1];
      row_slots[19] = row_text[2];
      row_slots[20] = row_text[3];
      row_slots[21] = row_text[4];
      rows = row_slots;
      row_count = 22u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_LOAD:
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
      if (parsed0 <= 0 || parsed0 > 32)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      cron_job_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_cron_jobs_load(found, parsed0, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 22u * sizeof *cells);
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
                     "%d", found[row].skill_count);
            snprintf(numbers[row * 5u + 1u], 32,
                     "%d", found[row].deliver_only_if_changed);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%d", found[row].deliver_first_run_silent);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%d", found[row].pre_wake_gate);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].enabled);
            cells[row * 22u + 0u] = found[row].id;
            cells[row * 22u + 1u] = found[row].schedule;
            cells[row * 22u + 2u] = found[row].mode;
            cells[row * 22u + 3u] = found[row].script;
            cells[row * 22u + 4u] = found[row].prompt;
            cells[row * 22u + 5u] = found[row].workdir;
            cells[row * 22u + 6u] = found[row].context_from;
            cells[row * 22u + 7u] = found[row].when_context_contains;
            cells[row * 22u + 8u] = found[row].skills[0];
            cells[row * 22u + 9u] = found[row].skills[1];
            cells[row * 22u + 10u] = found[row].skills[2];
            cells[row * 22u + 11u] = found[row].skills[3];
            cells[row * 22u + 12u] = found[row].skills[4];
            cells[row * 22u + 13u] = found[row].skills[5];
            cells[row * 22u + 14u] = found[row].skills[6];
            cells[row * 22u + 15u] = found[row].skills[7];
            cells[row * 22u + 16u] = numbers[row * 5u + 0u];
            cells[row * 22u + 17u] = found[row].deliver_target;
            cells[row * 22u + 18u] = numbers[row * 5u + 1u];
            cells[row * 22u + 19u] = numbers[row * 5u + 2u];
            cells[row * 22u + 20u] = numbers[row * 5u + 3u];
            cells[row * 22u + 21u] = numbers[row * 5u + 4u];
         }
         rows = cells;
         row_count = produced * 22u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_SET_ENABLED:
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
      rc = db1_cron_job_set_enabled(field[0], parsed1);
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_SET_ENABLED_ALL:
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
      rc = db1_cron_jobs_set_enabled_all(parsed0);
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_DELETE:
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
      rc = db1_cron_job_delete(field[0]);
      break;
   case AIMEE_DB1_OP_CRON_JOB_RECORD_RUN:
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
      int64_t produced = db1_cron_job_record_run(field[0], field[1], parsed2, parsed3, field[4], field[5], field[6]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_LIST_JSON:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char *produced = db1_cron_jobs_list_json();
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_HISTORY_JSON:
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
      char *produced = db1_cron_job_history_json(field[0], parsed1);
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_LATEST_OUTPUT:
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
      char *produced = db1_cron_job_latest_output(field[0]);
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_CRON_JOB_LAST_OUTPUT_HASH:
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
      char *produced = db1_cron_job_last_output_hash(field[0]);
      rc = produced ? 1 : 0;
      text_owned = produced;
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
   free(scalar_owned);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
