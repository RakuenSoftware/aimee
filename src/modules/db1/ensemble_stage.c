/* modules/db1/ensemble_stage.c: the ensemble stage handler.
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
#include "ensemble.h"

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

aimee_module_status_t aimee_db1_stage_ensemble(const uint8_t *request_body, uint32_t request_len,
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
   ensemble_id_result_t row_ensemble_id_result_t;
   ensemble_view_t row_ensemble_view_t;
   const char *row_slots[18];
   char row_text[6][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;
   /* Members the domain allocated with the row. They are released
      after the reply is written, not before: write_reply reads them. */
   char *member_owned[2] = {0};

   switch (op)
   {
   case AIMEE_DB1_OP_ENSEMBLE_CREATE:
   {
      if (count != 5u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[2][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_ensemble_id_result_t, 0, sizeof row_ensemble_id_result_t);
      rc = db1_ensemble_create_id(field[0], field[1], field[2], field[3], field[4], &row_ensemble_id_result_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_ensemble_id_result_t.rc);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_ensemble_id_result_t.id);
      row_slots[0] = row_text[0];
      row_slots[1] = row_ensemble_id_result_t.err;
      row_slots[2] = row_text[1];
      rows = row_slots;
      row_count = 3u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ENSEMBLE_VIEW:
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
      memset(&row_ensemble_view_t, 0, sizeof row_ensemble_view_t);
      rc = db1_ensemble_view(parsed0, &row_ensemble_view_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_ensemble_view_t.rc);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_ensemble_view_t.info.id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_ensemble_view_t.info.current_phase);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_ensemble_view_t.info.current_turn);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_ensemble_view_t.info.phase_count);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_ensemble_view_t.info.turns_in_phase);
      row_slots[0] = row_text[0];
      row_slots[1] = row_ensemble_view_t.err;
      row_slots[2] = row_text[1];
      row_slots[3] = row_ensemble_view_t.info.template_name;
      row_slots[4] = row_ensemble_view_t.info.channel;
      row_slots[5] = row_ensemble_view_t.info.status;
      row_slots[6] = row_text[2];
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_text[5];
      row_slots[10] = row_ensemble_view_t.info.phase_name;
      row_slots[11] = row_ensemble_view_t.info.expected_agent;
      row_slots[12] = row_ensemble_view_t.info.expected_role;
      row_slots[13] = row_ensemble_view_t.info.paused_reason;
      row_slots[14] = row_ensemble_view_t.info.created_at;
      row_slots[15] = row_ensemble_view_t.info.updated_at;
      member_owned[0] = row_ensemble_view_t.prompt;
      row_slots[16] = row_ensemble_view_t.prompt ? row_ensemble_view_t.prompt : "";
      member_owned[1] = row_ensemble_view_t.context;
      row_slots[17] = row_ensemble_view_t.context ? row_ensemble_view_t.context : "";
      rows = row_slots;
      row_count = 18u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ENSEMBLE_ADVANCE:
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
      memset(&row_ensemble_view_t, 0, sizeof row_ensemble_view_t);
      rc = db1_ensemble_advance_view(parsed0, field[1], field[2], &row_ensemble_view_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_ensemble_view_t.rc);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_ensemble_view_t.info.id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_ensemble_view_t.info.current_phase);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_ensemble_view_t.info.current_turn);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_ensemble_view_t.info.phase_count);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_ensemble_view_t.info.turns_in_phase);
      row_slots[0] = row_text[0];
      row_slots[1] = row_ensemble_view_t.err;
      row_slots[2] = row_text[1];
      row_slots[3] = row_ensemble_view_t.info.template_name;
      row_slots[4] = row_ensemble_view_t.info.channel;
      row_slots[5] = row_ensemble_view_t.info.status;
      row_slots[6] = row_text[2];
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_text[5];
      row_slots[10] = row_ensemble_view_t.info.phase_name;
      row_slots[11] = row_ensemble_view_t.info.expected_agent;
      row_slots[12] = row_ensemble_view_t.info.expected_role;
      row_slots[13] = row_ensemble_view_t.info.paused_reason;
      row_slots[14] = row_ensemble_view_t.info.created_at;
      row_slots[15] = row_ensemble_view_t.info.updated_at;
      member_owned[0] = row_ensemble_view_t.prompt;
      row_slots[16] = row_ensemble_view_t.prompt ? row_ensemble_view_t.prompt : "";
      member_owned[1] = row_ensemble_view_t.context;
      row_slots[17] = row_ensemble_view_t.context ? row_ensemble_view_t.context : "";
      rows = row_slots;
      row_count = 18u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ENSEMBLE_PAUSE:
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
      rc = db1_ensemble_pause(parsed0, field[1], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_ENSEMBLE_LIST:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      ensemble_info_t *found = NULL;
      int produced_rows = 0;
      rc = db1_ensemble_list_rows(&found, &produced_rows);
      domain_rows = found;
      rc = (rc == 0) ? produced_rows : -1;
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)512)
                                 ? (uint32_t)rc : (uint32_t)512;
         const char **cells = malloc((size_t)produced * 14u * sizeof *cells);
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
                     "%d", found[row].current_phase);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%d", found[row].current_turn);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%d", found[row].phase_count);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].turns_in_phase);
            cells[row * 14u + 0u] = numbers[row * 5u + 0u];
            cells[row * 14u + 1u] = found[row].template_name;
            cells[row * 14u + 2u] = found[row].channel;
            cells[row * 14u + 3u] = found[row].status;
            cells[row * 14u + 4u] = numbers[row * 5u + 1u];
            cells[row * 14u + 5u] = numbers[row * 5u + 2u];
            cells[row * 14u + 6u] = numbers[row * 5u + 3u];
            cells[row * 14u + 7u] = numbers[row * 5u + 4u];
            cells[row * 14u + 8u] = found[row].phase_name;
            cells[row * 14u + 9u] = found[row].expected_agent;
            cells[row * 14u + 10u] = found[row].expected_role;
            cells[row * 14u + 11u] = found[row].paused_reason;
            cells[row * 14u + 12u] = found[row].created_at;
            cells[row * 14u + 13u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 14u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_ENSEMBLE_FIND_CURRENT_BY_CHANNEL:
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
      memset(&row_ensemble_id_result_t, 0, sizeof row_ensemble_id_result_t);
      rc = db1_ensemble_find_current_id(field[0], &row_ensemble_id_result_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_ensemble_id_result_t.rc);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_ensemble_id_result_t.id);
      row_slots[0] = row_text[0];
      row_slots[1] = row_ensemble_id_result_t.err;
      row_slots[2] = row_text[1];
      rows = row_slots;
      row_count = 3u;
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
   for (size_t slot = 0; slot < 2u; ++slot)
      free(member_owned[slot]);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
