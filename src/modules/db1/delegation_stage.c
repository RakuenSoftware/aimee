/* modules/db1/delegation_stage.c: the delegation stage handler.
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
#include "agent_jobs.h"
#include "delegate_learning.h"
#include "delegate_reservation.h"
#include "delegation_checkpoint.h"
#include "delegations.h"

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

aimee_module_status_t aimee_db1_stage_delegation(const uint8_t *request_body, uint32_t request_len,
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
   db1_agent_job_t row_db1_agent_job_t;
   const char *row_slots[16];
   char row_text[5][32];
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
   /* Members the domain allocated with the row. They are released
      after the reply is written, not before: write_reply reads them. */
   char *member_owned[2] = {0};

   switch (op)
   {
   case AIMEE_DB1_OP_DELEGATION_MESSAGE_RECORD:
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
      rc = db1_delegation_message_record(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_DELEGATION_SPAWN_RECORD:
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
      if (!field[3][0])
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
      rc = db1_delegation_spawn_record(field[0], field[1], field[2], parsed3, field[4]);
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_COMPLETE:
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
      rc = db1_delegation_spawn_complete(field[0]);
      break;
   case AIMEE_DB1_OP_DELEGATION_SPAWN_PREEMPT:
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
      int produced = db1_delegation_spawn_preempt(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_STATUS:
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
      rc = db1_delegation_spawn_status(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_DELEGATION_SPAWN_STOP_REASON:
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
      rc = db1_delegation_spawn_stop_reason(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_DELEGATION_SPAWN_IS_STOPPED:
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
      int produced = db1_delegation_spawn_is_stopped(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_IS_CANCELLED:
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
      int produced = db1_delegation_spawn_is_cancelled(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_IS_ACTIVE:
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
      int produced = db1_delegation_spawn_is_active(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_COUNT_TOTAL:
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
      int produced = db1_delegation_spawn_count_total(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_FIND_ROOT:
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
      rc = db1_delegation_spawn_find_root(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_DELEGATION_SPAWN_COUNT_DESCENDANTS:
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
      int produced = db1_delegation_spawn_count_descendants(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_LIST_ACTIVE:
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
      rc = db1_delegation_spawn_list_active(found, parsed0);
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
   case AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_BY_ID:
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
      int produced = db1_delegation_spawn_cancel_by_id(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_RECURSIVE:
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
      int produced = db1_delegation_spawn_cancel_recursive(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_STALE:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_delegation_spawn_cancel_stale();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATE_RESERVATION_GET:
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
      scalar_owned = calloc(1u, DB1_AJ_PARTICIPANT_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar1 = scalar_owned;
      int scalar0 = 0;
      rc = db1_delegate_reservation_get(field[0], &scalar0, scalar1, (size_t)DB1_AJ_PARTICIPANT_LEN);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      row_slots[1] = scalar1;
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATE_RESERVATION_ADOPT:
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
      scalar_owned = calloc(1u, DB1_AJ_PARTICIPANT_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar1 = scalar_owned;
      int scalar0 = 0;
      rc = db1_delegate_reservation_adopt_sole_legacy(field[0], field[1], &scalar0, scalar1, (size_t)DB1_AJ_PARTICIPANT_LEN);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      row_slots[1] = scalar1;
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATE_RESERVATION_SAVE:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_delegate_reservation_save(field[0], field[1], parsed2, field[3]);
      break;
   }
   case AIMEE_DB1_OP_DELEGATE_RESERVATION_FORGET:
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
      rc = db1_delegate_reservation_forget(field[0]);
      break;
   case AIMEE_DB1_OP_DELEGATE_RESERVATION_FORGET_IF_MATCHES:
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
      int produced = db1_delegate_reservation_forget_if_matches(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_CHECKPOINT_SAVE:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_delegation_checkpoint_save(field[0], field[1], parsed2, field[3], field[4], field[5]);
      break;
   }
   case AIMEE_DB1_OP_DELEGATION_CHECKPOINT_LOAD:
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
      scalar_owned = calloc(1u, DB1_CHECKPOINT_STEPS_LEN + DB1_CHECKPOINT_ERROR_LEN + DB1_CHECKPOINT_OUTPUT_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      char *scalar1 = scalar_owned + DB1_CHECKPOINT_STEPS_LEN;
      char *scalar2 = scalar_owned + DB1_CHECKPOINT_STEPS_LEN + DB1_CHECKPOINT_ERROR_LEN;
      rc = db1_delegation_checkpoint_load(field[0], scalar0, (size_t)DB1_CHECKPOINT_STEPS_LEN, scalar1, (size_t)DB1_CHECKPOINT_ERROR_LEN, scalar2, (size_t)DB1_CHECKPOINT_OUTPUT_LEN);
      row_slots[0] = scalar0;
      row_slots[1] = scalar1;
      row_slots[2] = scalar2;
      rows = row_slots;
      row_count = 3u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_CREATE:
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
      int produced = db1_agent_job_create(field[0], field[1], field[2], field[3]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_UPDATE:
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
      db1_agent_job_update(parsed0, field[1], parsed2, field[3]);
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_COMPLETE:
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
      rc = db1_agent_job_complete(parsed0, field[1], parsed2, field[3], parsed4, parsed5);
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_SET_AGENT:
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
      db1_agent_job_set_agent(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT:
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
      db1_agent_job_heartbeat(parsed0);
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT_EXT:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_job_heartbeat_ext(parsed0, field[1], parsed2);
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_IS_CANCELLED:
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
      int produced = db1_agent_job_is_cancelled(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_CLASSIFY_STALE:
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
      rc = db1_agent_job_classify_stale(parsed0, parsed1, parsed2, value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_GET:
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
      memset(&row_db1_agent_job_t, 0, sizeof row_db1_agent_job_t);
      rc = db1_agent_job_get(parsed0, &row_db1_agent_job_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_agent_job_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_agent_job_t.cursor_turn);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_agent_job_t.api_call_count);
      snprintf(row_text[3], sizeof row_text[3], "%.17g", (double)row_db1_agent_job_t.cost_usd);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_db1_agent_job_t.cost_known);
      row_slots[0] = row_text[0];
      row_slots[1] = row_db1_agent_job_t.role;
      member_owned[0] = row_db1_agent_job_t.prompt;
      row_slots[2] = row_db1_agent_job_t.prompt ? row_db1_agent_job_t.prompt : "";
      row_slots[3] = row_db1_agent_job_t.agent_name;
      row_slots[4] = row_db1_agent_job_t.participant_token;
      row_slots[5] = row_db1_agent_job_t.status;
      member_owned[1] = row_db1_agent_job_t.result;
      row_slots[6] = row_db1_agent_job_t.result ? row_db1_agent_job_t.result : "";
      row_slots[7] = row_text[1];
      row_slots[8] = row_db1_agent_job_t.lease_owner;
      row_slots[9] = row_db1_agent_job_t.heartbeat_at;
      row_slots[10] = row_db1_agent_job_t.current_tool;
      row_slots[11] = row_text[2];
      row_slots[12] = row_text[3];
      row_slots[13] = row_text[4];
      row_slots[14] = row_db1_agent_job_t.created_at;
      row_slots[15] = row_db1_agent_job_t.updated_at;
      rows = row_slots;
      row_count = 16u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_GET_BY_PARTICIPANT:
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
      memset(&row_db1_agent_job_t, 0, sizeof row_db1_agent_job_t);
      rc = db1_agent_job_get_by_participant(field[0], &row_db1_agent_job_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_agent_job_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_db1_agent_job_t.cursor_turn);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_agent_job_t.api_call_count);
      snprintf(row_text[3], sizeof row_text[3], "%.17g", (double)row_db1_agent_job_t.cost_usd);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_db1_agent_job_t.cost_known);
      row_slots[0] = row_text[0];
      row_slots[1] = row_db1_agent_job_t.role;
      member_owned[0] = row_db1_agent_job_t.prompt;
      row_slots[2] = row_db1_agent_job_t.prompt ? row_db1_agent_job_t.prompt : "";
      row_slots[3] = row_db1_agent_job_t.agent_name;
      row_slots[4] = row_db1_agent_job_t.participant_token;
      row_slots[5] = row_db1_agent_job_t.status;
      member_owned[1] = row_db1_agent_job_t.result;
      row_slots[6] = row_db1_agent_job_t.result ? row_db1_agent_job_t.result : "";
      row_slots[7] = row_text[1];
      row_slots[8] = row_db1_agent_job_t.lease_owner;
      row_slots[9] = row_db1_agent_job_t.heartbeat_at;
      row_slots[10] = row_db1_agent_job_t.current_tool;
      row_slots[11] = row_text[2];
      row_slots[12] = row_text[3];
      row_slots[13] = row_text[4];
      row_slots[14] = row_db1_agent_job_t.created_at;
      row_slots[15] = row_db1_agent_job_t.updated_at;
      rows = row_slots;
      row_count = 16u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT_IS_STALE:
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
      int produced = db1_agent_job_heartbeat_is_stale(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_TAKE_LEASE:
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
      rc = db1_agent_job_take_lease(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_LIST_RECENT:
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
      db1_agent_job_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_job_list_recent(found, parsed0, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 16u * sizeof *cells);
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
                     "%d", found[row].cursor_turn);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%d", found[row].api_call_count);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%.17g", (double)found[row].cost_usd);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].cost_known);
            cells[row * 16u + 0u] = numbers[row * 5u + 0u];
            cells[row * 16u + 1u] = found[row].role;
            cells[row * 16u + 2u] = found[row].prompt ? found[row].prompt : "";
            cells[row * 16u + 3u] = found[row].agent_name;
            cells[row * 16u + 4u] = found[row].participant_token;
            cells[row * 16u + 5u] = found[row].status;
            cells[row * 16u + 6u] = found[row].result ? found[row].result : "";
            cells[row * 16u + 7u] = numbers[row * 5u + 1u];
            cells[row * 16u + 8u] = found[row].lease_owner;
            cells[row * 16u + 9u] = found[row].heartbeat_at;
            cells[row * 16u + 10u] = found[row].current_tool;
            cells[row * 16u + 11u] = numbers[row * 5u + 2u];
            cells[row * 16u + 12u] = numbers[row * 5u + 3u];
            cells[row * 16u + 13u] = numbers[row * 5u + 4u];
            cells[row * 16u + 14u] = found[row].created_at;
            cells[row * 16u + 15u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 16u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_LIST_RUNNING_IDS:
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
      rc = db1_agent_job_list_running_ids(found, parsed0);
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
   case AIMEE_DB1_OP_AGENT_JOB_CANCEL_BY_ID:
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
      int produced = db1_agent_job_cancel_by_id(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_CANCEL_UNASSIGNED:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_agent_job_cancel_unassigned(parsed0, field[1], parsed2);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_CANCEL_NONTERMINAL_ON_RESTART:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_agent_job_cancel_nonterminal_on_restart(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_JOB_CANCEL_STALE:
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
      int produced = db1_agent_job_cancel_stale(parsed0, field[1]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_LOG_ENTRY_LIST:
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
      db1_agent_log_entry_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_agent_log_list(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
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
                     "%d", found[row].turns);
            snprintf(numbers[row * 7u + 1u], 32,
                     "%d", found[row].tool_calls);
            snprintf(numbers[row * 7u + 2u], 32,
                     "%d", found[row].success);
            snprintf(numbers[row * 7u + 3u], 32,
                     "%d", found[row].confidence);
            snprintf(numbers[row * 7u + 4u], 32,
                     "%d", found[row].prompt_tokens);
            snprintf(numbers[row * 7u + 5u], 32,
                     "%d", found[row].completion_tokens);
            snprintf(numbers[row * 7u + 6u], 32,
                     "%d", found[row].latency_ms);
            cells[row * 10u + 0u] = found[row].agent_name;
            cells[row * 10u + 1u] = found[row].role;
            cells[row * 10u + 2u] = numbers[row * 7u + 0u];
            cells[row * 10u + 3u] = numbers[row * 7u + 1u];
            cells[row * 10u + 4u] = numbers[row * 7u + 2u];
            cells[row * 10u + 5u] = numbers[row * 7u + 3u];
            cells[row * 10u + 6u] = numbers[row * 7u + 4u];
            cells[row * 10u + 7u] = numbers[row * 7u + 5u];
            cells[row * 10u + 8u] = numbers[row * 7u + 6u];
            cells[row * 10u + 9u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 10u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_DELEGATE_LEARNING_RECORD:
   {
      if (count != 6u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[5][0])
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
      rc = db1_delegate_learning_record(field[0], field[1], field[2], field[3], field[4], parsed5);
      break;
   }
   case AIMEE_DB1_OP_DELEGATE_LEARNING_INJECT_PROMPT:
   {
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
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
      char *produced = db1_delegate_learning_inject_prompt(field[0], field[1], parsed2);
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
   for (size_t slot = 0; slot < 2u; ++slot)
      free(member_owned[slot]);
   free(scalar_owned);
   switch (op)
   {
   case AIMEE_DB1_OP_AGENT_JOB_LIST_RECENT:
      if (domain_rows)
      {
         db1_agent_job_t *held = domain_rows;
         for (uint32_t at = 0; at < row_count / 16u; ++at)
         {
            free(held[at].prompt);
            free(held[at].result);
         }
      }
      break;
   default:
      break;
   }
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
