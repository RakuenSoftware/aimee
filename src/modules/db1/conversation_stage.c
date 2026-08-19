/* modules/db1/conversation_stage.c: the conversation stage handler.
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
#include "clarify.h"
#include "conv_context.h"
#include "db1_windows.h"
#include "payload_rewrite_state.h"
#include "user_memory.h"
#include "wm.h"

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

aimee_module_status_t aimee_db1_stage_conversation(const uint8_t *request_body, uint32_t request_len,
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
   payload_rewrite_state_t row_payload_rewrite_state_t;
   wm_entry_t row_wm_entry_t;
   clarify_session_t row_clarify_session_t;
   const char *row_slots[48];
   char row_text[20][32];
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
   case AIMEE_DB1_OP_REWRITE_STATE_GET:
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
      memset(&row_payload_rewrite_state_t, 0, sizeof row_payload_rewrite_state_t);
      rc = db1_payload_rewrite_state_get(field[0], &row_payload_rewrite_state_t);
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)row_payload_rewrite_state_t.payload_epoch);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_payload_rewrite_state_t.compaction_epoch);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_payload_rewrite_state_t.last_payload_tokens);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_payload_rewrite_state_t.deferred_rewrite_count);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_payload_rewrite_state_t.consecutive_deferred_count);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_payload_rewrite_state_t.bytes_saved_pending);
      row_slots[0] = row_payload_rewrite_state_t.session_id;
      row_slots[1] = row_text[0];
      row_slots[2] = row_text[1];
      row_slots[3] = row_payload_rewrite_state_t.last_prefix_hash;
      row_slots[4] = row_text[2];
      row_slots[5] = row_payload_rewrite_state_t.last_rewrite_at;
      row_slots[6] = row_text[3];
      row_slots[7] = row_text[4];
      row_slots[8] = row_text[5];
      row_slots[9] = row_payload_rewrite_state_t.rewrite_reason;
      row_slots[10] = row_payload_rewrite_state_t.updated_at;
      rows = row_slots;
      row_count = 11u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_REWRITE_STATE_SET:
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
      payload_rewrite_state_t row;
      memset(&row, 0, sizeof row);
      snprintf(row.session_id, sizeof row.session_id, "%s", field[0]);
      int64_t member_1 = 0;
      if (parse_int64(field[1], &member_1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.payload_epoch = member_1;
      int64_t member_2 = 0;
      if (parse_int64(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.compaction_epoch = member_2;
      snprintf(row.last_prefix_hash, sizeof row.last_prefix_hash, "%s", field[3]);
      int member_4 = 0;
      if (parse_int(field[4], &member_4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.last_payload_tokens = member_4;
      snprintf(row.last_rewrite_at, sizeof row.last_rewrite_at, "%s", field[5]);
      int member_6 = 0;
      if (parse_int(field[6], &member_6) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.deferred_rewrite_count = member_6;
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.consecutive_deferred_count = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.bytes_saved_pending = member_8;
      snprintf(row.rewrite_reason, sizeof row.rewrite_reason, "%s", field[9]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[10]);
      rc = db1_payload_rewrite_state_set(&row);
      break;
   }
   case AIMEE_DB1_OP_WM_SET:
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
      int parsed4;
      if (parse_int(field[4], &parsed4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_wm_set(field[0], field[1], field[2], field[3], parsed4);
      break;
   }
   case AIMEE_DB1_OP_WM_GET:
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
      memset(&row_wm_entry_t, 0, sizeof row_wm_entry_t);
      rc = db1_wm_get(field[0], field[1], &row_wm_entry_t);
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)row_wm_entry_t.id);
      row_slots[0] = row_text[0];
      row_slots[1] = row_wm_entry_t.session_id;
      row_slots[2] = row_wm_entry_t.key;
      row_slots[3] = row_wm_entry_t.value;
      row_slots[4] = row_wm_entry_t.category;
      row_slots[5] = row_wm_entry_t.created_at;
      row_slots[6] = row_wm_entry_t.updated_at;
      row_slots[7] = row_wm_entry_t.expires_at;
      rows = row_slots;
      row_count = 8u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_WM_LIST:
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
      wm_entry_t *found = calloc((size_t)parsed2, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wm_list(field[0], field[1], found, parsed2);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed2)
                                 ? (uint32_t)rc : (uint32_t)parsed2;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%lld", (long long)found[row].id);
            cells[row * 8u + 0u] = numbers[row * 1u + 0u];
            cells[row * 8u + 1u] = found[row].session_id;
            cells[row * 8u + 2u] = found[row].key;
            cells[row * 8u + 3u] = found[row].value;
            cells[row * 8u + 4u] = found[row].category;
            cells[row * 8u + 5u] = found[row].created_at;
            cells[row * 8u + 6u] = found[row].updated_at;
            cells[row * 8u + 7u] = found[row].expires_at;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WM_ASSEMBLE_CONTEXT:
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
      char *produced = db1_wm_assemble_context(field[0]);
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_REWRITE_RECORD:
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
      int parsed3;
      if (parse_int(field[3], &parsed3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_payload_rewrite_record(field[0], parsed1, parsed2, parsed3, field[4], field[5]);
      break;
   }
   case AIMEE_DB1_OP_WM_SEARCH_SESSION_IDS:
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
      char (*found)[WM_SESSION_ID_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_wm_search_session_ids(field[0], found, parsed1);
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
   case AIMEE_DB1_OP_CONV_RECORD_EVENT:
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
      int64_t produced = db1_conv_record_event(field[0], field[1], field[2], field[3], parsed4);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CONV_SET_CHAIN_ID:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t parsed1;
      if (parse_int64(field[1], &parsed1) != 0)
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
      rc = db1_conv_set_chain_id(parsed0, parsed1, parsed2);
      break;
   }
   case AIMEE_DB1_OP_CONV_INSERT_CHAIN:
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
      int64_t parsed1;
      if (parse_int64(field[1], &parsed1) != 0)
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
      int64_t produced = db1_conv_insert_chain(field[0], parsed1, parsed2, field[3], field[4], parsed5, parsed6);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CONV_PENDING_EVENTS:
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
      conv_tool_event_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_conv_pending_events(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%d", found[row].result_bytes);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%lld", (long long)found[row].chain_id);
            cells[row * 8u + 0u] = numbers[row * 3u + 0u];
            cells[row * 8u + 1u] = found[row].session_id;
            cells[row * 8u + 2u] = found[row].tool_name;
            cells[row * 8u + 3u] = found[row].tool_input;
            cells[row * 8u + 4u] = found[row].tool_result;
            cells[row * 8u + 5u] = numbers[row * 3u + 1u];
            cells[row * 8u + 6u] = numbers[row * 3u + 2u];
            cells[row * 8u + 7u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_CONV_LIST_CHAINS:
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
      conv_tool_chain_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_conv_list_chains(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 10u * sizeof *cells);
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
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 5u + 1u], 32,
                     "%lld", (long long)found[row].event_id_first);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%lld", (long long)found[row].event_id_last);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%d", found[row].raw_bytes);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].stub_bytes);
            cells[row * 10u + 0u] = numbers[row * 5u + 0u];
            cells[row * 10u + 1u] = found[row].session_id;
            cells[row * 10u + 2u] = numbers[row * 5u + 1u];
            cells[row * 10u + 3u] = numbers[row * 5u + 2u];
            cells[row * 10u + 4u] = found[row].tools;
            cells[row * 10u + 5u] = found[row].stub;
            cells[row * 10u + 6u] = numbers[row * 5u + 3u];
            cells[row * 10u + 7u] = numbers[row * 5u + 4u];
            cells[row * 10u + 8u] = found[row].state;
            cells[row * 10u + 9u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 10u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_CONV_CHAIN_EVENTS:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      conv_tool_event_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_conv_chain_events(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 8u * sizeof *cells);
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
                     "%d", found[row].result_bytes);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%lld", (long long)found[row].chain_id);
            cells[row * 8u + 0u] = numbers[row * 3u + 0u];
            cells[row * 8u + 1u] = found[row].session_id;
            cells[row * 8u + 2u] = found[row].tool_name;
            cells[row * 8u + 3u] = found[row].tool_input;
            cells[row * 8u + 4u] = found[row].tool_result;
            cells[row * 8u + 5u] = numbers[row * 3u + 1u];
            cells[row * 8u + 6u] = numbers[row * 3u + 2u];
            cells[row * 8u + 7u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_CONV_SEARCH_CHAINS:
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
      conv_tool_chain_t *found = calloc((size_t)parsed2, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_conv_search_chains(field[0], field[1], found, parsed2);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed2)
                                 ? (uint32_t)rc : (uint32_t)parsed2;
         const char **cells = malloc((size_t)produced * 10u * sizeof *cells);
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
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 5u + 1u], 32,
                     "%lld", (long long)found[row].event_id_first);
            snprintf(numbers[row * 5u + 2u], 32,
                     "%lld", (long long)found[row].event_id_last);
            snprintf(numbers[row * 5u + 3u], 32,
                     "%d", found[row].raw_bytes);
            snprintf(numbers[row * 5u + 4u], 32,
                     "%d", found[row].stub_bytes);
            cells[row * 10u + 0u] = numbers[row * 5u + 0u];
            cells[row * 10u + 1u] = found[row].session_id;
            cells[row * 10u + 2u] = numbers[row * 5u + 1u];
            cells[row * 10u + 3u] = numbers[row * 5u + 2u];
            cells[row * 10u + 4u] = found[row].tools;
            cells[row * 10u + 5u] = found[row].stub;
            cells[row * 10u + 6u] = numbers[row * 5u + 3u];
            cells[row * 10u + 7u] = numbers[row * 5u + 4u];
            cells[row * 10u + 8u] = found[row].state;
            cells[row * 10u + 9u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 10u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_CONV_STATE_GET:
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
      int64_t scalar0 = 0;
      int scalar1 = 0;
      int scalar2 = 0;
      rc = db1_conv_state_get(field[0], &scalar0, &scalar1, &scalar2);
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%d", scalar1);
      row_slots[1] = row_text[1];
      snprintf(row_text[2], sizeof row_text[2], "%d", scalar2);
      row_slots[2] = row_text[2];
      rows = row_slots;
      row_count = 3u;
      break;
   }
   case AIMEE_DB1_OP_CONV_STATE_UPDATE:
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
      if (!field[3][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t parsed1;
      if (parse_int64(field[1], &parsed1) != 0)
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
      rc = db1_conv_state_update(field[0], parsed1, parsed2, parsed3);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_SCAN_STATE:
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
      rc = db1_windows_session_scan_state(field[0], &scalar0, &scalar1);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%d", scalar1);
      row_slots[1] = row_text[1];
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_WINDOW_SESSION_ID:
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
      rc = db1_window_session_id(parsed0, value, sizeof value);
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_WINDOW_CREATE_RAW:
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
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t produced = db1_window_create_raw(field[0], parsed1, field[2], field[3]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WINDOW_ADD_TERM:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_window_add_term(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_ADD_FILE:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_window_add_file(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_IDS_BY_TIER:
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
      int64_t *found = calloc((size_t)parsed2, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_windows_list_ids_by_tier_before_days(field[0], parsed1, found, parsed2);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed2)
                                 ? (uint32_t)rc : (uint32_t)parsed2;
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
                     "%lld", (long long)found[row]);
            cells[row * 1u + 0u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WINDOW_CANDIDATES_BY_TERMS:
   {
      if (count < 1u || count > 1u + 32u)
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
      db1_window_search_candidate_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_windows_find_candidates_by_terms(&field[1], (int)(count - 1u), found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 6u * sizeof *cells);
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
                     "%lld", (long long)found[row].window_id);
            snprintf(numbers[row * 3u + 1u], 32,
                     "%d", found[row].seq);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%d", found[row].match_count);
            cells[row * 6u + 0u] = numbers[row * 3u + 0u];
            cells[row * 6u + 1u] = found[row].session_id;
            cells[row * 6u + 2u] = numbers[row * 3u + 1u];
            cells[row * 6u + 3u] = found[row].summary;
            cells[row * 6u + 4u] = found[row].created_at;
            cells[row * 6u + 5u] = numbers[row * 3u + 2u];
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WINDOW_LIST_FILES:
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
      if (parsed1 <= 0 || parsed1 > 64)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[MAX_PATH_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_window_list_files(parsed0, found, parsed1);
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
   case AIMEE_DB1_OP_WINDOW_INDEX_SUMMARY:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_window_index_summary(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_LEXICAL_HITS:
   {
      if (count < 1u || count > 1u + 32u)
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
      db1_window_lexical_hit_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_windows_find_lexical_hits(&field[1], (int)(count - 1u), found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 2u * sizeof *cells);
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
                     "%lld", (long long)found[row].window_id);
            snprintf(numbers[row * 2u + 1u], 32,
                     "%.17g", (double)found[row].rank);
            cells[row * 2u + 0u] = numbers[row * 2u + 0u];
            cells[row * 2u + 1u] = numbers[row * 2u + 1u];
         }
         rows = cells;
         row_count = produced * 2u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WINDOW_SET_TIER:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_window_set_tier(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_PRUNE_TERMS:
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
      rc = db1_window_prune_terms_keep_top(parsed0, parsed1);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_DELETE_ALL_FILES:
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
      rc = db1_window_delete_all_files(parsed0);
      break;
   }
   case AIMEE_DB1_OP_WINDOW_PRUNE_FILES:
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
      rc = db1_window_prune_files_keep_top(parsed0, parsed1);
      break;
   }
   case AIMEE_DB1_OP_WINDOWS_DELETE_AFTER_TURN:
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
      int produced = db1_windows_delete_after_turn(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_USER_MEMORY_LIST_RECALL:
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
      if (parsed1 <= 0 || parsed1 > 32)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_user_memory_row_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_user_memory_list_recall(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
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
                     "%lld", (long long)found[row].id);
            cells[row * 5u + 0u] = numbers[row * 1u + 0u];
            cells[row * 5u + 1u] = found[row].tier;
            cells[row * 5u + 2u] = found[row].kind;
            cells[row * 5u + 3u] = found[row].key;
            cells[row * 5u + 4u] = found[row].content;
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_USER_MEMORY_ANY:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_user_memory_any();
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_USER_MEMORY_UPSERT:
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
      double parsed4;
      if (parse_double(field[4], &parsed4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_user_memory_upsert(field[0], field[1], field[2], field[3], parsed4, field[5]);
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_START:
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
      memset(&row_clarify_session_t, 0, sizeof row_clarify_session_t);
      rc = db1_clarify_start(field[0], &row_clarify_session_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_clarify_session_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_clarify_session_t.status);
      snprintf(row_text[2], sizeof row_text[2], "%.9g", (double)row_clarify_session_t.score);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_clarify_session_t.qa[0].answered);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_clarify_session_t.qa[0].seq);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_clarify_session_t.qa[1].answered);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_clarify_session_t.qa[1].seq);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_clarify_session_t.qa[2].answered);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_clarify_session_t.qa[2].seq);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_clarify_session_t.qa[3].answered);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_clarify_session_t.qa[3].seq);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_clarify_session_t.qa[4].answered);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_clarify_session_t.qa[4].seq);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_clarify_session_t.qa[5].answered);
      snprintf(row_text[14], sizeof row_text[14], "%d", row_clarify_session_t.qa[5].seq);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_clarify_session_t.qa[6].answered);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_clarify_session_t.qa[6].seq);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_clarify_session_t.qa[7].answered);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_clarify_session_t.qa[7].seq);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_clarify_session_t.qa_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_clarify_session_t.description;
      row_slots[2] = row_text[1];
      row_slots[3] = row_text[2];
      row_slots[4] = row_clarify_session_t.qa[0].dimension;
      row_slots[5] = row_clarify_session_t.qa[0].question;
      row_slots[6] = row_clarify_session_t.qa[0].answer;
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_clarify_session_t.qa[1].dimension;
      row_slots[10] = row_clarify_session_t.qa[1].question;
      row_slots[11] = row_clarify_session_t.qa[1].answer;
      row_slots[12] = row_text[5];
      row_slots[13] = row_text[6];
      row_slots[14] = row_clarify_session_t.qa[2].dimension;
      row_slots[15] = row_clarify_session_t.qa[2].question;
      row_slots[16] = row_clarify_session_t.qa[2].answer;
      row_slots[17] = row_text[7];
      row_slots[18] = row_text[8];
      row_slots[19] = row_clarify_session_t.qa[3].dimension;
      row_slots[20] = row_clarify_session_t.qa[3].question;
      row_slots[21] = row_clarify_session_t.qa[3].answer;
      row_slots[22] = row_text[9];
      row_slots[23] = row_text[10];
      row_slots[24] = row_clarify_session_t.qa[4].dimension;
      row_slots[25] = row_clarify_session_t.qa[4].question;
      row_slots[26] = row_clarify_session_t.qa[4].answer;
      row_slots[27] = row_text[11];
      row_slots[28] = row_text[12];
      row_slots[29] = row_clarify_session_t.qa[5].dimension;
      row_slots[30] = row_clarify_session_t.qa[5].question;
      row_slots[31] = row_clarify_session_t.qa[5].answer;
      row_slots[32] = row_text[13];
      row_slots[33] = row_text[14];
      row_slots[34] = row_clarify_session_t.qa[6].dimension;
      row_slots[35] = row_clarify_session_t.qa[6].question;
      row_slots[36] = row_clarify_session_t.qa[6].answer;
      row_slots[37] = row_text[15];
      row_slots[38] = row_text[16];
      row_slots[39] = row_clarify_session_t.qa[7].dimension;
      row_slots[40] = row_clarify_session_t.qa[7].question;
      row_slots[41] = row_clarify_session_t.qa[7].answer;
      row_slots[42] = row_text[17];
      row_slots[43] = row_text[18];
      row_slots[44] = row_text[19];
      row_slots[45] = row_clarify_session_t.spec;
      row_slots[46] = row_clarify_session_t.created_at;
      row_slots[47] = row_clarify_session_t.updated_at;
      rows = row_slots;
      row_count = 48u;
      reads = 1;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_GET:
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
      memset(&row_clarify_session_t, 0, sizeof row_clarify_session_t);
      rc = db1_clarify_get(parsed0, &row_clarify_session_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_clarify_session_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_clarify_session_t.status);
      snprintf(row_text[2], sizeof row_text[2], "%.9g", (double)row_clarify_session_t.score);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_clarify_session_t.qa[0].answered);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_clarify_session_t.qa[0].seq);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_clarify_session_t.qa[1].answered);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_clarify_session_t.qa[1].seq);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_clarify_session_t.qa[2].answered);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_clarify_session_t.qa[2].seq);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_clarify_session_t.qa[3].answered);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_clarify_session_t.qa[3].seq);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_clarify_session_t.qa[4].answered);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_clarify_session_t.qa[4].seq);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_clarify_session_t.qa[5].answered);
      snprintf(row_text[14], sizeof row_text[14], "%d", row_clarify_session_t.qa[5].seq);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_clarify_session_t.qa[6].answered);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_clarify_session_t.qa[6].seq);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_clarify_session_t.qa[7].answered);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_clarify_session_t.qa[7].seq);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_clarify_session_t.qa_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_clarify_session_t.description;
      row_slots[2] = row_text[1];
      row_slots[3] = row_text[2];
      row_slots[4] = row_clarify_session_t.qa[0].dimension;
      row_slots[5] = row_clarify_session_t.qa[0].question;
      row_slots[6] = row_clarify_session_t.qa[0].answer;
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_clarify_session_t.qa[1].dimension;
      row_slots[10] = row_clarify_session_t.qa[1].question;
      row_slots[11] = row_clarify_session_t.qa[1].answer;
      row_slots[12] = row_text[5];
      row_slots[13] = row_text[6];
      row_slots[14] = row_clarify_session_t.qa[2].dimension;
      row_slots[15] = row_clarify_session_t.qa[2].question;
      row_slots[16] = row_clarify_session_t.qa[2].answer;
      row_slots[17] = row_text[7];
      row_slots[18] = row_text[8];
      row_slots[19] = row_clarify_session_t.qa[3].dimension;
      row_slots[20] = row_clarify_session_t.qa[3].question;
      row_slots[21] = row_clarify_session_t.qa[3].answer;
      row_slots[22] = row_text[9];
      row_slots[23] = row_text[10];
      row_slots[24] = row_clarify_session_t.qa[4].dimension;
      row_slots[25] = row_clarify_session_t.qa[4].question;
      row_slots[26] = row_clarify_session_t.qa[4].answer;
      row_slots[27] = row_text[11];
      row_slots[28] = row_text[12];
      row_slots[29] = row_clarify_session_t.qa[5].dimension;
      row_slots[30] = row_clarify_session_t.qa[5].question;
      row_slots[31] = row_clarify_session_t.qa[5].answer;
      row_slots[32] = row_text[13];
      row_slots[33] = row_text[14];
      row_slots[34] = row_clarify_session_t.qa[6].dimension;
      row_slots[35] = row_clarify_session_t.qa[6].question;
      row_slots[36] = row_clarify_session_t.qa[6].answer;
      row_slots[37] = row_text[15];
      row_slots[38] = row_text[16];
      row_slots[39] = row_clarify_session_t.qa[7].dimension;
      row_slots[40] = row_clarify_session_t.qa[7].question;
      row_slots[41] = row_clarify_session_t.qa[7].answer;
      row_slots[42] = row_text[17];
      row_slots[43] = row_text[18];
      row_slots[44] = row_text[19];
      row_slots[45] = row_clarify_session_t.spec;
      row_slots[46] = row_clarify_session_t.created_at;
      row_slots[47] = row_clarify_session_t.updated_at;
      rows = row_slots;
      row_count = 48u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_ANSWER:
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
      memset(&row_clarify_session_t, 0, sizeof row_clarify_session_t);
      rc = db1_clarify_answer(parsed0, field[1], &row_clarify_session_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_clarify_session_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_clarify_session_t.status);
      snprintf(row_text[2], sizeof row_text[2], "%.9g", (double)row_clarify_session_t.score);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_clarify_session_t.qa[0].answered);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_clarify_session_t.qa[0].seq);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_clarify_session_t.qa[1].answered);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_clarify_session_t.qa[1].seq);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_clarify_session_t.qa[2].answered);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_clarify_session_t.qa[2].seq);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_clarify_session_t.qa[3].answered);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_clarify_session_t.qa[3].seq);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_clarify_session_t.qa[4].answered);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_clarify_session_t.qa[4].seq);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_clarify_session_t.qa[5].answered);
      snprintf(row_text[14], sizeof row_text[14], "%d", row_clarify_session_t.qa[5].seq);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_clarify_session_t.qa[6].answered);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_clarify_session_t.qa[6].seq);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_clarify_session_t.qa[7].answered);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_clarify_session_t.qa[7].seq);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_clarify_session_t.qa_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_clarify_session_t.description;
      row_slots[2] = row_text[1];
      row_slots[3] = row_text[2];
      row_slots[4] = row_clarify_session_t.qa[0].dimension;
      row_slots[5] = row_clarify_session_t.qa[0].question;
      row_slots[6] = row_clarify_session_t.qa[0].answer;
      row_slots[7] = row_text[3];
      row_slots[8] = row_text[4];
      row_slots[9] = row_clarify_session_t.qa[1].dimension;
      row_slots[10] = row_clarify_session_t.qa[1].question;
      row_slots[11] = row_clarify_session_t.qa[1].answer;
      row_slots[12] = row_text[5];
      row_slots[13] = row_text[6];
      row_slots[14] = row_clarify_session_t.qa[2].dimension;
      row_slots[15] = row_clarify_session_t.qa[2].question;
      row_slots[16] = row_clarify_session_t.qa[2].answer;
      row_slots[17] = row_text[7];
      row_slots[18] = row_text[8];
      row_slots[19] = row_clarify_session_t.qa[3].dimension;
      row_slots[20] = row_clarify_session_t.qa[3].question;
      row_slots[21] = row_clarify_session_t.qa[3].answer;
      row_slots[22] = row_text[9];
      row_slots[23] = row_text[10];
      row_slots[24] = row_clarify_session_t.qa[4].dimension;
      row_slots[25] = row_clarify_session_t.qa[4].question;
      row_slots[26] = row_clarify_session_t.qa[4].answer;
      row_slots[27] = row_text[11];
      row_slots[28] = row_text[12];
      row_slots[29] = row_clarify_session_t.qa[5].dimension;
      row_slots[30] = row_clarify_session_t.qa[5].question;
      row_slots[31] = row_clarify_session_t.qa[5].answer;
      row_slots[32] = row_text[13];
      row_slots[33] = row_text[14];
      row_slots[34] = row_clarify_session_t.qa[6].dimension;
      row_slots[35] = row_clarify_session_t.qa[6].question;
      row_slots[36] = row_clarify_session_t.qa[6].answer;
      row_slots[37] = row_text[15];
      row_slots[38] = row_text[16];
      row_slots[39] = row_clarify_session_t.qa[7].dimension;
      row_slots[40] = row_clarify_session_t.qa[7].question;
      row_slots[41] = row_clarify_session_t.qa[7].answer;
      row_slots[42] = row_text[17];
      row_slots[43] = row_text[18];
      row_slots[44] = row_text[19];
      row_slots[45] = row_clarify_session_t.spec;
      row_slots[46] = row_clarify_session_t.created_at;
      row_slots[47] = row_clarify_session_t.updated_at;
      rows = row_slots;
      row_count = 48u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_SCORE:
   {
      if (count != 48u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      clarify_session_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      snprintf(row.description, sizeof row.description, "%s", field[1]);
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.status = member_2;
      double member_3 = 0;
      if (parse_double(field[3], &member_3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.score = member_3;
      snprintf(row.qa[0].dimension, sizeof row.qa[0].dimension, "%s", field[4]);
      snprintf(row.qa[0].question, sizeof row.qa[0].question, "%s", field[5]);
      snprintf(row.qa[0].answer, sizeof row.qa[0].answer, "%s", field[6]);
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].answered = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].seq = member_8;
      snprintf(row.qa[1].dimension, sizeof row.qa[1].dimension, "%s", field[9]);
      snprintf(row.qa[1].question, sizeof row.qa[1].question, "%s", field[10]);
      snprintf(row.qa[1].answer, sizeof row.qa[1].answer, "%s", field[11]);
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].answered = member_12;
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].seq = member_13;
      snprintf(row.qa[2].dimension, sizeof row.qa[2].dimension, "%s", field[14]);
      snprintf(row.qa[2].question, sizeof row.qa[2].question, "%s", field[15]);
      snprintf(row.qa[2].answer, sizeof row.qa[2].answer, "%s", field[16]);
      int member_17 = 0;
      if (parse_int(field[17], &member_17) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].answered = member_17;
      int member_18 = 0;
      if (parse_int(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].seq = member_18;
      snprintf(row.qa[3].dimension, sizeof row.qa[3].dimension, "%s", field[19]);
      snprintf(row.qa[3].question, sizeof row.qa[3].question, "%s", field[20]);
      snprintf(row.qa[3].answer, sizeof row.qa[3].answer, "%s", field[21]);
      int member_22 = 0;
      if (parse_int(field[22], &member_22) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].answered = member_22;
      int member_23 = 0;
      if (parse_int(field[23], &member_23) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].seq = member_23;
      snprintf(row.qa[4].dimension, sizeof row.qa[4].dimension, "%s", field[24]);
      snprintf(row.qa[4].question, sizeof row.qa[4].question, "%s", field[25]);
      snprintf(row.qa[4].answer, sizeof row.qa[4].answer, "%s", field[26]);
      int member_27 = 0;
      if (parse_int(field[27], &member_27) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].answered = member_27;
      int member_28 = 0;
      if (parse_int(field[28], &member_28) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].seq = member_28;
      snprintf(row.qa[5].dimension, sizeof row.qa[5].dimension, "%s", field[29]);
      snprintf(row.qa[5].question, sizeof row.qa[5].question, "%s", field[30]);
      snprintf(row.qa[5].answer, sizeof row.qa[5].answer, "%s", field[31]);
      int member_32 = 0;
      if (parse_int(field[32], &member_32) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].answered = member_32;
      int member_33 = 0;
      if (parse_int(field[33], &member_33) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].seq = member_33;
      snprintf(row.qa[6].dimension, sizeof row.qa[6].dimension, "%s", field[34]);
      snprintf(row.qa[6].question, sizeof row.qa[6].question, "%s", field[35]);
      snprintf(row.qa[6].answer, sizeof row.qa[6].answer, "%s", field[36]);
      int member_37 = 0;
      if (parse_int(field[37], &member_37) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].answered = member_37;
      int member_38 = 0;
      if (parse_int(field[38], &member_38) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].seq = member_38;
      snprintf(row.qa[7].dimension, sizeof row.qa[7].dimension, "%s", field[39]);
      snprintf(row.qa[7].question, sizeof row.qa[7].question, "%s", field[40]);
      snprintf(row.qa[7].answer, sizeof row.qa[7].answer, "%s", field[41]);
      int member_42 = 0;
      if (parse_int(field[42], &member_42) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].answered = member_42;
      int member_43 = 0;
      if (parse_int(field[43], &member_43) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].seq = member_43;
      int member_44 = 0;
      if (parse_int(field[44], &member_44) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa_count = member_44;
      snprintf(row.spec, sizeof row.spec, "%s", field[45]);
      snprintf(row.created_at, sizeof row.created_at, "%s", field[46]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[47]);
      float produced = db1_clarify_score(&row);
      rc = 0;
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_WEAKEST_DIM:
   {
      if (count != 48u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      clarify_session_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      snprintf(row.description, sizeof row.description, "%s", field[1]);
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.status = member_2;
      double member_3 = 0;
      if (parse_double(field[3], &member_3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.score = member_3;
      snprintf(row.qa[0].dimension, sizeof row.qa[0].dimension, "%s", field[4]);
      snprintf(row.qa[0].question, sizeof row.qa[0].question, "%s", field[5]);
      snprintf(row.qa[0].answer, sizeof row.qa[0].answer, "%s", field[6]);
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].answered = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].seq = member_8;
      snprintf(row.qa[1].dimension, sizeof row.qa[1].dimension, "%s", field[9]);
      snprintf(row.qa[1].question, sizeof row.qa[1].question, "%s", field[10]);
      snprintf(row.qa[1].answer, sizeof row.qa[1].answer, "%s", field[11]);
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].answered = member_12;
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].seq = member_13;
      snprintf(row.qa[2].dimension, sizeof row.qa[2].dimension, "%s", field[14]);
      snprintf(row.qa[2].question, sizeof row.qa[2].question, "%s", field[15]);
      snprintf(row.qa[2].answer, sizeof row.qa[2].answer, "%s", field[16]);
      int member_17 = 0;
      if (parse_int(field[17], &member_17) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].answered = member_17;
      int member_18 = 0;
      if (parse_int(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].seq = member_18;
      snprintf(row.qa[3].dimension, sizeof row.qa[3].dimension, "%s", field[19]);
      snprintf(row.qa[3].question, sizeof row.qa[3].question, "%s", field[20]);
      snprintf(row.qa[3].answer, sizeof row.qa[3].answer, "%s", field[21]);
      int member_22 = 0;
      if (parse_int(field[22], &member_22) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].answered = member_22;
      int member_23 = 0;
      if (parse_int(field[23], &member_23) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].seq = member_23;
      snprintf(row.qa[4].dimension, sizeof row.qa[4].dimension, "%s", field[24]);
      snprintf(row.qa[4].question, sizeof row.qa[4].question, "%s", field[25]);
      snprintf(row.qa[4].answer, sizeof row.qa[4].answer, "%s", field[26]);
      int member_27 = 0;
      if (parse_int(field[27], &member_27) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].answered = member_27;
      int member_28 = 0;
      if (parse_int(field[28], &member_28) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].seq = member_28;
      snprintf(row.qa[5].dimension, sizeof row.qa[5].dimension, "%s", field[29]);
      snprintf(row.qa[5].question, sizeof row.qa[5].question, "%s", field[30]);
      snprintf(row.qa[5].answer, sizeof row.qa[5].answer, "%s", field[31]);
      int member_32 = 0;
      if (parse_int(field[32], &member_32) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].answered = member_32;
      int member_33 = 0;
      if (parse_int(field[33], &member_33) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].seq = member_33;
      snprintf(row.qa[6].dimension, sizeof row.qa[6].dimension, "%s", field[34]);
      snprintf(row.qa[6].question, sizeof row.qa[6].question, "%s", field[35]);
      snprintf(row.qa[6].answer, sizeof row.qa[6].answer, "%s", field[36]);
      int member_37 = 0;
      if (parse_int(field[37], &member_37) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].answered = member_37;
      int member_38 = 0;
      if (parse_int(field[38], &member_38) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].seq = member_38;
      snprintf(row.qa[7].dimension, sizeof row.qa[7].dimension, "%s", field[39]);
      snprintf(row.qa[7].question, sizeof row.qa[7].question, "%s", field[40]);
      snprintf(row.qa[7].answer, sizeof row.qa[7].answer, "%s", field[41]);
      int member_42 = 0;
      if (parse_int(field[42], &member_42) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].answered = member_42;
      int member_43 = 0;
      if (parse_int(field[43], &member_43) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].seq = member_43;
      int member_44 = 0;
      if (parse_int(field[44], &member_44) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa_count = member_44;
      snprintf(row.spec, sizeof row.spec, "%s", field[45]);
      snprintf(row.created_at, sizeof row.created_at, "%s", field[46]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[47]);
      scalar_owned = calloc(1u, CLARIFY_DIM_NAME_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      db1_clarify_weakest_dim(&row, scalar0, (size_t)CLARIFY_DIM_NAME_LEN);
      rc = 0;
      row_slots[0] = scalar0;
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_NEXT_QUESTION:
   {
      if (count != 48u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      clarify_session_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      snprintf(row.description, sizeof row.description, "%s", field[1]);
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.status = member_2;
      double member_3 = 0;
      if (parse_double(field[3], &member_3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.score = member_3;
      snprintf(row.qa[0].dimension, sizeof row.qa[0].dimension, "%s", field[4]);
      snprintf(row.qa[0].question, sizeof row.qa[0].question, "%s", field[5]);
      snprintf(row.qa[0].answer, sizeof row.qa[0].answer, "%s", field[6]);
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].answered = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].seq = member_8;
      snprintf(row.qa[1].dimension, sizeof row.qa[1].dimension, "%s", field[9]);
      snprintf(row.qa[1].question, sizeof row.qa[1].question, "%s", field[10]);
      snprintf(row.qa[1].answer, sizeof row.qa[1].answer, "%s", field[11]);
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].answered = member_12;
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].seq = member_13;
      snprintf(row.qa[2].dimension, sizeof row.qa[2].dimension, "%s", field[14]);
      snprintf(row.qa[2].question, sizeof row.qa[2].question, "%s", field[15]);
      snprintf(row.qa[2].answer, sizeof row.qa[2].answer, "%s", field[16]);
      int member_17 = 0;
      if (parse_int(field[17], &member_17) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].answered = member_17;
      int member_18 = 0;
      if (parse_int(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].seq = member_18;
      snprintf(row.qa[3].dimension, sizeof row.qa[3].dimension, "%s", field[19]);
      snprintf(row.qa[3].question, sizeof row.qa[3].question, "%s", field[20]);
      snprintf(row.qa[3].answer, sizeof row.qa[3].answer, "%s", field[21]);
      int member_22 = 0;
      if (parse_int(field[22], &member_22) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].answered = member_22;
      int member_23 = 0;
      if (parse_int(field[23], &member_23) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].seq = member_23;
      snprintf(row.qa[4].dimension, sizeof row.qa[4].dimension, "%s", field[24]);
      snprintf(row.qa[4].question, sizeof row.qa[4].question, "%s", field[25]);
      snprintf(row.qa[4].answer, sizeof row.qa[4].answer, "%s", field[26]);
      int member_27 = 0;
      if (parse_int(field[27], &member_27) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].answered = member_27;
      int member_28 = 0;
      if (parse_int(field[28], &member_28) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].seq = member_28;
      snprintf(row.qa[5].dimension, sizeof row.qa[5].dimension, "%s", field[29]);
      snprintf(row.qa[5].question, sizeof row.qa[5].question, "%s", field[30]);
      snprintf(row.qa[5].answer, sizeof row.qa[5].answer, "%s", field[31]);
      int member_32 = 0;
      if (parse_int(field[32], &member_32) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].answered = member_32;
      int member_33 = 0;
      if (parse_int(field[33], &member_33) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].seq = member_33;
      snprintf(row.qa[6].dimension, sizeof row.qa[6].dimension, "%s", field[34]);
      snprintf(row.qa[6].question, sizeof row.qa[6].question, "%s", field[35]);
      snprintf(row.qa[6].answer, sizeof row.qa[6].answer, "%s", field[36]);
      int member_37 = 0;
      if (parse_int(field[37], &member_37) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].answered = member_37;
      int member_38 = 0;
      if (parse_int(field[38], &member_38) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].seq = member_38;
      snprintf(row.qa[7].dimension, sizeof row.qa[7].dimension, "%s", field[39]);
      snprintf(row.qa[7].question, sizeof row.qa[7].question, "%s", field[40]);
      snprintf(row.qa[7].answer, sizeof row.qa[7].answer, "%s", field[41]);
      int member_42 = 0;
      if (parse_int(field[42], &member_42) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].answered = member_42;
      int member_43 = 0;
      if (parse_int(field[43], &member_43) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].seq = member_43;
      int member_44 = 0;
      if (parse_int(field[44], &member_44) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa_count = member_44;
      snprintf(row.spec, sizeof row.spec, "%s", field[45]);
      snprintf(row.created_at, sizeof row.created_at, "%s", field[46]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[47]);
      scalar_owned = calloc(1u, CLARIFY_TEXT_LEN + CLARIFY_DIM_NAME_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      char *scalar1 = scalar_owned + CLARIFY_TEXT_LEN;
      rc = db1_clarify_next_question(&row, scalar0, (size_t)CLARIFY_TEXT_LEN, scalar1, (size_t)CLARIFY_DIM_NAME_LEN);
      row_slots[0] = scalar0;
      row_slots[1] = scalar1;
      rows = row_slots;
      row_count = 2u;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_CLARIFY_CRYSTALLIZE:
   {
      if (count != 48u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      clarify_session_t row;
      memset(&row, 0, sizeof row);
      int member_0 = 0;
      if (parse_int(field[0], &member_0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.id = member_0;
      snprintf(row.description, sizeof row.description, "%s", field[1]);
      int member_2 = 0;
      if (parse_int(field[2], &member_2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.status = member_2;
      double member_3 = 0;
      if (parse_double(field[3], &member_3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.score = member_3;
      snprintf(row.qa[0].dimension, sizeof row.qa[0].dimension, "%s", field[4]);
      snprintf(row.qa[0].question, sizeof row.qa[0].question, "%s", field[5]);
      snprintf(row.qa[0].answer, sizeof row.qa[0].answer, "%s", field[6]);
      int member_7 = 0;
      if (parse_int(field[7], &member_7) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].answered = member_7;
      int member_8 = 0;
      if (parse_int(field[8], &member_8) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[0].seq = member_8;
      snprintf(row.qa[1].dimension, sizeof row.qa[1].dimension, "%s", field[9]);
      snprintf(row.qa[1].question, sizeof row.qa[1].question, "%s", field[10]);
      snprintf(row.qa[1].answer, sizeof row.qa[1].answer, "%s", field[11]);
      int member_12 = 0;
      if (parse_int(field[12], &member_12) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].answered = member_12;
      int member_13 = 0;
      if (parse_int(field[13], &member_13) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[1].seq = member_13;
      snprintf(row.qa[2].dimension, sizeof row.qa[2].dimension, "%s", field[14]);
      snprintf(row.qa[2].question, sizeof row.qa[2].question, "%s", field[15]);
      snprintf(row.qa[2].answer, sizeof row.qa[2].answer, "%s", field[16]);
      int member_17 = 0;
      if (parse_int(field[17], &member_17) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].answered = member_17;
      int member_18 = 0;
      if (parse_int(field[18], &member_18) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[2].seq = member_18;
      snprintf(row.qa[3].dimension, sizeof row.qa[3].dimension, "%s", field[19]);
      snprintf(row.qa[3].question, sizeof row.qa[3].question, "%s", field[20]);
      snprintf(row.qa[3].answer, sizeof row.qa[3].answer, "%s", field[21]);
      int member_22 = 0;
      if (parse_int(field[22], &member_22) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].answered = member_22;
      int member_23 = 0;
      if (parse_int(field[23], &member_23) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[3].seq = member_23;
      snprintf(row.qa[4].dimension, sizeof row.qa[4].dimension, "%s", field[24]);
      snprintf(row.qa[4].question, sizeof row.qa[4].question, "%s", field[25]);
      snprintf(row.qa[4].answer, sizeof row.qa[4].answer, "%s", field[26]);
      int member_27 = 0;
      if (parse_int(field[27], &member_27) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].answered = member_27;
      int member_28 = 0;
      if (parse_int(field[28], &member_28) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[4].seq = member_28;
      snprintf(row.qa[5].dimension, sizeof row.qa[5].dimension, "%s", field[29]);
      snprintf(row.qa[5].question, sizeof row.qa[5].question, "%s", field[30]);
      snprintf(row.qa[5].answer, sizeof row.qa[5].answer, "%s", field[31]);
      int member_32 = 0;
      if (parse_int(field[32], &member_32) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].answered = member_32;
      int member_33 = 0;
      if (parse_int(field[33], &member_33) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[5].seq = member_33;
      snprintf(row.qa[6].dimension, sizeof row.qa[6].dimension, "%s", field[34]);
      snprintf(row.qa[6].question, sizeof row.qa[6].question, "%s", field[35]);
      snprintf(row.qa[6].answer, sizeof row.qa[6].answer, "%s", field[36]);
      int member_37 = 0;
      if (parse_int(field[37], &member_37) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].answered = member_37;
      int member_38 = 0;
      if (parse_int(field[38], &member_38) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[6].seq = member_38;
      snprintf(row.qa[7].dimension, sizeof row.qa[7].dimension, "%s", field[39]);
      snprintf(row.qa[7].question, sizeof row.qa[7].question, "%s", field[40]);
      snprintf(row.qa[7].answer, sizeof row.qa[7].answer, "%s", field[41]);
      int member_42 = 0;
      if (parse_int(field[42], &member_42) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].answered = member_42;
      int member_43 = 0;
      if (parse_int(field[43], &member_43) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa[7].seq = member_43;
      int member_44 = 0;
      if (parse_int(field[44], &member_44) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.qa_count = member_44;
      snprintf(row.spec, sizeof row.spec, "%s", field[45]);
      snprintf(row.created_at, sizeof row.created_at, "%s", field[46]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[47]);
      char *produced = db1_clarify_crystallize(&row);
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
