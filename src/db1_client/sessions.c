/* db1_client/sessions.c: the sessions family, reached over the bus.
 *
 * WAS GENERATED from the store catalog by scripts/gen_db1_contract.py. Both
 * moved on: the catalog is now server-go/modules/aimee/operations.json, and the
 * generator was deleted with the C module.
 *
 * So this is maintained BY HAND now, and the header used to say "Do not edit"
 * while pointing at a generator that no longer exists and a path that no longer
 * resolves -- which is a dead end at exactly the moment someone needs to change
 * something. Edit it, and keep it agreeing with the catalog:
 * scripts/check-db1-client-contract.py matches every call site here against the
 * catalog by arity and reply width, and runs in lint on every pull request.
 * That check is what replaced the generator.
 *
 * Same functions, same contract, different side of the boundary: the daemon
 * links this instead of the DB1 domain, so nothing that calls these had to
 * change.
 *
 * It lives OUTSIDE modules/db1 deliberately. The module's descriptor owns every
 * .c beside it and compiles them into the DB1 process, so a client with these
 * names in that directory would be linked twice into the one binary that must
 * not have it -- once as the caller and once as the implementation.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
#include "primary_sessions.h"
#include "db1_client/server_sessions.h"
#include "session_paths.h"
#include "webchat_claude_sessions.h"
#include "webchat_live.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_SESSIONS_CALL_TIMEOUT_MS 2000

static db1_family_t family = {"db1.sessions", "sessions", AIMEE_DB1_EVENT_SESSIONS,
                              AIMEE_DB1_STAGE_SESSIONS, DB1_SESSIONS_CALL_TIMEOUT_MS, 0};

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_server_session_create(const char *id, const char *client_type, const char *principal)
{
   if (!id || !id[0])
      return -1;
   const char *fields[] = {id, client_type ? client_type : "", principal ? principal : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_CREATE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   if (!id || !id[0] || !out)
      return -1;
   const char *fields[] = {id};
   memset(out, 0, sizeof *out);
   char *const values[] = {out->id, out->client_type, out->principal, out->title, out->created_at, out->last_activity_at, out->claude_session_id, out->outcome, out->source, out->chat_key};
   const size_t caps[] = {sizeof out->id, sizeof out->client_type, sizeof out->principal, sizeof out->title, sizeof out->created_at, sizeof out->last_activity_at, sizeof out->claude_session_id, sizeof out->outcome, sizeof out->source, sizeof out->chat_key};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_GET, fields, 1, values, caps, 10, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   return 0;
}

int db1_server_session_set_outcome(const char *id, const char *outcome)
{
   if (!id || !id[0])
      return -1;
   const char *fields[] = {id, outcome ? outcome : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_SET_OUTCOME, fields, 2, NULL, NULL, 0, NULL));
}

int db1_server_session_delete(const char *id)
{
   if (!id || !id[0])
      return -1;
   const char *fields[] = {id};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_server_session_list_recent(db1_server_session_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 10u + 0u] = out[wire_row].id;
      wire_caps[wire_row * 10u + 0u] = sizeof out[wire_row].id;
      wire_values[wire_row * 10u + 1u] = out[wire_row].client_type;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].client_type;
      wire_values[wire_row * 10u + 2u] = out[wire_row].principal;
      wire_caps[wire_row * 10u + 2u] = sizeof out[wire_row].principal;
      wire_values[wire_row * 10u + 3u] = out[wire_row].title;
      wire_caps[wire_row * 10u + 3u] = sizeof out[wire_row].title;
      wire_values[wire_row * 10u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 10u + 5u] = out[wire_row].last_activity_at;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].last_activity_at;
      wire_values[wire_row * 10u + 6u] = out[wire_row].claude_session_id;
      wire_caps[wire_row * 10u + 6u] = sizeof out[wire_row].claude_session_id;
      wire_values[wire_row * 10u + 7u] = out[wire_row].outcome;
      wire_caps[wire_row * 10u + 7u] = sizeof out[wire_row].outcome;
      wire_values[wire_row * 10u + 8u] = out[wire_row].source;
      wire_caps[wire_row * 10u + 8u] = sizeof out[wire_row].source;
      wire_values[wire_row * 10u + 9u] = out[wire_row].chat_key;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].chat_key;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_LIST_RECENT, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 10), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 10u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 10u);
   return wire_rows;
}

int db1_server_session_search_by_title(const char *pattern, db1_server_session_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {pattern ? pattern : "", arg1};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 10u + 0u] = out[wire_row].id;
      wire_caps[wire_row * 10u + 0u] = sizeof out[wire_row].id;
      wire_values[wire_row * 10u + 1u] = out[wire_row].client_type;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].client_type;
      wire_values[wire_row * 10u + 2u] = out[wire_row].principal;
      wire_caps[wire_row * 10u + 2u] = sizeof out[wire_row].principal;
      wire_values[wire_row * 10u + 3u] = out[wire_row].title;
      wire_caps[wire_row * 10u + 3u] = sizeof out[wire_row].title;
      wire_values[wire_row * 10u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 10u + 5u] = out[wire_row].last_activity_at;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].last_activity_at;
      wire_values[wire_row * 10u + 6u] = out[wire_row].claude_session_id;
      wire_caps[wire_row * 10u + 6u] = sizeof out[wire_row].claude_session_id;
      wire_values[wire_row * 10u + 7u] = out[wire_row].outcome;
      wire_caps[wire_row * 10u + 7u] = sizeof out[wire_row].outcome;
      wire_values[wire_row * 10u + 8u] = out[wire_row].source;
      wire_caps[wire_row * 10u + 8u] = sizeof out[wire_row].source;
      wire_values[wire_row * 10u + 9u] = out[wire_row].chat_key;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].chat_key;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_SEARCH_BY_TITLE, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 10), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 10u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 10u);
   return wire_rows;
}

int db1_server_session_count(const char *since_or_null)
{
   const char *fields[] = {since_or_null ? since_or_null : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_COUNT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_server_session_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_ID_LEN], int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", threshold_seconds);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out_ids, 0, (size_t)max * sizeof *out_ids);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out_ids[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out_ids[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_LIST_EXPIRED, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   return wire_rows;
}

int db1_server_session_delete_expired(int threshold_seconds)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", threshold_seconds);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_DELETE_EXPIRED, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_server_session_list_by_subject(const char *principal,
                                       char (*out_ids)[DB1_SS_ID_LEN], int max)
{
   if (!principal || !principal[0] || !out_ids || max <= 0 || max > 4096)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {principal, arg1};
   char **values = calloc((size_t)max, sizeof(*values));
   size_t *caps = calloc((size_t)max, sizeof(*caps));
   if (!values || !caps)
   {
      free(values);
      free(caps);
      return -1;
   }
   for (int i = 0; i < max; i++)
   {
      values[i] = out_ids[i];
      caps[i] = DB1_SS_ID_LEN;
   }
   uint32_t filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_LIST_BY_SUBJECT, fields, 2, values,
                                caps, (size_t)max, &filled);
   free(values);
   free(caps);
   return wire_status == (int)AIMEE_DB1_STATUS_OK ? (int)filled : -1;
}

int db1_server_session_erase_subject(const char *request_id, const char *principal)
{
   if (!request_id || !request_id[0] || !principal || !principal[0])
      return -1;
   const char *fields[] = {request_id, principal};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_ERASE_SUBJECT, fields, 2, values, caps,
                                1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

char *db1_server_session_erasure_receipt(const char *request_id, const char *principal)
{
 if (!request_id || !request_id[0] || !principal || !principal[0]) return NULL;
 const char *fields[]={request_id,principal};
 size_t capacity=512u*1024u;
 char *receipt=malloc(capacity);if(!receipt)return NULL;
 char *values[]={receipt};const size_t caps[]={capacity};
 int status=db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_ERASURE_RECEIPT,fields,2,values,caps,1,NULL);
 if(status!=(int)AIMEE_DB1_STATUS_OK){free(receipt);return NULL;}
 return receipt;
}

/* Read-only variable-width replies. Retry only these reads when the transport
 * reports the complete frame size; writes must never be replayed here. Decode
 * into owned strings so a large transcript is bounded by the bus contract,
 * rather than a per-cell malloc ceiling. */
