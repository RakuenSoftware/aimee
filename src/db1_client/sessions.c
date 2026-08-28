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

static void warn_unreachable(int reason)
{
   static int warned;
   if (warned)
      return;
   warned = 1;
   /* Said once per process: enough to tell a store that is down from one that
      is quiet, without one line per call. The numeric
      aimee_module_call_result_t, not its name, so this does not pull the whole
      event-bus library in behind the client for one string. */
   LOG_WARN("db1.sessions", "DB1 %s is unreachable (module call result %d)", "sessions",
            reason);
}

/* Size the frame from the arguments themselves.

   These carry prompts, results and JSON documents, not just identifiers, and
   in-process callers have always passed them whole. A fixed cap here would
   refuse exactly those calls and return the same -1 as a broken store -- fine
   in a test with short strings, wrong the first time a real prompt arrives. The
   bus bounds the message instead. */
static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{
   /* Zero fields is a legal request: an operation that takes no arguments
      sends the header alone. The upper bound still applies. */
   if (count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      /* Empty is legal on the wire: an optional field the caller left out
         travels as zero length. Which fields may be empty is the operation's
         business, checked before the frame is built. */
      if (!fields[i])
         return -1;
      size_t n = strlen(fields[i]);
      if (n > AIMEE_MODULE_MESSAGE_MAX_BODY - need - 4u)
         return -1;
      need += 4u + n;
   }
   *need_out = need;
   return 0;
}

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static void encode(uint8_t *out, uint32_t op, const char *const *fields, uint32_t count)
{
   uint32_t at = 0;
   aimee_db1_put_u32(out + at, op);
   at += 4u;
   aimee_db1_put_u32(out + at, count);
   at += 4u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(fields[i]);
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, fields[i], n);
      at += n;
   }
}

