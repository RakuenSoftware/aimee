/* db1_client/runtime.c: the runtime family, reached over the bus.
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
#include "caches.h"
#include "decisions.h"
#include "env.h"
#include "fsnap.h"
#include "local_operator.h"
#include "mcp_osv_cache.h"
#include "model_catalog.h"
#include "db1_client/model_pricing.h"
#include "project_clones.h"
#include "runtime_state.h"
#include "tool_local_availability.h"
#include "web_page_cache.h"
#include "working_profile_local.h"

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

#define DB1_RUNTIME_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.runtime", "DB1 %s is unreachable (module call result %d)", "runtime",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_RUNTIME))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_RUNTIME_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_RUNTIME, AIMEE_DB1_STAGE_RUNTIME, 0, deadline,
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


int db1_runtime_state_set(const char *key, const char *value)
{
   if (!key || !key[0])
      return -1;
   const char *fields[] = {key, value ? value : ""};
   return write_result(call_stage(AIMEE_DB1_OP_RUNTIME_STATE_SET, fields, 2, NULL, NULL, 0, NULL));
}

int db1_runtime_state_get(const char *key, char *out, size_t out_len)
{
   if (!key || !key[0] || !out || out_len == 0)
      return -1;
   const char *fields[] = {key};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_len, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_RUNTIME_STATE_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_runtime_state_add_int(const char *key, int delta, int *new_value_out)
{
   if (!key || !key[0] || !new_value_out)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", delta);
   const char *fields[] = {key, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_RUNTIME_STATE_ADD_INT, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *new_value_out = (int)strtol(slot0, NULL, 10);
   return 0;
}

int db1_project_clone_upsert(const char *clone_path, const char *project_uuid, const char *canonical_url, const char *origin_url, const char *upstream_url)
{
   if (!clone_path || !clone_path[0])
      return -1;
   const char *fields[] = {clone_path, project_uuid ? project_uuid : "", canonical_url ? canonical_url : "", origin_url ? origin_url : "", upstream_url ? upstream_url : ""};
   return write_result(call_stage(AIMEE_DB1_OP_PROJECT_CLONE_UPSERT, fields, 5, NULL, NULL, 0, NULL));
}

int db1_project_clone_get(const char *clone_path, db1_project_clone_t *out)
{
   if (!clone_path || !clone_path[0] || !out)
      return -1;
   const char *fields[] = {clone_path};
   memset(out, 0, sizeof *out);
   char *const values[] = {out->clone_path, out->project_uuid, out->canonical_url, out->origin_url, out->upstream_url, out->last_seen_at};
   const size_t caps[] = {sizeof out->clone_path, sizeof out->project_uuid, sizeof out->canonical_url, sizeof out->origin_url, sizeof out->upstream_url, sizeof out->last_seen_at};
   int wire_status = call_stage(AIMEE_DB1_OP_PROJECT_CLONE_GET, fields, 1, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   return 0;
}

int db1_project_clone_delete(const char *clone_path)
{
   if (!clone_path || !clone_path[0])
      return -1;
   const char *fields[] = {clone_path};
   return write_result(call_stage(AIMEE_DB1_OP_PROJECT_CLONE_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_project_clone_list(db1_project_clone_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = out[wire_row].clone_path;
      wire_caps[wire_row * 6u + 0u] = sizeof out[wire_row].clone_path;
      wire_values[wire_row * 6u + 1u] = out[wire_row].project_uuid;
      wire_caps[wire_row * 6u + 1u] = sizeof out[wire_row].project_uuid;
      wire_values[wire_row * 6u + 2u] = out[wire_row].canonical_url;
      wire_caps[wire_row * 6u + 2u] = sizeof out[wire_row].canonical_url;
      wire_values[wire_row * 6u + 3u] = out[wire_row].origin_url;
      wire_caps[wire_row * 6u + 3u] = sizeof out[wire_row].origin_url;
      wire_values[wire_row * 6u + 4u] = out[wire_row].upstream_url;
      wire_caps[wire_row * 6u + 4u] = sizeof out[wire_row].upstream_url;
      wire_values[wire_row * 6u + 5u] = out[wire_row].last_seen_at;
      wire_caps[wire_row * 6u + 5u] = sizeof out[wire_row].last_seen_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_PROJECT_CLONE_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   return wire_rows;
}

int db1_project_clone_list_by_project(const char *project_uuid, db1_project_clone_t *out, int max)
{
   if (!project_uuid || !project_uuid[0] || !out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {project_uuid, arg1};
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = out[wire_row].clone_path;
      wire_caps[wire_row * 6u + 0u] = sizeof out[wire_row].clone_path;
      wire_values[wire_row * 6u + 1u] = out[wire_row].project_uuid;
      wire_caps[wire_row * 6u + 1u] = sizeof out[wire_row].project_uuid;
      wire_values[wire_row * 6u + 2u] = out[wire_row].canonical_url;
      wire_caps[wire_row * 6u + 2u] = sizeof out[wire_row].canonical_url;
      wire_values[wire_row * 6u + 3u] = out[wire_row].origin_url;
      wire_caps[wire_row * 6u + 3u] = sizeof out[wire_row].origin_url;
      wire_values[wire_row * 6u + 4u] = out[wire_row].upstream_url;
      wire_caps[wire_row * 6u + 4u] = sizeof out[wire_row].upstream_url;
      wire_values[wire_row * 6u + 5u] = out[wire_row].last_seen_at;
      wire_caps[wire_row * 6u + 5u] = sizeof out[wire_row].last_seen_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_PROJECT_CLONE_LIST_BY_PROJECT, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   return wire_rows;
}

int db1_local_operator_upsert(const char *secret_ref, const char *operator_uuid, int active, const char *display_hint)
{
   if (!secret_ref || !secret_ref[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", active);
   const char *fields[] = {secret_ref, operator_uuid ? operator_uuid : "", arg2, display_hint ? display_hint : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_LOCAL_OPERATOR_UPSERT, fields, 4, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_local_operator_get(const char *secret_ref, db1_local_operator_t *out)
{
   if (!secret_ref || !secret_ref[0] || !out)
      return -1;
   const char *fields[] = {secret_ref};
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->secret_ref, out->operator_uuid, slot2, out->display_hint, out->created_at};
   const size_t caps[] = {sizeof out->secret_ref, sizeof out->operator_uuid, sizeof slot2, sizeof out->display_hint, sizeof out->created_at};
   int wire_status = call_stage(AIMEE_DB1_OP_LOCAL_OPERATOR_GET, fields, 1, values, caps, 5, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->active = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_local_operator_get_active(db1_local_operator_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->secret_ref, out->operator_uuid, slot2, out->display_hint, out->created_at};
   const size_t caps[] = {sizeof out->secret_ref, sizeof out->operator_uuid, sizeof slot2, sizeof out->display_hint, sizeof out->created_at};
   int wire_status = call_stage(AIMEE_DB1_OP_LOCAL_OPERATOR_GET_ACTIVE, fields, 0, values, caps, 5, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->active = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_local_operator_set_active(const char *secret_ref)
{
   if (!secret_ref || !secret_ref[0])
      return -1;
   const char *fields[] = {secret_ref};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_LOCAL_OPERATOR_SET_ACTIVE, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_local_operator_delete(const char *secret_ref)
{
   if (!secret_ref || !secret_ref[0])
      return -1;
   const char *fields[] = {secret_ref};
   return write_result(call_stage(AIMEE_DB1_OP_LOCAL_OPERATOR_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_local_operator_list(db1_local_operator_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 5u + 0u] = out[wire_row].secret_ref;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].secret_ref;
      wire_values[wire_row * 5u + 1u] = out[wire_row].operator_uuid;
      wire_caps[wire_row * 5u + 1u] = sizeof out[wire_row].operator_uuid;
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 5u + 3u] = out[wire_row].display_hint;
      wire_caps[wire_row * 5u + 3u] = sizeof out[wire_row].display_hint;
      wire_values[wire_row * 5u + 4u] = out[wire_row].created_at;
      wire_caps[wire_row * 5u + 4u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_LOCAL_OPERATOR_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 5u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 5u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].active = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_env_capability_set(const char *key, const char *value)
{
   if (!key || !key[0])
      return -1;
   const char *fields[] = {key, value ? value : ""};
   return write_result(call_stage(AIMEE_DB1_OP_ENV_CAPABILITY_SET, fields, 2, NULL, NULL, 0, NULL));
}

int db1_env_capability_get(const char *key, char *value_out, size_t value_len, char *detected_at_out, size_t detected_at_len)
{
   if (!key || !key[0] || !value_out || value_len == 0 || !detected_at_out || detected_at_len == 0)
      return -1;
   const char *fields[] = {key};
   char *const values[] = {value_out, detected_at_out};
   const size_t caps[] = {value_len, detected_at_len};
   int wire_status = call_stage(AIMEE_DB1_OP_ENV_CAPABILITY_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   return 1;
}

int db1_env_capability_list(db1_env_capability_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 3u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 3u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 3u + 0u] = out[wire_row].key;
      wire_caps[wire_row * 3u + 0u] = sizeof out[wire_row].key;
      wire_values[wire_row * 3u + 1u] = out[wire_row].value;
      wire_caps[wire_row * 3u + 1u] = sizeof out[wire_row].value;
      wire_values[wire_row * 3u + 2u] = out[wire_row].detected_at;
      wire_caps[wire_row * 3u + 2u] = sizeof out[wire_row].detected_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_ENV_CAPABILITY_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 3), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 3u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 3u);
   return wire_rows;
}

int db1_maintenance_state_load(const char *key, db1_maintenance_state_t *out)
{
   if (!key || !key[0] || !out)
      return -1;
   const char *fields[] = {key};
   char slot0[32];
   char slot2[32];
   char slot3[32];
   char slot4[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->last_run_at, slot2, slot3, slot4, out->last_summary_json};
   const size_t caps[] = {sizeof slot0, sizeof out->last_run_at, sizeof slot2, sizeof slot3, sizeof slot4, sizeof out->last_summary_json};
   int wire_status = call_stage(AIMEE_DB1_OP_MAINTENANCE_STATE_LOAD, fields, 1, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->present = (int)strtol(slot0, NULL, 10);
   out->last_memory_count = (int64_t)strtoll(slot2, NULL, 10);
   out->last_changes = (int)strtol(slot3, NULL, 10);
   out->last_elapsed_ms = strtod(slot4, NULL);
   return 0;
}

int db1_maintenance_state_save(const char *key, const db1_maintenance_state_t *st)
{
   if (!key || !key[0] || !st)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", st->present);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%lld", (long long)st->last_memory_count);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", st->last_changes);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%.17g", (double)st->last_elapsed_ms);
   const char *fields[] = {key, arg1, st->last_run_at, arg3, arg4, arg5, st->last_summary_json};
   return write_result(call_stage(AIMEE_DB1_OP_MAINTENANCE_STATE_SAVE, fields, 7, NULL, NULL, 0, NULL));
}

int db1_model_catalog_is_fresh(const char *provider, int ttl_seconds)
{
   if (!provider || !provider[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", ttl_seconds);
   const char *fields[] = {provider, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_MODEL_CATALOG_IS_FRESH, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_model_catalog_get(const char *provider, provider_model_t **models_out, int *n_out)
{
   if (!provider || !provider[0] || !models_out || !n_out)
      return -1;
   const char *fields[] = {provider};
   provider_model_t *wire_held = calloc((size_t)512, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)512 * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)512 * 6u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)512 * 4u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)512 * sizeof *wire_held);
   for (int wire_row = 0; wire_row < 512; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = wire_held[wire_row].id;
      wire_caps[wire_row * 6u + 0u] = sizeof wire_held[wire_row].id;
      wire_values[wire_row * 6u + 1u] = wire_held[wire_row].display_name;
      wire_caps[wire_row * 6u + 1u] = sizeof wire_held[wire_row].display_name;
      wire_values[wire_row * 6u + 2u] = wire_scratch[wire_row * 4u + 0u];
      wire_caps[wire_row * 6u + 2u] = sizeof wire_scratch[wire_row * 4u + 0u];
      wire_values[wire_row * 6u + 3u] = wire_scratch[wire_row * 4u + 1u];
      wire_caps[wire_row * 6u + 3u] = sizeof wire_scratch[wire_row * 4u + 1u];
      wire_values[wire_row * 6u + 4u] = wire_scratch[wire_row * 4u + 2u];
      wire_caps[wire_row * 6u + 4u] = sizeof wire_scratch[wire_row * 4u + 2u];
      wire_values[wire_row * 6u + 5u] = wire_scratch[wire_row * 4u + 3u];
      wire_caps[wire_row * 6u + 5u] = sizeof wire_scratch[wire_row * 4u + 3u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_MODEL_CATALOG_GET, fields, 1, wire_values, wire_caps,
                           (uint32_t)(512 * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      wire_held[wire_row].context_window = (int)strtol(wire_scratch[wire_row * 4u + 0u], NULL, 10);
      wire_held[wire_row].max_output = (int)strtol(wire_scratch[wire_row * 4u + 1u], NULL, 10);
      wire_held[wire_row].caps = (int)strtol(wire_scratch[wire_row * 4u + 2u], NULL, 10);
      wire_held[wire_row].deprecated = (int)strtol(wire_scratch[wire_row * 4u + 3u], NULL, 10);
   }
   free(wire_scratch);
   *models_out = wire_held;
   *n_out = wire_rows;
   return 0;
}

int db1_model_catalog_replace(const char *provider, const provider_model_t *models, int n)
{
   if (!provider || !provider[0] || !models || n <= 0 || n > 512)
      return -1;
   const char *fields[3073];
   fields[0] = provider;
   char (*wire_rendered)[32] = malloc((size_t)n * 4u * sizeof *wire_rendered);
   if (!wire_rendered)
      return -1;
   for (int at = 0; at < n; ++at)
   {
      fields[1 + at * 6 + 0] = models[at].id;
      fields[1 + at * 6 + 1] = models[at].display_name;
      snprintf(wire_rendered[at * 4u + 0u], 32, "%d", models[at].context_window);
      fields[1 + at * 6 + 2] = wire_rendered[at * 4u + 0u];
      snprintf(wire_rendered[at * 4u + 1u], 32, "%d", models[at].max_output);
      fields[1 + at * 6 + 3] = wire_rendered[at * 4u + 1u];
      snprintf(wire_rendered[at * 4u + 2u], 32, "%d", models[at].caps);
      fields[1 + at * 6 + 4] = wire_rendered[at * 4u + 2u];
      snprintf(wire_rendered[at * 4u + 3u], 32, "%d", models[at].deprecated);
      fields[1 + at * 6 + 5] = wire_rendered[at * 4u + 3u];
   }
   int wire_status = call_stage(AIMEE_DB1_OP_MODEL_CATALOG_REPLACE, fields, (uint32_t)(1 + n * 6), NULL, NULL, 0, NULL);
   free(wire_rendered);
   return write_result(wire_status);
}

int db1_model_price_get(const char *model, double *in_per_mtok, double *out_per_mtok)
{
   if (!model || !model[0] || !in_per_mtok || !out_per_mtok)
      return -1;
   const char *fields[] = {model};
   char slot0[32];
   char slot1[32];
   char *const values[] = {slot0, slot1};
   const size_t caps[] = {sizeof slot0, sizeof slot1};
   int wire_status = call_stage(AIMEE_DB1_OP_MODEL_PRICE_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *in_per_mtok = strtod(slot0, NULL);
   *out_per_mtok = strtod(slot1, NULL);
   return 0;
}

int db1_model_price_set(const char *model, double in_per_mtok, double out_per_mtok)
{
   if (!model || !model[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%.17g", (double)in_per_mtok);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)out_per_mtok);
   const char *fields[] = {model, arg1, arg2};
   return write_result(call_stage(AIMEE_DB1_OP_MODEL_PRICE_SET, fields, 3, NULL, NULL, 0, NULL));
}

int db1_model_price_delete(const char *model)
{
   if (!model || !model[0])
      return -1;
   const char *fields[] = {model};
   return write_result(call_stage(AIMEE_DB1_OP_MODEL_PRICE_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_working_profile_local_observe(const char *field, const char *value, double confidence, const char *session_id, int threshold)
{
   if (!field || !field[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)confidence);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", threshold);
   const char *fields[] = {field, value ? value : "", arg2, session_id ? session_id : "", arg4};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_OBSERVE, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_working_profile_local_list(db1_working_profile_local_state_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 2u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 5u + 0u] = out[wire_row].field;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].field;
      wire_values[wire_row * 5u + 1u] = out[wire_row].value;
      wire_caps[wire_row * 5u + 1u] = sizeof out[wire_row].value;
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 5u + 4u] = out[wire_row].updated_at;
      wire_caps[wire_row * 5u + 4u] = sizeof out[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 5u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 5u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].observation_count = (int)strtol(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].score = strtod(wire_scratch[wire_row * 2u + 1u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_working_profile_local_get(const char *field, db1_working_profile_local_state_t *out)
{
   if (!field || !field[0] || !out)
      return -1;
   const char *fields[] = {field};
   char slot2[32];
   char slot3[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->field, out->value, slot2, slot3, out->updated_at};
   const size_t caps[] = {sizeof out->field, sizeof out->value, sizeof slot2, sizeof slot3, sizeof out->updated_at};
   int wire_status = call_stage(AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_GET, fields, 1, values, caps, 5, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   out->observation_count = (int)strtol(slot2, NULL, 10);
   out->score = strtod(slot3, NULL);
   return 1;
}

int db1_working_profile_local_reset_field(const char *field)
{
   if (!field || !field[0])
      return -1;
   const char *fields[] = {field};
   return write_result(call_stage(AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_RESET_FIELD, fields, 1, NULL, NULL, 0, NULL));
}

int db1_tool_local_availability_set(const char *tool_uuid, int usable, const char *binary_path)
{
   if (!tool_uuid || !tool_uuid[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", usable);
   const char *fields[] = {tool_uuid, arg1, binary_path ? binary_path : ""};
   return write_result(call_stage(AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_SET, fields, 3, NULL, NULL, 0, NULL));
}

int db1_tool_local_availability_get(const char *tool_uuid, db1_tool_local_availability_t *out)
{
   if (!tool_uuid || !tool_uuid[0] || !out)
      return -1;
   const char *fields[] = {tool_uuid};
   char slot1[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->tool_uuid, slot1, out->binary_path, out->checked_at};
   const size_t caps[] = {sizeof out->tool_uuid, sizeof slot1, sizeof out->binary_path, sizeof out->checked_at};
   int wire_status = call_stage(AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_GET, fields, 1, values, caps, 4, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->usable = (int)strtol(slot1, NULL, 10);
   return 0;
}

int db1_tool_local_availability_delete(const char *tool_uuid)
{
   if (!tool_uuid || !tool_uuid[0])
      return -1;
   const char *fields[] = {tool_uuid};
   return write_result(call_stage(AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_tool_local_availability_list(db1_tool_local_availability_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 4u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 4u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 4u + 0u] = out[wire_row].tool_uuid;
      wire_caps[wire_row * 4u + 0u] = sizeof out[wire_row].tool_uuid;
      wire_values[wire_row * 4u + 1u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 4u + 1u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 4u + 2u] = out[wire_row].binary_path;
      wire_caps[wire_row * 4u + 2u] = sizeof out[wire_row].binary_path;
      wire_values[wire_row * 4u + 3u] = out[wire_row].checked_at;
      wire_caps[wire_row * 4u + 3u] = sizeof out[wire_row].checked_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 4), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 4u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 4u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].usable = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_context_cache_get(const char *hash, char *out, size_t out_len)
{
   if (!hash || !hash[0] || !out || out_len == 0)
      return -1;
   const char *fields[] = {hash};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_len, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_CACHE_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

void db1_context_cache_put(const char *hash, const char *output)
{
   if (!hash || !hash[0])
      return;
   const char *fields[] = {hash, output ? output : ""};
   (void)call_stage(AIMEE_DB1_OP_CONTEXT_CACHE_PUT, fields, 2, NULL, NULL, 0, NULL);
}

void db1_context_cache_invalidate()
{
   const char *const *fields = NULL;
   (void)call_stage(AIMEE_DB1_OP_CONTEXT_CACHE_INVALIDATE, fields, 0, NULL, NULL, 0, NULL);
}

int db1_context_snapshot_insert(const char *session_id, int64_t memory_id, double relevance_score)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)memory_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)relevance_score);
   const char *fields[] = {session_id, arg1, arg2};
   return write_result(call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_INSERT, fields, 3, NULL, NULL, 0, NULL));
}

int db1_context_snapshot_count_memories_with_min_samples(int min_samples)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", min_samples);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_COUNT_MIN_SAMPLES, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_context_snapshot_list_memory_ids_with_min_samples(int min_samples, int64_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 1024)
      max = 1024;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", min_samples);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 1u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_IDS_MIN_SAMPLES, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row] = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_context_snapshot_count_for_memory(int64_t memory_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)memory_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_COUNT_FOR_MEMORY, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_context_snapshot_list_sessions_for_memory(int64_t memory_id, char (*out)[DB1_CONTEXT_SNAPSHOT_SESSION_LEN], int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)memory_id);
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
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_SESSIONS_FOR_MEMORY, fields, 2, wire_values, wire_caps,
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

int db1_context_snapshot_insert_turn(const char *session_id, int64_t memory_id,
                                     double relevance_score, int64_t turn_index)
{
   if (!session_id || !session_id[0] || memory_id <= 0 || turn_index <= 0)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)memory_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)relevance_score);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%lld", (long long)turn_index);
   const char *fields[] = {session_id, arg1, arg2, arg3};
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_INSERT_TURN, fields, 4, NULL, NULL, 0, NULL);
   return wire_status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

int db1_context_snapshot_activation(const char *session_id,
                                    char (*out)[DB1_CONTEXT_ACTIVATION_ROW_LEN], int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_ACTIVATION, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   return (int)(wire_filled / 1u);
}

int db1_context_snapshot_has_memory(int64_t memory_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)memory_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_CONTEXT_SNAPSHOT_HAS_MEMORY, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

char *db1_agent_cache_get(const char *role, const char *prompt)
{
   const char *fields[] = {role ? role : "", prompt ? prompt : ""};
   char *value = malloc(1048576u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {1048576u};
   int wire_status = call_stage(AIMEE_DB1_OP_AGENT_CACHE_GET, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

void db1_agent_cache_put(const char *role, const char *prompt, const char *result)
{
   const char *fields[] = {role ? role : "", prompt ? prompt : "", result ? result : ""};
   (void)call_stage(AIMEE_DB1_OP_AGENT_CACHE_PUT, fields, 3, NULL, NULL, 0, NULL);
}

char *db1_web_page_get(const char *url, long *age_secs, char *pinned_addr_out, size_t pinned_addr_len)
{
   if (!url || !url[0] || !age_secs || !pinned_addr_out || pinned_addr_len == 0)
      return NULL;
   const char *fields[] = {url};
   char *value = malloc(1048576u);
   if (!value)
      return NULL;
   value[0] = '\0';
   char slot1[32];
   char *const values[] = {value, slot1, pinned_addr_out};
   const size_t caps[] = {1048576u, sizeof slot1, pinned_addr_len};
   int wire_status = call_stage(AIMEE_DB1_OP_WEB_PAGE_GET, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   *age_secs = (int64_t)strtoll(slot1, NULL, 10);
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

int db1_web_page_put(const char *url, const char *body, const char *pinned_addr)
{
   if (!url || !url[0])
      return -1;
   const char *fields[] = {url, body ? body : "", pinned_addr ? pinned_addr : ""};
   return write_result(call_stage(AIMEE_DB1_OP_WEB_PAGE_PUT, fields, 3, NULL, NULL, 0, NULL));
}

void db1_web_page_drop(const char *url)
{
   if (!url || !url[0])
      return;
   const char *fields[] = {url};
   (void)call_stage(AIMEE_DB1_OP_WEB_PAGE_DROP, fields, 1, NULL, NULL, 0, NULL);
}

int db1_web_page_canonical_url(const char *url, char *out, size_t out_len)
{
   if (!url || !url[0] || !out || out_len == 0)
      return -1;
   const char *fields[] = {url};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_len, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_WEB_PAGE_CANONICAL_URL, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int64_t db1_fsnap_create(const char *session_id, int turn, const char *label)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", turn);
   const char *fields[] = {session_id, arg1, label ? label : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_FSNAP_CREATE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int64_t db1_fsnap_get_or_create(const char *session_id, int turn, const char *label)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", turn);
   const char *fields[] = {session_id, arg1, label ? label : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_FSNAP_GET_OR_CREATE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_fsnap_record_file(int64_t snap_id, const char *path)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)snap_id);
   const char *fields[] = {arg0, path ? path : ""};
   return write_result(call_stage(AIMEE_DB1_OP_FSNAP_RECORD_FILE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_fsnap_prune(const char *session_id, int keep)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", keep);
   const char *fields[] = {session_id, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_FSNAP_PRUNE, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_fsnap_list(const char *session_id, fsnap_info_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 6u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 6u + 1u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 6u + 1u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 6u + 2u] = out[wire_row].session_id;
      wire_caps[wire_row * 6u + 2u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 6u + 3u] = out[wire_row].created_at;
      wire_caps[wire_row * 6u + 3u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 6u + 4u] = out[wire_row].label;
      wire_caps[wire_row * 6u + 4u] = sizeof out[wire_row].label;
      wire_values[wire_row * 6u + 5u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 6u + 5u] = sizeof wire_scratch[wire_row * 3u + 2u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_FSNAP_LIST, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].turn = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].file_count = (int)strtol(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_fsnap_restore(int64_t snap_id, int *files_restored, int *files_deleted)
{
   if (!files_restored || !files_deleted)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)snap_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char *const values[] = {slot0, slot1};
   const size_t caps[] = {sizeof slot0, sizeof slot1};
   int wire_status = call_stage(AIMEE_DB1_OP_FSNAP_RESTORE, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *files_restored = (int)strtol(slot0, NULL, 10);
   *files_deleted = (int)strtol(slot1, NULL, 10);
   return 0;
}

int db1_fsnap_get(int64_t snap_id, fsnap_info_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)snap_id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   char slot5[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, out->session_id, out->created_at, out->label, slot5};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof out->session_id, sizeof out->created_at, sizeof out->label, sizeof slot5};
   int wire_status = call_stage(AIMEE_DB1_OP_FSNAP_GET, fields, 1, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   out->turn = (int)strtol(slot1, NULL, 10);
   out->file_count = (int)strtol(slot5, NULL, 10);
   return 0;
}

int db1_decision_record(int64_t window_id, const char *description, const char *created_at)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)window_id);
   const char *fields[] = {arg0, description ? description : "", created_at ? created_at : ""};
   return write_result(call_stage(AIMEE_DB1_OP_DECISION_RECORD, fields, 3, NULL, NULL, 0, NULL));
}

int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version, int ttl_hours, db1_mcp_osv_cache_row_t *out)
{
   if (!ecosystem || !ecosystem[0] || !name || !name[0] || !version || !version[0] || !out)
      return -1;
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", ttl_hours);
   const char *fields[] = {ecosystem, name, version, arg3};
   char slot6[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->client_name, out->ecosystem, out->name, out->version, out->verdict, out->advisory_ids, slot6, out->checked_at_text};
   const size_t caps[] = {sizeof out->client_name, sizeof out->ecosystem, sizeof out->name, sizeof out->version, sizeof out->verdict, sizeof out->advisory_ids, sizeof slot6, sizeof out->checked_at_text};
   int wire_status = call_stage(AIMEE_DB1_OP_MCP_OSV_CACHE_GET, fields, 4, values, caps, 8, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   out->checked_at = (int64_t)strtoll(slot6, NULL, 10);
   return 1;
}

int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version, const char *verdict, const char *advisory_ids)
{
   if (!ecosystem || !ecosystem[0] || !name || !name[0] || !version || !version[0])
      return -1;
   const char *fields[] = {ecosystem, name, version, verdict ? verdict : "", advisory_ids ? advisory_ids : ""};
   return write_result(call_stage(AIMEE_DB1_OP_MCP_OSV_CACHE_UPSERT, fields, 5, NULL, NULL, 0, NULL));
}

int db1_mcp_osv_cache_list(db1_mcp_osv_cache_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = out[wire_row].client_name;
      wire_caps[wire_row * 8u + 0u] = sizeof out[wire_row].client_name;
      wire_values[wire_row * 8u + 1u] = out[wire_row].ecosystem;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].ecosystem;
      wire_values[wire_row * 8u + 2u] = out[wire_row].name;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].name;
      wire_values[wire_row * 8u + 3u] = out[wire_row].version;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].version;
      wire_values[wire_row * 8u + 4u] = out[wire_row].verdict;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].verdict;
      wire_values[wire_row * 8u + 5u] = out[wire_row].advisory_ids;
      wire_caps[wire_row * 8u + 5u] = sizeof out[wire_row].advisory_ids;
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].checked_at_text;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].checked_at_text;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_MCP_OSV_CACHE_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].checked_at = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name, const char *version, const char *verdict, const char *action, const char *advisory_ids)
{
   if (!ecosystem || !ecosystem[0] || !name || !name[0] || !version || !version[0])
      return -1;
   const char *fields[] = {client_name ? client_name : "", ecosystem, name, version, verdict ? verdict : "", action ? action : "", advisory_ids ? advisory_ids : ""};
   return write_result(call_stage(AIMEE_DB1_OP_MCP_OSV_AUDIT, fields, 7, NULL, NULL, 0, NULL));
}

/* clang-format on */