static int primary_read(uint32_t op, const char *const *fields, uint32_t count,
                        char ***out, uint32_t *filled)
{
   *out = NULL;
   *filled = 0;
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_SESSIONS))
      return -1;
   size_t request_len;
   if (db1_request_frame_size(fields, count, &request_len) != 0)
      return -1;
   uint8_t *request = malloc(request_len);
   uint32_t capacity = 4096, length = 0;
   uint8_t *response = malloc(capacity);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   db1_request_encode(request, op, fields, count);
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_SESSIONS_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc;
   for (;;)
   {
      rc = obs_bus_module_call(AIMEE_DB1_EVENT_SESSIONS, AIMEE_DB1_STAGE_SESSIONS, 0, deadline,
                              request, (uint32_t)request_len, response, capacity, &length,
                              NULL, NULL);
      if (rc != AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE || length <= capacity ||
          length > AIMEE_MODULE_MESSAGE_MAX_BODY)
         break;
      uint8_t *grown = realloc(response, length);
      if (!grown)
         break;
      response = grown;
      capacity = length;
   }
   free(request);
   int status = -1;
   char **cells = NULL;
   uint32_t n = 0;
   if (rc != AIMEE_MODULE_CALL_OK || length < 8 || length > capacity)
      goto done;
   status = (int)aimee_db1_get_u32(response);
   n = aimee_db1_get_u32(response + 4);
   if (n > (length - 8) / 4 || (status != AIMEE_DB1_STATUS_OK && n))
      goto invalid;
   cells = calloc(n ? n : 1, sizeof(*cells));
   if (!cells)
      goto invalid;
   uint32_t at = 8;
   for (uint32_t i = 0; i < n; i++)
   {
      if (length - at < 4)
         goto invalid;
      uint32_t size = aimee_db1_get_u32(response + at);
      at += 4;
      if (size > length - at || memchr(response + at, 0, size))
         goto invalid;
      cells[i] = malloc((size_t)size + 1);
      if (!cells[i])
         goto invalid;
      memcpy(cells[i], response + at, size);
      cells[i][size] = 0;
      at += size;
   }
   if (at != length)
      goto invalid;
   *out = cells;
   *filled = n;
   goto done;
