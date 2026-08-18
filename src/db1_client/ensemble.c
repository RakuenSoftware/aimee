/* db1_client/ensemble.c: the ensemble family, reached over the bus.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
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
#include "ensemble.h"

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

#define DB1_ENSEMBLE_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.ensemble", "DB1 %s is unreachable (module call result %d)", "ensemble",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_ENSEMBLE))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_ENSEMBLE_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_ENSEMBLE, AIMEE_DB1_STAGE_ENSEMBLE, 0, deadline,
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


int db1_ensemble_create_id(const char *project_root, const char *config_dir, const char *template_name, const char *channel, const char *assignments_json, ensemble_id_result_t *out)
{
   if (!template_name || !template_name[0] || !out)
      return -1;
   const char *fields[] = {project_root ? project_root : "", config_dir ? config_dir : "", template_name, channel ? channel : "", assignments_json ? assignments_json : ""};
   char slot0[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->err, slot2};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2};
   int wire_status = call_stage(AIMEE_DB1_OP_ENSEMBLE_CREATE, fields, 5, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->id = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_ensemble_view(int id, ensemble_view_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot2[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   memset(out, 0, sizeof *out);
   out->prompt = malloc(1048576u);
   if (!out->prompt)
   {
      free(out->prompt);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->prompt[0] = '\0';
   out->context = malloc(1048576u);
   if (!out->context)
   {
      free(out->prompt);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->context[0] = '\0';
   char *const values[] = {slot0, out->err, slot2, out->info.template_name, out->info.channel, out->info.status, slot6, slot7, slot8, slot9, out->info.phase_name, out->info.expected_agent, out->info.expected_role, out->info.paused_reason, out->info.created_at, out->info.updated_at, out->prompt, out->context};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2, sizeof out->info.template_name, sizeof out->info.channel, sizeof out->info.status, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9, sizeof out->info.phase_name, sizeof out->info.expected_agent, sizeof out->info.expected_role, sizeof out->info.paused_reason, sizeof out->info.created_at, sizeof out->info.updated_at, 1048576u, 1048576u};
   int wire_status = call_stage(AIMEE_DB1_OP_ENSEMBLE_VIEW, fields, 1, values, caps, 18, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->prompt);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->info.id = (int)strtol(slot2, NULL, 10);
   out->info.current_phase = (int)strtol(slot6, NULL, 10);
   out->info.current_turn = (int)strtol(slot7, NULL, 10);
   out->info.phase_count = (int)strtol(slot8, NULL, 10);
   out->info.turns_in_phase = (int)strtol(slot9, NULL, 10);
   char *shrunk_prompt = realloc(out->prompt, strlen(out->prompt) + 1u);
   if (shrunk_prompt)
      out->prompt = shrunk_prompt;
   char *shrunk_context = realloc(out->context, strlen(out->context) + 1u);
   if (shrunk_context)
      out->context = shrunk_context;
   return 0;
}

int db1_ensemble_advance_view(int id, const char *sender, const char *text, ensemble_view_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, sender ? sender : "", text ? text : ""};
   char slot0[32];
   char slot2[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   memset(out, 0, sizeof *out);
   out->prompt = malloc(1048576u);
   if (!out->prompt)
   {
      free(out->prompt);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->prompt[0] = '\0';
   out->context = malloc(1048576u);
   if (!out->context)
   {
      free(out->prompt);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->context[0] = '\0';
   char *const values[] = {slot0, out->err, slot2, out->info.template_name, out->info.channel, out->info.status, slot6, slot7, slot8, slot9, out->info.phase_name, out->info.expected_agent, out->info.expected_role, out->info.paused_reason, out->info.created_at, out->info.updated_at, out->prompt, out->context};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2, sizeof out->info.template_name, sizeof out->info.channel, sizeof out->info.status, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9, sizeof out->info.phase_name, sizeof out->info.expected_agent, sizeof out->info.expected_role, sizeof out->info.paused_reason, sizeof out->info.created_at, sizeof out->info.updated_at, 1048576u, 1048576u};
   int wire_status = call_stage(AIMEE_DB1_OP_ENSEMBLE_ADVANCE, fields, 3, values, caps, 18, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->prompt);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->info.id = (int)strtol(slot2, NULL, 10);
   out->info.current_phase = (int)strtol(slot6, NULL, 10);
   out->info.current_turn = (int)strtol(slot7, NULL, 10);
   out->info.phase_count = (int)strtol(slot8, NULL, 10);
   out->info.turns_in_phase = (int)strtol(slot9, NULL, 10);
   char *shrunk_prompt = realloc(out->prompt, strlen(out->prompt) + 1u);
   if (shrunk_prompt)
      out->prompt = shrunk_prompt;
   char *shrunk_context = realloc(out->context, strlen(out->context) + 1u);
   if (shrunk_context)
      out->context = shrunk_context;
   return 0;
}

int db1_ensemble_pause(int id, const char *reason, char *err, size_t errlen)
{
   if (!err || errlen == 0)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, reason ? reason : ""};
   char slot_rc[32];
   char *const values[] = {err, slot_rc};
   const size_t caps[] = {errlen, sizeof slot_rc};
   int wire_status = call_stage(AIMEE_DB1_OP_ENSEMBLE_PAUSE, fields, 2, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_ensemble_list_rows(ensemble_info_t **out, int *out_count)
{
   if (!out || !out_count)
      return -1;
   const char *const *fields = NULL;
   ensemble_info_t *wire_held = calloc((size_t)512, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)512 * 14u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)512 * 14u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)512 * 5u * sizeof *wire_scratch);
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
      wire_values[wire_row * 14u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 14u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 14u + 1u] = wire_held[wire_row].template_name;
      wire_caps[wire_row * 14u + 1u] = sizeof wire_held[wire_row].template_name;
      wire_values[wire_row * 14u + 2u] = wire_held[wire_row].channel;
      wire_caps[wire_row * 14u + 2u] = sizeof wire_held[wire_row].channel;
      wire_values[wire_row * 14u + 3u] = wire_held[wire_row].status;
      wire_caps[wire_row * 14u + 3u] = sizeof wire_held[wire_row].status;
      wire_values[wire_row * 14u + 4u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 14u + 4u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 14u + 5u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 14u + 5u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 14u + 6u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 14u + 6u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 14u + 7u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 14u + 7u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 14u + 8u] = wire_held[wire_row].phase_name;
      wire_caps[wire_row * 14u + 8u] = sizeof wire_held[wire_row].phase_name;
      wire_values[wire_row * 14u + 9u] = wire_held[wire_row].expected_agent;
      wire_caps[wire_row * 14u + 9u] = sizeof wire_held[wire_row].expected_agent;
      wire_values[wire_row * 14u + 10u] = wire_held[wire_row].expected_role;
      wire_caps[wire_row * 14u + 10u] = sizeof wire_held[wire_row].expected_role;
      wire_values[wire_row * 14u + 11u] = wire_held[wire_row].paused_reason;
      wire_caps[wire_row * 14u + 11u] = sizeof wire_held[wire_row].paused_reason;
      wire_values[wire_row * 14u + 12u] = wire_held[wire_row].created_at;
      wire_caps[wire_row * 14u + 12u] = sizeof wire_held[wire_row].created_at;
      wire_values[wire_row * 14u + 13u] = wire_held[wire_row].updated_at;
      wire_caps[wire_row * 14u + 13u] = sizeof wire_held[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_ENSEMBLE_LIST, fields, 0, wire_values, wire_caps,
                           (uint32_t)(512 * 14), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 14u != 0u)
   {
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 14u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      wire_held[wire_row].id = (int)strtol(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      wire_held[wire_row].current_phase = (int)strtol(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      wire_held[wire_row].current_turn = (int)strtol(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      wire_held[wire_row].phase_count = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      wire_held[wire_row].turns_in_phase = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   *out = wire_held;
   *out_count = wire_rows;
   return 0;
}

int db1_ensemble_find_current_id(const char *channel, ensemble_id_result_t *out)
{
   if (!channel || !channel[0] || !out)
      return -1;
   const char *fields[] = {channel};
   char slot0[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->err, slot2};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2};
   int wire_status = call_stage(AIMEE_DB1_OP_ENSEMBLE_FIND_CURRENT_BY_CHANNEL, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->id = (int)strtol(slot2, NULL, 10);
   return 0;
}

/* clang-format on */