/* Returns the module's status, or -1 when the call never produced one. */
/* Fills up to `slots` reply values, each into the buffer and capacity the
   caller supplied. A write passes none; a read passes one; a row passes one per
   member; a list passes one per member per row it is willing to accept.

   `filled_out` reports how many values the reply actually carried, which is how
   a list learns its length: the rows are not counted separately on the wire
   because an operation already knows how wide its rows are. Callers that expect
   a fixed shape pass NULL. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *const *values,
                      const size_t *caps, uint32_t slots, uint32_t *filled_out)
{
   if (filled_out)
      *filled_out = 0u;
   for (uint32_t i = 0; i < slots; ++i)
      if (values[i] && caps[i])
         values[i][0] = '\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_SESSIONS))
   {
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }

   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
      return -1;
   /* The reply is bounded by the caller's own buffer: it asked for at most
      value_len bytes, so there is no reason to hold more than that. */
   size_t response_cap = 8u;
   for (uint32_t i = 0; i < slots; ++i)
      response_cap += 4u + caps[i];
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   encode(request, op, fields, count);

   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_SESSIONS_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_SESSIONS, AIMEE_DB1_STAGE_SESSIONS, 0, deadline,
                           request, (uint32_t)request_len, response, (uint32_t)response_cap,
                           &response_len, NULL, NULL);
   free(request);

   int result = -1;
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
      warn_unreachable((int)rc);
   else
   {
      uint32_t status = aimee_db1_get_u32(response);
      uint32_t fields_in = aimee_db1_get_u32(response + 4u);
      /* Read the reply's own count rather than assuming an arity: a status with
         no values is how a write answers, one value is a read, and a member
         apiece is a row. */
      result = (int)status;
      /* More values than the caller has room for is a contract mismatch, not
         something to read the first few of: the caller asked for at most this
         many rows, and a stage answering with more is not answering this call. */
      if (fields_in > slots)
         result = -1;
      else if (filled_out)
         *filled_out = fields_in;
      /* Fewer values than the caller has slots for is the same contract
         mismatch read from the other side, and it used to pass: the unfilled
         slots keep the empty string cleared above, so the caller reads a row
         whose last members are blank and cannot tell that from a row that is
         blank. A list says how many rows it found through filled_out and is
         variable by construction; every other shape has one arity, and a stage
         answering with a different one is a stage built against a different
         version of this contract. Two processes, two binaries, two deployment
         times -- so say it rather than zero-fill. */
      else if (status == (uint32_t)AIMEE_DB1_STATUS_OK && fields_in != slots)
         result = -1;
      uint32_t at = 8u;
      for (uint32_t i = 0; i < fields_in && result != -1; ++i)
      {
         if (at + 4u > response_len)
         {
            result = -1;
            break;
         }
         uint32_t n = aimee_db1_get_u32(response + at);
         at += 4u;
         /* A reply whose declared length runs past what arrived is not a reply
            to read part of. */
         if (at + n > response_len)
         {
            result = -1;
            break;
         }
         if (i < slots && values[i] && caps[i])
         {
            /* No room for the terminator is no room: writing it would land one
               byte past the buffer the caller owns. */
            if (n >= caps[i])
               result = -1;
            else
            {
               memcpy(values[i], response + at, n);
               values[i][n] = '\0';
            }
         }
         at += n;
      }
   }
   free(response);
   return result;
}

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
   return write_result(call_stage(AIMEE_DB1_OP_SERVER_SESSION_CREATE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   if (!id || !id[0] || !out)
      return -1;
   const char *fields[] = {id};
   memset(out, 0, sizeof *out);
   char *const values[] = {out->id, out->client_type, out->principal, out->title, out->created_at, out->last_activity_at, out->claude_session_id, out->outcome, out->source, out->chat_key};
   const size_t caps[] = {sizeof out->id, sizeof out->client_type, sizeof out->principal, sizeof out->title, sizeof out->created_at, sizeof out->last_activity_at, sizeof out->claude_session_id, sizeof out->outcome, sizeof out->source, sizeof out->chat_key};
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_GET, fields, 1, values, caps, 10, NULL);
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
   return write_result(call_stage(AIMEE_DB1_OP_SERVER_SESSION_SET_OUTCOME, fields, 2, NULL, NULL, 0, NULL));
}

int db1_server_session_delete(const char *id)
{
   if (!id || !id[0])
      return -1;
   const char *fields[] = {id};
   return write_result(call_stage(AIMEE_DB1_OP_SERVER_SESSION_DELETE, fields, 1, NULL, NULL, 0, NULL));
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_LIST_RECENT, fields, 1, wire_values, wire_caps,
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_SEARCH_BY_TITLE, fields, 2, wire_values, wire_caps,
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_COUNT, fields, 1, values, caps, 1, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_LIST_EXPIRED, fields, 2, wire_values, wire_caps,
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_DELETE_EXPIRED, fields, 1, values, caps, 1, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_LIST_BY_SUBJECT, fields, 2, values,
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_ERASE_SUBJECT, fields, 2, values, caps,
                                1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_primary_session_save(const char *session_id, const char *agent_name, const char *provider, const char *messages_json)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, agent_name ? agent_name : "", provider ? provider : "", messages_json ? messages_json : ""};
   return write_result(call_stage(AIMEE_DB1_OP_PRIMARY_SESSION_SAVE, fields, 4, NULL, NULL, 0, NULL));
}