invalid:
   for (uint32_t i = 0; cells && i < n; i++)
      free(cells[i]);
   free(cells);
   status = -1;
done:
   free(response);
   return status;
}

static void primary_cells_free(char **cells, uint32_t count)
{
   for (uint32_t i = 0; i < count; i++)
      free(cells[i]);
   free(cells);
}

int db1_session_ir_history_load(const char *session_id, const char *scope, const char *actor,
                                const char *active_turn, char **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!session_id || !scope || !actor)
      return -1;
   const char *fields[] = {session_id, scope, actor, active_turn ? active_turn : ""};
   char **cells = NULL;
   uint32_t count = 0;
   int status = primary_read(AIMEE_DB1_OP_SESSION_IR_HISTORY_LOAD, fields, 4, &cells, &count);
   int rc = status == AIMEE_DB1_STATUS_MISSING ? 1 : -1;
   if (status == AIMEE_DB1_STATUS_OK && count == 1)
   {
      *out = cells[0];
      cells[0] = NULL;
      rc = 0;
   }
   primary_cells_free(cells, count);
   return rc;
}

int db1_session_ir_running_history_load(const char *session_id, const char *actor,
                                        const char *active_turn, char **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!session_id || !actor || !active_turn)
      return -1;
   const char *fields[] = {session_id, actor, active_turn};
   char **cells = NULL;
   uint32_t count = 0;
   int status = primary_read(AIMEE_DB1_OP_SESSION_IR_RUNNING_HISTORY_LOAD, fields, 3, &cells, &count);
   int rc = status == AIMEE_DB1_STATUS_MISSING ? 1 : -1;
   if (status == AIMEE_DB1_STATUS_OK && count == 1)
   {
      *out = cells[0];
      cells[0] = NULL;
      rc = 0;
   }
   primary_cells_free(cells, count);
   return rc;
}

