/* modules/db1/runtime_stage.c: the runtime stage handler.
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
#include "caches.h"
#include "decisions.h"
#include "env.h"
#include "fsnap.h"
#include "local_operator.h"
#include "mcp_osv_cache.h"
#include "model_catalog.h"
#include "model_pricing.h"
#include "project_clones.h"
#include "runtime_state.h"
#include "tool_local_availability.h"
#include "web_page_cache.h"
#include "working_profile_local.h"

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

aimee_module_status_t aimee_db1_stage_runtime(const uint8_t *request_body, uint32_t request_len,
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
   db1_project_clone_t row_db1_project_clone_t;
   db1_local_operator_t row_db1_local_operator_t;
   db1_maintenance_state_t row_db1_maintenance_state_t;
   db1_working_profile_local_state_t row_db1_working_profile_local_state_t;
   db1_tool_local_availability_t row_db1_tool_local_availability_t;
   fsnap_info_t row_fsnap_info_t;
   db1_mcp_osv_cache_row_t row_db1_mcp_osv_cache_row_t;
   const char *row_slots[8];
   char row_text[4][32];
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
   char *member_owned[1] = {0};

   switch (op)
   {
   case AIMEE_DB1_OP_RUNTIME_STATE_SET:
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
      rc = db1_runtime_state_set(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_RUNTIME_STATE_GET:
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
      rc = db1_runtime_state_get(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_RUNTIME_STATE_ADD_INT:
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
      rc = db1_runtime_state_add_int(field[0], parsed1, &scalar0);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PROJECT_CLONE_UPSERT:
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
      rc = db1_project_clone_upsert(field[0], field[1], field[2], field[3], field[4]);
      break;
   case AIMEE_DB1_OP_PROJECT_CLONE_GET:
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
      memset(&row_db1_project_clone_t, 0, sizeof row_db1_project_clone_t);
      rc = db1_project_clone_get(field[0], &row_db1_project_clone_t);
      row_slots[0] = row_db1_project_clone_t.clone_path;
      row_slots[1] = row_db1_project_clone_t.project_uuid;
      row_slots[2] = row_db1_project_clone_t.canonical_url;
      row_slots[3] = row_db1_project_clone_t.origin_url;
      row_slots[4] = row_db1_project_clone_t.upstream_url;
      row_slots[5] = row_db1_project_clone_t.last_seen_at;
      rows = row_slots;
      row_count = 6u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_PROJECT_CLONE_DELETE:
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
      rc = db1_project_clone_delete(field[0]);
      break;
   case AIMEE_DB1_OP_PROJECT_CLONE_LIST:
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
      db1_project_clone_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_project_clone_list(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 6u * sizeof *cells);
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
            cells[row * 6u + 0u] = found[row].clone_path;
            cells[row * 6u + 1u] = found[row].project_uuid;
            cells[row * 6u + 2u] = found[row].canonical_url;
            cells[row * 6u + 3u] = found[row].origin_url;
            cells[row * 6u + 4u] = found[row].upstream_url;
            cells[row * 6u + 5u] = found[row].last_seen_at;
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_PROJECT_CLONE_LIST_BY_PROJECT:
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
      db1_project_clone_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_project_clone_list_by_project(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 6u * sizeof *cells);
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
            cells[row * 6u + 0u] = found[row].clone_path;
            cells[row * 6u + 1u] = found[row].project_uuid;
            cells[row * 6u + 2u] = found[row].canonical_url;
            cells[row * 6u + 3u] = found[row].origin_url;
            cells[row * 6u + 4u] = found[row].upstream_url;
            cells[row * 6u + 5u] = found[row].last_seen_at;
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_LOCAL_OPERATOR_UPSERT:
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
      int parsed2;
      if (parse_int(field[2], &parsed2) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_local_operator_upsert(field[0], field[1], parsed2, field[3]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_LOCAL_OPERATOR_GET:
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
      memset(&row_db1_local_operator_t, 0, sizeof row_db1_local_operator_t);
      rc = db1_local_operator_get(field[0], &row_db1_local_operator_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_local_operator_t.active);
      row_slots[0] = row_db1_local_operator_t.secret_ref;
      row_slots[1] = row_db1_local_operator_t.operator_uuid;
      row_slots[2] = row_text[0];
      row_slots[3] = row_db1_local_operator_t.display_hint;
      row_slots[4] = row_db1_local_operator_t.created_at;
      rows = row_slots;
      row_count = 5u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_LOCAL_OPERATOR_GET_ACTIVE:
   {
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_local_operator_t, 0, sizeof row_db1_local_operator_t);
      rc = db1_local_operator_get_active(&row_db1_local_operator_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_local_operator_t.active);
      row_slots[0] = row_db1_local_operator_t.secret_ref;
      row_slots[1] = row_db1_local_operator_t.operator_uuid;
      row_slots[2] = row_text[0];
      row_slots[3] = row_db1_local_operator_t.display_hint;
      row_slots[4] = row_db1_local_operator_t.created_at;
      rows = row_slots;
      row_count = 5u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_LOCAL_OPERATOR_SET_ACTIVE:
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
      int produced = db1_local_operator_set_active(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_LOCAL_OPERATOR_DELETE:
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
      rc = db1_local_operator_delete(field[0]);
      break;
   case AIMEE_DB1_OP_LOCAL_OPERATOR_LIST:
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
      db1_local_operator_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_local_operator_list(found, parsed0);
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
                     "%d", found[row].active);
            cells[row * 5u + 0u] = found[row].secret_ref;
            cells[row * 5u + 1u] = found[row].operator_uuid;
            cells[row * 5u + 2u] = numbers[row * 1u + 0u];
            cells[row * 5u + 3u] = found[row].display_hint;
            cells[row * 5u + 4u] = found[row].created_at;
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_ENV_CAPABILITY_SET:
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
      rc = db1_env_capability_set(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_ENV_CAPABILITY_GET:
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
      scalar_owned = calloc(1u, DB1_ENV_VALUE_LEN + DB1_ENV_TS_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar0 = scalar_owned;
      char *scalar1 = scalar_owned + DB1_ENV_VALUE_LEN;
      rc = db1_env_capability_get(field[0], scalar0, (size_t)DB1_ENV_VALUE_LEN, scalar1, (size_t)DB1_ENV_TS_LEN);
      row_slots[0] = scalar0;
      row_slots[1] = scalar1;
      rows = row_slots;
      row_count = 2u;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_ENV_CAPABILITY_LIST:
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
      db1_env_capability_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_env_capability_list(found, parsed0);
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
            cells[row * 3u + 0u] = found[row].key;
            cells[row * 3u + 1u] = found[row].value;
            cells[row * 3u + 2u] = found[row].detected_at;
         }
         rows = cells;
         row_count = produced * 3u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_MAINTENANCE_STATE_LOAD:
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
      memset(&row_db1_maintenance_state_t, 0, sizeof row_db1_maintenance_state_t);
      rc = db1_maintenance_state_load(field[0], &row_db1_maintenance_state_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_maintenance_state_t.present);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_db1_maintenance_state_t.last_memory_count);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_db1_maintenance_state_t.last_changes);
      snprintf(row_text[3], sizeof row_text[3], "%.17g", (double)row_db1_maintenance_state_t.last_elapsed_ms);
      row_slots[0] = row_text[0];
      row_slots[1] = row_db1_maintenance_state_t.last_run_at;
      row_slots[2] = row_text[1];
      row_slots[3] = row_text[2];
      row_slots[4] = row_text[3];
      row_slots[5] = row_db1_maintenance_state_t.last_summary_json;
      rows = row_slots;
      row_count = 6u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_MAINTENANCE_STATE_SAVE:
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
      db1_maintenance_state_t row;
      memset(&row, 0, sizeof row);
      int member_1 = 0;
      if (parse_int(field[1], &member_1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.present = member_1;
      snprintf(row.last_run_at, sizeof row.last_run_at, "%s", field[2]);
      int64_t member_3 = 0;
      if (parse_int64(field[3], &member_3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.last_memory_count = member_3;
      int member_4 = 0;
      if (parse_int(field[4], &member_4) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.last_changes = member_4;
      double member_5 = 0;
      if (parse_double(field[5], &member_5) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.last_elapsed_ms = member_5;
      snprintf(row.last_summary_json, sizeof row.last_summary_json, "%s", field[6]);
      rc = db1_maintenance_state_save(field[0], &row);
      break;
   }
   case AIMEE_DB1_OP_MODEL_CATALOG_IS_FRESH:
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
      int produced = db1_model_catalog_is_fresh(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_MODEL_CATALOG_GET:
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
      provider_model_t *found = NULL;
      int produced_rows = 0;
      rc = db1_model_catalog_get(field[0], &found, &produced_rows);
      domain_rows = found;
      rc = (rc == 0) ? produced_rows : -1;
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)512)
                                 ? (uint32_t)rc : (uint32_t)512;
         const char **cells = malloc((size_t)produced * 6u * sizeof *cells);
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
                     "%d", found[row].context_window);
            snprintf(numbers[row * 4u + 1u], 32,
                     "%d", found[row].max_output);
            snprintf(numbers[row * 4u + 2u], 32,
                     "%d", found[row].caps);
            snprintf(numbers[row * 4u + 3u], 32,
                     "%d", found[row].deprecated);
            cells[row * 6u + 0u] = found[row].id;
            cells[row * 6u + 1u] = found[row].display_name;
            cells[row * 6u + 2u] = numbers[row * 4u + 0u];
            cells[row * 6u + 3u] = numbers[row * 4u + 1u];
            cells[row * 6u + 4u] = numbers[row * 4u + 2u];
            cells[row * 6u + 5u] = numbers[row * 4u + 3u];
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_MODEL_CATALOG_REPLACE:
   {
      if (count < 1u || count > 1u + 3072u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if ((count - 1u) % 6u != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int repeated_rows = (int)((count - 1u) / 6u);
      provider_model_t *repeated_held = calloc((size_t)repeated_rows + 1u, sizeof *repeated_held);
      if (!repeated_held)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = repeated_held;
      for (int at = 0; at < repeated_rows; ++at)
      {
         snprintf(repeated_held[at].id, sizeof repeated_held[at].id, "%s", field[1 + at * 6 + 0]);
         snprintf(repeated_held[at].display_name, sizeof repeated_held[at].display_name, "%s", field[1 + at * 6 + 1]);
         int member_2 = 0;
         if (parse_int(field[1 + at * 6 + 2], &member_2) != 0)
         {
            free(scratch);
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         }
         repeated_held[at].context_window = member_2;
         int member_3 = 0;
         if (parse_int(field[1 + at * 6 + 3], &member_3) != 0)
         {
            free(scratch);
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         }
         repeated_held[at].max_output = member_3;
         int member_4 = 0;
         if (parse_int(field[1 + at * 6 + 4], &member_4) != 0)
         {
            free(scratch);
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         }
         repeated_held[at].caps = member_4;
         int member_5 = 0;
         if (parse_int(field[1 + at * 6 + 5], &member_5) != 0)
         {
            free(scratch);
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         }
         repeated_held[at].deprecated = member_5;
      }
      rc = db1_model_catalog_replace(field[0], repeated_held, repeated_rows);
      break;
   }
   case AIMEE_DB1_OP_MODEL_PRICE_GET:
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
      double scalar1 = 0;
      rc = db1_model_price_get(field[0], &scalar0, &scalar1);
      snprintf(row_text[0], sizeof row_text[0], "%.17g", (double)scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)scalar1);
      row_slots[1] = row_text[1];
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_MODEL_PRICE_SET:
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
      double parsed1;
      if (parse_double(field[1], &parsed1) != 0)
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
      rc = db1_model_price_set(field[0], parsed1, parsed2);
      break;
   }
   case AIMEE_DB1_OP_MODEL_PRICE_DELETE:
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
      rc = db1_model_price_delete(field[0]);
      break;
   case AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_OBSERVE:
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
      double parsed2;
      if (parse_double(field[2], &parsed2) != 0)
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
      int produced = db1_working_profile_local_observe(field[0], field[1], parsed2, field[3], parsed4);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_LIST:
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
      db1_working_profile_local_state_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_working_profile_local_list(found, parsed0);
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
                     "%d", found[row].observation_count);
            snprintf(numbers[row * 2u + 1u], 32,
                     "%.17g", (double)found[row].score);
            cells[row * 5u + 0u] = found[row].field;
            cells[row * 5u + 1u] = found[row].value;
            cells[row * 5u + 2u] = numbers[row * 2u + 0u];
            cells[row * 5u + 3u] = numbers[row * 2u + 1u];
            cells[row * 5u + 4u] = found[row].updated_at;
         }
         rows = cells;
         row_count = produced * 5u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_GET:
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
      memset(&row_db1_working_profile_local_state_t, 0, sizeof row_db1_working_profile_local_state_t);
      rc = db1_working_profile_local_get(field[0], &row_db1_working_profile_local_state_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_working_profile_local_state_t.observation_count);
      snprintf(row_text[1], sizeof row_text[1], "%.17g", (double)row_db1_working_profile_local_state_t.score);
      row_slots[0] = row_db1_working_profile_local_state_t.field;
      row_slots[1] = row_db1_working_profile_local_state_t.value;
      row_slots[2] = row_text[0];
      row_slots[3] = row_text[1];
      row_slots[4] = row_db1_working_profile_local_state_t.updated_at;
      rows = row_slots;
      row_count = 5u;
      reads = 1;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_RESET_FIELD:
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
      rc = db1_working_profile_local_reset_field(field[0]);
      break;
   case AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_SET:
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
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_tool_local_availability_set(field[0], parsed1, field[2]);
      break;
   }
   case AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_GET:
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
      memset(&row_db1_tool_local_availability_t, 0, sizeof row_db1_tool_local_availability_t);
      rc = db1_tool_local_availability_get(field[0], &row_db1_tool_local_availability_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_tool_local_availability_t.usable);
      row_slots[0] = row_db1_tool_local_availability_t.tool_uuid;
      row_slots[1] = row_text[0];
      row_slots[2] = row_db1_tool_local_availability_t.binary_path;
      row_slots[3] = row_db1_tool_local_availability_t.checked_at;
      rows = row_slots;
      row_count = 4u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_DELETE:
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
      rc = db1_tool_local_availability_delete(field[0]);
      break;
   case AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_LIST:
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
      db1_tool_local_availability_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_tool_local_availability_list(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
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
                     "%d", found[row].usable);
            cells[row * 4u + 0u] = found[row].tool_uuid;
            cells[row * 4u + 1u] = numbers[row * 1u + 0u];
            cells[row * 4u + 2u] = found[row].binary_path;
            cells[row * 4u + 3u] = found[row].checked_at;
         }
         rows = cells;
         row_count = produced * 4u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_CONTEXT_CACHE_GET:
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
      rc = db1_context_cache_get(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_CONTEXT_CACHE_PUT:
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
      db1_context_cache_put(field[0], field[1]);
      rc = 0;
      break;
   case AIMEE_DB1_OP_CONTEXT_CACHE_INVALIDATE:
      if (count != 0u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_context_cache_invalidate();
      rc = 0;
      break;
   case AIMEE_DB1_OP_CONTEXT_SNAPSHOT_INSERT:
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
      int64_t parsed1;
      if (parse_int64(field[1], &parsed1) != 0)
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
      rc = db1_context_snapshot_insert(field[0], parsed1, parsed2);
      break;
   }
   case AIMEE_DB1_OP_CONTEXT_SNAPSHOT_COUNT_MIN_SAMPLES:
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
      int produced = db1_context_snapshot_count_memories_with_min_samples(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CONTEXT_SNAPSHOT_IDS_MIN_SAMPLES:
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
      if (parsed1 <= 0 || parsed1 > 1024)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_context_snapshot_list_memory_ids_with_min_samples(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
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
   case AIMEE_DB1_OP_CONTEXT_SNAPSHOT_COUNT_FOR_MEMORY:
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
      int produced = db1_context_snapshot_count_for_memory(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_CONTEXT_SNAPSHOT_SESSIONS_FOR_MEMORY:
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
      if (parsed1 <= 0 || parsed1 > 512)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_CONTEXT_SNAPSHOT_SESSION_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_context_snapshot_list_sessions_for_memory(parsed0, found, parsed1);
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
   case AIMEE_DB1_OP_CONTEXT_SNAPSHOT_HAS_MEMORY:
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
      int produced = db1_context_snapshot_has_memory(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_AGENT_CACHE_GET:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char *produced = db1_agent_cache_get(field[0], field[1]);
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_AGENT_CACHE_PUT:
      if (count != 3u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_agent_cache_put(field[0], field[1], field[2]);
      rc = 0;
      break;
   case AIMEE_DB1_OP_WEB_PAGE_GET:
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
      scalar_owned = calloc(1u, DB1_WEB_PAGE_ADDR_LEN);
      if (!scalar_owned)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      char *scalar1 = scalar_owned;
      long scalar0 = 0;
      char *produced = db1_web_page_get(field[0], &scalar0, scalar1, (size_t)DB1_WEB_PAGE_ADDR_LEN);
      rc = produced ? 1 : 0;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)scalar0);
      row_slots[1] = row_text[0];
      row_slots[2] = scalar1;
      row_slots[0] = produced ? produced : "";
      found = 1;
      rows = row_slots;
      row_count = 3u;
      text_owned = produced;
      break;
   }
   case AIMEE_DB1_OP_WEB_PAGE_PUT:
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
      rc = db1_web_page_put(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_WEB_PAGE_DROP:
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
      db1_web_page_drop(field[0]);
      rc = 0;
      break;
   case AIMEE_DB1_OP_WEB_PAGE_CANONICAL_URL:
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
      rc = db1_web_page_canonical_url(field[0], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_FSNAP_CREATE:
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
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t produced = db1_fsnap_create(field[0], parsed1, field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_FSNAP_GET_OR_CREATE:
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
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int64_t produced = db1_fsnap_get_or_create(field[0], parsed1, field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_FSNAP_RECORD_FILE:
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
      rc = db1_fsnap_record_file(parsed0, field[1]);
      break;
   }
   case AIMEE_DB1_OP_FSNAP_PRUNE:
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
      int produced = db1_fsnap_prune(field[0], parsed1);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_FSNAP_LIST:
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
      fsnap_info_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_fsnap_list(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
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
                     "%lld", (long long)found[row].id);
            snprintf(numbers[row * 3u + 1u], 32,
                     "%d", found[row].turn);
            snprintf(numbers[row * 3u + 2u], 32,
                     "%d", found[row].file_count);
            cells[row * 6u + 0u] = numbers[row * 3u + 0u];
            cells[row * 6u + 1u] = numbers[row * 3u + 1u];
            cells[row * 6u + 2u] = found[row].session_id;
            cells[row * 6u + 3u] = found[row].created_at;
            cells[row * 6u + 4u] = found[row].label;
            cells[row * 6u + 5u] = numbers[row * 3u + 2u];
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_FSNAP_RESTORE:
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
      int scalar0 = 0;
      int scalar1 = 0;
      rc = db1_fsnap_restore(parsed0, &scalar0, &scalar1);
      snprintf(row_text[0], sizeof row_text[0], "%d", scalar0);
      row_slots[0] = row_text[0];
      snprintf(row_text[1], sizeof row_text[1], "%d", scalar1);
      row_slots[1] = row_text[1];
      rows = row_slots;
      row_count = 2u;
      break;
   }
   case AIMEE_DB1_OP_FSNAP_GET:
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
      memset(&row_fsnap_info_t, 0, sizeof row_fsnap_info_t);
      rc = db1_fsnap_get(parsed0, &row_fsnap_info_t);
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)row_fsnap_info_t.id);
      snprintf(row_text[1], sizeof row_text[1], "%d", row_fsnap_info_t.turn);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_fsnap_info_t.file_count);
      row_slots[0] = row_text[0];
      row_slots[1] = row_text[1];
      row_slots[2] = row_fsnap_info_t.session_id;
      row_slots[3] = row_fsnap_info_t.created_at;
      row_slots[4] = row_fsnap_info_t.label;
      row_slots[5] = row_text[2];
      rows = row_slots;
      row_count = 6u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_DECISION_RECORD:
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
      int64_t parsed0;
      if (parse_int64(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_decision_record(parsed0, field[1], field[2]);
      break;
   }
   case AIMEE_DB1_OP_MCP_OSV_CACHE_GET:
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
      int parsed3;
      if (parse_int(field[3], &parsed3) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_mcp_osv_cache_row_t, 0, sizeof row_db1_mcp_osv_cache_row_t);
      rc = db1_mcp_osv_cache_get(field[0], field[1], field[2], parsed3, &row_db1_mcp_osv_cache_row_t);
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)row_db1_mcp_osv_cache_row_t.checked_at);
      row_slots[0] = row_db1_mcp_osv_cache_row_t.client_name;
      row_slots[1] = row_db1_mcp_osv_cache_row_t.ecosystem;
      row_slots[2] = row_db1_mcp_osv_cache_row_t.name;
      row_slots[3] = row_db1_mcp_osv_cache_row_t.version;
      row_slots[4] = row_db1_mcp_osv_cache_row_t.verdict;
      row_slots[5] = row_db1_mcp_osv_cache_row_t.advisory_ids;
      row_slots[6] = row_text[0];
      row_slots[7] = row_db1_mcp_osv_cache_row_t.checked_at_text;
      rows = row_slots;
      row_count = 8u;
      reads = 1;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_MCP_OSV_CACHE_UPSERT:
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
      rc = db1_mcp_osv_cache_upsert(field[0], field[1], field[2], field[3], field[4]);
      break;
   case AIMEE_DB1_OP_MCP_OSV_CACHE_LIST:
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
      db1_mcp_osv_cache_row_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_mcp_osv_cache_list(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
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
                     "%lld", (long long)found[row].checked_at);
            cells[row * 8u + 0u] = found[row].client_name;
            cells[row * 8u + 1u] = found[row].ecosystem;
            cells[row * 8u + 2u] = found[row].name;
            cells[row * 8u + 3u] = found[row].version;
            cells[row * 8u + 4u] = found[row].verdict;
            cells[row * 8u + 5u] = found[row].advisory_ids;
            cells[row * 8u + 6u] = numbers[row * 1u + 0u];
            cells[row * 8u + 7u] = found[row].checked_at_text;
         }
         rows = cells;
         row_count = produced * 8u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_MCP_OSV_AUDIT:
      if (count != 7u)
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
      rc = db1_mcp_osv_audit(field[0], field[1], field[2], field[3], field[4], field[5], field[6]);
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
   for (size_t slot = 0; slot < 1u; ++slot)
      free(member_owned[slot]);
   free(scalar_owned);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