char *db1_primary_session_load(const char *session_id, const char *agent_name, const char *provider)
{
   if (!session_id || !session_id[0])
      return NULL;
   const char *fields[] = {session_id, agent_name ? agent_name : "", provider ? provider : ""};
   char *value = malloc(1048576u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {1048576u};
   int wire_status = call_stage(AIMEE_DB1_OP_PRIMARY_SESSION_LOAD, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

int db1_primary_session_delete(const char *session_id, const char *agent_name, const char *provider)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, agent_name ? agent_name : "", provider ? provider : ""};
   return write_result(call_stage(AIMEE_DB1_OP_PRIMARY_SESSION_DELETE, fields, 3, NULL, NULL, 0, NULL));
}

int db1_primary_session_alloc_recent(db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   db1_primary_session_row_t *wire_held = calloc((size_t)max, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)max * sizeof *wire_held);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = wire_held[wire_row].session_id;
      wire_caps[wire_row * 6u + 0u] = sizeof wire_held[wire_row].session_id;
      wire_values[wire_row * 6u + 1u] = wire_held[wire_row].agent_name;
      wire_caps[wire_row * 6u + 1u] = sizeof wire_held[wire_row].agent_name;
      wire_values[wire_row * 6u + 2u] = wire_held[wire_row].provider;
      wire_caps[wire_row * 6u + 2u] = sizeof wire_held[wire_row].provider;
      wire_values[wire_row * 6u + 3u] = wire_held[wire_row].created_at;
      wire_caps[wire_row * 6u + 3u] = sizeof wire_held[wire_row].created_at;
      wire_values[wire_row * 6u + 4u] = wire_held[wire_row].updated_at;
      wire_caps[wire_row * 6u + 4u] = sizeof wire_held[wire_row].updated_at;
      wire_held[wire_row].messages_json = malloc(1048576u);
      if (!wire_held[wire_row].messages_json)
      {
         for (int wire_done = 0; wire_done < wire_row; ++wire_done)
         {
            free(wire_held[wire_done].messages_json);
            wire_held[wire_done].messages_json = NULL;
         }
         free(wire_values);
         free(wire_caps);
         free(wire_held);
         return -1;
      }
      wire_held[wire_row].messages_json[0] = '\0';
      wire_values[wire_row * 6u + 5u] = wire_held[wire_row].messages_json;
      wire_caps[wire_row * 6u + 5u] = 1048576u;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_RECENT, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      for (int wire_done = 0; wire_done < max; ++wire_done)
      {
         free(wire_held[wire_done].messages_json);
         wire_held[wire_done].messages_json = NULL;
      }
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   for (int wire_row = wire_rows; wire_row < max; ++wire_row)
   {
      free(wire_held[wire_row].messages_json);
      wire_held[wire_row].messages_json = NULL;
   }
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      char *wire_shrunk = realloc(wire_held[wire_row].messages_json, strlen(wire_held[wire_row].messages_json) + 1u);
      if (wire_shrunk)
         wire_held[wire_row].messages_json = wire_shrunk;
   }
   *out = wire_held;
   return wire_rows;
}

int db1_primary_session_alloc_search(const char *query, db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {query ? query : "", arg1};
   db1_primary_session_row_t *wire_held = calloc((size_t)max, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)max * sizeof *wire_held);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = wire_held[wire_row].session_id;
      wire_caps[wire_row * 6u + 0u] = sizeof wire_held[wire_row].session_id;
      wire_values[wire_row * 6u + 1u] = wire_held[wire_row].agent_name;
      wire_caps[wire_row * 6u + 1u] = sizeof wire_held[wire_row].agent_name;
      wire_values[wire_row * 6u + 2u] = wire_held[wire_row].provider;
      wire_caps[wire_row * 6u + 2u] = sizeof wire_held[wire_row].provider;
      wire_values[wire_row * 6u + 3u] = wire_held[wire_row].created_at;
      wire_caps[wire_row * 6u + 3u] = sizeof wire_held[wire_row].created_at;
      wire_values[wire_row * 6u + 4u] = wire_held[wire_row].updated_at;
      wire_caps[wire_row * 6u + 4u] = sizeof wire_held[wire_row].updated_at;
      wire_held[wire_row].messages_json = malloc(1048576u);
      if (!wire_held[wire_row].messages_json)
      {
         for (int wire_done = 0; wire_done < wire_row; ++wire_done)
         {
            free(wire_held[wire_done].messages_json);
            wire_held[wire_done].messages_json = NULL;
         }
         free(wire_values);
         free(wire_caps);
         free(wire_held);
         return -1;
      }
      wire_held[wire_row].messages_json[0] = '\0';
      wire_values[wire_row * 6u + 5u] = wire_held[wire_row].messages_json;
      wire_caps[wire_row * 6u + 5u] = 1048576u;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_SEARCH, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      for (int wire_done = 0; wire_done < max; ++wire_done)
      {
         free(wire_held[wire_done].messages_json);
         wire_held[wire_done].messages_json = NULL;
      }
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   for (int wire_row = wire_rows; wire_row < max; ++wire_row)
   {
      free(wire_held[wire_row].messages_json);
      wire_held[wire_row].messages_json = NULL;
   }
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      char *wire_shrunk = realloc(wire_held[wire_row].messages_json, strlen(wire_held[wire_row].messages_json) + 1u);
      if (wire_shrunk)
         wire_held[wire_row].messages_json = wire_shrunk;
   }
   *out = wire_held;
   return wire_rows;
}