int db1_session_ir_history_save(const char *session_id, const char *checkpoint, const char *scope,
                                const char *actor, const char *messages_json)
{
   if (!session_id || !checkpoint || !scope || !actor || !messages_json)
      return -1;
   const char *fields[] = {session_id, checkpoint, scope, actor, messages_json};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SESSION_IR_HISTORY_SAVE, fields, 5,
                                  NULL, NULL, 0, NULL));
}

static int primary_row_take(db1_primary_session_row_t *row, char **cells)
{
   const size_t capacities[] = {sizeof row->session_id, sizeof row->agent_name,
                               sizeof row->provider, sizeof row->created_at, sizeof row->updated_at};
   char *destinations[] = {row->session_id, row->agent_name, row->provider,
                          row->created_at, row->updated_at};
   for (int i = 0; i < 5; i++)
   {
      if (strlen(cells[i]) >= capacities[i])
         return -1;
      memcpy(destinations[i], cells[i], strlen(cells[i]) + 1);
   }
   row->messages_json = cells[5];
   cells[5] = NULL;
   return 0;
}

int db1_primary_session_save(const char *session_id, const char *agent_name, const char *provider, const char *messages_json)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, agent_name ? agent_name : "", provider ? provider : "", messages_json ? messages_json : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_PRIMARY_SESSION_SAVE, fields, 4, NULL, NULL, 0, NULL));
}

char *db1_primary_session_load(const char *session_id, const char *agent_name, const char *provider)
{
   if (!session_id || !session_id[0])
      return NULL;
   const char *fields[] = {session_id, agent_name ? agent_name : "", provider ? provider : ""};
   char **cells = NULL;
   uint32_t count = 0;
   int status = primary_read(AIMEE_DB1_OP_PRIMARY_SESSION_LOAD, fields, 3, &cells, &count);
   char *value = NULL;
   if (status == AIMEE_DB1_STATUS_OK && count == 1 && cells[0][0])
   {
      value = cells[0];
      cells[0] = NULL;
   }
   primary_cells_free(cells, count);
   return value;
}

int db1_primary_session_delete(const char *session_id, const char *agent_name, const char *provider)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, agent_name ? agent_name : "", provider ? provider : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_PRIMARY_SESSION_DELETE, fields, 3, NULL, NULL, 0, NULL));
}

static int primary_rows_read(uint32_t op, const char *const *fields, uint32_t argc,
                              db1_primary_session_row_t **out, int max)
{
   *out = NULL;
   char **cells = NULL;
   uint32_t count = 0;
   int status = primary_read(op, fields, argc, &cells, &count);
   if (status != AIMEE_DB1_STATUS_OK || count % 6 || count / 6 > (uint32_t)max)
   {
      primary_cells_free(cells, count);
      return -1;
   }
   uint32_t n = count / 6;
   db1_primary_session_row_t *rows = calloc(n ? n : 1, sizeof(*rows));
   if (!rows)
   {
      primary_cells_free(cells, count);
      return -1;
   }
   for (uint32_t i = 0; i < n; i++)
      if (primary_row_take(&rows[i], cells + i * 6) != 0)
      {
         db1_primary_session_rows_free(rows, (int)n);
         primary_cells_free(cells, count);
         return -1;
      }
   primary_cells_free(cells, count);
   *out = rows;
   return (int)n;
}

int db1_primary_session_alloc_recent(db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char bound[32];
   snprintf(bound, sizeof bound, "%d", max);
   const char *fields[] = {bound};
   return primary_rows_read(AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_RECENT, fields, 1, out, max);
}

int db1_primary_session_alloc_search(const char *query, db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char bound[32];
   snprintf(bound, sizeof bound, "%d", max);
   const char *fields[] = {query ? query : "", bound};
   return primary_rows_read(AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_SEARCH, fields, 2, out, max);
}

