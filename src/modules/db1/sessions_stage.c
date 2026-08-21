/* modules/db1/sessions_stage.c: the sessions stage handler.
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
#include "primary_sessions.h"
#include "server_sessions.h"
#include "session_paths.h"
#include "webchat_claude_sessions.h"
#include "webchat_live.h"

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

aimee_module_status_t aimee_db1_stage_sessions(const uint8_t *request_body, uint32_t request_len,
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
   db1_server_session_t row_db1_server_session_t;
   db1_primary_session_row_t row_db1_primary_session_row_t;
   const char *row_slots[10];
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
   char *member_owned[3] = {0};

   switch (op)
   {
   case AIMEE_DB1_OP_SERVER_SESSION_CREATE:
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
      rc = db1_server_session_create(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_SERVER_SESSION_GET:
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
      memset(&row_db1_server_session_t, 0, sizeof row_db1_server_session_t);
      rc = db1_server_session_get(field[0], &row_db1_server_session_t);
      row_slots[0] = row_db1_server_session_t.id;
      row_slots[1] = row_db1_server_session_t.client_type;
      row_slots[2] = row_db1_server_session_t.principal;
      row_slots[3] = row_db1_server_session_t.title;
      row_slots[4] = row_db1_server_session_t.created_at;
      row_slots[5] = row_db1_server_session_t.last_activity_at;
      row_slots[6] = row_db1_server_session_t.claude_session_id;
      row_slots[7] = row_db1_server_session_t.outcome;
      row_slots[8] = row_db1_server_session_t.source;
      row_slots[9] = row_db1_server_session_t.chat_key;
      rows = row_slots;
      row_count = 10u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_SERVER_SESSION_SET_OUTCOME:
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
      rc = db1_server_session_set_outcome(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_SERVER_SESSION_DELETE:
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
      rc = db1_server_session_delete(field[0]);
      break;
   case AIMEE_DB1_OP_SERVER_SESSION_LIST_RECENT:
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
      db1_server_session_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_server_session_list_recent(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 10u * sizeof *cells);
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
            cells[row * 10u + 0u] = found[row].id;
            cells[row * 10u + 1u] = found[row].client_type;
            cells[row * 10u + 2u] = found[row].principal;
            cells[row * 10u + 3u] = found[row].title;
            cells[row * 10u + 4u] = found[row].created_at;
            cells[row * 10u + 5u] = found[row].last_activity_at;
            cells[row * 10u + 6u] = found[row].claude_session_id;
            cells[row * 10u + 7u] = found[row].outcome;
            cells[row * 10u + 8u] = found[row].source;
            cells[row * 10u + 9u] = found[row].chat_key;
         }
         rows = cells;
         row_count = produced * 10u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_SERVER_SESSION_SEARCH_BY_TITLE:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_server_session_t *found = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_server_session_search_by_title(field[0], found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 10u * sizeof *cells);
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
            cells[row * 10u + 0u] = found[row].id;
            cells[row * 10u + 1u] = found[row].client_type;
            cells[row * 10u + 2u] = found[row].principal;
            cells[row * 10u + 3u] = found[row].title;
            cells[row * 10u + 4u] = found[row].created_at;
            cells[row * 10u + 5u] = found[row].last_activity_at;
            cells[row * 10u + 6u] = found[row].claude_session_id;
            cells[row * 10u + 7u] = found[row].outcome;
            cells[row * 10u + 8u] = found[row].source;
            cells[row * 10u + 9u] = found[row].chat_key;
         }
         rows = cells;
         row_count = produced * 10u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_SERVER_SESSION_COUNT:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_server_session_count(field[0][0] ? field[0] : NULL);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_SERVER_SESSION_LIST_EXPIRED:
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
      char (*found)[DB1_SS_ID_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_server_session_list_expired(parsed0, found, parsed1);
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
   case AIMEE_DB1_OP_SERVER_SESSION_DELETE_EXPIRED:
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
      int produced = db1_server_session_delete_expired(parsed0);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_PRIMARY_SESSION_SAVE:
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
      rc = db1_primary_session_save(field[0], field[1], field[2], field[3]);
      break;
   case AIMEE_DB1_OP_PRIMARY_SESSION_LOAD:
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
      char *produced = db1_primary_session_load(field[0], field[1], field[2]);
      rc = produced ? 1 : 0;
      text_owned = produced;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_PRIMARY_SESSION_DELETE:
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
      rc = db1_primary_session_delete(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_RECENT:
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
      db1_primary_session_row_t *found = NULL;
      rc = db1_primary_session_alloc_recent(&found, parsed0);
      domain_rows = found;
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
            cells[row * 6u + 0u] = found[row].session_id;
            cells[row * 6u + 1u] = found[row].agent_name;
            cells[row * 6u + 2u] = found[row].provider;
            cells[row * 6u + 3u] = found[row].created_at;
            cells[row * 6u + 4u] = found[row].updated_at;
            cells[row * 6u + 5u] = found[row].messages_json ? found[row].messages_json : "";
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_SEARCH:
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
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_primary_session_row_t *found = NULL;
      rc = db1_primary_session_alloc_search(field[0], &found, parsed1);
      domain_rows = found;
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
            cells[row * 6u + 0u] = found[row].session_id;
            cells[row * 6u + 1u] = found[row].agent_name;
            cells[row * 6u + 2u] = found[row].provider;
            cells[row * 6u + 3u] = found[row].created_at;
            cells[row * 6u + 4u] = found[row].updated_at;
            cells[row * 6u + 5u] = found[row].messages_json ? found[row].messages_json : "";
         }
         rows = cells;
         row_count = produced * 6u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_PRIMARY_SESSION_GET_LATEST:
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
      memset(&row_db1_primary_session_row_t, 0, sizeof row_db1_primary_session_row_t);
      rc = db1_primary_session_get_latest(field[0], &row_db1_primary_session_row_t);
      row_slots[0] = row_db1_primary_session_row_t.session_id;
      row_slots[1] = row_db1_primary_session_row_t.agent_name;
      row_slots[2] = row_db1_primary_session_row_t.provider;
      row_slots[3] = row_db1_primary_session_row_t.created_at;
      row_slots[4] = row_db1_primary_session_row_t.updated_at;
      member_owned[0] = row_db1_primary_session_row_t.messages_json;
      row_slots[5] = row_db1_primary_session_row_t.messages_json ? row_db1_primary_session_row_t.messages_json : "";
      rows = row_slots;
      row_count = 6u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_SESSION_WRITE_PATH_RECORD:
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
      rc = db1_session_write_path_record(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_SESSION_STALE_READS:
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
      if (parsed2 <= 0 || parsed2 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_SESSION_PATH_LEN] = calloc((size_t)parsed2, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_session_stale_reads(field[0], field[1], found, parsed2);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed2)
                                 ? (uint32_t)rc : (uint32_t)parsed2;
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
   case AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_GET:
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
      rc = db1_webchat_claude_session_get(field[0], field[1], value, sizeof value);
      snprintf(row_text[0], sizeof row_text[0], "%d", rc);
      row_slots[0] = value;
      row_slots[1] = row_text[0];
      rows = row_slots;
      row_count = 2u;
      rc = 0;
      reads = 1;
      break;
   case AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_OWNED_BY_OTHER:
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
      int produced = db1_webchat_claude_session_owned_by_other(field[0], field[1], field[2]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_BIND:
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
      rc = db1_webchat_claude_session_bind(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_WEBCHAT_LIVE_SET:
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
      rc = db1_webchat_live_set(field[0], field[1], field[2], field[3]);
      break;
   case AIMEE_DB1_OP_WEBCHAT_LIVE_GET:
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
      int64_t parsed1;
      if (parse_int64(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char *scalar0 = NULL;
      char *scalar1 = NULL;
      char *scalar2 = NULL;
      long long scalar3 = 0;
      rc = db1_webchat_live_get(field[0], parsed1, &scalar0, &scalar1, &scalar2, &scalar3);
      member_owned[0] = scalar0;
      row_slots[0] = scalar0 ? scalar0 : "";
      member_owned[1] = scalar1;
      row_slots[1] = scalar1 ? scalar1 : "";
      member_owned[2] = scalar2;
      row_slots[2] = scalar2 ? scalar2 : "";
      snprintf(row_text[3], sizeof row_text[3], "%lld", (long long)scalar3);
      row_slots[3] = row_text[3];
      rows = row_slots;
      row_count = 4u;
      found = 1;
      break;
   }
   case AIMEE_DB1_OP_SERVER_SESSION_PERSONA_DELIVERY_CLAIM:
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
      int produced = db1_server_session_persona_delivery_claim(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_SERVER_SESSION_PERSONA_DELIVERY_FINISH:
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
      rc = db1_server_session_persona_delivery_finish(field[0], parsed1);
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
   for (size_t slot = 0; slot < 3u; ++slot)
      free(member_owned[slot]);
   free(scalar_owned);
   switch (op)
   {
   case AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_RECENT:
      if (domain_rows)
      {
         db1_primary_session_row_t *held = domain_rows;
         for (uint32_t at = 0; at < row_count / 6u; ++at)
         {
            free(held[at].messages_json);
         }
      }
      break;
   case AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_SEARCH:
      if (domain_rows)
      {
         db1_primary_session_row_t *held = domain_rows;
         for (uint32_t at = 0; at < row_count / 6u; ++at)
         {
            free(held[at].messages_json);
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