int db1_primary_session_get_latest(const char *session_id, db1_primary_session_row_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   const char *fields[] = {session_id};
   memset(out, 0, sizeof *out);
   out->messages_json = malloc(1048576u);
   if (!out->messages_json)
   {
      free(out->messages_json);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->messages_json[0] = '\0';
   char *const values[] = {out->session_id, out->agent_name, out->provider, out->created_at, out->updated_at, out->messages_json};
   const size_t caps[] = {sizeof out->session_id, sizeof out->agent_name, sizeof out->provider, sizeof out->created_at, sizeof out->updated_at, 1048576u};
   int wire_status = call_stage(AIMEE_DB1_OP_PRIMARY_SESSION_GET_LATEST, fields, 1, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->messages_json);
      memset(out, 0, sizeof *out);
      return -1;
   }
   char *shrunk_messages_json = realloc(out->messages_json, strlen(out->messages_json) + 1u);
   if (shrunk_messages_json)
      out->messages_json = shrunk_messages_json;
   return 0;
}

int db1_session_write_path_record(const char *session_id, const char *path)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, path ? path : ""};
   return write_result(call_stage(AIMEE_DB1_OP_SESSION_WRITE_PATH_RECORD, fields, 2, NULL, NULL, 0, NULL));
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
   int wire_status = call_stage(AIMEE_DB1_OP_SESSION_STALE_READS, fields, 3, wire_values, wire_caps,
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
   int wire_status = call_stage(AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_GET, fields, 2, values, caps, 2, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_OWNED_BY_OTHER, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_webchat_claude_session_bind(const char *principal, const char *aimee_session_id, const char *claude_session_id)
{
   if (!principal || !principal[0] || !aimee_session_id || !aimee_session_id[0])
      return -1;
   const char *fields[] = {principal, aimee_session_id, claude_session_id ? claude_session_id : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_BIND, fields, 3, NULL, NULL, 0, NULL));
}

int db1_webchat_live_set(const char *session_id, const char *turn_id, const char *text, const char *status)
{
   if (!session_id || !session_id[0])
      return -1;
   const char *fields[] = {session_id, turn_id ? turn_id : "", text ? text : "", status ? status : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WEBCHAT_LIVE_SET, fields, 4, NULL, NULL, 0, NULL));
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
   int wire_status = call_stage(AIMEE_DB1_OP_WEBCHAT_LIVE_GET, fields, 2, values, caps, 4, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_SERVER_SESSION_PERSONA_DELIVERY_CLAIM, fields, 1, values, caps, 1, NULL);
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
   return write_result(call_stage(AIMEE_DB1_OP_SERVER_SESSION_PERSONA_DELIVERY_FINISH, fields, 2, NULL, NULL, 0, NULL));
}

/* clang-format on */