int db1_primary_session_get_latest(const char *session_id, db1_primary_session_row_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   memset(out, 0, sizeof *out);
   const char *fields[] = {session_id};
   char **cells = NULL;
   uint32_t count = 0;
   int status = primary_read(AIMEE_DB1_OP_PRIMARY_SESSION_GET_LATEST, fields, 1, &cells, &count);
   int rc = status == AIMEE_DB1_STATUS_MISSING ? 1 : -1;
   if (status == AIMEE_DB1_STATUS_OK && count == 6)
      rc = primary_row_take(out, cells);
   primary_cells_free(cells, count);
   return rc;
}

int db1_session_write_path_record(const char *session_id, const char *path)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, path ? path : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SESSION_WRITE_PATH_RECORD, fields, 2, NULL, NULL, 0, NULL));
}

int db1_session_stale_reads(const char *parent_session_id, const char *child_session_id, char (*out_paths)[DB1_SESSION_PATH_LEN], int max)
{
   if (!parent_session_id || !parent_session_id[0] || !child_session_id || !child_session_id[0] || !out_paths || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {parent_session_id, child_session_id, arg2};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out_paths, 0, (size_t)max * sizeof *out_paths);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out_paths[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out_paths[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SESSION_STALE_READS, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   return wire_rows;
}

int db1_webchat_claude_session_get(const char *principal, const char *aimee_session_id, char *out, size_t out_n)
{
   if (!principal || !principal[0] || !aimee_session_id || !aimee_session_id[0] || !out || out_n == 0)
      return -1;
   const char *fields[] = {principal, aimee_session_id};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_n, sizeof slot_rc};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_GET, fields, 2, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_webchat_claude_session_owned_by_other(const char *principal, const char *aimee_session_id, const char *claude_session_id)
{
   if (!principal || !principal[0] || !aimee_session_id || !aimee_session_id[0])
      return -1;
   const char *fields[] = {principal, aimee_session_id, claude_session_id ? claude_session_id : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_OWNED_BY_OTHER, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_webchat_claude_session_bind(const char *principal, const char *aimee_session_id, const char *claude_session_id)
{
   if (!principal || !principal[0] || !aimee_session_id || !aimee_session_id[0])
      return -1;
   const char *fields[] = {principal, aimee_session_id, claude_session_id ? claude_session_id : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_BIND, fields, 3, NULL, NULL, 0, NULL));
}

int db1_webchat_live_set(const char *session_id, const char *turn_id, const char *text, const char *status)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, turn_id ? turn_id : "", text ? text : "", status ? status : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WEBCHAT_LIVE_SET, fields, 4, NULL, NULL, 0, NULL));
}

int db1_webchat_live_get(const char *session_id, long long since_rev, char **turn_id, char **text, char **status, long long *rev)
{
   if (!session_id || !session_id[0] || !turn_id || !text || !status || !rev)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)since_rev);
   const char *fields[] = {session_id, arg1};
   char *held0 = malloc(256u);
   if (!held0)
   {
      return -1;
   }
   held0[0] = '\0';
   char *held1 = malloc(1048576u);
   if (!held1)
   {
      free(held0);
      return -1;
   }
   held1[0] = '\0';
   char *held2 = malloc(64u);
   if (!held2)
   {
      free(held0);
      free(held1);
      return -1;
   }
   held2[0] = '\0';
   char slot3[32];
   char *const values[] = {held0, held1, held2, slot3};
   const size_t caps[] = {256u, 1048576u, 64u, sizeof slot3};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WEBCHAT_LIVE_GET, fields, 2, values, caps, 4, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(held0);
      free(held1);
      free(held2);
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   *rev = (int64_t)strtoll(slot3, NULL, 10);
   char *shrunk0 = realloc(held0, strlen(held0) + 1u);
   *turn_id = shrunk0 ? shrunk0 : held0;
   char *shrunk1 = realloc(held1, strlen(held1) + 1u);
   *text = shrunk1 ? shrunk1 : held1;
   char *shrunk2 = realloc(held2, strlen(held2) + 1u);
   *status = shrunk2 ? shrunk2 : held2;
   return 1;
}

