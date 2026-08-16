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
#include "payload_rewrite_state.h"

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

aimee_module_status_t aimee_db1_stage_conversation(const uint8_t *request_body, uint32_t request_len,
                                             uint8_t *response_body, uint32_t response_capacity,
                                             uint32_t *response_len)
{
   if (request_len < 8u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t op = aimee_db1_get_u32(request_body);
   uint32_t count = aimee_db1_get_u32(request_body + 4u);
   /* Bounds the fixed array below. Without it a well-formed frame declaring
      more fields than any operation takes writes past it. */
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
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
   /* A row answers with a value per member; a plain read answers with one. */
   const char *const *rows = NULL;
   uint32_t row_count = 0u;

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
      payload_rewrite_state_t out;
      memset(&out, 0, sizeof out);
      rc = db1_payload_rewrite_state_get(field[0], &out);
      char text1[24];
      snprintf(text1, sizeof text1, "%lld", (long long)out.payload_epoch);
      char text2[24];
      snprintf(text2, sizeof text2, "%lld", (long long)out.compaction_epoch);
      char text4[24];
      snprintf(text4, sizeof text4, "%d", out.last_payload_tokens);
      char text6[24];
      snprintf(text6, sizeof text6, "%d", out.deferred_rewrite_count);
      char text7[24];
      snprintf(text7, sizeof text7, "%d", out.consecutive_deferred_count);
      char text8[24];
      snprintf(text8, sizeof text8, "%d", out.bytes_saved_pending);
      const char *const row_values[] = {out.session_id, text1, text2, out.last_prefix_hash, text4, out.last_rewrite_at, text6, text7, text8, out.rewrite_reason, out.updated_at};
      rows = row_values;
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
      if (parse_int64(field[1], &row.payload_epoch) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int64(field[2], &row.compaction_epoch) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      snprintf(row.last_prefix_hash, sizeof row.last_prefix_hash, "%s", field[3]);
      if (parse_int(field[4], &row.last_payload_tokens) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      snprintf(row.last_rewrite_at, sizeof row.last_rewrite_at, "%s", field[5]);
      if (parse_int(field[6], &row.deferred_rewrite_count) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[7], &row.consecutive_deferred_count) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parse_int(field[8], &row.bytes_saved_pending) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      snprintf(row.rewrite_reason, sizeof row.rewrite_reason, "%s", field[9]);
      snprintf(row.updated_at, sizeof row.updated_at, "%s", field[10]);
      rc = db1_payload_rewrite_state_set(&row);
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
   if (rows)
      /* A row-returning domain answers 0 or -1: there is no found/not-found
         distinction to preserve, so neither is invented. */
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (reads)
   {
      if (rc < 0)
         status = AIMEE_DB1_STATUS_FAILED;
      else if (rc == 0 || !value[0])
         status = AIMEE_DB1_STATUS_MISSING;
      else
         status = AIMEE_DB1_STATUS_OK;
   }
   else
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;

   {
      const char *one = (status == AIMEE_DB1_STATUS_OK) ? value : "";
      const char *const single[] = {one};
      const char *const *out_values = rows ? rows : (reads ? single : NULL);
      uint32_t out_count = rows ? row_count : (reads ? 1u : 0u);
      if (status != AIMEE_DB1_STATUS_OK && rows)
         out_count = 0u; /* nothing to report but the status */
      write_reply(response_body, response_capacity, response_len, status, out_values, out_count);
   }
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