int db1_server_session_persona_delivery_claim(const char *id)
{
   if (!id || !id[0])
      return -1;
   const char *fields[] = {id};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_PERSONA_DELIVERY_CLAIM, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_server_session_persona_delivery_finish(const char *id, int delivered)
{
   if (!id || !id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", delivered);
   const char *fields[] = {id, arg1};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SERVER_SESSION_PERSONA_DELIVERY_FINISH, fields, 2, NULL, NULL, 0, NULL));
}

/* clang-format on */

int db1_session_ir_append(const char *session_id, const char *event_id, const char *turn_id,
                          const char *actor, const char *payload, char out_sequence[32])
{
   if (out_sequence)
      out_sequence[0] = '\0';
   if (!session_id || !event_id || !turn_id || !actor || !payload || !out_sequence)
      return -1;
   const char *fields[] = {session_id, event_id, turn_id, actor, payload};
   char *const values[] = {out_sequence};
   const size_t caps[] = {32u};
   return write_result(
       db1_call(&family, AIMEE_DB1_OP_SESSION_IR_APPEND, fields, 5, values, caps, 1, NULL));
}

int db1_session_ir_append_verified(const char *session_id, const char *event_id,
                                   const char *turn_id, const char *actor, const char *payload,
                                   char out_sequence[32])
{
   if (out_sequence)
      out_sequence[0] = '\0';
   if (!session_id || !event_id || !turn_id || !actor || !payload || !out_sequence)
      return -1;
   const char *fields[] = {session_id, event_id, turn_id, actor, payload};
   char *const values[] = {out_sequence};
   const size_t caps[] = {32u};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SESSION_IR_APPEND_VERIFIED, fields, 5, values,
                                caps, 1, NULL));
}

char *db1_session_ir_page(const char *session_id, const char *after, int limit,
                          const char *principal)
{
   if (!session_id || !after || limit < 1 || limit > 128)
      return NULL;
   char bound[32];
   snprintf(bound, sizeof(bound), "%d", limit);
   const char *fields[] = {session_id, after, bound, principal};
   char *json = malloc(4u * 1024u * 1024u);
   if (!json)
      return NULL;
   char *const values[] = {json};
   const size_t caps[] = {4u * 1024u * 1024u};
   if (db1_call(&family, AIMEE_DB1_OP_SESSION_IR_PAGE, fields, 4, values, caps, 1, NULL) !=
       AIMEE_DB1_STATUS_OK)
   {
      free(json);
      return NULL;
   }
   return json;
}

int db1_session_harness_get(const char *session_id, const char *harness, char *out_native_id,
                            size_t capacity, const char *principal)
{
   if (!session_id || !harness || !out_native_id || !capacity)
      return -1;
   const char *fields[] = {session_id, harness, principal};
   char *values[] = {out_native_id};
   const size_t caps[] = {capacity};
   int status =
       db1_call(&family, AIMEE_DB1_OP_SESSION_HARNESS_GET, fields, 3, values, caps, 1, NULL);
   return status == AIMEE_DB1_STATUS_OK ? 0 : status == AIMEE_DB1_STATUS_MISSING ? 1 : -1;
}
int db1_session_harness_bind(const char *session_id, const char *harness, const char *native_id,
                             const char *principal)
{
   if (!session_id || !harness || !native_id)
      return -1;
   const char *fields[] = {session_id, harness, native_id, principal};
   return write_result(
       db1_call(&family, AIMEE_DB1_OP_SESSION_HARNESS_BIND, fields, 4, NULL, NULL, 0, NULL));
}

int db1_session_harness_compare_swap(const char *session_id, const char *harness,
                                     const char *expected, const char *replacement,
                                     const char *principal)
{
   if (!session_id || !harness || !expected || !replacement || !principal)
      return -1;
   const char *fields[] = {session_id, harness, expected, replacement, principal};
   int status =
       db1_call(&family, AIMEE_DB1_OP_SESSION_HARNESS_COMPARE_SWAP, fields, 5, NULL, NULL, 0, NULL);
   return status == AIMEE_DB1_STATUS_OK ? 0 : status == AIMEE_DB1_STATUS_MISSING ? 1 : -1;
}

char *db1_session_ir_search(const char *principal, const char *request_json)
{
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   char **cells = NULL;
   uint32_t count = 0;
   char *result = NULL;
   int status = primary_read(AIMEE_DB1_OP_SESSION_IR_SEARCH, fields, 2, &cells, &count);
   if (status == AIMEE_DB1_STATUS_OK && count == 1)
   {
      result = cells[0];
      cells[0] = NULL;
   }
   primary_cells_free(cells, count);
   return result;
}

int db1_session_native_owner_claim(const char *principal, const char *request_json)
{
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   return write_result(
       db1_call(&family, AIMEE_DB1_OP_SESSION_NATIVE_OWNER_CLAIM, fields, 2, NULL, NULL, 0, NULL));
}
int db1_session_native_reconcile(const char *principal, const char *request_json,
                                 char out_sequence[32])
{
   if (!out_sequence)
      return -1;
   out_sequence[0] = 0;
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   char *const values[] = {out_sequence};
   const size_t caps[] = {32u};
   return write_result(
       db1_call(&family, AIMEE_DB1_OP_SESSION_NATIVE_RECONCILE, fields, 2, values, caps, 1, NULL));
}

static int native_epoch_call(uint32_t op, const char *principal, const char *request_json,
                             char out_epoch[33])
{
   if (!out_epoch)
      return -1;
   out_epoch[0] = 0;
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   char *const values[] = {out_epoch};
   const size_t caps[] = {33u};
   return write_result(db1_call(&family, op, fields, 2, values, caps, 1, NULL));
}
int db1_session_native_reserve(const char *principal, const char *request_json, char out_epoch[33])
{
   return native_epoch_call(AIMEE_DB1_OP_SESSION_NATIVE_RESERVE, principal, request_json,
                            out_epoch);
}
int db1_session_native_epoch(const char *principal, const char *request_json, char out_epoch[33])
{
   return native_epoch_call(AIMEE_DB1_OP_SESSION_NATIVE_EPOCH, principal, request_json, out_epoch);
}
int db1_session_native_reservation_reconcile(const char *principal, const char *request_json,
                                             char out_sequence[32], char out_epoch[33])
{
   if (!out_sequence || !out_epoch)
      return -1;
   out_sequence[0] = 0;
   out_epoch[0] = 0;
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   char *const values[] = {out_sequence, out_epoch};
   const size_t caps[] = {32u, 33u};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SESSION_NATIVE_RESERVATION_RECONCILE, fields,
                                2, values, caps, 2, NULL));
}
int db1_session_native_reservation_lookup_checked(const char *principal, const char *request_json,
                                                  char **out_json)
{
   if (!out_json)
      return -1;
   *out_json = NULL;
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   char **cells = NULL;
   uint32_t count = 0;
   int status =
       primary_read(AIMEE_DB1_OP_SESSION_NATIVE_RESERVATION_LOOKUP, fields, 2, &cells, &count);
   int rc = -1;
   if (status == AIMEE_DB1_STATUS_OK && count == 1 && cells[0] && cells[0][0])
   {
      *out_json = cells[0];
      cells[0] = NULL;
      rc = 0;
   }
   else if (status == AIMEE_DB1_STATUS_MISSING && !count)
      rc = 1;
   primary_cells_free(cells, count);
   return rc;
}
char *db1_session_native_reservation_lookup(const char *principal, const char *request_json)
{
   char *result = NULL;
   return db1_session_native_reservation_lookup_checked(principal, request_json, &result) ? NULL
                                                                                          : result;
}

int db1_session_native_reservation_finish(const char *principal, const char *request_json,
                                          char out_sequence[32], char out_epoch[33])
{
   if (!out_sequence || !out_epoch)
      return -1;
   out_sequence[0] = 0;
   out_epoch[0] = 0;
   const char *fields[] = {principal ? principal : "", request_json ? request_json : ""};
   char *const values[] = {out_sequence, out_epoch};
   const size_t caps[] = {32u, 33u};
   return write_result(db1_call(&family, AIMEE_DB1_OP_SESSION_NATIVE_RESERVATION_FINISH, fields, 2,
                                values, caps, 2, NULL));
}
